/*
 * vittil: the device SDK of Vittil, crash reports from ESP32 devices in
 * the field. The one public call is vittil_start().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the SDK: read the device's identity (the factory base MAC as the
 * device id, the firmware version and the ELF's sha256 from the app
 * descriptor) and the boot facts (the reset reason, whether a core dump is
 * staged in flash: its checksum is read on the caller's task, a few hundred
 * bytes of its stack), and create the one task, which never exits. The task
 * first reports the last crash, once: a staged dump is uploaded and erased
 * on the server's yes (kept in flash for the next boot otherwise); a crash
 * reset that left no dump is reported as such; any other boot sends
 * nothing. Then it pings the server every CONFIG_VITTIL_PING_INTERVAL_S
 * seconds (1800 by default, the dashboard shows the device Offline after a
 * day of silence). The task's stack is reserved at boot; the upload borrows
 * about 7 KB from the heap for its duration
 * and nothing else allocates. Call it once, from app_main, before or after
 * the network is up: the task waits for the network itself and never
 * blocks the caller. Set the clock (SNTP) before the network is up if you
 * want crash times; otherwise a crash is stamped when it arrives. Returns
 * ESP_OK, or ESP_ERR_INVALID_STATE on a second call, or esp_read_mac's
 * error when the chip has no readable factory MAC (one log line, nothing
 * is sent). The dump needs CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH, the ELF
 * format and a coredump partition; without them crashes arrive without a
 * dump. */
esp_err_t vittil_start(void);

/* The application's transport (CONFIG_VITTIL_TRANSPORT_APP). By default
 * the SDK sends through ESP-IDF's HTTP client over https, which links
 * ESP-IDF's TLS and its root certificate bundle when nothing else in the
 * application does. An application that already has an HTTP path of its
 * own (its own client, its own certificates) hands the SDK these three
 * functions instead, and ESP-IDF's HTTP client is not linked for the SDK.
 * begin opens a POST of len bytes to url (https://app.vittil.io/api/...)
 * with two headers, Content-Type: application/json and Authorization: auth
 * (the project key as "Bearer <key>"; NULL when none is set, then no
 * header), and gives back a handle; it returns the connect error, which
 * the SDK logs and retries. write sends one piece of the body, ESP_FAIL
 * when the connection dropped it. end finishes the request and frees the
 * handle on every path: with whole true the body went entirely, so it
 * reads the answer and returns the HTTP status, or -1 when no answer
 * came; with whole false the body is short, so it only frees and returns
 * -1. The SDK calls them from its own task only, one request at a time,
 * the ping's body under 200 bytes and a coredump's up to the partition's
 * size in pieces of 3,072 bytes. */
typedef struct vittil_transport_s {
    esp_err_t (*begin)(const char *url, const char *auth, size_t len, void **handle);
    esp_err_t (*write)(void *handle, const void *data, size_t len);
    int (*end)(void *handle, bool whole);
} vittil_transport_t;

/* Register the application's transport, before vittil_start(). The
 * struct is copied. ESP_ERR_INVALID_ARG for NULL or a missing function
 * (one log line, nothing changed); ESP_ERR_INVALID_STATE after
 * vittil_start(); ESP_ERR_NOT_SUPPORTED with CONFIG_VITTIL_TRANSPORT_APP
 * off, where the default transport sends and the call changes nothing.
 * With it on, vittil_start() refuses (ESP_ERR_INVALID_STATE, one log
 * line) until a transport is registered. */
esp_err_t vittil_set_transport(const vittil_transport_t *t);

#ifdef __cplusplus
}
#endif
