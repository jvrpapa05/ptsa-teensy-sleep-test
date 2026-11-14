// test_main.cpp -- scenario tests for sleep_test_core against the simulation.
//
//   make -C test
//
// Each scenario checks that the Teensy-side test PASSES a board that behaves
// as the sys_sleep contract in README.md says, and FAILS one that does not.
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "sim.h"

static int failures = 0;
static std::string current;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      printf("  FAIL %s: %s (line %d)\n", current.c_str(), #cond, __LINE__);    \
      failures++;                                                                \
      return;                                                                    \
    }                                                                            \
  } while (0)

static bool lastStartsWith(const World& w, const char* s) {
  return strstr(w.eeprom.last, s) != nullptr;
}

// --- Normal wiring: the Teensy is on the buck rail and loses power. --------
static void on_rail_pass() {
  World w;
  w.run(2000);                    // Teensy boots, idles
  w.press();
  w.run(3000);
  CHECK(w.eeprom.pending == 1);   // saved before the power went
  CHECK(w.accel->sleeps == 1);
  w.run(30000);                   // power-down, 15 s sleep, wake, reboot, verify
  CHECK(w.teensy_boots == 2);
  CHECK(w.eeprom.pending == 0);
  CHECK(w.eeprom.passes == 1 && w.eeprom.fails == 0);
  CHECK(lastStartsWith(w, "PASS: Teensy lost power and came back"));
  CHECK(lastStartsWith(w, "length not measured"));
}

static void on_rail_with_rtc_measures_length() {
  World w;
  w.rtc_battery = true;
  w.run(2000);
  w.press();
  w.run(35000);
  CHECK(w.eeprom.passes == 1);
  CHECK(lastStartsWith(w, "off ~2"));   // ~21-22 s: 6 s power-down + 15 s + boot
}

static void on_rail_watchdog_reset_fails() {
  AccelOptions o;
  o.reset_cause_after_wake = "watchdog";
  World w(o);
  w.run(2000);
  w.press();
  w.run(35000);
  CHECK(w.eeprom.fails == 1);
  CHECK(lastStartsWith(w, "Reset Cause 'watchdog'"));
}

static void on_rail_early_wake_caught_by_rtc() {
  AccelOptions o;
  o.wake_early = true;   // 7.5 s instead of 15 s
  o.power_down_ms = 500;
  World w(o);
  w.rtc_battery = true;
  w.run(2000);
  w.press();
  w.run(30000);
  CHECK(w.eeprom.fails == 1);
  CHECK(lastStartsWith(w, "less than 15 s"));
}

// --- Bench: the Teensy is powered from USB and stays up. -------------------
static void usb_powered_pass_measures_live() {
  World w;
  w.teensy_on_rail = false;
  w.run(2000);
  w.press();
  w.run(30000);
  CHECK(w.teensy_boots == 1);   // never lost power
  CHECK(w.eeprom.passes == 1);
  CHECK(lastStartsWith(w, "Teensy stayed powered; slept 2"));  // 6 s + 15 s + 1.5 s boot
  CHECK(w.logged("not on the buck rail"));
}

static void usb_powered_early_wake_fails() {
  AccelOptions o;
  o.wake_early = true;
  o.power_down_ms = 500;
  World w(o);
  w.teensy_on_rail = false;
  w.run(2000);
  w.press();
  w.run(30000);
  CHECK(w.eeprom.fails == 1);
  CHECK(lastStartsWith(w, "before 15 s"));
}

// --- Refusals: nothing must sleep. -----------------------------------------
static void old_firmware_is_not_put_to_sleep() {
  AccelOptions o;
  o.version = "7.12.0";
  World w(o);
  w.run(2000);
  w.press();
  w.run(5000);
  CHECK(w.accel->sleeps == 0);
  CHECK(w.eeprom.fails == 1);
  CHECK(lastStartsWith(w, "older than 7.12.1"));
}

static void no_acknowledgement_fails() {
  AccelOptions o;
  o.ack = false;
  World w(o);
  w.teensy_on_rail = false;
  w.run(2000);
  w.press();
  w.run(5000);
  CHECK(w.eeprom.fails == 1);
  CHECK(lastStartsWith(w, "no 'Sleep for 15 s'"));
}

static void dfu_refusal_reported() {
  AccelOptions o;
  o.refuse = true;
  World w(o);
  w.run(2000);
  w.press();
  w.run(5000);
  CHECK(w.eeprom.fails == 1);
  CHECK(lastStartsWith(w, "refused, DFU session active"));
}

// --- Button handling. -------------------------------------------------------
static void bounce_and_hold_start_one_test() {
  World w;
  w.teensy_on_rail = false;
  w.run(2000);
  for (int i = 0; i < 5; i++) {  // 5 ms chatter, then a long hold
    w.button = true;  w.run(5);
    w.button = false; w.run(5);
  }
  w.button = true;
  w.run(5000);                    // held through the request
  w.button = false;
  w.run(30000);
  CHECK(w.accel->sleeps == 1);
  CHECK(w.eeprom.seq == 1);
  CHECK(w.eeprom.passes == 1);
}

static void two_tests_in_a_row_count_up() {
  World w;
  w.run(2000);
  w.press();
  w.run(35000);
  w.press();
  w.run(35000);
  CHECK(w.accel->sleeps == 2);
  CHECK(w.eeprom.seq == 2 && w.eeprom.passes == 2);
  CHECK(w.teensy_boots == 3);
}

int main(int argc, char** argv) {
  bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  (void)verbose;
  std::vector<std::pair<const char*, std::function<void()>>> tests = {
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
  for (auto& t : tests) {
    current = t.first;
    int before = failures;
    t.second();
    if (failures == before) printf("  ok   %s\n", t.first);
  }
  printf("%zu scenarios, %d failed\n", tests.size(), failures);
  return failures ? 1 : 0;
}
