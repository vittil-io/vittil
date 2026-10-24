/* vittil internals: the crash summary (vittil_summary.c). Not an API.
 * Its own header, apart from vittil_priv.h, so the file builds alone. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#define VITTIL_SUMMARY_MAGIC   0x4D555356u /* bytes 56 53 55 4D, "VSUM" */
#define VITTIL_SUMMARY_VERSION 2
#define VITTIL_SUMMARY_FRAMES  32

/* The log tail's room: CONFIG_VITTIL_SUMMARY_LOG_BYTES rounded up to 4 (the
 * CRC32 after it stays aligned), and 4 when it is 0, so the layout keeps one
 * shape and the server reads log_cap rather than the setting. */
#ifdef CONFIG_VITTIL_SUMMARY_LOG_BYTES
#define VITTIL_SUMMARY_LOG_SET CONFIG_VITTIL_SUMMARY_LOG_BYTES
#else
#define VITTIL_SUMMARY_LOG_SET 0 /* the summary off: the setting is not there */
#endif
#define VITTIL_SUMMARY_LOG (VITTIL_SUMMARY_LOG_SET ? ((VITTIL_SUMMARY_LOG_SET + 3) & ~3) : 4)

#if __XTENSA__
#define VITTIL_SUMMARY_ARCH  1
#define VITTIL_SUMMARY_REGS  24   /* pc ps a0..a15 sar exccause excvaddr lbeg lend lcount */
#define VITTIL_SUMMARY_STACK 4    /* unused: the backtrace is walked on the device */
#else
#define VITTIL_SUMMARY_ARCH  2
#define VITTIL_SUMMARY_REGS  37   /* RvExcFrame: mepc ra sp gp tp t0..t6 s0..s11 a0..a7 mstatus mtvec mcause mtval mhartid */
#define VITTIL_SUMMARY_STACK 1024 /* no frame walk on RISC-V: the top of the stack for the server */
#endif

#define VITTIL_SUMMARY_F_PSEUDO  1u /* the cause is IDF's pseudo cause (abort, a watchdog) */
#define VITTIL_SUMMARY_F_LOG_CUT 2u /* the ring held older bytes than the tail: its first line may be partial */

/* The summary: what a crash leaves when there is no coredump partition (a
 * field device updated by OTA, whose table never changes). Kept in memory
 * the crash reboot does not clear and sent on a later boot. Little-endian,
 * every value raw; the server reads it, the frames against the build's
 * ELF. The magic is written last, after the
 * CRC32, which covers version up to crc. The log tail is the ring's newest
 * bytes as raw text, oldest first; the server splits the lines. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                /* sizeof(vittil_summary_t) */
    uint8_t  arch;                /* 1 Xtensa, 2 RISC-V */
    uint8_t  core;
    uint8_t  exception;           /* panic_exception_t */
    uint8_t  flags;               /* VITTIL_SUMMARY_F_* */
    uint32_t cause;               /* EXCCAUSE / mcause */
    uint32_t fault_addr;          /* EXCVADDR / mtval */
    uint32_t uptime_us[2];        /* esp_timer_get_time() at the crash, low word first; 0 when not read */
    uint8_t  build[32];           /* the crashed app's ELF SHA-256 */
    char     fw[32];              /* the crashed app's version */
    char     task[16];            /* the crashed task's name, "" when not read */
    char     detail[96];          /* the abort or assert line, "" otherwise */
    uint32_t regs[VITTIL_SUMMARY_REGS];
    uint16_t n_frames;
    uint16_t stack_len;
    uint32_t frames[VITTIL_SUMMARY_FRAMES];
    uint32_t sps[VITTIL_SUMMARY_FRAMES]; /* each frame's sp, as IDF prints it beside the pc; 0 when not known */
    uint32_t stack_sp;
    uint8_t  stack[VITTIL_SUMMARY_STACK];
    uint16_t log_cap;             /* sizeof(log) */
    uint16_t log_len;             /* bytes of log in use */
    char     log[VITTIL_SUMMARY_LOG]; /* the newest log bytes, oldest first, unwrapped */
    uint32_t crc;
} vittil_summary_t;

/* At boot, once (vittil_start): the summary area is checked. Anything that
 * is not a whole summary (no magic, another version or size, a bad CRC32)
 * is zeroed with one log line; a valid one is kept until it is sent.
 * Returns true when one is waiting. */
bool vittil_summary_boot(void);

/* The waiting summary's info line, once it is known to be sent (no dump
 * staged beside it). */
void vittil_summary_log(void);

/* The waiting summary, or NULL. Valid until vittil_summary_clear or the
 * next crash (which reboots). */
const vittil_summary_t *vittil_summary_get(void);

/* Sent, or not needed (a dump is staged): zeroed. */
void vittil_summary_clear(void);

/* The SDK's log ring, for the log tail (vittil_record.c): its bytes, its size
 * and its head (bytes ever written; head % size is the next slot). Called
 * inside the panic: IRAM, no lock, no log. False on a NULL argument. */
bool vittil_record_ring(const char **ring, uint32_t *size, uint32_t *head);
