/*
 * vittil.c: the one public call, vittil_start(), and the SDK's task. One
 * task, created at start, never exits; its stack is a .bss array reserved
 * at boot, so a ping can never fail later for lack of memory. The task
 * sends the boot's report (vittil_upload.c; the upload borrows its two chunk
 * buffers from the heap for its duration), then pings on the interval.
 */
#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "vittil.h"
#include "vittil_priv.h"

static const char *TAG = "vittil";

/* Priority 1, the lowest above idle and app_main's own: the ping is never
 * urgent and must never take the processor from the application's work.
 * The stack is VITTIL_TASK_STACK bytes of .bss. */
#define VITTIL_TASK_PRIORITY 1

/* The file's state, one struct: the task's stack and control block in .bss,
 * and whether vittil_start has run. */
static struct {
    StackType_t stack[VITTIL_TASK_STACK / sizeof(StackType_t)];
    StaticTask_t tcb;
    bool started;
} vittil_sdk;

/* The application's transport, checked here (the struct, the start) and
 * kept by the transport. */
esp_err_t vittil_set_transport(const vittil_transport_t *t)
{
#if CONFIG_VITTIL_TRANSPORT_APP
    if (!t || !t->begin || !t->write || !t->end) {
        ESP_LOGE(TAG, "vittil_set_transport: NULL transport or a missing function, not set");
        return ESP_ERR_INVALID_ARG;
    }
    if (vittil_sdk.started) {
        ESP_LOGE(TAG, "vittil_set_transport after vittil_start, not set");
        return ESP_ERR_INVALID_STATE;
    }
    vittil_transport_set_app(t);
    return ESP_OK;
#else
    (void)t;
    ESP_LOGW(TAG, "vittil_set_transport: CONFIG_VITTIL_TRANSPORT_APP is off, the default transport sends");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void vittil_task(void *arg)
{
    (void)arg;
    /* seconds to ticks directly: pdMS_TO_TICKS overflows past 42,949 s */
    const TickType_t interval = (TickType_t)CONFIG_VITTIL_PING_INTERVAL_S * configTICK_RATE_HZ;
    /* the boot's one report (the staged dump, or the no-dump event, or
     * nothing) before the first ping */
    vittil_record_clock_note(); /* the wall clock beside the timer, for the record's epoch */
    vittil_upload_round();
    ESP_LOGD(TAG, "task stack high-water %u bytes free of %u after the upload round",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)sizeof(vittil_sdk.stack));
    for (;;) {
        vittil_record_clock_note(); /* again each round: SNTP may have set the clock since */
        vittil_ping_round();
        ESP_LOGD(TAG, "task stack high-water %u bytes free of %u", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)sizeof(vittil_sdk.stack));
        vTaskDelay(interval);
    }
}

/* The project key from CONFIG_VITTIL_PROJECT_KEY: "ecr_" and 43 characters
 * of [A-Za-z0-9_-], as the Vittil app shows it. Anything else is not set,
 * with one warning naming the length, so the start line says "no key"; the
 * key itself is never logged. A key already set (vittil_transport_set_key,
 * before vittil_start) is kept. */
#define VITTIL_KEY_LEN 47
static void project_key(void)
{
    /* a string literal: its length is its size, known at compile time */
    static const char k[] = CONFIG_VITTIL_PROJECT_KEY;
    const size_t n = sizeof(k) - 1;
    if (vittil_transport_has_key() || n == 0) {
        return;
    }
    bool ok = n == VITTIL_KEY_LEN && strncmp(k, "ecr_", 4) == 0;
    for (size_t i = 4; ok && i < n; i++) {
        char c = k[i];
        ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    if (!ok) {
        ESP_LOGW(TAG, "CONFIG_VITTIL_PROJECT_KEY is not a Vittil project key (\"ecr_\" and 43 characters; %u given)",
                 (unsigned)n);
        return;
    }
    vittil_transport_set_key(k);
    ESP_LOGD(TAG, "project key from CONFIG_VITTIL_PROJECT_KEY");
}

/* The start line: who the device is, where it sends, and what a crash will
 * bring (the dump with the partition's size, or the summary, or the reset
 * alone, and why). One info line; a missing key is a warning of its own. */
static void start_line(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    const char *why = "no coredump partition";
#else
    const esp_partition_t *p = NULL;
    const char *why = "coredump to flash off";
#endif
#if CONFIG_VITTIL_WRITER
    const char *fit = ", fitted to it";
#else
    const char *fit = "";
#endif
#if CONFIG_VITTIL_SUMMARY
    const char *instead = "a crash summary";
#else
    const char *instead = "its reset reason only";
#endif
    const char *key = vittil_transport_has_key() ? "key set" : "no key";
#if CONFIG_VITTIL_TRANSPORT_APP
    const char *via = " by the application's transport";
#else
    const char *via = "";
#endif
    if (p) {
        ESP_LOGI(TAG, "start: device %s, fw %s, build %s; server %s%s, %s; a crash sends its coredump "
                 "(partition %u bytes%s); ping every %d s",
                 vittil_device_id(), vittil_fw_version(), vittil_build_id(), CONFIG_VITTIL_SERVER_URL, via, key,
                 (unsigned)p->size, fit, CONFIG_VITTIL_PING_INTERVAL_S);
    } else {
        ESP_LOGI(TAG, "start: device %s, fw %s, build %s; server %s%s, %s; a crash sends %s (%s); ping every %d s",
                 vittil_device_id(), vittil_fw_version(), vittil_build_id(), CONFIG_VITTIL_SERVER_URL, via, key,
                 instead, why, CONFIG_VITTIL_PING_INTERVAL_S);
    }
    if (!vittil_transport_has_key()) {
        ESP_LOGW(TAG, "no project key: the server will refuse every send");
    }
}

esp_err_t vittil_start(void)
{
    if (vittil_sdk.started) {
        ESP_LOGE(TAG, "vittil_start called twice");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_VITTIL_TRANSPORT_APP
    if (!vittil_transport_app_set()) {
        ESP_LOGE(TAG, "not started: CONFIG_VITTIL_TRANSPORT_APP is on and no transport is registered "
                 "(vittil_set_transport before vittil_start); no crash reports from this boot");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    esp_err_t err = vittil_identity_init();
    if (err != ESP_OK) {
        /* the cause logged there; no task, nothing is sent */
        ESP_LOGE(TAG, "not started (%s): no crash reports from this boot", esp_err_to_name(err));
        return err;
    }
    err = vittil_upload_init(); /* the boot facts, one log line */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not started (%s): no crash reports from this boot", esp_err_to_name(err));
        return err;
    }
    vittil_record_init(); /* the log ring fills from here on */
#if CONFIG_VITTIL_WRITER
    vittil_writer_boot_check(); /* the partition against the floor, one warning */
#endif
    project_key();
    start_line();
    TaskHandle_t t = xTaskCreateStatic(vittil_task, "vittil", sizeof(vittil_sdk.stack), NULL, VITTIL_TASK_PRIORITY,
                                       vittil_sdk.stack, &vittil_sdk.tcb);
    if (!t) {
        ESP_LOGE(TAG, "not started (task not created): no crash reports from this boot");
        return ESP_FAIL;
    }
    vittil_sdk.started = true;
    return ESP_OK;
}
