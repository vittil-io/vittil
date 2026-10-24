/*
 * vittil_summary.c: the crash summary. A field device updated by OTA keeps the partition table it shipped
 * with, so it may have no coredump partition and no dump is ever written.
 * The summary is what such a crash still leaves: the cause and the fault
 * address, the registers at the fault, the backtrace (walked here on Xtensa;
 * the top of the stack on RISC-V for the server to unwind), the task, the
 * abort or assert line, and the crashed build's ELF SHA and version, so the
 * server decodes it against the right ELF even after an OTA, and the log
 * tail: the newest CONFIG_VITTIL_SUMMARY_LOG_BYTES of the SDK's log ring
 * (vittil_record.c), the lines before the crash.
 *
 * Where it lives: memory the crash reboot does not clear. RTC_NOINIT_ATTR on
 * the chips with RTC memory (RTC slow on esp32, s2, s3; RTC fast on c3, c5,
 * c6, h2, p4); __NOINIT_ATTR in normal RAM on the two without it (c2, c61),
 * where RTC_NOINIT_ATTR is empty (esp_attr.h) and would put it in .bss,
 * zeroed at every boot. The linker places it beside the app's own variables
 * of that kind, so it cannot land on a ULP program; a full region is a link
 * error.
 *
 * The hook: __wrap_esp_panic_handler, linked in front of IDF's handler with
 * -Wl,--wrap=esp_panic_handler (CMakeLists.txt), so it runs before anything
 * the handler prints or writes. Its rules, since it runs inside the panic:
 *   - IRAM and DRAM only: no flash, no allocation, no lock, no log; every
 *     IDF call it makes is inline, in ROM (the CRC), or checked at run time
 *     to sit in IRAM (the two FreeRTOS calls for the task's name);
 *   - every pointer range-checked before it is read (the frame, each stack
 *     word the walk reads, the abort line, the task's control block, the
 *     log ring);
 *   - once per panic: the first core to enter takes it (compare-and-set),
 *     a second entry (a fault inside the hook, the other core) skips it;
 *   - the magic cleared first and written last, after the CRC32, so a
 *     summary cut off by a second fault is never taken as valid.
 * The boot side trusts nothing: magic, version, size and CRC32 must all
 * match or the area is zeroed. A valid summary stays through normal reboots
 * until it is sent; a newer crash overwrites it.
 */
#include <string.h>
#include <stddef.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "esp_app_desc.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if __XTENSA__
#include "xtensa_context.h"
#include "esp_cpu_utils.h"
#else
#include "riscv/rvruntime-frames.h"
#endif

#include "vittil_summary.h"


#if __XTENSA__
_Static_assert(offsetof(vittil_summary_t, log_cap) == 568, "the Xtensa log tail at 568");
_Static_assert(sizeof(vittil_summary_t) == 576 + VITTIL_SUMMARY_LOG, "the Xtensa summary is 576 bytes and its log");
#else
_Static_assert(offsetof(vittil_summary_t, log_cap) == 1640, "the RISC-V log tail at 1640");
_Static_assert(sizeof(vittil_summary_t) == 1648 + VITTIL_SUMMARY_LOG, "the RISC-V summary is 1648 bytes and its log");
_Static_assert(sizeof(RvExcFrame) >= VITTIL_SUMMARY_REGS * 4, "RvExcFrame holds the 37 words");
#endif
_Static_assert(offsetof(vittil_summary_t, regs) == 204, "the registers at 204");
_Static_assert(VITTIL_SUMMARY_LOG <= 0xFFFF, "log_cap and log_len are 16-bit");
_Static_assert(sizeof(vittil_summary_t) <= 4096, "the server takes a summary of at most 4096 bytes");

#if CONFIG_VITTIL_SUMMARY

static const char *TAG = "vittil";

#if CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED
static RTC_NOINIT_ATTR vittil_summary_t s_sum;
#define SUM_WHERE "RTC"
#else
static __NOINIT_ATTR vittil_summary_t s_sum;
#define SUM_WHERE "noinit RAM"
#endif

/* Copied before app_main (a constructor): the hook never reads flash, and a
 * crash before vittil_start still names its build. */
static uint8_t s_build[32];
static char s_fw[32];
/* 0 until a panic takes the hook; the first core's compare-and-set wins. */
static volatile uint32_t s_taken;

__attribute__((constructor)) static void summary_ident(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) {
        memcpy(s_build, d->app_elf_sha256, sizeof(s_build));
        strlcpy(s_fw, d->version, sizeof(s_fw));
    }
}

/* ---- the panic side (IRAM) --------------------------------------------- */

static IRAM_ATTR uint32_t sum_crc(const vittil_summary_t *s)
{
    const uint8_t *from = (const uint8_t *)&s->version;
    return esp_rom_crc32_le(0, from, (uint32_t)(offsetof(vittil_summary_t, crc) - offsetof(vittil_summary_t, version)));
}

/* n bytes from a lie in internal data RAM, with no wrap. */
static IRAM_ATTR bool dram_range(uint32_t a, uint32_t n)
{
    return n && a + n > a && esp_ptr_in_dram((const void *)a) && esp_ptr_in_dram((const void *)(a + n - 1));
}

/* Copy a string of at most n-1 bytes from DRAM, each byte's address checked
 * before it is read; stops at the first byte outside DRAM. Always ends in a
 * NUL. */
static IRAM_ATTR void copy_dram_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (src) {
        for (; i + 1 < n && esp_ptr_in_dram(src + i) && src[i]; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

/* A function the hook may call only when the build placed it in IRAM (the
 * FreeRTOS kernel is there by default, CONFIG_FREERTOS_IN_IRAM; a build that
 * moves it to flash gets no task name, not a fault with the cache off). */
static IRAM_ATTR bool in_iram(const void *fn)
{
    return esp_ptr_in_iram(fn);
}

static IRAM_ATTR void fill_task(vittil_summary_t *s)
{
    if (!in_iram((const void *)xTaskGetSchedulerState) || !in_iram((const void *)xTaskGetCurrentTaskHandle) ||
        !in_iram((const void *)pcTaskGetName)) {
        return;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    if (!h || !dram_range((uint32_t)h, sizeof(void *) * 4)) {
        return; /* a wild handle is never followed */
    }
    copy_dram_str(s->task, sizeof(s->task), pcTaskGetName(h));
}

static IRAM_ATTR void fill_regs(vittil_summary_t *s, uint32_t f)
{
#if __XTENSA__
    if (!dram_range(f, sizeof(XtExcFrame))) {
        return;
    }
    const XtExcFrame *x = (const XtExcFrame *)f;
    const uint32_t *a = (const uint32_t *)&x->a0;
    s->regs[0] = x->pc;
    s->regs[1] = x->ps;
    for (int i = 0; i < 16; i++) {
        s->regs[2 + i] = a[i];
    }
    s->regs[18] = x->sar;
    s->regs[19] = x->exccause;
    s->regs[20] = x->excvaddr;
#if XCHAL_HAVE_LOOPS /* the esp32s2 has no loop registers; theirs stay 0 */
    s->regs[21] = x->lbeg;
    s->regs[22] = x->lend;
    s->regs[23] = x->lcount;
#endif
    s->cause = x->exccause;
    s->fault_addr = x->excvaddr;

    /* IDF's own walk (esp_system/port/arch/xtensa/debug_helpers.c), each
     * read checked before it is made: a frame's base save area is the 16
     * bytes under its sp, the return address at sp-16, the caller's sp at
     * sp-12. The frames are call addresses (esp_cpu_process_stack_pc), the
     * first one too, each beside its sp, as IDF prints them. */
    uint32_t pc = x->pc, sp = x->a1, next = x->a0;
    s->sps[s->n_frames] = sp;
    s->frames[s->n_frames++] = esp_cpu_process_stack_pc(pc);
    s->stack_sp = sp;
    while (s->n_frames < VITTIL_SUMMARY_FRAMES && next != 0) {
        if (!esp_stack_ptr_is_sane(sp) || !dram_range(sp - 16, 8)) {
            break;
        }
        pc = next;
        next = *(const uint32_t *)(sp - 16);
        sp = *(const uint32_t *)(sp - 12);
        pc = esp_cpu_process_stack_pc(pc);
        if (!esp_ptr_executable((const void *)pc)) {
            break;
        }
        s->sps[s->n_frames] = sp;
        s->frames[s->n_frames++] = pc;
    }
#else
    if (!dram_range(f, sizeof(RvExcFrame))) {
        return;
    }
    const RvExcFrame *r = (const RvExcFrame *)f;
    const uint32_t *w = (const uint32_t *)r;
    for (int i = 0; i < VITTIL_SUMMARY_REGS; i++) {
        s->regs[i] = w[i];
    }
    s->cause = r->mcause;
    s->fault_addr = r->mtval;
    s->sps[s->n_frames] = r->sp; /* the caller's sp is not known without the unwind tables: 0 */
    s->frames[s->n_frames++] = r->mepc;
    s->frames[s->n_frames++] = r->ra;

    /* no frame walk without the ELF's unwind tables: the top of the stack,
     * shortened by 64 bytes until all of it is DRAM */
    uint32_t sp = r->sp, n = VITTIL_SUMMARY_STACK;
    while (n && !dram_range(sp, n)) {
        n = n > 64 ? n - 64 : 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        s->stack[i] = ((const uint8_t *)sp)[i];
    }
    s->stack_sp = sp;
    s->stack_len = (uint16_t)n;
#endif
}

/* The log tail: the newest bytes of the ring, oldest first, unwrapped. No
 * lock (the panic takes none, as the dump's copy of the ring): a line being
 * copied on the other core may come out torn, and is still text. The
 * accessor must be in IRAM and the whole ring in DRAM; every index stays
 * under the ring's size and every write under sizeof(log). */
static IRAM_ATTR void fill_log(vittil_summary_t *s)
{
    s->log_cap = (uint16_t)sizeof(s->log);
#if VITTIL_SUMMARY_LOG_SET
    const char *ring = NULL;
    uint32_t size = 0, head = 0;
    if (!in_iram((const void *)vittil_record_ring) || !vittil_record_ring(&ring, &size, &head) || !ring || !size ||
        !dram_range((uint32_t)ring, size)) {
        return;
    }
    uint32_t n = head < size ? head : size;
    if (n > VITTIL_SUMMARY_LOG_SET) {
        n = VITTIL_SUMMARY_LOG_SET;
    }
    if (n > sizeof(s->log)) {
        n = sizeof(s->log);
    }
    uint32_t at = (head - n) % size;
    for (uint32_t i = 0; i < n; i++) {
        s->log[i] = ring[at];
        if (++at == size) {
            at = 0;
        }
    }
    s->log_len = (uint16_t)n;
    if (head > n) {
        s->flags |= VITTIL_SUMMARY_F_LOG_CUT;
    }
#endif
}

static IRAM_ATTR void fill(const panic_info_t *info)
{
    vittil_summary_t *s = &s_sum;
    s->magic = 0;
    __asm__ volatile("" ::: "memory");

    uint8_t *p = (uint8_t *)s + sizeof(s->magic);
    for (size_t i = sizeof(s->magic); i < sizeof(*s); i++) {
        *p++ = 0;
    }
    s->version = VITTIL_SUMMARY_VERSION;
    s->size = sizeof(*s);
    s->arch = VITTIL_SUMMARY_ARCH;
    s->core = (uint8_t)info->core;
    s->exception = (uint8_t)info->exception;
    s->flags = info->pseudo_excause ? VITTIL_SUMMARY_F_PSEUDO : 0;
    for (int i = 0; i < 32; i++) {
        s->build[i] = s_build[i];
        s->fw[i] = s_fw[i];
    }
    s->fw[sizeof(s->fw) - 1] = 0;
    /* abort() and assert() reach the handler as a debug exception; IDF's
     * handler marks them ABORT after this hook, from the same two globals */
    if (g_panic_abort) {
        s->exception = PANIC_EXCEPTION_ABORT;
        copy_dram_str(s->detail, sizeof(s->detail), g_panic_abort_details);
    }
    /* the uptime from the timer only when the build placed it in IRAM
     * (CONFIG_ESP_TIMER_IN_IRAM, the default); kept as two words, no
     * 64-bit division in the panic */
    if (in_iram((const void *)esp_timer_get_time)) {
        uint64_t us = (uint64_t)esp_timer_get_time();
        s->uptime_us[0] = (uint32_t)us;
        s->uptime_us[1] = (uint32_t)(us >> 32);
    }
    fill_task(s);
    fill_regs(s, (uint32_t)info->frame);
    fill_log(s);

    s->crc = sum_crc(s);
    __asm__ volatile("" ::: "memory");
    s->magic = VITTIL_SUMMARY_MAGIC;
}

extern void __real_esp_panic_handler(panic_info_t *info);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info && esp_cpu_compare_and_set(&s_taken, 0, 1)) {
        fill(info);
    }
    __real_esp_panic_handler(info);
}

/* ---- the boot side ----------------------------------------------------- */

void vittil_summary_clear(void)
{
    memset(&s_sum, 0, sizeof(s_sum));
}

bool vittil_summary_boot(void)
{
    const char *why = NULL;
    if (s_sum.magic != VITTIL_SUMMARY_MAGIC) {
        const uint8_t *b = (const uint8_t *)&s_sum;
        size_t i = 0;
        while (i < sizeof(s_sum) && b[i] == 0) {
            i++;
        }
        if (i == sizeof(s_sum)) {
            return false; /* nothing: cleared after a send, or never written */
        }
        why = "no marker"; /* a power-on's random contents, or a write cut off */
    } else if (s_sum.version != VITTIL_SUMMARY_VERSION) {
        why = "another version";
    } else if (s_sum.size != sizeof(s_sum)) {
        why = "another size";
    } else if (s_sum.crc != sum_crc(&s_sum)) {
        why = "bad checksum";
    }
    if (why) {
        vittil_summary_clear();
        ESP_LOGD(TAG, "crash summary area (%s, %u bytes at %p): %s, cleared", SUM_WHERE, (unsigned)sizeof(s_sum),
                 (void *)&s_sum, why);
        return false;
    }
    ESP_LOGD(TAG, "crash summary area (%s): %u bytes at %p, log tail %u of %u bytes%s", SUM_WHERE,
             (unsigned)sizeof(s_sum), (void *)&s_sum, (unsigned)s_sum.log_len, (unsigned)s_sum.log_cap,
             (s_sum.flags & VITTIL_SUMMARY_F_LOG_CUT) ? ", older lines before it" : "");
    return true;
}

void vittil_summary_log(void)
{
    /* the name bounded by its field: valid means the hook wrote it, terminated, but the
     * print never relies on that */
    ESP_LOGI(TAG, "crash summary waiting: %u frames, task %.*s, %u log bytes", (unsigned)s_sum.n_frames,
             s_sum.task[0] ? (int)sizeof(s_sum.task) : 1, s_sum.task[0] ? s_sum.task : "-", (unsigned)s_sum.log_len);
}

const vittil_summary_t *vittil_summary_get(void)
{
    if (s_sum.magic != VITTIL_SUMMARY_MAGIC || s_sum.version != VITTIL_SUMMARY_VERSION ||
        s_sum.size != sizeof(s_sum) || s_sum.crc != sum_crc(&s_sum)) {
        return NULL;
    }
    return &s_sum;
}

#else /* CONFIG_VITTIL_SUMMARY off: no hook, no memory, nothing waits */

bool vittil_summary_boot(void)
{
    return false;
}

void vittil_summary_log(void)
{
}

const vittil_summary_t *vittil_summary_get(void)
{
    return NULL;
}

void vittil_summary_clear(void)
{
}

#endif
