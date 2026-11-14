/*
 * sleep_test_core.h -- the sys_sleep button test, in portable C.
 *
 * No Arduino headers here on purpose: the same code runs on the Teensy (the
 * HAL is filled in by PtsaSleepTest.ino) and on a PC against a simulated
 * accelerator (test/). Everything that touches hardware goes through
 * pst_hal_t.
 *
 * WHAT IT DOES. A press of the Teensy's button (pin 3) sends `sys_sleep 15` to
 * the accelerator's shell on Serial1. Then one of two things happens:
 *
 *   On the accelerator's power rail (normal wiring): the accelerator powers the
 *   Teensy down with everything else. Before that, the test saves a "pending"
 *   record to EEPROM. At the next boot -- when the accelerator wakes and its
 *   rails come back -- the test finds the record, asks the accelerator for
 *   sys_info, and checks that it has just done a software reset. If the Teensy
 *   has an RTC coin cell, the sleep length is measured too.
 *
 *   Powered from USB (bench): the Teensy stays up, sees the accelerator's boot
 *   banner, and measures the sleep with its own clock.
 *
 * The behaviour being checked is described in README.md ("The sys_sleep
 * contract").
 */
#ifndef SLEEP_TEST_CORE_H
#define SLEEP_TEST_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PST_SLEEP_S 15u

/* sys_sleep's timer starts only after the power-down, which can take up to
 * ~11 s (SIM AT check / AT+CPOF wait), and the boot adds a few seconds. */
#define PST_POWER_DOWN_MAX_MS 11000u
#define PST_BOOT_MAX_MS       10000u

/* Minimum firmware: 7.12.0 drives P0.08 high into the unpowered Teensy. */
#define PST_MIN_MAJOR 7
#define PST_MIN_MINOR 12
#define PST_MIN_PATCH 1

#define PST_PERSIST_MAGIC 0x50535431u /* "PST1" */

#define PST_MAX_LINES 24
#define PST_LINE_LEN  96

/* What survives the Teensy losing power. Stored in EEPROM by the sketch. */
typedef struct {
    uint32_t magic;
    uint8_t  pending;    /* 1: a sleep was acknowledged; verify after the reboot */
    uint32_t seq;        /* test number, counts up */
    uint32_t rtc_before; /* RTC seconds at the acknowledgement */
    uint32_t passes;
    uint32_t fails;
    char     last[128];  /* last result, printed at boot */
} pst_persist_t;

/* Hardware access. Every function gets `ctx` back as its first argument. */
typedef struct {
    void *ctx;
    uint32_t (*ms)(void *ctx);                     /* milliseconds since this boot */
    int      (*uart_read)(void *ctx);              /* next byte from the accelerator, -1 if none */
    void     (*uart_write)(void *ctx, const char *s);
    bool     (*button_down)(void *ctx);            /* true while pressed */
    void     (*led)(void *ctx, bool on);
    void     (*log)(void *ctx, const char *line);  /* one line, to USB Serial */
    void     (*load)(void *ctx, pst_persist_t *p);
    void     (*save)(void *ctx, const pst_persist_t *p);
    uint32_t (*rtc)(void *ctx);                    /* seconds; keeps counting through power loss only with a coin cell */
} pst_hal_t;

/* What the accelerator's sys_info said. */
typedef struct {
    bool ok;
    int  major, minor, patch;
    long uptime_ms;
    char reset_cause[24];
    char board_rev[4];
} pst_info_t;

typedef enum {
    PST_BOOT_SYNC, PST_BOOT_INFO,             /* after a reboot with a pending test */
    PST_IDLE,
    PST_REQ_SYNC, PST_REQ_INFO, PST_REQ_ACK,  /* button pressed */
    PST_ASLEEP,                               /* acknowledged; waiting to lose power or see the banner */
    PST_LIVE_SYNC, PST_LIVE_INFO,             /* still powered and the accelerator woke */
} pst_phase_t;

typedef enum { PST_CMD_NONE, PST_CMD_BUSY, PST_CMD_DONE, PST_CMD_TIMEOUT } pst_cmd_t;

/* The whole test state. Treat the fields as private; use the functions below. */
typedef struct {
    const pst_hal_t *hal;
    pst_persist_t    p;
    pst_phase_t      phase;

    /* Receive buffer. A prompt has no newline after it, so it ends a line too. */
    char     lines[PST_MAX_LINES][PST_LINE_LEN];
    size_t   nlines;
    char     cur[PST_LINE_LEN];
    size_t   curlen;
    bool     prompt, banner, ack;
    uint32_t banner_ms;

    /* Command in flight */
    pst_cmd_t cmd;
    bool      syncing;
    uint32_t  cmd_start, cmd_timeout, last_cr;

    /* Timing */
    uint32_t ack_ms;
    bool     announced_still_powered;

    /* Button debounce */
    bool     btn_stable, btn_last;
    uint32_t btn_change_ms;
} pst_test_t;

void pst_init(pst_test_t *t, const pst_hal_t *hal); /* zero the state and attach the HAL */
void pst_begin(pst_test_t *t);                      /* once at boot */
void pst_poll(pst_test_t *t);                       /* from loop() */

bool pst_parse_info(const char lines[][PST_LINE_LEN], size_t n, pst_info_t *out);
bool pst_version_ok(const pst_info_t *i);

#ifdef __cplusplus
}
#endif

#endif /* SLEEP_TEST_CORE_H */
