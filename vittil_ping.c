/*
 * vittil_ping.c: the ping. The device says who it is on an interval and
 * the Devices list shows it Live. The identity, the ping's body, and the
 * round every send goes through: the id is the factory base MAC alone, the
 * retry is bounded with backoff and jitter, a refused key stops the sends.
 * The task that calls the round is vittil.c. Every function checks what it
 * is given and every buffer write is bounded with its return checked.
 */
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "vittil_priv.h"

static const char *TAG = "vittil";

/* The file's state, one struct: the identity, filled once by
 * vittil_identity_init, read-only after (the task reads it; nothing writes
 * it again); and whether the last ping round was delivered (the task alone
 * writes it). */
static struct {
    char device_id[13];  /* the factory base MAC, 12 lowercase hex */
    char fw_version[32]; /* esp_app_desc_t.version, 32 in the descriptor */
    char build_id[65];   /* the ELF's sha256 as the descriptor keeps it, up to 64 hex */
    bool delivering;     /* the last round delivered: its next success is a debug line */
} vittil_ping;

esp_err_t vittil_identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        /* a factory MAC with a bad CRC exists in the field; no id is invented */
        ESP_LOGE(TAG, "no device id: esp_read_mac failed: %s", esp_err_to_name(err));
        return err;
    }
    int n = snprintf(vittil_ping.device_id, sizeof(vittil_ping.device_id), "%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (n != 12) {
        ESP_LOGE(TAG, "no device id: formatted %d characters, not 12", n);
        vittil_ping.device_id[0] = 0;
        return ESP_FAIL;
    }
    const esp_app_desc_t *d = esp_app_get_description();
    if (!d) {
        ESP_LOGE(TAG, "no app descriptor");
        return ESP_FAIL;
    }
    /* the version is the user's text (PROJECT_VER, a git tag may hold a quote) */
    vittil_json_safe(vittil_ping.fw_version, sizeof(vittil_ping.fw_version), d->version, sizeof(d->version));
    strlcpy(vittil_ping.build_id, esp_app_get_elf_sha256_str(), sizeof(vittil_ping.build_id));
    if (strncmp(vittil_ping.fw_version, d->version, sizeof(d->version)) != 0) {
        ESP_LOGD(TAG, "fw version %.32s sent as %s (JSON-safe)", d->version, vittil_ping.fw_version);
    }
    return ESP_OK;
}

void vittil_json_safe(char *dst, size_t n, const char *src, size_t src_n)
{
    if (!dst || n == 0) {
        return;
    }
    size_t i = 0;
    for (; src && i + 1 < n && i < src_n && src[i]; i++) {
        char c = src[i];
        dst[i] = (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? '_' : c;
    }
    dst[i] = 0;
}

const char *vittil_device_id(void)
{
    return vittil_ping.device_id;
}

const char *vittil_fw_version(void)
{
    return vittil_ping.fw_version;
}

const char *vittil_build_id(void)
{
    return vittil_ping.build_id;
}

/* The body, {"device_id","fw_version","build_id"}; the server fills `at`
 * with its own clock. 46 bytes of JSON around at most 12 + 31 + 64 of
 * values, so 160 holds the longest. */
static esp_err_t ping_once(int *status)
{
    char body[160];
    if (!vittil_ping.device_id[0]) {
        ESP_LOGE(TAG, "ping: no device id");
        return ESP_ERR_NOT_FOUND;
    }
    int n = snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"fw_version\":\"%s\",\"build_id\":\"%s\"}",
                     vittil_ping.device_id, vittil_ping.fw_version, vittil_ping.build_id);
    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "ping body too long (%d), not sent", n);
        return ESP_ERR_INVALID_SIZE;
    }
    return vittil_post_small("/api/ping", body, "ping", status);
}

/* The network check is free and stays frequent: one read a second,
 * for as long as it takes. A minute without one is said once per wait, so a
 * device that never gets a network is not silent about why nothing is sent. */
static void wait_net(const char *what)
{
    uint32_t s = 0;
    while (!vittil_net_ready()) {
        if (++s == 60) {
            ESP_LOGW(TAG, "%s waiting: no network for 60 s", what);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s >= 60) {
        ESP_LOGI(TAG, "%s: network up after %" PRIu32 " s", what, s);
    }
}

/* A delay drawn between half and the whole of its step, so devices that
 * rebooted together do not retry together.
 * A 1 s step has no half to draw from and is used as it is. */
static uint32_t jittered_s(uint32_t step)
{
    uint32_t half = step / 2;
    if (half == 0) {
        return step;
    }
    return half + esp_random() % half;
}

/* One round of a send (the ping; the upload and the no-dump event use the
 * same). The send, then on a failure the server did not choose (no
 * connection, a timeout, no answer, a 5xx) a retry after
 * VITTIL_RETRY_DELAY_S doubling each time, VITTIL_RETRY_COUNT
 * retries in all, then the round ends and the send waits for its next turn
 * (the ping's interval, the upload's next boot). A status under 500
 * the server answered is its word on these bytes and is not retried; a
 * refused key ends the sends until the next boot; the network going
 * away between the check and the send is not an attempt, the round goes
 * back to waiting for it. Ticks are computed from seconds directly:
 * pdMS_TO_TICKS overflows its 32 bits past 42,949 s at 100 Hz. */
esp_err_t vittil_round(esp_err_t (*once)(int *status), const char *what, int *status)
{
    if (status) {
        *status = -1;
    }
    if (!once || !what) {
        ESP_LOGE(TAG, "round: NULL send or name");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t step = VITTIL_RETRY_DELAY_S;
    int retries = 0;
    for (;;) {
        wait_net(what);
        int st = -1;
        esp_err_t err = once(&st);
        if (status) {
            *status = st;
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_ALLOWED || err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE ||
            err == ESP_ERR_NOT_FOUND) {
            return err; /* logged where it happened; nothing a retry would change */
        }
        if (err == ESP_ERR_INVALID_STATE) {
            continue;
        }
        if (st >= 200 && st < 500) {
            ESP_LOGW(TAG, "%s answered HTTP %d, not retried", what, st);
            return err;
        }
        if (retries >= VITTIL_RETRY_COUNT) {
            ESP_LOGW(TAG, "%s not delivered after %d retries, next at its turn", what, retries);
            return err;
        }
        uint32_t wait = jittered_s(step);
        ESP_LOGW(TAG, "%s failed (%s, status %d), retry %d of %d in %" PRIu32 " s", what, esp_err_to_name(err), st,
                 retries + 1, VITTIL_RETRY_COUNT, wait);
        vTaskDelay((TickType_t)wait * configTICK_RATE_HZ);
        retries++;
        step *= 2;
    }
}

/* One info line when the pings start reaching the server (the boot's first,
 * or the first after a round that failed), debug after: the ping every
 * interval is not a line each time. */
void vittil_ping_round(void)
{
    int st = -1;
    esp_err_t err = vittil_round(ping_once, "ping", &st);
    if (err == ESP_OK && !vittil_ping.delivering) {
        ESP_LOGI(TAG, "ping delivered (HTTP %d), every %d s from here", st, CONFIG_VITTIL_PING_INTERVAL_S);
    } else if (err == ESP_OK) {
        ESP_LOGD(TAG, "ping delivered (HTTP %d)", st);
    }
    vittil_ping.delivering = (err == ESP_OK);
}
