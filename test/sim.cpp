// sim.cpp -- see sim.h.
#include "sim.h"

#include <cstring>

// ---------------------------------------------------------------- accelerator
void FakeAccel::say(const std::string& s) {
  for (char c : s) tx.push_back(c);
}

void FakeAccel::rx(char c) {
  if (!rails_ || !shell_ || w_.now < ready_at_) return;  // asleep, or still booting
  if (c == '\r' || c == '\n') {
    std::string line = in_;
    in_.clear();
    say(line + "\r\n");  // echo
    handle(line);
  } else {
    in_ += c;
  }
}

void FakeAccel::handle(const std::string& line) {
  char buf[256];
  if (line.empty()) {
  } else if (line == "sys_info") {
    snprintf(buf, sizeof buf,
             "- System Information -\r\nBoard Rev.: B\r\nUptime: %ld ms\r\nApp. Version: v-%s\r\n"
             "Reset Cause: %s\r\nImage: confirmed\r\nSIM Check: none\r\n",
             w_.now - boot_at_, o_.version.c_str(), reset_cause_.c_str());
    say(buf);
  } else if (line.rfind("sys_sleep", 0) == 0) {
    long secs = -1;
    if (line.size() > 10) {
      char* end;
      secs = strtol(line.c_str() + 10, &end, 10);
      if (*end) secs = -1;
    }
    if (secs < 1 || secs > 86400) {
      say("Sleep time must be a number of seconds in 1..86400\r\n");
    } else if (o_.refuse) {
      say("sys_sleep: refused, DFU session active\r\n");
    } else {
      if (o_.ack) {
        snprintf(buf, sizeof buf, "Sleep for %ld s\r\n", secs);
        say(buf);
      }
      sleeps++;
      shell_ = false;
      down_at_ = w_.now + o_.power_down_ms;
      wake_at_ = down_at_ + (o_.wake_early ? secs * 500 : secs * 1000);
      return;  // no prompt
    }
  } else {
    say(line + ": command not found\r\n");
  }
  say("ptsa:~$ ");
}

void FakeAccel::tick() {
  if (down_at_ >= 0 && w_.now >= down_at_) {
    rails_ = false;  // buck + e-fuse off; the Teensy on the rail goes dark
    down_at_ = -1;
  }
  if (wake_at_ >= 0 && w_.now >= wake_at_) {
    // Cold reset: rails back at the power-on defaults.
    wake_at_ = -1;
    rails_ = true;
    shell_ = true;
    boot_at_ = w_.now;
    reset_cause_ = o_.reset_cause_after_wake;
    ready_at_ = w_.now + o_.boot_ms;
    tx.clear();
  }
  if (ready_at_ >= 0 && w_.now == ready_at_) {
    char buf[96];
    snprintf(buf, sizeof buf, "*** Booting Power Tree Sim Adapter (PTSA) v%s-0000000 ***\r\n",
             o_.version.c_str());
    say(buf);
    say("ptsa:~$ ");
  }
}

// ---------------------------------------------------------------- Teensy HAL
uint32_t SimHal::ms() { return (uint32_t)(w_.now - w_.teensy_boot_at); }

int SimHal::uartRead() {
  if (w_.accel->tx.empty()) return -1;
  char c = w_.accel->tx.front();
  w_.accel->tx.pop_front();
  return (unsigned char)c;
}

void SimHal::uartWrite(const char* s) {
  for (; *s; s++) w_.accel->rx(*s);
}

bool SimHal::buttonDown() { return w_.button; }

void SimHal::log(const char* s) {
  w_.log.push_back(s);
  if (w_.verbose) printf("    [%7ld ms] %s\n", w_.now, s);
}

void SimHal::load(pst::Persist& p) { p = w_.eeprom; }

void SimHal::save(const pst::Persist& p) {
  w_.eeprom = p;
  w_.eeprom_written = true;
}

// With a coin cell the RTC counts through power loss. Without one it restarts
// at the Teensy core's default date, which is earlier than any saved value.
uint32_t SimHal::rtc() {
  const uint32_t base = 1794700000;  // 2026-11
  if (w_.rtc_battery) return base + (uint32_t)(w_.now / 1000);
  return 1546300800 + (uint32_t)((w_.now - w_.teensy_boot_at) / 1000);  // 2019-01-01 + uptime
}
