/*
 * sim.h -- a simulated Teensy + accelerator for testing sleep_test_core on a PC.
 *
 * Time is simulated in 1 ms steps, so a 15 s sleep runs in milliseconds.
 * The accelerator side follows the sys_sleep contract in README.md; the Teensy
 * side models what matters to the test: whether it loses power with the
 * accelerator's rails (on the rail) or not (USB powered), its EEPROM, which
 * survives, and its RTC, which survives only with a coin cell.
 */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>

#include "../PtsaSleepTest/src/sleep_test_core.h"

typedef struct {
    char     version[16];
    char     reset_cause_after_wake[16];
    bool     ack;        /* print "Sleep for N s" */
    bool     wake_early; /* bug: sleeps half the time */
    bool     refuse;     /* answer as if a DFU session is open */
    long     power_down_ms;
    long     boot_ms;    /* reset -> banner -> shell ready */
} accel_opts_t;

#define SIM_TX_SIZE 8192
#define SIM_LOG_MAX 64

typedef struct {
    /* World */
    long now; /* ms */
    bool teensy_on_rail;
    bool rtc_battery;
    bool button;

    /* Teensy */
    pst_persist_t eeprom; /* survives power loss */
    bool          teensy_alive;
    long          teensy_boot_at;
    int           teensy_boots;
    pst_hal_t     hal;
    pst_test_t    test;
    char          log[SIM_LOG_MAX][200];
    int           nlog;

    /* Accelerator */
    accel_opts_t o;
    char         tx[SIM_TX_SIZE]; /* bytes to the Teensy, ring buffer */
    size_t       tx_head, tx_tail;
    char         in[128];
    size_t       inlen;
    bool         rails, shell;
    long         boot_at, down_at, wake_at, ready_at;
    char         reset_cause[16];
    int          sleeps; /* sys_sleep commands accepted */
} sim_t;

accel_opts_t sim_default_opts(void);
void sim_init(sim_t *s, const accel_opts_t *o); /* Teensy on the rail, no RTC battery */
void sim_step(sim_t *s);
void sim_run(sim_t *s, long ms);
void sim_press(sim_t *s, long hold_ms);
bool sim_logged(const sim_t *s, const char *needle);

#endif /* SIM_H */
