# Vittil SDK for ESP-IDF

Crash reports from ESP32 devices in the field. When a device crashes, the
SDK sends the crash to [Vittil](https://vittil.io) on the next boot, where it
is decoded against your build's ELF: the cause, the backtrace with file and
line, the task, the registers, the variables and the last log lines.

- Requires ESP-IDF 5.5 or later.
- Targets: esp32, esp32s2, esp32s3, esp32c2, esp32c3, esp32c5, esp32c6,
  esp32c61, esp32h2, esp32p4.
- One call, `vittil_start()`, and two lines of configuration.

## Quickstart

Two parts: add Vittil to your own project, or try the example first.

### Add Vittil to your project

1. Add the SDK to your project:

   ```
   idf.py add-dependency "vittil-io/vittil^0.1.2"
   ```

2. Add these two lines to the end of `sdkconfig`. Your project key is on
   Settings in the Vittil app.

   ```
   CONFIG_VITTIL_PROJECT_KEY="ecr_your_project_key"
   CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
   ```

   Using a custom partition table? See [Partition table](#partition-table).

3. Start Vittil in `app_main`:

   ```c
   #include "vittil.h"

   void app_main(void)
   {
       vittil_start();
       /* your code */
   }
   ```

   Your app brings up its network as usual (Wi-Fi, Ethernet or any other);
   Vittil sends once it is up.

4. Build and flash:

   ```
   idf.py build flash monitor
   ```

   The monitor shows, once at boot:

   ```
   I vittil: start: device <id>, fw <version>, build <sha>; server https://app.vittil.io, key set; a crash sends its coredump (partition 65536 bytes, fitted to it); ping every 1800 s
   I vittil: ping delivered (HTTP 200), every 1800 s from here
   ```

   Your device shows Live on Devices in the Vittil app. The build also
   uploads its ELF, which decodes the crashes, and says so once per new
   build:

   ```
   vittil: ELF <build> uploaded
   ```

5. Optional, a test crash: call `abort()` once. After the reboot the crash
   is on Issues, decoded.

### Try the example

[examples/quickstart](examples/quickstart) is a complete project: it starts
Vittil, connects to Wi-Fi with ESP-IDF's `example_connect()`, and crashes on
purpose when you press the board's BOOT button.

You need an Espressif development board of one of the targets (the example
reads its BOOT button), ESP-IDF 5.5 or later, a 2.4 GHz Wi-Fi network and
your project key. The esp32h2 and esp32p4 have no Wi-Fi: the example
connects them by Ethernet (the esp32p4 board's own port, a W5500 SPI module
on the esp32h2), set in `idf.py menuconfig` (Example Connection
Configuration).

1. Get the example:

   ```
   git clone https://github.com/vittil-io/vittil.git
   cd vittil/examples/quickstart
   ```

2. Before anything else, fill in the three empty values in
   `sdkconfig.defaults`. Its first two lines are the two lines of step 2
   above:

   ```
   CONFIG_VITTIL_PROJECT_KEY="ecr_your_project_key"
   CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y

   CONFIG_EXAMPLE_WIFI_SSID="your network"
   CONFIG_EXAMPLE_WIFI_PASSWORD="your password"
   ```

   The file is read only when `sdkconfig` is made, at the first build.
   Filled it in after that? Delete `sdkconfig` and build again.

3. Set your board's chip first (esp32 is the default), for example:

   ```
   idf.py set-target esp32c3
   ```

4. Connect the board by USB, then build, flash and open the monitor:

   ```
   idf.py build flash monitor
   ```

   Add `-p PORT` when idf.py does not find the board; Ctrl+] leaves the
   monitor. Once it shows `I quickstart: press BOOT for a test crash` and
   `I vittil: ping delivered (HTTP 200)`, your device shows Live on Devices
   in the Vittil app.

5. Press BOOT. The device crashes and reboots:

   ```
   W quickstart: BOOT pressed: test crash
   abort() was called at PC 0x... on core 0
   ...
   I vittil: reset PANIC, coredump staged: yes
   I vittil: coredump uploaded, erased from flash
   ```

   The crash is on Issues, decoded against the ELF the build uploaded.

Not working? See [Troubleshooting the example](#troubleshooting-the-example).

## Troubleshooting the example

If the example does not connect, it keeps running offline and says what to
fix. Change the value in `idf.py menuconfig` (Example Connection
Configuration), then run `idf.py build flash monitor` again.

    E quickstart: no Wi-Fi name: set CONFIG_EXAMPLE_WIFI_SSID ...
      the network name is empty

    E quickstart: Wi-Fi password is N characters, a WPA password has at least 8 ...
      the password is too short to be right

    E quickstart: the network did not connect ...
      after 7 tries: a wrong name or password, or a 5 GHz-only network

A key problem shows in the SDK's own lines, see [Log lines](#log-lines).

## How it works

`vittil_start()` reads the device's identity (the factory MAC as the device
id, the firmware version and the ELF's SHA-256 from the app descriptor) and
starts one task. The task:

- reports the last crash, once per boot: a coredump staged in flash is
  uploaded and erased when the server has it (kept for the next boot
  otherwise); a crash summary is sent when there is no coredump; a crash
  reset that left neither is reported with its reset reason;
- then pings the server every 30 minutes, so the device shows Live, and
  Offline after a day of silence.

`vittil_start()` never blocks. Call it once, before or after your network is
up: the task waits for the network and never brings it up itself. Set the
clock (SNTP) to get crash times; otherwise a crash is stamped when it
arrives.

What a crash sends depends on the device's setup:

- A coredump, with a coredump partition: every task's backtrace, stacks and
  local variables, the global and static variables, the last log lines (not
  the heap). When the partition is too small, the variables are cut first
  (.bss, then .data, from their ends), then the other tasks' stacks; the
  crashed task and the log lines are never cut, and the Issue names what was
  left out.
- A crash summary, without one: the cause and fault address, the abort or
  assert line, the crashed task's registers and backtrace, the last log
  lines (about six), the firmware version and build. It is kept in memory a
  reboot does not clear and checked by a CRC32 at the next boot.
- The reset reason alone, when the crash summary is turned off too.

Every request is HTTPS to app.vittil.io with your project key. The key can
only send crashes, pings and ELFs into its own project, and it is never
written to the log.

## ELF upload

A crash is decoded against the ELF of the build that crashed. After the
link, the build runs `tools/elf_upload.py`, which sends `build/<project>.elf`
to Vittil with the project key from `sdkconfig`. `idf.py build` and
`idf.py flash` both run it, and so does a CI build: nothing is added to your
build commands.

- It runs once per new ELF. A build that leaves the ELF unchanged sends
  nothing, and an ELF Vittil already has is not stored twice.
- It never fails the build and prints nothing when the upload cannot be
  made: no network, the server down, a refused key. The next build tries
  again.
- It is skipped when the project key is empty.

To turn it off, set `CONFIG_VITTIL_ELF_UPLOAD=n`. An ELF can still be
uploaded by hand, on the crash's Issue page (Upload ELF), or from a script:

```
curl -X PUT -H "Authorization: Bearer $VITTIL_PROJECT_KEY" \
     --data-binary @build/<project>.elf \
     https://app.vittil.io/api/firmware/<build>/elf
```

`<build>` is the first 9 characters of the ELF's SHA-256
(`CONFIG_APP_RETRIEVE_LEN_ELF_SHA`), the build the device reports at boot.

## Partition table

Vittil sends the full coredump when the firmware has a coredump partition.
ESP-IDF's usual partition tables add it by themselves once
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` is on. On your own table, make sure it
has a line like this, or add one:

```
coredump, data, coredump,, 64K
```

Any partition of type data and subtype coredump works, whatever its name. A
smaller one works too: Vittil fits the dump to it.

Without one, crashes arrive as crash summaries. Devices already in the
field keep the partition table they shipped with (an OTA update does not
change it), so they send summaries. The start line at boot says which one a
device sends.

## Configuration

`idf.py menuconfig`, under Vittil. Only the project key is needed. The key
can also come from the `VITTIL_PROJECT_KEY` environment variable when the
build first creates `sdkconfig`, for a CI. `sdkconfig.defaults` is read only
for settings `sdkconfig` does not hold yet, so on a project built before,
put the two quickstart lines in `sdkconfig` itself.

With coredump to flash on, Vittil turns on three coredump settings itself:
the dump on its own stack, the first crash of a crash loop kept until it is
uploaded, and the global and static variables in the dump.

Under Advanced, each with its help in menuconfig:

    CONFIG_VITTIL_ELF_UPLOAD         y      upload the ELF at every build that changes it
    CONFIG_VITTIL_PING_INTERVAL_S    1800   seconds between pings
    CONFIG_VITTIL_LOG_RING_BYTES     2048   log kept for the coredump, about 25 lines
    CONFIG_VITTIL_WRITER             y      fit the dump to the partition, with the variables
    CONFIG_VITTIL_SUMMARY            y      a crash summary when there is no coredump
    CONFIG_VITTIL_SUMMARY_LOG_BYTES  512    log kept in a crash summary, about six lines

## Log lines

All lines carry the tag `vittil`. A healthy device logs the start line at
boot, one line per report and the first ping, then nothing.

    W no project key: the server will refuse every send
      CONFIG_VITTIL_PROJECT_KEY is empty, or sdkconfig still holds an older value

    W CONFIG_VITTIL_PROJECT_KEY is not a Vittil project key ...
      copy the key again from Settings

    E project key refused (HTTP 401): nothing more is sent until the next boot
      a wrong or rotated key

    W coredump partition N bytes, a dump needs at least M ...
      the partition is too small for any dump; crashes arrive as summaries

    W ... waiting: no network for 60 s
      the SDK waits for your application's network

    W ... failed (...), retry 1 of 5 in 23 s
      the server was not reached; the SDK retries

## Footprint

Measured on the quickstart example, ESP-IDF 5.5.5, esp32:

- Flash: 12.3 KB of code.
- RAM: 9.0 KB of .bss, most of it the task's 6 KB stack, and 2.3 KB of .data
  for the log ring. Reserved at boot.
- IRAM: 1,022 bytes, the code that runs inside the panic handler.
- RTC memory: 1,088 bytes for the crash summary.
- Heap: the HTTP client and TLS while a request is open, and 7.2 KB more
  during an upload; all of it freed when the request ends.
- Network: one ping every 30 minutes; one upload per crash.

## License

Apache-2.0. See [LICENSE](LICENSE).
