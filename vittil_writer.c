/*
 * vittil_writer.c: the coredump ELF writer with the fit pass.
 *
 * Vittil's own writer, list-driven, kept short: fewer lines in the panic
 * handler are fewer places to crash. The stock panic path
 * reaches its ELF writer through the global symbol esp_core_dump_store();
 * vittil links with -Wl,--wrap=esp_core_dump_store, so that call lands in
 * __wrap_esp_core_dump_store() below. The flash layer (write_init, prepare,
 * start, data, end), the checksum and the Xtensa port (task snapshots, the
 * register dump, the extra info) stay stock and are called through
 * espcoredump's private headers. Read against ESP-IDF v5.5.5
 * components/espcoredump/src/core_dump_elf.c (the ELF the reader expects).
 *
 * The shape: one census of the tasks and the memory regions into a static
 * table, the ladder applied to the table one region at a time until the
 * image fits the partition, then one write stream (the ELF header, the
 * program headers, the segments) in a fixed order. The stock writer walks
 * the tasks three times (sizing, headers, data); this one walks them once
 * and the table after that.
 *
 * The ELF: two PT_NOTE segments (the tasks' CORE register notes; then
 * ESP_CORE_DUMP_INFO, ESP_EXTRA_INFO and the VITTIL fit note) and one
 * PT_LOAD per control block, per stack slice, per memory region. The stock's
 * ESP_PANIC_DETAILS note is not written: the record (vittil_record.c) holds
 * the reason and the description.
 *
 * The variables: .data and .bss (CONFIG_ESP_COREDUMP_CAPTURE_DRAM, which
 * Kconfig turns on with this writer) are written as pieces around what the
 * dump already holds or what no reader wants: every task's control block,
 * its stack buffer (a static task's stack sits in .bss; its live part is
 * the task's own slice), the interrupted stack, this file's own table, and
 * the record's range inside .data, written as a region of its own (the
 * floor) as it is without CAPTURE_DRAM.
 * The heap blocks the stock writes in those builds are never written:
 * walking the allocator's own headers inside the panic handler is the one
 * part of the stock this writer leaves out.
 *
 * The ladder, what is cut first when the image is over the partition. The
 * variables are the coredump's last tier, so they go before any stack:
 *   1  the .bss pieces, then the .data pieces, last to first, each kept
 *      from its low end and cut at the high end by what the image is over,
 *      or dropped when that is all of it; then the COREDUMP_*_ATTR ranges
 *      but the record's, last to first;
 *   2  the other tasks' stacks, last to first, to their top 256 bytes;
 *   3  the other tasks' stacks, last to first, dropped;
 *   4  the interrupted task's stack when the panic came from an ISR;
 *   5  the other tasks' register notes and control blocks, as pairs, last
 *      to first.
 * The floor, never cut: the crashed task's note, control block and stack
 * slice, the record's range, the info notes. When the floor does not fit
 * nothing is written and the reason is left for the next boot's event
 * (VITTIL_NODUMP_UNDER_FLOOR, vittil_priv.h). The stack slice the port
 * gives is [saved top, end) already, so no step trims a stack to its
 * high-water mark.
 *
 * Runs inside the panic handler: no allocation, no locks, no ESP_LOGx
 * (ESP_COREDUMP_LOGx is esp_rom_printf), nothing on the stack but a few
 * words (CONFIG_ESP_COREDUMP_STACK_SIZE is the budget).
 */
#include "sdkconfig.h"

/* Everything below needs espcoredump's private headers, which are on the
 * include path only when the writer is on (CMakeLists.txt). */
#if CONFIG_VITTIL_WRITER && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_private/startup_internal.h"
#include "hal/efuse_hal.h"
#include "core_dump_checksum.h"
#include "esp_core_dump_port.h"
#include "esp_core_dump_common.h"
#include "vittil_priv.h"

#define ELF_CLASS ELFCLASS32
#include "elf.h"

/* Kconfig selects it with the writer; the census below relies on it. */
#if !CONFIG_ESP_COREDUMP_CAPTURE_DRAM
#error "vittil's writer needs CONFIG_ESP_COREDUMP_CAPTURE_DRAM"
#endif

static const char TAG[] = "vittil";

#define ALIGN4(x) (((x) + 3u) & ~3u)

/* Note names and types the server's reader knows (esp-coredump reads the
 * first three). */
#define NOTE_CORE_TYPE      1
#define NOTE_INFO_NAME      "ESP_CORE_DUMP_INFO"
#define NOTE_INFO_TYPE      8266
#define NOTE_EXTRA_NAME     "ESP_EXTRA_INFO"
#define NOTE_EXTRA_TYPE     677
#define FIT_NOTE_NAME       "VITTIL"
#define FIT_NOTE_TYPE       0x00464954      /* "FIT" */
#define FIT_NOTE_VERSION    1
#define FIT_STACK_TAIL      256

/* Region kinds and cut codes, as the server reads the fit note. */
enum {
    FIT_NOTES = 1, FIT_CRASHED_TASK, FIT_ISR_STACK, FIT_TCB, FIT_STACK,
    FIT_TAGGED, FIT_DATA, FIT_BSS, FIT_HEAP_STACK, FIT_HEAP, FIT_KIND_MAX
};
enum { FIT_CUT_DROPPED = 0, FIT_CUT_TAIL = 3, FIT_CUT_HEAD = 5 };

/* The ranges kept out of .data and .bss (the census's holes) and the pieces
 * they leave. A hole under two program headers is not worth its piece, and
 * a gap that small between two holes (the padding between a static task's
 * stack and its control block) is not written. More holes than the table
 * holds: the smallest go, their bytes written twice. */
#define FIT_HOLES           16
#define FIT_HOLE_MIN        (2 * sizeof(elf_phdr))
#define FIT_MEM_MAX         (COREDUMP_MEMORY_MAX + FIT_HOLES + 1)   /* the regions, a piece per hole, the record */
#define FIT_STACK_MAX       65536u          /* a stack buffer below its slice, at most */
#define FIT_SECTOR          4096u           /* the flash layer's erase unit, SPI_FLASH_SEC_SIZE */

/* The boot check's floor before the crashed task's stack: the flash header,
 * the ELF header, five program headers (the register notes, the crashed
 * control block, its stack, the record, the info notes), one register note
 * (608 bytes on Xtensa: the note header, "CORE", 72 bytes of status and
 * 129 words of registers; 224 on RISC-V: the header, "CORE" and the
 * port's 204-byte prstatus with its 32 registers), the info notes within
 * 512 bytes, the crashed control block and the record, the checksum. */
#if __XTENSA__
#define FIT_REGS_NOTE_BYTES 608
#else
#define FIT_REGS_NOTE_BYTES 224
#endif
#define FIT_INFO_NOTES_MAX  512
#define ELF_APP_SHA256_SIZE 66

/* ESP_CORE_DUMP_INFO's body, as the stock writes it. */
typedef struct {
    uint32_t version;
    uint8_t  app_elf_sha256[ELF_APP_SHA256_SIZE];
} elf_version_info_t;

/* One task of the census: its control block and the stack slice the port
 * gives ([saved top, end) for another task, [exception frame, end) for the
 * crashed one), with the plan's cut. */
typedef struct {
    void    *tcb;
    uint32_t stk_addr;
    uint32_t stk_len;
    uint32_t keep;          /* stack bytes kept at the high end: stk_len whole, FIT_STACK_TAIL, 0 dropped */
    uint8_t  keep_tcb;      /* the control block and the register note stay */
} fit_task_t;

/* One memory region of the census (the interrupted stack, the COREDUMP_*_ATTR
 * ranges, the pieces of .data and .bss) with the plan's cut: kept from its
 * low end, out_len bytes (len whole, 0 dropped). */
typedef struct {
    uint8_t  kind;
    bool     record;        /* the record's range, never cut */
    uint32_t addr, len, out_len;
} fit_mem_t;

typedef struct {
    uint32_t orig_count, kept_count, orig_bytes, kept_bytes;
} fit_kind_t;

/* One segment of the write, as the walk hands it to a visitor. */
typedef struct {
    uint32_t type;          /* PT_NOTE or PT_LOAD */
    uint32_t vaddr;
    uint32_t len;
    const void *data;       /* a PT_LOAD's bytes */
    void (*notes)(void);    /* a PT_NOTE's writer */
} fit_seg_t;

/* The file's state, one struct: the budget read at boot,
 * the census with its plan, the sizes of the plan, the write. */
static struct {
    uint32_t budget;                /* coredump partition bytes in whole sectors */
    bool     part;                  /* a coredump partition exists */
    /* the census */
    fit_task_t task[CONFIG_ESP_COREDUMP_MAX_TASKS_NUM + 1]; /* [0] the crashed task */
    int        n_task;
    bool       crashed;             /* task[0] is the crashed task */
    fit_mem_t  isr;                 /* the interrupted stack; len 0 when none */
    fit_mem_t  mem[FIT_MEM_MAX];
    int        n_mem;
    struct { uint32_t lo, hi; } hole[FIT_HOLES]; /* by address once sorted */
    int        n_hole;
    uint32_t   regs_len;            /* one register note's body */
    uint32_t   extra_len;           /* ESP_EXTRA_INFO's body */
    /* the plan's sizes, by fit_size */
    fit_kind_t kinds[FIT_KIND_MAX];
    uint32_t   n_kinds, n_rows;
    uint32_t   notes_n;             /* notes in the two note segments */
    uint32_t   regs_seg, info_seg;  /* the two note segments' bytes */
    uint32_t   n_seg, elf_len;
    uint32_t   notes_n_wanted, notes_wanted; /* the census, nothing cut */
    uint32_t   wanted, stored;      /* the image, uncut and as planned */
    /* the write */
    core_dump_write_data_t wr;
    uint32_t   off;                 /* the next segment's file offset */
    esp_err_t  err;
} vittil_writer;

extern int _coredump_dram_start;
extern int _coredump_dram_end;

/* ------------------------------------------------------------------ boot */

ESP_SYSTEM_INIT_FN(vittil_writer_init, SECONDARY, BIT(0), 131)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    /* whole sectors: the flash layer erases the image's sectors and asserts
     * they lie inside the partition, so a partition of 6K takes a 4K image */
    vittil_writer.part = (p != NULL);
    vittil_writer.budget = p ? (p->size & ~(FIT_SECTOR - 1)) : 0;
    return ESP_OK;
}

/* The image the flash layer writes for an ELF of elf_len bytes: the flash
 * header, the ELF, padding to the write cache, the checksum
 * (esp_core_dump_flash_write_prepare). */
static uint32_t fit_image_size(uint32_t elf_len)
{
    uint32_t tot = sizeof(core_dump_header_t) + elf_len;
    uint32_t mod = tot % COREDUMP_CACHE_SIZE;
    if (mod) {
        tot += COREDUMP_CACHE_SIZE - mod;
    }
    return tot + esp_core_dump_checksum_size();
}

/* What a dump needs before the crashed task's stack; the boot check's
 * yardstick, an estimate (the stack is the one part only the panic knows). */
static uint32_t fit_floor_min(void)
{
    uint32_t elf = sizeof(elfhdr) + 5 * sizeof(elf_phdr) + FIT_REGS_NOTE_BYTES + FIT_INFO_NOTES_MAX
                   + esp_core_dump_get_tcb_len()
                   + ALIGN4((uint32_t)&_coredump_dram_end - (uint32_t)&_coredump_dram_start);
    return fit_image_size(elf);
}

void vittil_writer_boot_check(void)
{
    if (!vittil_writer.part) {
        return; /* vittil_start's line says it: no coredump partition, and what a crash sends instead */
    }
    uint32_t floor = fit_floor_min();
    if (vittil_writer.budget < floor) {
        ESP_LOGW(TAG, "coredump partition %u bytes, a dump needs at least %u before the crashed task's stack "
                 "(%u missing): a crash will be reported without a dump",
                 (unsigned)vittil_writer.budget, (unsigned)floor, (unsigned)(floor - vittil_writer.budget));
    }
}

/* ---------------------------------------------------------------- census */

/* The port's view of a task of the table (the port derives the registers
 * and the physical address of a fake stack from these three words). */
static void task_hdr(const fit_task_t *t, core_dump_task_header_t *h)
{
    h->tcb_addr = t->tcb;
    h->stack_start = t->stk_addr;
    h->stack_end = t->stk_addr + t->stk_len;
}

static void task_add(const core_dump_task_header_t *h)
{
    fit_task_t *t = &vittil_writer.task[vittil_writer.n_task++];
    uint32_t lo = h->stack_start < h->stack_end ? h->stack_start : h->stack_end;
    uint32_t hi = h->stack_start < h->stack_end ? h->stack_end : h->stack_start;
    t->tcb = h->tcb_addr;
    t->stk_addr = lo;
    t->stk_len = ALIGN4(hi - lo);
    t->keep = t->stk_len;
    t->keep_tcb = 1;
}

static void mem_set(fit_mem_t *m, uint8_t kind, uint32_t addr, uint32_t len, bool record)
{
    m->kind = kind;
    m->record = record;
    m->addr = addr;
    m->len = m->out_len = ALIGN4(len);
}

/* A region into the table; the last row is the record's, so nothing else
 * can take it. A full table leaves the region out (the table is sized for
 * every region and a piece per hole, so it is never full). */
static void mem_add(uint8_t kind, uint32_t addr, uint32_t len, bool record)
{
    typeof(vittil_writer) *w = &vittil_writer;
    if (w->n_mem < (int)FIT_MEM_MAX - (record ? 0 : 1)) {
        mem_set(&w->mem[w->n_mem++], kind, addr, len, record);
    }
}

/* The kind of a stock user RAM region: .data and .bss (both halves on the
 * ESP32-P4 revisions that split them), or a COREDUMP_*_ATTR range. */
static uint8_t mem_kind(coredump_region_t r)
{
    switch (r) {
    case COREDUMP_MEMORY_DRAM_BSS:
#if CONFIG_IDF_TARGET_ESP32P4
    case COREDUMP_MEMORY_DRAM_BSS_HIGH:
#endif
        return FIT_BSS;
    case COREDUMP_MEMORY_DRAM_DATA:
#if CONFIG_IDF_TARGET_ESP32P4
    case COREDUMP_MEMORY_DRAM_DATA_HIGH:
#endif
        return FIT_DATA;
    default:
        return FIT_TAGGED;
    }
}

/* A range to keep out of .data and .bss, words inward; the table keeps the
 * largest. */
static void hole_add(uint32_t lo, uint32_t hi)
{
    typeof(vittil_writer) *w = &vittil_writer;
    lo = ALIGN4(lo);
    hi &= ~3u;
    if (hi <= lo || hi - lo < FIT_HOLE_MIN) {
        return;
    }
    int at = w->n_hole;
    if (at == FIT_HOLES) {
        at = 0;
        for (int i = 1; i < FIT_HOLES; i++) {
            if (w->hole[i].hi - w->hole[i].lo < w->hole[at].hi - w->hole[at].lo) {
                at = i;
            }
        }
        if (hi - lo <= w->hole[at].hi - w->hole[at].lo) {
            return;
        }
    } else {
        w->n_hole++;
    }
    w->hole[at].lo = lo;
    w->hole[at].hi = hi;
}

/* The holes: each task's control block and stack buffer (the buffer's start
 * from the control block, used only as a number; the crashed task's slice
 * is the interrupt stack after an ISR, so its own buffer is not asked),
 * the interrupted stack, this table, the record. Sorted by address. */
static void fit_holes(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    uint32_t tcb_len = esp_core_dump_get_tcb_len();

    w->n_hole = 0;
    for (int i = 0; i < w->n_task; i++) {
        const fit_task_t *t = &w->task[i];
        uint32_t lo = t->stk_addr, hi = t->stk_addr + t->stk_len;
        if (!(i == 0 && w->crashed && w->isr.len > 0)) {
            uint32_t start = (uint32_t)pxTaskGetStackStart((TaskHandle_t)t->tcb);
            if (start < lo && lo - start <= FIT_STACK_MAX) {
                lo = start;
            }
        }
        hole_add((uint32_t)t->tcb, (uint32_t)t->tcb + tcb_len);
        hole_add(lo, hi);
    }
    if (w->isr.len > 0) {
        hole_add(w->isr.addr, w->isr.addr + w->isr.len);
    }
    hole_add((uint32_t)&vittil_writer, (uint32_t)&vittil_writer + sizeof(vittil_writer));
    hole_add((uint32_t)&_coredump_dram_start, (uint32_t)&_coredump_dram_end);
    for (int i = 1; i < w->n_hole; i++) {
        for (int j = i; j > 0 && w->hole[j].lo < w->hole[j - 1].lo; j--) {
            typeof(w->hole[0]) x = w->hole[j];
            w->hole[j] = w->hole[j - 1];
            w->hole[j - 1] = x;
        }
    }
}

/* A stock region into the table: .data and .bss as the pieces the holes
 * leave, any other region whole. A piece after a hole is written only when
 * it is worth its program header. */
static void mem_region(coredump_region_t r, uint32_t start, uint32_t len)
{
    typeof(vittil_writer) *w = &vittil_writer;
    uint8_t kind = mem_kind(r);
    uint32_t at = start, end = start + len;

    if (kind == FIT_DATA || kind == FIT_BSS) {
        for (int i = 0; i < w->n_hole; i++) {
            if (w->hole[i].hi <= at || w->hole[i].lo >= end) {
                continue;
            }
            if (w->hole[i].lo > at && (at == start || w->hole[i].lo - at >= FIT_HOLE_MIN)) {
                mem_add(kind, at, w->hole[i].lo - at, false);
            }
            at = w->hole[i].hi;
        }
        if (at >= end || (at != start && end - at < FIT_HOLE_MIN)) {
            return;
        }
    }
    mem_add(kind, at, end - at, false);
}

/* One walk of the tasks and the regions into the table, nothing cut. The
 * crashed task first (the port notes its control block for the extra info
 * and, from an ISR, swaps its slice for the interrupt stack and hands the
 * interrupted stack back). A task whose control block or stack fails the
 * port's checks is left out, as the stock leaves it out. */
static void fit_census(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    core_dump_task_header_t h = { 0 };
    core_dump_mem_seg_header_t isr = { 0 };
    TaskIterator_t it;
    void *regs = NULL;
    const int slots = (int)(sizeof(w->task) / sizeof(w->task[0]));

    memset(w->task, 0, sizeof(w->task));
    w->n_task = 0;
    w->crashed = false;
    memset(&w->isr, 0, sizeof(w->isr));
    memset(w->mem, 0, sizeof(w->mem));
    w->n_mem = 0;

    esp_core_dump_reset_tasks_snapshots_iter();
    /* NULL before the scheduler has a current task (a panic in startup);
     * the port would hand NULL to vTaskGetSnapshot */
    void *cur = esp_core_dump_get_current_task_handle();
    if (cur && esp_core_dump_get_task_snapshot(cur, &h, &isr)) {
        task_add(&h);
        w->crashed = true;
        if (isr.size > 0) {
            mem_set(&w->isr, FIT_ISR_STACK, isr.start, isr.size, false);
        }
    }
    esp_core_dump_task_iterator_init(&it);
    while (esp_core_dump_task_iterator_next(&it) != -1) {
        void *t = it.pxTaskHandle;
        if (!t || t == cur) {
            continue;
        }
        if (w->n_task >= slots) {
            ESP_COREDUMP_LOGW("vittil: more than %d tasks, the rest left out", slots);
            break;
        }
        if (esp_core_dump_get_task_snapshot(t, &h, NULL)) {
            task_add(&h);
        }
    }
    fit_holes();
    for (coredump_region_t r = COREDUMP_MEMORY_START; r < COREDUMP_MEMORY_MAX; r++) {
        uint32_t start = 0;
        int len = esp_core_dump_get_user_ram_info(r, &start);
        if (len > 0) {
            mem_region(r, start, (uint32_t)len);
        }
    }
    uint32_t rec = (uint32_t)&_coredump_dram_start;
    mem_add(FIT_TAGGED, rec, (uint32_t)&_coredump_dram_end - rec, true);
    w->regs_len = 0;
    if (w->n_task > 0) {
        task_hdr(&w->task[0], &h);
        w->regs_len = esp_core_dump_get_task_regs_dump(&h, &regs);
    }
    /* asked with a pointer: the RISC-V port answers 0 for NULL */
    void *extra = NULL;
    w->extra_len = esp_core_dump_get_extra_info(&extra);
}

/* ------------------------------------------------------------- the sizes */

static uint32_t note_len(const char *name, uint32_t desc_len)
{
    return sizeof(elf_note) + ALIGN4(strlen(name) + 1) + ALIGN4(desc_len);
}

static uint32_t fit_note_desc(void)
{
    return 4 * (6 + 5 * vittil_writer.n_kinds + 5 * vittil_writer.n_rows);
}

static void kind_add(uint8_t kind, uint32_t orig, uint32_t kept)
{
    fit_kind_t *k = &vittil_writer.kinds[kind];
    k->orig_count++;
    k->orig_bytes += orig;
    if (kept > 0) {
        k->kept_count++;
        k->kept_bytes += kept;
    }
    if (kept < orig) {
        vittil_writer.n_rows++;
    }
}

/* Size the plan in the table: the kind rows, the row count, the note
 * segments, the segment count, the ELF; returns the image size. */
static uint32_t fit_size(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    uint32_t tcb_len = esp_core_dump_get_tcb_len();
    uint32_t loads = 0, n_load = 0, kept_tasks = 0;

    memset(w->kinds, 0, sizeof(w->kinds));
    w->n_rows = 0;
    for (int i = 0; i < w->n_task; i++) {
        const fit_task_t *t = &w->task[i];
        uint8_t kind = (i == 0 && w->crashed) ? FIT_CRASHED_TASK : FIT_TCB;
        kind_add(kind, tcb_len, t->keep_tcb ? tcb_len : 0);
        kind_add((kind == FIT_TCB) ? FIT_STACK : kind, t->stk_len, t->keep);
        if (t->keep_tcb) {
            kept_tasks++;
            loads += tcb_len;
            n_load++;
        }
        if (t->keep > 0) {
            loads += t->keep;
            n_load++;
        }
    }
    for (int i = -1; i < w->n_mem; i++) {
        const fit_mem_t *m = (i < 0) ? &w->isr : &w->mem[i];
        if (m->len == 0) {
            continue;
        }
        kind_add(m->kind, m->len, m->out_len);
        if (m->out_len > 0) {
            loads += m->out_len;
            n_load++;
        }
    }
    /* the notes: one CORE note per kept task; INFO, EXTRA and the fit note,
     * whose body is 6 words, 5 per kind present, 5 per cut region */
    w->n_kinds = 1;                         /* the notes' own row */
    for (int k = FIT_CRASHED_TASK; k < FIT_KIND_MAX; k++) {
        if (w->kinds[k].orig_count > 0) {
            w->n_kinds++;
        }
    }
    w->regs_seg = kept_tasks * note_len("CORE", w->regs_len);
    w->info_seg = note_len(NOTE_INFO_NAME, sizeof(elf_version_info_t))
                  + note_len(NOTE_EXTRA_NAME, w->extra_len) + note_len(FIT_NOTE_NAME, fit_note_desc());
    w->notes_n = kept_tasks + 3;
    w->n_seg = 2 + n_load;
    w->elf_len = sizeof(elfhdr) + w->n_seg * sizeof(elf_phdr) + w->regs_seg + w->info_seg + loads;
    return fit_image_size(w->elf_len);
}

/* -------------------------------------------------------------- the plan */

static bool fits(void)
{
    return fit_size() <= vittil_writer.budget;
}

/* A .data or .bss piece kept from its low end: cut at the high end by what
 * the image is over, with room for the flash layer's padding, or dropped
 * when that is all of it. The piece counts as cut before the excess is
 * measured, so the fit note's row for it is in the size. */
static void mem_trim(fit_mem_t *m)
{
    m->out_len = (m->len > 4) ? m->len - 4 : 0;
    uint32_t size = fit_size();
    if (m->out_len == 0 || size <= vittil_writer.budget) {
        return;
    }
    uint32_t over = ALIGN4(size - vittil_writer.budget + COREDUMP_CACHE_SIZE - 1);
    m->out_len = (over >= m->out_len) ? 0 : m->out_len - over;
}

/* The ladder, one region a step, stopping at the first plan that fits.
 * False when even the floor is over the budget (the reason noted). */
static bool fit_plan(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    const int first_other = w->crashed ? 1 : 0;
    int i;

    w->wanted = fit_size();
    w->notes_n_wanted = w->notes_n;
    w->notes_wanted = w->regs_seg + w->info_seg;

    /* 1  the variables: the .bss pieces, the .data pieces, last to first,
     *    trimmed; then the other tagged ranges last to first, dropped */
    static const uint8_t vars[] = { FIT_BSS, FIT_DATA, FIT_TAGGED };
    for (int k = 0; k < 3; k++) {
        for (i = w->n_mem - 1; i >= 0 && !fits(); i--) {
            fit_mem_t *m = &w->mem[i];
            if (m->kind == vars[k] && !m->record) {
                if (m->kind == FIT_TAGGED) {
                    m->out_len = 0;
                } else {
                    mem_trim(m);
                }
            }
        }
    }
    /* 2, 3  the other tasks' stacks, last to first: the top 256 bytes, then dropped */
    for (i = w->n_task - 1; i >= first_other && !fits(); i--) {
        if (w->task[i].keep > FIT_STACK_TAIL) {
            w->task[i].keep = FIT_STACK_TAIL;
        }
    }
    for (i = w->n_task - 1; i >= first_other && !fits(); i--) {
        w->task[i].keep = 0;
    }
    /* 4  the interrupted task's stack */
    if (w->isr.len > 0 && !fits()) {
        w->isr.out_len = 0;
    }
    /* 5  the other tasks' register notes and control blocks, as pairs */
    for (i = w->n_task - 1; i >= first_other && !fits(); i--) {
        w->task[i].keep_tcb = 0;
    }
    if (!fits()) {
        uint32_t floor = fit_size();
        ESP_COREDUMP_LOGE("the floor is %u bytes, the partition %u: nothing written",
                          (unsigned)floor, (unsigned)w->budget);
        vittil_record_nodump_set(VITTIL_NODUMP_UNDER_FLOOR, floor, w->budget);
        return false;
    }
    w->stored = fit_size();
    return true;
}

/* ------------------------------------------------------------- the write */

/* Every write goes through wr. The first error sticks: every later wr is
 * skipped and the store returns it, so nothing below checks its own. */
static void wr(const void *data, uint32_t len)
{
    if (vittil_writer.err != ESP_OK || len == 0) {
        return;
    }
    vittil_writer.err = esp_core_dump_write_data(&vittil_writer.wr, (void *)data, len);
    if (vittil_writer.err != ESP_OK) {
        ESP_COREDUMP_LOGE("vittil: flash write failed (%d)", vittil_writer.err);
    }
}

/* Zeros from len up to the next multiple of 4. */
static void pad(uint32_t len)
{
    static const uint8_t zero[4] = { 0 };
    wr(zero, ALIGN4(len) - len);
}

/* A note's header and name; the body follows. */
static void note_head(const char *name, uint32_t type, uint32_t desc_len)
{
    uint32_t n = strlen(name) + 1;
    elf_note h = { .n_namesz = n, .n_descsz = desc_len, .n_type = type };
    wr(&h, sizeof(h));
    wr(name, n);
    pad(n);
}

static void note_write(const char *name, uint32_t type, const void *desc, uint32_t desc_len)
{
    note_head(name, type, desc_len);
    wr(desc, desc_len);
    pad(desc_len);
}

/* The port's answer at the write against the census's: another size stops
 * the write, as a failed flash write does. */
static bool same_len(const char *what, uint32_t len, uint32_t want, const void *p)
{
    if (len == want && p) {
        return true;
    }
    if (vittil_writer.err == ESP_OK) {
        ESP_COREDUMP_LOGE("vittil: %s %u bytes, expected %u", what, (unsigned)len, (unsigned)want);
        vittil_writer.err = ESP_FAIL;
    }
    return false;
}

static void row(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    const uint32_t v[5] = { a, b, c, d, e };
    wr(v, sizeof(v));
}

static void mem_row(const fit_mem_t *m)
{
    if (m->out_len < m->len) {
        row(m->kind, m->addr, m->len, m->out_len, m->out_len ? FIT_CUT_HEAD : FIT_CUT_DROPPED);
    }
}

/* The VITTIL note: the header, one row per kind present, one
 * row per region dropped or shortened, in the segments' order. */
static void fit_note_write(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    uint32_t tcb_len = esp_core_dump_get_tcb_len();
    const uint32_t hdr[6] = { FIT_NOTE_VERSION, w->budget, w->wanted, w->stored, w->n_kinds, w->n_rows };

    note_head(FIT_NOTE_NAME, FIT_NOTE_TYPE, fit_note_desc());
    wr(hdr, sizeof(hdr));
    row(FIT_NOTES, w->notes_n_wanted, w->notes_n, w->notes_wanted, w->regs_seg + w->info_seg);
    for (int k = FIT_CRASHED_TASK; k < FIT_KIND_MAX; k++) {
        const fit_kind_t *c = &w->kinds[k];
        if (c->orig_count > 0) {
            row(k, c->orig_count, c->kept_count, c->orig_bytes, c->kept_bytes);
        }
    }
    mem_row(&w->isr);
    for (int i = 0; i < w->n_task; i++) {
        const fit_task_t *t = &w->task[i];
        if (!t->keep_tcb) {
            row(FIT_TCB, (uint32_t)t->tcb, tcb_len, 0, FIT_CUT_DROPPED);
        }
        if (t->keep < t->stk_len) {
            row(FIT_STACK, t->stk_addr, t->stk_len, t->keep, t->keep ? FIT_CUT_TAIL : FIT_CUT_DROPPED);
        }
    }
    for (int i = 0; i < w->n_mem; i++) {
        mem_row(&w->mem[i]);
    }
}

/* The first note segment: a CORE note per kept task. */
static void regs_notes(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    for (int i = 0; i < w->n_task; i++) {
        core_dump_task_header_t h;
        void *regs = NULL;
        if (!w->task[i].keep_tcb) {
            continue;
        }
        task_hdr(&w->task[i], &h);
        uint32_t len = esp_core_dump_get_task_regs_dump(&h, &regs);
        if (!same_len("register dump", len, w->regs_len, regs)) {
            return;
        }
        note_write("CORE", NOTE_CORE_TYPE, regs, len);
    }
}

/* The last note segment: ESP_CORE_DUMP_INFO, ESP_EXTRA_INFO, the fit note. */
static void info_notes(void)
{
    elf_version_info_t info = { 0 };
    void *extra = NULL;
    info.version = esp_core_dump_elf_version();
    strlcpy((char *)info.app_elf_sha256, esp_app_get_elf_sha256_str(), sizeof(info.app_elf_sha256));
    note_write(NOTE_INFO_NAME, NOTE_INFO_TYPE, &info, sizeof(info));
    uint32_t len = esp_core_dump_get_extra_info(&extra);
    if (same_len("extra info", len, vittil_writer.extra_len, extra)) {
        note_write(NOTE_EXTRA_NAME, NOTE_EXTRA_TYPE, extra, len);
        fit_note_write();
    }
}

static void mem_seg(void (*visit)(const fit_seg_t *), const fit_mem_t *m)
{
    if (m->out_len > 0) {
        visit(&(fit_seg_t) { PT_LOAD, m->addr, m->out_len, (const void *)m->addr, NULL });
    }
}

/* The walk: every segment of the plan in the file's order, handed to the
 * visitor. The order is the one the fit note's rows follow. */
static void fit_walk(void (*visit)(const fit_seg_t *))
{
    typeof(vittil_writer) *w = &vittil_writer;
    uint32_t tcb_len = esp_core_dump_get_tcb_len();

    visit(&(fit_seg_t) { PT_NOTE, 0, w->regs_seg, NULL, regs_notes });
    for (int i = 0; i < w->n_task; i++) {
        const fit_task_t *t = &w->task[i];
        if (t->keep_tcb) {
            visit(&(fit_seg_t) { PT_LOAD, (uint32_t)t->tcb, tcb_len, t->tcb, NULL });
        }
        if (t->keep > 0) {
            core_dump_task_header_t h;
            uint32_t vaddr = 0, paddr = 0;
            task_hdr(t, &h);
            (void)esp_core_dump_get_stack(&h, &vaddr, &paddr);  /* a fake stack reads elsewhere */
            uint32_t off = t->stk_len - t->keep;
            visit(&(fit_seg_t) { PT_LOAD, t->stk_addr + off, t->keep, (const void *)(paddr + off), NULL });
        }
        if (i == 0) {
            mem_seg(visit, &w->isr);
        }
    }
    for (int i = 0; i < w->n_mem; i++) {
        mem_seg(visit, &w->mem[i]);
    }
    visit(&(fit_seg_t) { PT_NOTE, 0, w->info_seg, NULL, info_notes });
}

static void visit_phdr(const fit_seg_t *s)
{
    const elf_phdr p = { .p_type = s->type, .p_offset = vittil_writer.off, .p_vaddr = s->vaddr,
                         .p_paddr = s->vaddr, .p_filesz = s->len, .p_memsz = s->len, .p_flags = PF_R | PF_W };
    vittil_writer.off += s->len;
    wr(&p, sizeof(p));
}

static void visit_data(const fit_seg_t *s)
{
    if (s->notes) {
        s->notes();
    } else {
        wr(s->data, s->len);
    }
}

static void elf_header_write(void)
{
    const elfhdr e = {
        .e_ident = { ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, ELFCLASS32, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE },
        .e_type = ET_CORE,
        .e_machine = esp_core_dump_get_arch_id(),
        .e_version = EV_CURRENT,
        .e_phoff = sizeof(elfhdr),
        .e_ehsize = sizeof(elfhdr),
        .e_phentsize = sizeof(elf_phdr),
        .e_phnum = vittil_writer.n_seg,
        .e_shentsize = sizeof(elf_shdr),
        .e_shstrndx = SHN_UNDEF,
    };
    wr(&e, sizeof(e));
}

/* The stock panic path's call, through the linker's wrap. */
esp_err_t __wrap_esp_core_dump_store(void)
{
    typeof(vittil_writer) *w = &vittil_writer;
    core_dump_header_t hdr = { 0 };
    uint32_t tot;

    if (!w->part) {
        ESP_COREDUMP_LOGE("no coredump partition: nothing written");
        vittil_record_nodump_set(VITTIL_NODUMP_NO_PARTITION, 0, 0);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_core_dump_write_init();
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("vittil: flash init failed (%d)", err);
        return ESP_FAIL;
    }
    fit_census();
    if (!fit_plan()) {
        return ESP_ERR_NO_MEM;
    }
    ESP_COREDUMP_LOGI("budget %u, wanted %u, stored %u, %u regions cut, %u of %u notes kept",
                      (unsigned)w->budget, (unsigned)w->wanted, (unsigned)w->stored, (unsigned)w->n_rows,
                      (unsigned)w->notes_n, (unsigned)w->notes_n_wanted);

    memset(&w->wr, 0, sizeof(w->wr));
    w->err = ESP_OK;
    tot = sizeof(hdr) + w->elf_len;             /* prepare adds the padding and the checksum */
    err = esp_core_dump_write_prepare(&w->wr, &tot);
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("vittil: flash prepare failed (%d)", err);
        return err;
    }
    if (tot != w->stored) {
        ESP_COREDUMP_LOGE("image %u bytes, planned %u: nothing written", (unsigned)tot, (unsigned)w->stored);
        return ESP_FAIL;
    }
    err = esp_core_dump_write_start(&w->wr);
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("vittil: flash start failed (%d)", err);
        return err;
    }
    hdr.data_len = tot;
    hdr.version = esp_core_dump_elf_version();
    hdr.chip_rev = efuse_hal_chip_revision();
    w->off = sizeof(elfhdr) + w->n_seg * sizeof(elf_phdr);
    wr(&hdr, sizeof(hdr));
    elf_header_write();
    fit_walk(visit_phdr);
    fit_walk(visit_data);
    if (w->err != ESP_OK) {
        return w->err;
    }
    err = esp_core_dump_write_end(&w->wr);
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("vittil: flash end failed (%d)", err);
    }
    return err;
}

#endif /* CONFIG_VITTIL_WRITER && ..._ENABLE_TO_FLASH && ..._DATA_FORMAT_ELF */
