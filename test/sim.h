// sim.h -- a simulated Teensy + accelerator for testing sleep_test_core on a PC.
//
// Time is simulated in 1 ms steps, so a 15 s sleep runs in milliseconds.
// The accelerator side follows the sys_sleep contract in README.md;
// the Teensy side models what matters to the test: whether it loses power with
// the accelerator's rails (on the buck rail) or not (USB powered), its EEPROM,
// which survives, and its RTC, which survives only with a coin cell.
#ifndef SIM_H
#define SIM_H

#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../PtsaSleepTest/src/sleep_test_core.h"

struct AccelOptions {
  std::string version = "7.12.1";
  std::string reset_cause_after_wake = "software";
  bool ack = true;           // print "Sleep for N s"
  bool wake_early = false;   // bug: sleeps half the time
  bool refuse = false;       // answer like a DFU session is open
  uint32_t power_down_ms = 6000;
  uint32_t boot_ms = 1500;   // reset -> banner -> shell ready
};

struct World;

class FakeAccel {
 public:
  FakeAccel(World& w, AccelOptions o) : w_(w), o_(o) {}
  void rx(char c);           // byte from the Teensy
  void tick();               // advance one ms
  bool railsOn() const { return rails_; }
  std::deque<char> tx;       // bytes to the Teensy
  int sleeps = 0;            // how many sys_sleep commands were accepted

 private:
  void say(const std::string& s);
  void handle(const std::string& line);
  World& w_;
  AccelOptions o_;
  std::string in_;
  bool rails_ = true;
  bool shell_ = true;
  long boot_at_ = 0;         // world time of last reset
  long down_at_ = -1, wake_at_ = -1, ready_at_ = -1;
  std::string reset_cause_ = "power-on";
};

class SimHal : public pst::Hal {
 public:
  explicit SimHal(World& w) : w_(w) {}
  uint32_t ms() override;
  int uartRead() override;
  void uartWrite(const char* s) override;
  bool buttonDown() override;
  void led(bool on) override { led_ = on; }
  void log(const char* s) override;
  void load(pst::Persist& p) override;
  void save(const pst::Persist& p) override;
  uint32_t rtc() override;
  bool led_ = false;

 private:
  World& w_;
};

struct World {
  long now = 0;                       // ms
  bool teensy_on_rail = true;         // false: powered from USB
  bool rtc_battery = false;
  bool button = false;
  bool verbose = false;

  pst::Persist eeprom{};              // survives power loss
  bool eeprom_written = false;
  long teensy_boot_at = 0;
  std::vector<std::string> log;
  std::unique_ptr<FakeAccel> accel;
  std::unique_ptr<SimHal> hal;
  std::unique_ptr<pst::SleepTest> test;
  int teensy_boots = 0;

  explicit World(AccelOptions o = AccelOptions()) { accel.reset(new FakeAccel(*this, o)); }

  void bootTeensy() {
    teensy_boot_at = now;
    hal.reset(new SimHal(*this));
    test.reset(new pst::SleepTest(*hal));
    teensy_boots++;
    test->begin();
  }

  void step() {
    now++;
    accel->tick();
    bool powered = !teensy_on_rail || accel->railsOn();
    if (!powered && test) {  // the Teensy loses power
      test.reset();
      hal.reset();
    } else if (powered && !test) {
      bootTeensy();
    }
    if (test) test->poll();
  }

  void run(long ms) { for (long i = 0; i < ms; i++) step(); }

  void press(long hold_ms = 200) {
    button = true;
    run(hold_ms);
    button = false;
  }

  bool logged(const std::string& needle) const {
    for (auto& l : log) if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

#endif
