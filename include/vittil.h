/*
 * vittil: the device SDK of Vittil, crash reports from ESP32 devices in
 * the field. The one public call is vittil_start().
 */
#pragma once

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

#ifdef __cplusplus
}
#endif
