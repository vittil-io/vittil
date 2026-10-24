/*
 * vittil_upload.c: the upload. The staged coredump goes to the
 * server on the boot after the crash and is erased only on the server's
 * yes; a crash reset that left no dump is reported as such. The device
 * parses nothing out of the dump: it sends its identity, the versions, the
 * reset reason, an event id from the dump's CRC trailer and the bytes as
 * base64.
 *
 * The parts: the boot facts and the decision, the event id from the CRC,
 * the streamed body and the base64, the erase on the server's yes, the
 * no-dump event, the crash time. The transport's streamed body carries it;
 * the round with its retries is vittil_ping.c's.
 * Every function checks what it is given, every buffer write is bounded with
 * its return checked, the two heap buffers are freed on every path. Without
 * CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH in the application's build only the
 * no-dump event is compiled.
 *
 * The crash summary (vittil_summary.c): when no dump is staged, a
 * summary the last crash kept in RTC or noinit memory is the report, sent
 * as summary_b64 beside the same facts and cleared on the server's yes.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "mbedtls/base64.h"
#include "esp_private/panic_internal.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

#include "vittil_priv.h"
#include "vittil_summary.h"

static const char *TAG = "vittil";

/* Each chunk's base64 must be whole, so the chunks' encodings concatenate
 * to the dump's. */
_Static_assert(VITTIL_UPLOAD_CHUNK_BYTES % 3 == 0, "VITTIL_UPLOAD_CHUNK_BYTES must be a multiple of 3");

/* The file's state, one struct: the boot facts, filled once by
 * vittil_upload_init; have_dump cleared by the round once the dump's fate
 * is decided. The task reads them; nothing else writes them. */
static struct {
    esp_reset_reason_t reset; /* this boot's reset reason */
    bool have_dump;           /* a dump staged in flash with a good checksum */
    char idf_ver[32];         /* esp_app_desc_t.idf_ver, 32 in the descriptor */
    uint32_t nodump;          /* why the panic left no dump (VITTIL_NODUMP_*), 0 unknown */
    uint32_t nodump_a, nodump_b; /* UNDER_FLOOR: the floor and the partition, bytes */
    bool have_summary;        /* a crash summary waits (no dump staged) */
} vittil_upload;

/* The no-dump reasons as the server's reader keys them. */
static const char *nodump_name(uint32_t r)
{
    switch (r) {
    case VITTIL_NODUMP_WRITE_FAILED: return "write_failed";
    case VITTIL_NODUMP_NO_PARTITION: return "no_partition";
    case VITTIL_NODUMP_UNDER_FLOOR:  return "under_floor";
    default:                         return "unknown";
    }
}

/* The reset reasons as the server keys them. */
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "UNKNOWN";
    }
}

/* The resets a crash leaves behind; the others (power-on, the
 * application's own restart, deep sleep) are not reported. */
static bool is_crash_reset(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP;
}

/* The crash's time is now minus this boot's uptime, when the
 * application has set the clock (SNTP); 0 when it has not (the clock reads
 * 1970), the field is left out and the server stamps the arrival. */
static int64_t crash_epoch(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) {
        return 0;
    }
    return (int64_t)now - esp_timer_get_time() / 1000000;
}

/* A JSON body in a fixed buffer. Each add appends a piece; once one did not
 * fit (n at or past cap) the rest are skipped and body_ok refuses the body. */
typedef struct {
    char *b;
    size_t cap;
    int n;
} body_t;

static void add(body_t *j, const char *fmt, ...)
{
    if (j->n < 0 || (size_t)j->n >= j->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(j->b + j->n, j->cap - (size_t)j->n, fmt, ap);
    va_end(ap);
    j->n = (k < 0) ? -1 : j->n + k;
}

static bool body_ok(const body_t *j, const char *what)
{
    if (j->n < 0 || (size_t)j->n >= j->cap) {
        ESP_LOGE(TAG, "%s too long (%d), not sent", what, j->n);
        return false;
    }
    return true;
}

/* The facts every body opens with. */
static void add_facts(body_t *j, const char *id, const char *fw, const char *build, const char *reset)
{
    add(j, "{\"device_id\":\"%s\",\"fw_version\":\"%s\",\"build_id\":\"%s\",\"idf_ver\":\"%s\",\"reset_reason\":\"%s\"",
        id, fw, build, vittil_upload.idf_ver, reset);
}

/* This boot's crash: its time when the clock was set; why there is no dump
 * (never beside a dump: have_dump and nodump exclude each other), with the
 * floor and the partition when that is why. */
static void add_crash(body_t *j)
{
    int64_t at = crash_epoch();
    if (at) {
        add(j, ",\"at\":%lld", (long long)at);
    }
    if (!vittil_upload.nodump) {
        return;
    }
    add(j, ",\"no_dump\":\"%s\"", nodump_name(vittil_upload.nodump));
    if (vittil_upload.nodump == VITTIL_NODUMP_UNDER_FLOOR) {
        add(j, ",\"floor\":%u,\"partition\":%u", (unsigned)vittil_upload.nodump_a, (unsigned)vittil_upload.nodump_b);
    }
}

/* The device id, or NULL with `what`'s log line. */
static const char *device_id(const char *what)
{
    const char *id = vittil_device_id();
    if (!id[0]) {
        ESP_LOGE(TAG, "%s: no device id", what);
        return NULL;
    }
    return id;
}

/* An answer the same bytes would draw at every boot. */
static bool refused(int status)
{
    return status == 400 || status == 413;
}

/* What a streamed body's base64 is made of: size bytes in memory (mem), or
 * at off in the partition, read into raw a chunk at a time; b64 holds one
 * chunk's base64 and its NUL. */
typedef struct {
    const uint8_t *mem;
    const esp_partition_t *part;
    size_t off, size, chunk;
    uint8_t *raw;
    unsigned char *b64;
    size_t b64cap;
} b64_src_t;

static const char B64_SUFFIX[] = "\"}";

/* The streamed body's length: the prefix, the base64, the closing quote and brace. */
static size_t b64_body_len(const body_t *j, size_t size)
{
    return (size_t)j->n + 4 * ((size + 2) / 3) + sizeof(B64_SUFFIX) - 1;
}

/* One streamed body: the prefix, then the bytes base64 a chunk at a time,
 * written as it goes, then the suffix; the whole length known before the
 * first byte. A chunk the flash would not read is ESP_ERR_NOT_FOUND (not a
 * network failure: the dump is kept, the next boot tries) and a base64 that
 * did not fit ESP_ERR_INVALID_SIZE (our own sizing), neither retried: the
 * connection is still ended, which sees the body short and frees. */
static esp_err_t send_b64(const char *what, const body_t *j, const b64_src_t *src, int *status)
{
    vittil_post_t p;
    esp_err_t err = vittil_post_begin("/api/crashes", b64_body_len(j, src->size), what, &p);
    if (err != ESP_OK) {
        return err;
    }
    err = vittil_post_write(&p, j->b, (size_t)j->n);
    for (size_t off = 0; err == ESP_OK && off < src->size; off += src->chunk) {
        size_t len = src->size - off < src->chunk ? src->size - off : src->chunk;
        size_t olen = 0;
        const uint8_t *in = src->mem ? src->mem + off : src->raw;
        if (!src->mem && esp_partition_read(src->part, src->off + off, src->raw, len) != ESP_OK) {
            ESP_LOGE(TAG, "coredump read failed at %u", (unsigned)off);
            err = ESP_ERR_NOT_FOUND;
        } else if (mbedtls_base64_encode(src->b64, src->b64cap, &olen, in, len) != 0) {
            ESP_LOGE(TAG, "base64 of %u bytes did not fit %u", (unsigned)len, (unsigned)src->b64cap);
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = vittil_post_write(&p, src->b64, olen);
        }
    }
    if (err == ESP_OK) {
        err = vittil_post_write(&p, B64_SUFFIX, sizeof(B64_SUFFIX) - 1);
    }
    int st = -1;
    esp_err_t end = vittil_post_end(&p, what, &st); /* frees the handle on every path */
    if (status) {
        *status = st;
    }
    return err != ESP_OK ? err : end;
}

esp_err_t vittil_upload_init(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    if (!d) {
        ESP_LOGE(TAG, "no app descriptor");
        return ESP_FAIL;
    }
    vittil_json_safe(vittil_upload.idf_ver, sizeof(vittil_upload.idf_ver), d->idf_ver, sizeof(d->idf_ver));
    vittil_upload.reset = esp_reset_reason();
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    vittil_upload.have_dump = (esp_core_dump_image_check() == ESP_OK);
    if (!vittil_upload.have_dump) {
        /* the stock writer only writes into a blank partition (the no-overwrite
         * option): anything else in it that is not a whole dump (another
         * firmware's data, a write cut by a power loss) would keep every later
         * crash from writing one, so it is erased */
        const esp_partition_t *p =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        uint32_t first = UINT32_MAX;
        if (p && esp_partition_read(p, 0, &first, sizeof(first)) == ESP_OK && first != UINT32_MAX) {
            esp_err_t e = esp_core_dump_image_erase();
            ESP_LOGW(TAG, "coredump partition held no whole dump: erased (%s)", esp_err_to_name(e));
        }
    }
#else
    vittil_upload.have_dump = false;
#endif
    /* the reason the panic left no dump, from the word the
     * panic path kept in .noinit; taken once, on every boot, so a stale
     * one never outlives the boot it belongs to */
    uint32_t reason = 0, a = 0, b = 0;
    if (vittil_record_nodump_take(&reason, &a, &b) && !vittil_upload.have_dump &&
        is_crash_reset(vittil_upload.reset)) {
        vittil_upload.nodump = reason;
        vittil_upload.nodump_a = a;
        vittil_upload.nodump_b = b;
    }
    ESP_LOGI(TAG, "reset %s, coredump staged: %s%s%s", reset_name(vittil_upload.reset),
             vittil_upload.have_dump ? "yes" : "no", vittil_upload.nodump ? ", no dump: " : "",
             vittil_upload.nodump ? nodump_name(vittil_upload.nodump) : "");
    /* the summary area is checked on every boot (a power-on's random
     * contents zeroed); a staged dump holds everything a summary would, so
     * the summary is dropped beside it */
    vittil_upload.have_summary = vittil_summary_boot();
    if (vittil_upload.have_summary && vittil_upload.have_dump) {
        ESP_LOGD(TAG, "crash summary dropped: the staged dump holds it");
        vittil_summary_clear();
        vittil_upload.have_summary = false;
    } else if (vittil_upload.have_summary) {
        vittil_summary_log();
    }
    return ESP_OK;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
/* Where the dump is in the coredump partition (src's part, off and size)
 * and its CRC32 trailer (the last 4 bytes of the image), which tails the
 * event id so a retried upload is the same crash on the server and not a
 * second one. Every failure is ESP_ERR_NOT_FOUND after its own log line:
 * the dump cannot be read and a retry would not change that. */
static esp_err_t dump_locate(b64_src_t *src, uint32_t *crc)
{
    size_t addr = 0, len = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump image_get failed: %s", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p) {
        ESP_LOGE(TAG, "no coredump partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (len < 4 || addr < p->address || len > p->size || addr - p->address > p->size - len) {
        ESP_LOGE(TAG, "coredump image outside its partition (0x%x, %u bytes)", (unsigned)addr, (unsigned)len);
        return ESP_ERR_NOT_FOUND;
    }
    src->part = p;
    src->off = addr - p->address;
    src->size = len;
    err = esp_partition_read(p, src->off + len - 4, crc, sizeof(*crc));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump trailer read failed: %s", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* One upload. The JSON prefix (292 bytes at the longest values, in 320),
 * then "coredump_b64":" and the dump VITTIL_UPLOAD_CHUNK_BYTES at a
 * time. The two chunk buffers come from the heap for this call alone and
 * are freed on every path; a failed malloc is ESP_ERR_NO_MEM, which the
 * round retries. */
static esp_err_t upload_once(int *status)
{
    const size_t chunk = VITTIL_UPLOAD_CHUNK_BYTES;
    b64_src_t src = { .chunk = chunk, .b64cap = chunk / 3 * 4 + 1 };
    uint32_t crc = 0;
    esp_err_t err = dump_locate(&src, &crc);
    if (err != ESP_OK) {
        return err;
    }
    const char *id = device_id("upload");
    if (!id) {
        return ESP_ERR_NOT_FOUND;
    }
    char buf[320];
    body_t j = { buf, sizeof(buf), 0 };
    add_facts(&j, id, vittil_fw_version(), vittil_build_id(), reset_name(vittil_upload.reset));
    add(&j, ",\"event_id\":\"%s-%08" PRIx32 "\"", id, crc);
    add_crash(&j);
    add(&j, ",\"coredump_b64\":\"");
    if (!body_ok(&j, "upload prefix")) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "uploading coredump: %u bytes, body %u bytes, event %s-%08" PRIx32, (unsigned)src.size,
             (unsigned)b64_body_len(&j, src.size), id, crc);
    src.raw = malloc(chunk);
    src.b64 = malloc(src.b64cap);
    if (!src.raw || !src.b64) {
        ESP_LOGW(TAG, "no memory for the upload's buffers (%u bytes), not sent", (unsigned)(chunk + src.b64cap));
        err = ESP_ERR_NO_MEM;
    } else {
        err = send_b64("upload", &j, &src, status);
    }
    free(src.raw);
    free(src.b64);
    return err;
}
#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */

/* The same facts without a dump, through the small body (241 bytes at
 * the longest values, with the reason and its two sizes 60 more, in 320). */
static esp_err_t no_dump_once(int *status)
{
    const char *id = device_id("no-dump event");
    if (!id) {
        return ESP_ERR_NOT_FOUND;
    }
    char buf[320];
    body_t j = { buf, sizeof(buf), 0 };
    add_facts(&j, id, vittil_fw_version(), vittil_build_id(), reset_name(vittil_upload.reset));
    add_crash(&j);
    add(&j, "}");
    if (!body_ok(&j, "no-dump body")) {
        return ESP_ERR_INVALID_SIZE;
    }
    return vittil_post_small("/api/crashes", buf, "no-dump event", status);
}

/* The resets IDF's panic handler leaves (panic.c sets the hint by
 * the exception): after one of these the waiting summary is this boot's
 * crash. Any other boot finds an older one, kept because it was not
 * delivered. */
static bool is_panic_reset(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT;
}

/* The reset the summary's crash left, from its exception, as panic.c sets
 * the hint. */
static const char *summary_reset_name(const vittil_summary_t *s)
{
    switch (s->exception) {
    case PANIC_EXCEPTION_IWDT: return "INT_WDT";
    case PANIC_EXCEPTION_TWDT: return "TASK_WDT";
    default:                   return "PANIC";
    }
}

/* One summary upload. The facts the no-dump event sends, the
 * crashed build's version and ELF SHA from the summary (an OTA may have
 * changed the running one since), an event id from the summary's CRC32 so
 * a retry is the same crash, then "summary_b64" with the summary's bytes,
 * base64 48 bytes at a time on this stack (65 bytes). After this boot's own
 * crash it carries the crash time and the no-dump reason; an older summary
 * carries neither (the server stamps the arrival). The uptime is the
 * summary's, sent with either. */
static esp_err_t summary_once(int *status)
{
    const vittil_summary_t *sum = vittil_summary_get();
    if (!sum) {
        ESP_LOGE(TAG, "crash summary: gone or damaged, not sent");
        return ESP_ERR_NOT_FOUND;
    }
    const char *id = device_id("crash summary");
    if (!id) {
        return ESP_ERR_NOT_FOUND;
    }
    bool this_boot = is_panic_reset(vittil_upload.reset);
    char fw[sizeof(sum->fw)];
    vittil_json_safe(fw, sizeof(fw), sum->fw, sizeof(sum->fw)); /* nothing the memory held can break the body */
    if (!fw[0]) {
        strlcpy(fw, vittil_fw_version(), sizeof(fw));
    }
    /* the build id as the running build's is sent (the descriptor's length,
     * CONFIG_APP_RETRIEVE_LEN_ELF_SHA), from the crashed build's SHA */
    char build[65];
    size_t blen = strlen(vittil_build_id());
    if (blen == 0 || blen > 64) {
        blen = 64;
    }
    for (size_t i = 0; i < blen; i++) {
        static const char hex[] = "0123456789abcdef";
        uint8_t b = sum->build[i / 2];
        build[i] = hex[(i % 2) ? (b & 0xf) : (b >> 4)];
    }
    build[blen] = 0;

    char buf[448];
    body_t j = { buf, sizeof(buf), 0 };
    add_facts(&j, id, fw, build, this_boot ? reset_name(vittil_upload.reset) : summary_reset_name(sum));
    add(&j, ",\"event_id\":\"%s-s%08" PRIx32 "\"", id, sum->crc);
    /* the crashed boot's uptime, the summary's own, so an older summary has it too */
    uint64_t up_us = ((uint64_t)sum->uptime_us[1] << 32) | sum->uptime_us[0];
    if (up_us) {
        add(&j, ",\"uptime_ms\":%llu", (unsigned long long)(up_us / 1000));
    }
    if (this_boot) {
        add_crash(&j);
    }
    add(&j, ",\"summary_b64\":\"");
    if (!body_ok(&j, "crash summary prefix")) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "sending crash summary: %u bytes, body %u bytes, event %s-s%08" PRIx32, (unsigned)sizeof(*sum),
             (unsigned)b64_body_len(&j, sizeof(*sum)), id, sum->crc);
    unsigned char b64[65];
    b64_src_t src = { .mem = (const uint8_t *)sum, .size = sizeof(*sum), .chunk = 48, .b64 = b64, .b64cap = sizeof(b64) };
    return send_b64("crash summary", &j, &src, status);
}

/* The decision and the erase. A staged dump: the round, then its fate
 * by the answer. ESP_OK (a 2xx): erased, the server has it. 400 or 413:
 * the same bytes would draw the same answer at every boot and the one
 * slot would never hold the next crash's dump, so erased with an error
 * line; the server refused the row, so the crash is reported as a no-dump
 * event on this boot. Anything else (no
 * connection, the retries spent, a 5xx, the refused key, 404, 408, 429):
 * kept in flash for the next boot. No dump after a crash reset: the no-dump
 * event, dropped if not delivered (no flash copy to keep). */
void vittil_upload_round(void)
{
    int status = -1;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (vittil_upload.have_dump) {
        esp_err_t err = vittil_round(upload_once, "upload", &status);
        vittil_upload.have_dump = false;
        if (err == ESP_OK) {
            esp_err_t e = esp_core_dump_image_erase();
            if (e == ESP_OK) {
                ESP_LOGI(TAG, "coredump uploaded, erased from flash");
            } else {
                /* sent again next boot under the same event id, one crash on the server */
                ESP_LOGW(TAG, "coredump uploaded, erase failed (%s): sent again next boot", esp_err_to_name(e));
            }
            return;
        }
        if (refused(status)) {
            esp_err_t e = esp_core_dump_image_erase();
            ESP_LOGE(TAG, "coredump refused (HTTP %d), erased so the next crash has the slot (%s)", status,
                     esp_err_to_name(e));
            /* falls through to the no-dump event: the crash is still reported */
        } else {
            ESP_LOGW(TAG, "coredump not delivered (%s, status %d), kept in flash for the next boot",
                     esp_err_to_name(err), status);
            return;
        }
    }
#endif
    /* no dump, a summary waits. Cleared on a 2xx, and on a 400 or
     * 413 (the same bytes would draw the same answer at every boot); kept
     * otherwise for the next boot. When it is this boot's crash it is the
     * report; an older one goes first and this boot's crash reset, if any,
     * still gets its no-dump event. */
    if (vittil_upload.have_summary) {
        vittil_upload.have_summary = false;
        esp_err_t err = vittil_round(summary_once, "crash summary", &status);
        if (err == ESP_OK || refused(status)) {
            vittil_summary_clear();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "crash summary sent, cleared");
            } else {
                ESP_LOGE(TAG, "crash summary refused (HTTP %d), cleared", status);
            }
        } else {
            ESP_LOGW(TAG, "crash summary not delivered (%s, status %d), kept for the next boot", esp_err_to_name(err),
                     status);
        }
        if (is_panic_reset(vittil_upload.reset) && !refused(status)) {
            return; /* reported, or kept for the next boot */
        }
    }
    if (is_crash_reset(vittil_upload.reset)) {
        esp_err_t err = vittil_round(no_dump_once, "no-dump event", &status);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "no-dump event sent (%s)", reset_name(vittil_upload.reset));
        } else {
            ESP_LOGW(TAG, "no-dump event not delivered (%s, status %d), dropped", esp_err_to_name(err), status);
        }
    }
}
