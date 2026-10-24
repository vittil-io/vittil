/* vittil internals shared between the SDK's files. Not an API. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/* Fixed values, not settings: the retry's first delay and count, the
 * upload's chunk (a multiple of 3, so each chunk's base64 is whole), the
 * SDK task's stack, the longest log line the ring keeps. */
#define VITTIL_RETRY_DELAY_S       30
#define VITTIL_RETRY_COUNT         5
#define VITTIL_UPLOAD_CHUNK_BYTES  3072
#define VITTIL_TASK_STACK          6144
#define VITTIL_LOG_LINE_MAX        160

/* The transport (vittil_transport.c). */

/* True once the application has initialised the TCP/IP stack, its default
 * interface is up and has an address (IPv4, or IPv6 that is not link-local)
 * and, for a server URL with a host name, a DNS server.
 * Plain reads, returns at once, safe to call before
 * esp_netif_init(). The one "is there a network" function; nothing in the
 * SDK blocks waiting for it, the calling loop does. */
bool vittil_net_ready(void);

/* The project key for every request from now on (Authorization: Bearer).
 * Copied; NULL or "" sends none; one that would not fit is refused with a
 * log line and none is sent. */
void vittil_transport_set_key(const char *key);

/* True when a key is set (the start line says so; the key itself is never
 * logged). */
bool vittil_transport_has_key(void);

/* POST a string body (Content-Type application/json, its length as
 * Content-Length) to CONFIG_VITTIL_SERVER_URL + path. Logs the status code
 * and the start of the response as `what`. ESP_OK on a 2xx; ESP_FAIL on
 * any other answer or a failed write; the open error (connect, timeout)
 * when no connection was made; ESP_ERR_NOT_ALLOWED at once, nothing sent,
 * once a 401 or 403 has refused the key (until the next boot);
 * ESP_ERR_INVALID_STATE at once when vittil_net_ready() is false;
 * ESP_ERR_INVALID_ARG for a NULL argument, a path not starting with / or a
 * URL over 160 bytes (one log line, nothing sent); ESP_ERR_NO_MEM when the
 * client or one of its two headers could not be allocated (nothing sent, so
 * a missing Authorization header never reads as a refused key; the caller
 * retries). `status`, if given, gets
 * the HTTP status code, or -1 when no answer came. One task at a time. */
esp_err_t vittil_post_small(const char *path, const char *body, const char *what, int *status);

/* A body of known length written in pieces (the upload's dump). begin
 * opens the connection with `len` as the Content-Length and refuses as
 * post_small does (its errors above, and ESP_ERR_INVALID_SIZE for a `len`
 * past INT_MAX, the client's limit; nothing opened on any of them); write
 * sends one piece, ESP_ERR_INVALID_SIZE past `len`, ESP_FAIL when the
 * connection dropped it; end reads the answer (post_small's results and
 * the 401/403 rule) and frees the handle on every path, a short body
 * included (ESP_FAIL, no answer read). One end per successful begin; the
 * caller never frees. One task at a time. */
typedef struct {
    void *c;     /* the esp_http_client handle; NULL between end and the next begin */
    size_t left; /* bytes of the body still to write */
} vittil_post_t;
esp_err_t vittil_post_begin(const char *path, size_t len, const char *what, vittil_post_t *p);
esp_err_t vittil_post_write(vittil_post_t *p, const void *data, size_t len);
esp_err_t vittil_post_end(vittil_post_t *p, const char *what, int *status);

/* The ping (vittil_ping.c); the task that calls it is vittil.c. */

/* Read the identity once: the factory base MAC as 12 lowercase hex
 * (the device id), the firmware version and the ELF's sha256 from the app
 * descriptor. esp_read_mac's error is logged and returned; no id is invented
 * and the three readers below answer "" until it succeeds. */
esp_err_t vittil_identity_init(void);
const char *vittil_device_id(void);
/* A string for a JSON body: at most n-1 bytes of src (and of its src_n), every
 * byte outside printable ASCII or a quote or backslash as '_', always ended
 * in a NUL. The versions (an app's PROJECT_VER is the user's text) and the
 * summary's strings go through it, so no value can break a body the server
 * would refuse with a 400 (the dump erased on it). */
void vittil_json_safe(char *dst, size_t n, const char *src, size_t src_n);
const char *vittil_fw_version(void);
const char *vittil_build_id(void);

/* One round of `once` with its retries, named `what` in the log. Waits
 * for the network (one read a second), calls once, and on a failure the
 * server did not choose retries VITTIL_RETRY_COUNT times from
 * VITTIL_RETRY_DELAY_S doubling, each delay drawn between half and
 * the whole of its step. Returns once's last result (ESP_OK on a 2xx; at
 * once on an answer under 500, a refused key, ESP_ERR_INVALID_ARG,
 * ESP_ERR_INVALID_SIZE or ESP_ERR_NOT_FOUND, which a retry would not
 * change; after the retries are spent otherwise); `status`, if given, gets
 * the last status. Called from the SDK's task only. */
esp_err_t vittil_round(esp_err_t (*once)(int *status), const char *what, int *status);

/* One scheduled ping, {"device_id","fw_version","build_id"} to
 * /api/ping through the round. */
void vittil_ping_round(void);

/* The upload (vittil_upload.c); called from vittil.c. */

/* The boot facts, read once at vittil_start: the reset reason, whether
 * a dump is staged with a good checksum, the IDF version; one log line.
 * ESP_FAIL with a log line when the app descriptor is missing. */
esp_err_t vittil_upload_init(void);

/* The boot's one report, before the first ping. A staged dump is streamed
 * to /api/crashes through the round and erased on a 2xx (and on a 400 or
 * 413, which the same bytes would draw again); on any
 * other outcome it stays in flash for the next boot. No dump after a crash
 * reset: the no-dump event through post_small, dropped if not delivered.
 * Any other reset: nothing. Called from the SDK's task only, once. */
void vittil_upload_round(void);

/* The capture: the record (vittil_record.c) and the writer
 * (vittil_writer.c). */

#include <stdint.h>

#define VITTIL_RECORD_MAGIC   0x314C5456u /* bytes 56 54 4C 31, "VTL1" */
#define VITTIL_RECORD_VERSION 1

#define VITTIL_F_FILLED       (1u << 0) /* the panic path filled the record */
#define VITTIL_F_PSEUDO_CAUSE (1u << 1) /* exccause has a special meaning (abort, a watchdog) */
#define VITTIL_F_ISR          (1u << 2) /* the panic came from an interrupt handler */

/* The record: the facts of the panic and the log ring, one PT_LOAD of the
 * dump (it lives in the coredump's own DRAM section, COREDUMP_DRAM_ATTR,
 * which the stock writer and vittil's store alike). The fixed part first,
 * 180 bytes, then the ring, whose size is in the header, so the server
 * reads it with no ELF. Every value raw; the server does the reading. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;            /* sizeof(vittil_record_t) */
    uint32_t flags;           /* VITTIL_F_* */
    uint32_t uptime_ms;       /* esp_log_timestamp() at the panic: the clock the log lines carry */
    int64_t  uptime_us;       /* esp_timer_get_time() at the panic: the true time since boot */
    int64_t  epoch;           /* seconds since 1970 at the panic when the clock was set, else 0 */
    uint32_t core;            /* the core that panicked */
    uint32_t exception;       /* panic_exception_t */
    uint32_t pc;              /* panic_info_t.addr */
    uint32_t exccause;        /* from the exception frame: EXCCAUSE, or mcause on RISC-V */
    uint32_t excvaddr;        /* EXCVADDR, or mtval on RISC-V */
    uint32_t ps;              /* the processor status from the exception frame: PS, or mstatus */
    uint32_t ring_bytes;      /* CONFIG_VITTIL_LOG_RING_BYTES */
    uint32_t ring_head;       /* bytes ever written; head % ring_bytes is the next slot */
    uint32_t ring_lines;      /* log lines ever written */
    char     task[16];        /* the crashed task's name (configMAX_TASK_NAME_LEN) */
    char     reason[32];      /* panic_info_t.reason, e.g. "LoadProhibited" */
    char     description[64]; /* panic_info_t.description */
    char     ring[CONFIG_VITTIL_LOG_RING_BYTES]; /* the log ring, raw text */
} vittil_record_t;

/* Install the log hook (every ESP_LOGx line to the console as before,
 * then a copy into the ring). Idempotent; called by vittil_start. */
void vittil_record_init(void);

/* Note the clock. time(NULL) cannot be read inside the panic handler
 * (newlib's lock), so the SDK's task notes the wall clock and the timer
 * together at every round, and the panic extends the note by the elapsed
 * timer. Nothing noted while the clock still reads before 2020. Task
 * context only. */
void vittil_record_clock_note(void);

/* Why the last panic left no dump, kept in .noinit across the panic
 * reset. The wrap sets WRITE_FAILED before the write; the writer replaces
 * it when it refuses; a written dump makes it moot (the dump is there).
 * take reads it once (false when nothing was set since the last take) and
 * clears it; a and b carry the floor and the partition for UNDER_FLOOR. */
#define VITTIL_NODUMP_WRITE_FAILED 1
#define VITTIL_NODUMP_NO_PARTITION 2
#define VITTIL_NODUMP_UNDER_FLOOR  3
void vittil_record_nodump_set(uint32_t reason, uint32_t a, uint32_t b);
bool vittil_record_nodump_take(uint32_t *reason, uint32_t *a, uint32_t *b);

#if CONFIG_VITTIL_WRITER
/* At boot, one ESP_LOGW when the coredump partition is missing or
 * smaller than the floor (what a dump needs before the crashed task's
 * stack), naming the bytes. Called by vittil_start. */
void vittil_writer_boot_check(void);
#endif

#if CONFIG_VITTIL_QEMU_ETH
/* The emulator transport (vittil_qemu_eth.c): QEMU's OpenCores Ethernet with
 * DHCP; returns once the guest has an address or after 30 s. */
esp_err_t vittil_qemu_eth_start(void);
/* Quiet the emulated MAC before the panic reset (QEMU does not reset it). */
void vittil_qemu_eth_panic_quiesce(void);
#endif
