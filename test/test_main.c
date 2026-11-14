/*
 * test_main.c -- scenario tests for sleep_test_core against the simulation.
 *
 *   make -C test
 *
 * Each scenario checks that the Teensy-side test PASSES a board that behaves
 * as the sys_sleep contract in README.md says, and FAILS one that does not.
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"

static int         failures;
static const char *current;
static sim_t       w; /* large; static rather than on the stack */

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s: %s (line %d)\n", current, #cond, __LINE__);     \
            failures++;                                                        \
            return;                                                            \
        }                                                                      \
    } while (0)

static bool last_has(const char *s) { return strstr(w.eeprom.last, s) != NULL; }

/* --- Normal wiring: the Teensy is on the accelerator's rail. ------------- */
static void on_rail_pass(void)
{
    accel_opts_t o = sim_default_opts();
    sim_init(&w, &o);
    sim_run(&w, 2000);              /* Teensy boots, idles */
    sim_press(&w, 200);
    sim_run(&w, 3000);
    CHECK(w.eeprom.pending == 1);   /* saved before the power went */
    CHECK(w.sleeps == 1);
    sim_run(&w, 30000);             /* power-down, 15 s sleep, wake, reboot, verify */
    CHECK(w.teensy_boots == 2);
    CHECK(w.eeprom.pending == 0);
    CHECK(w.eeprom.passes == 1 && w.eeprom.fails == 0);
    CHECK(last_has("PASS: Teensy lost power and came back"));
    CHECK(last_has("length not measured"));
}

static void on_rail_with_rtc_measures_length(void)
{
    accel_opts_t o = sim_default_opts();
    sim_init(&w, &o);
    w.rtc_battery = true;
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 35000);
    CHECK(w.eeprom.passes == 1);
    CHECK(last_has("off ~2")); /* ~21-22 s: 6 s power-down + 15 s + boot */
}

static void on_rail_watchdog_reset_fails(void)
{
    accel_opts_t o = sim_default_opts();
    snprintf(o.reset_cause_after_wake, sizeof o.reset_cause_after_wake, "watchdog");
    sim_init(&w, &o);
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 35000);
    CHECK(w.eeprom.fails == 1);
    CHECK(last_has("Reset Cause 'watchdog'"));
}

static void on_rail_early_wake_caught_by_rtc(void)
{
    accel_opts_t o = sim_default_opts();
    o.wake_early    = true; /* 7.5 s instead of 15 s */
    o.power_down_ms = 500;
    sim_init(&w, &o);
    w.rtc_battery = true;
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 30000);
    CHECK(w.eeprom.fails == 1);
    CHECK(last_has("less than 15 s"));
}

/* --- Bench: the Teensy is powered from USB and stays up. ----------------- */
static void usb_powered_pass_measures_live(void)
{
    accel_opts_t o = sim_default_opts();
    sim_init(&w, &o);
    w.teensy_on_rail = false;
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 30000);
    CHECK(w.teensy_boots == 1); /* never lost power */
    CHECK(w.eeprom.passes == 1);
    CHECK(last_has("Teensy stayed powered; slept 2")); /* 6 s + 15 s + 1.5 s boot */
    CHECK(sim_logged(&w, "not on the accelerator's rail"));
}

static void usb_powered_early_wake_fails(void)
{
    accel_opts_t o = sim_default_opts();
    o.wake_early    = true;
    o.power_down_ms = 500;
    sim_init(&w, &o);
    w.teensy_on_rail = false;
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 30000);
    CHECK(w.eeprom.fails == 1);
    CHECK(last_has("before 15 s"));
}

/* --- Refusals: nothing must sleep. --------------------------------------- */
static void old_firmware_is_not_put_to_sleep(void)
{
    accel_opts_t o = sim_default_opts();
    snprintf(o.version, sizeof o.version, "7.12.0");
    sim_init(&w, &o);
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 5000);
    CHECK(w.sleeps == 0);
    CHECK(w.eeprom.fails == 1);
    CHECK(last_has("older than 7.12.1"));
}

static void no_acknowledgement_fails(void)
{
    accel_opts_t o = sim_default_opts();
    o.ack = false;
    sim_init(&w, &o);
    w.teensy_on_rail = false;
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 5000);
    CHECK(w.eeprom.fails == 1);
    CHECK(last_has("no 'Sleep for 15 s'"));
}

static void dfu_refusal_reported(void)
{
    accel_opts_t o = sim_default_opts();
    o.refuse = true;
    sim_init(&w, &o);
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 5000);
    CHECK(w.eeprom.fails == 1);
    CHECK(last_has("refused, DFU session active"));
}

/* --- Button handling. ---------------------------------------------------- */
static void bounce_and_hold_start_one_test(void)
{
    accel_opts_t o = sim_default_opts();
    int          i;

    sim_init(&w, &o);
    w.teensy_on_rail = false;
    sim_run(&w, 2000);
    for (i = 0; i < 5; i++) { /* 5 ms chatter, then a long hold */
        w.button = true;
        sim_run(&w, 5);
        w.button = false;
        sim_run(&w, 5);
    }
    sim_press(&w, 5000); /* held through the request */
    sim_run(&w, 30000);
    CHECK(w.sleeps == 1);
    CHECK(w.eeprom.seq == 1);
    CHECK(w.eeprom.passes == 1);
}

static void two_tests_in_a_row_count_up(void)
{
    accel_opts_t o = sim_default_opts();
    sim_init(&w, &o);
    sim_run(&w, 2000);
    sim_press(&w, 200);
    sim_run(&w, 35000);
    sim_press(&w, 200);
    sim_run(&w, 35000);
    CHECK(w.sleeps == 2);
    CHECK(w.eeprom.seq == 2 && w.eeprom.passes == 2);
    CHECK(w.teensy_boots == 3);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} scenario_t;

int main(void)
{
    static const scenario_t tests[] = {
        {"on_rail_pass", on_rail_pass},
        {"on_rail_with_rtc_measures_length", on_rail_with_rtc_measures_length},
        {"on_rail_watchdog_reset_fails", on_rail_watchdog_reset_fails},
        {"on_rail_early_wake_caught_by_rtc", on_rail_early_wake_caught_by_rtc},
        {"usb_powered_pass_measures_live", usb_powered_pass_measures_live},
        {"usb_powered_early_wake_fails", usb_powered_early_wake_fails},
        {"old_firmware_is_not_put_to_sleep", old_firmware_is_not_put_to_sleep},
        {"no_acknowledgement_fails", no_acknowledgement_fails},
        {"dfu_refusal_reported", dfu_refusal_reported},
        {"bounce_and_hold_start_one_test", bounce_and_hold_start_one_test},
        {"two_tests_in_a_row_count_up", two_tests_in_a_row_count_up},
    };
    const size_t n = sizeof tests / sizeof tests[0];
    size_t       i;

    for (i = 0; i < n; i++) {
        int before = failures;
        current    = tests[i].name;
        tests[i].fn();
        if (failures == before) {
            printf("  ok   %s\n", tests[i].name);
        }
    }
    printf("%zu scenarios, %d failed\n", n, failures);
    return failures != 0;
}
