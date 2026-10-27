/*
 * vittil_record.c: the record inside the dump, and the log ring that
 * feeds it.
 *
 * Two hooks into stock ESP-IDF:
 *   1. __wrap_esp_core_dump_write(): linked in front of the stock coredump
 *      entry with -Wl,--wrap=esp_core_dump_write (panic.c calls it). It
 *      fills the record from the panic_info_t the panic handler hands over,
 *      then calls the stock entry, which writes the dump (through vittil's
 *      writer when CONFIG_VITTIL_WRITER is on, vittil_writer.c).
 *   2. esp_log_set_vprintf(): every ESP_LOGx line goes to the console as
 *      before, then a copy lands in the ring. The hook never blocks on a
 *      lock, never allocates, and formats on the caller's stack.
 *
 * Nothing here walks frames or decodes anything: the server unwinds from
 * the registers and the stack with the ELF. Raw values only: the clocks
 * (esp_log_timestamp, the 64-bit timer, the epoch), the ISR flag and the
 * processor status. Every function checks what it is given; every string
 * copy is bounded; nothing is allocated.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/panic_internal.h"
#include "esp_memory_utils.h"
#if __XTENSA__
#include "xtensa_context.h"
#else
#include "riscv/rvruntime-frames.h"
#endif

#include "vittil_priv.h"
#include "vittil_summary.h"

/* The wire layout the server reads: the ring at 180, the two 64-bit
 * fields on 8-byte boundaries. */
_Static_assert(offsetof(vittil_record_t, ring) == 180, "the record's fixed part is 180 bytes");
_Static_assert(offsetof(vittil_record_t, uptime_us) == 16, "uptime_us at 16");
/* A line and its newline must fit the ring whole (ring_put's two copies). */
_Static_assert(VITTIL_LOG_LINE_MAX + 1 <= CONFIG_VITTIL_LOG_RING_BYTES,
               "VITTIL_LOG_LINE_MAX must be smaller than CONFIG_VITTIL_LOG_RING_BYTES");
#if CONFIG_VITTIL_SUMMARY
/* The summary's log tail is the ring's newest bytes (vittil_summary.c). */
_Static_assert(CONFIG_VITTIL_SUMMARY_LOG_BYTES <= CONFIG_VITTIL_LOG_RING_BYTES,
               "CONFIG_VITTIL_SUMMARY_LOG_BYTES must be at most CONFIG_VITTIL_LOG_RING_BYTES");
#endif

/* The clock is trusted once it reads past 2020-01-01 (1577836800). */
#define VITTIL_EPOCH_MIN 1577836800LL

/* The file's state, one struct, in the coredump's own DRAM section so the
 * stock writer and vittil's alike store it as one PT_LOAD: the record first
 * (the wire layout), then the hook's own state, which rides along in the
 * dump and is read by nobody. The record's header values are set here, so
 * a dump written before the panic path ran (it cannot happen, the fill
 * runs first) still describes itself. */
static COREDUMP_DRAM_ATTR struct {
    vittil_record_t rec;
    vprintf_like_t prev;  /* the console's vprintf, called first */
    portMUX_TYPE mux;     /* the ring's lock; held for one line's copy */
    bool installed;
    int64_t clock_epoch;  /* time(NULL) at the last clock note, 0 until the clock is set */
    int64_t clock_us;     /* esp_timer_get_time() at that note */
} vittil_record = {
    .rec = { .magic = VITTIL_RECORD_MAGIC,
             .version = VITTIL_RECORD_VERSION,
             .size = sizeof(vittil_record_t),
             .ring_bytes = CONFIG_VITTIL_LOG_RING_BYTES },
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

/* The no-dump reason, in .noinit so it outlives the panic reset (RAM is
 * kept across a software reset; a power-on clears the magic with the rest).
 * Its own struct: a different section from the record's. */
#define VITTIL_NOINIT_MAGIC 0x4E4C5456u /* "VTLN" */
static __NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t a, b;
} vittil_record_noinit;

/* ---- the log ring ------------------------------------------------------ */

/* Copy len bytes at the head, wrapping once. Under the lock; len is at most
 * VITTIL_LOG_LINE_MAX + 1, which the static assert keeps under the
 * ring's size, so the second copy never runs past the start. */
static void ring_put(const char *s, size_t len)
{
    vittil_record_t *r = &vittil_record.rec;
    size_t h = r->ring_head % CONFIG_VITTIL_LOG_RING_BYTES;
    size_t first = CONFIG_VITTIL_LOG_RING_BYTES - h;
    if (first > len) {
        first = len;
    }
    memcpy(r->ring + h, s, first);
    memcpy(r->ring, s + first, len - first);
    r->ring_head += len;
}

/* The byte count of s that ends on a whole UTF-8 character: a sequence a
 * cut left incomplete at the end is dropped, so a cut line still decodes. */
static size_t utf8_whole(const char *s, size_t len)
{
    size_t i = len, k = 0;
    while (i > 0 && k < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
        i--;
        k++;
    }
    if (i == 0) {
        return len;
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return (need == 1 || k + 1 >= need) ? len : i - 1;
}

/* One line into the ring: at most VITTIL_LOG_LINE_MAX - 1 bytes of it (a
 * longer line is cut, never inside a UTF-8 character), a newline appended
 * when it has none, counted. Under the lock: ESP_LOGx is task context only
 * (ESP_DRAM_LOGx and ESP_EARLY_LOGx bypass the vprintf hook), so the lock
 * is the task-side spinlock, interrupts off for one copy; the SAFE form
 * keeps it correct if a line ever comes from an ISR. */
static void ring_line(const char *line, size_t len)
{
    if (len >= VITTIL_LOG_LINE_MAX - 1) {
        len = utf8_whole(line, VITTIL_LOG_LINE_MAX - 1);
    }
    if (len == 0) {
        return;
    }
    portENTER_CRITICAL_SAFE(&vittil_record.mux);
    ring_put(line, len);
    if (line[len - 1] != '\n') {
        ring_put("\n", 1);
    }
    vittil_record.rec.ring_lines++;
    portEXIT_CRITICAL_SAFE(&vittil_record.mux);
}

/* The hook. Console first, exactly as before vittil was linked in; then the
 * line formatted into VITTIL_LOG_LINE_MAX bytes on this task's stack
 * (vsnprintf's return read) and into the ring. */
static int vittil_log_vprintf(const char *fmt, va_list ap)
{
    if (!fmt) {
        return 0;
    }
    va_list ap2;
    va_copy(ap2, ap);
    int ret = vittil_record.prev ? vittil_record.prev(fmt, ap) : vprintf(fmt, ap);
    char line[VITTIL_LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n <= 0) {
        return ret;
    }
    ring_line(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    return ret;
}

/* The application's own line (vittil.h): a logger that owns esp_log's
 * vprintf hook, or prints by another path, hands its lines here so the
 * tail before a crash is its lines. Nothing is printed; the line goes into
 * the ring as the hook's would, cut at VITTIL_LOG_LINE_MAX - 1 bytes. Safe
 * from any task and before vittil_start (the ring is static). */
void vittil_log_line(const char *line, size_t len)
{
    if (!line) {
        return;
    }
    ring_line(line, len);
}

void vittil_record_init(void)
{
    if (vittil_record.installed) {
        return;
    }
    vittil_record.installed = true;
    vittil_record.prev = esp_log_set_vprintf(vittil_log_vprintf);
}

/* For the summary's log tail, inside the panic (vittil_summary.c): IRAM, no
 * lock, no log. The record is in DRAM (COREDUMP_DRAM_ATTR). */
IRAM_ATTR bool vittil_record_ring(const char **ring, uint32_t *size, uint32_t *head)
{
    if (!ring || !size || !head) {
        return false;
    }
    *ring = vittil_record.rec.ring;
    *size = CONFIG_VITTIL_LOG_RING_BYTES;
    *head = vittil_record.rec.ring_head;
    return true;
}

void vittil_record_clock_note(void)
{
    time_t now = time(NULL);
    if (now < VITTIL_EPOCH_MIN) {
        return; /* the clock is not set (it reads 1970): no note, the epoch stays 0 */
    }
    int64_t us = esp_timer_get_time();
    /* the pair written together, so the panic reads a consistent one */
    portENTER_CRITICAL(&vittil_record.mux);
    vittil_record.clock_epoch = (int64_t)now;
    vittil_record.clock_us = us;
    portEXIT_CRITICAL(&vittil_record.mux);
}

/* ---- the no-dump reason ------------------------------------------------ */

void vittil_record_nodump_set(uint32_t reason, uint32_t a, uint32_t b)
{
    vittil_record_noinit.reason = reason;
    vittil_record_noinit.a = a;
    vittil_record_noinit.b = b;
    vittil_record_noinit.magic = VITTIL_NOINIT_MAGIC;
}

bool vittil_record_nodump_take(uint32_t *reason, uint32_t *a, uint32_t *b)
{
    if (!reason || !a || !b) {
        ESP_LOGE("vittil", "nodump_take: NULL argument");
        return false;
    }
    if (vittil_record_noinit.magic != VITTIL_NOINIT_MAGIC) {
        return false;
    }
    *reason = vittil_record_noinit.reason;
    *a = vittil_record_noinit.a;
    *b = vittil_record_noinit.b;
    vittil_record_noinit.magic = 0; /* read once: the next boot starts clean */
    return true;
}

/* ---- the panic-time fill ----------------------------------------------- */

/* Bounded copy, always terminated; src NULL copies "". */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (src) {
        for (; i + 1 < n && src[i]; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

static void vittil_fill_record(const panic_info_t *info)
{
    vittil_record_t *r = &vittil_record.rec;

    /* the clock the log lines carry (uint32 ms, wraps at 49.7 days), the
     * timer as the true uptime, the epoch from the task's last clock note */
    r->uptime_ms = esp_log_timestamp();
    r->uptime_us = esp_timer_get_time();
    r->epoch = 0;
    if (vittil_record.clock_epoch >= VITTIL_EPOCH_MIN && r->uptime_us >= vittil_record.clock_us) {
        r->epoch = vittil_record.clock_epoch + (r->uptime_us - vittil_record.clock_us) / 1000000;
    }
    r->core = (uint32_t)info->core;
    r->exception = (uint32_t)info->exception;
    r->pc = (uint32_t)info->addr;
    copy_str(r->reason, sizeof(r->reason), info->reason);
    copy_str(r->description, sizeof(r->description), info->description);
    /* abort() and assert(): IDF's handler clears the description and keeps
     * the line in g_panic_abort_details, which the stock writer's
     * ESP_PANIC_DETAILS note carried; the record carries it instead, read
     * only from internal RAM (on RISC-V the trap's mcause is the abort's
     * illegal instruction, so the line is what names it a stop) */
    if (info->exception == PANIC_EXCEPTION_ABORT && !r->description[0] && g_panic_abort_details &&
        esp_ptr_internal(g_panic_abort_details)) {
        copy_str(r->description, sizeof(r->description), g_panic_abort_details);
    }
    /* the task's name only once the scheduler has a current task, and only
     * from a handle inside internal RAM: the control block is read here
     * before the port's own sanity check sees it, and a wild pointer would
     * be a second exception inside the panic handler (no dump at all) */
    TaskHandle_t h = (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) ? NULL : xTaskGetCurrentTaskHandle();
    copy_str(r->task, sizeof(r->task), (h && esp_ptr_internal(h)) ? pcTaskGetName(h) : "-");
    r->flags = VITTIL_F_FILLED | (info->pseudo_excause ? VITTIL_F_PSEUDO_CAUSE : 0) |
               (xPortInterruptedFromISRContext() ? VITTIL_F_ISR : 0);
    r->exccause = 0;
    r->excvaddr = 0;
    r->ps = 0;
#if __XTENSA__
    const XtExcFrame *xf = (const XtExcFrame *)info->frame;
    if (xf) {
        r->exccause = xf->exccause;
        r->excvaddr = xf->excvaddr;
        r->ps = xf->ps;
    }
#else
    /* RISC-V: the trap's cause and value in the same two fields
     * (mcause, mtval: the fault address on an access fault), mstatus in the
     * status's place; the server reads them by the dump's architecture */
    const RvExcFrame *rf = (const RvExcFrame *)info->frame;
    if (rf) {
        r->exccause = rf->mcause;
        r->excvaddr = rf->mtval;
        r->ps = rf->mstatus;
    }
#endif
}

/* The wrap. panic.c calls esp_core_dump_write(info); the linker sends that
 * call here, and __real_ is the stock entry. Only linked when the stock
 * writer is in the build. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
extern void __real_esp_core_dump_write(panic_info_t *info);

void __wrap_esp_core_dump_write(panic_info_t *info)
{
#if CONFIG_VITTIL_QEMU_ETH
    vittil_qemu_eth_panic_quiesce();
#endif
    /* the reason stands until the writer replaces it or the dump is there */
    vittil_record_nodump_set(VITTIL_NODUMP_WRITE_FAILED, 0, 0);
    if (!info) {
        esp_rom_printf("vittil: no panic info, nothing written\r\n");
        return;
    }
    vittil_fill_record(info);
    esp_rom_printf("vittil: record filled (task %s, %s, uptime %u ms, %u log lines)\r\n", vittil_record.rec.task,
                   vittil_record.rec.reason, (unsigned)vittil_record.rec.uptime_ms,
                   (unsigned)vittil_record.rec.ring_lines);
    __real_esp_core_dump_write(info);
}
#endif
