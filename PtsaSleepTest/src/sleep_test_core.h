// sleep_test_core.h -- the sys_sleep button test, as portable C++.
//
// No Arduino headers here on purpose: the same code runs on the Teensy (through
// the Hal implemented in PtsaSleepTest.ino) and on a PC against a simulated
// accelerator (test/). Everything that touches hardware goes through
// Hal.
//
// WHAT IT DOES. A press of the Teensy's button (pin 3) sends `sys_sleep 15` to
// the accelerator's shell on Serial1. Then one of two things happens:
//
//   On the buck rail (normal wiring): the accelerator powers the Teensy down
//   with everything else. Before that, the test saves a "pending" record to
//   EEPROM. At the next boot -- when the accelerator wakes and its rails come
//   back -- the test finds the record, asks the accelerator for sys_info, and
//   checks that it has just done a software reset. If the Teensy has an RTC
//   coin cell, the sleep length is measured too.
//
//   Powered from USB (bench): the Teensy stays up, sees the accelerator's boot
//   banner, and measures the sleep with its own clock.
//
// The behaviour being checked is described in README.md ("The sys_sleep contract").
#ifndef SLEEP_TEST_CORE_H
#define SLEEP_TEST_CORE_H

#include <stddef.h>
#include <stdint.h>

namespace pst {

constexpr uint32_t SLEEP_S = 15;

// sys_sleep's timer starts only after the power-down, which can take up to
// ~11 s (SIM AT check / AT+CPOF wait), and the boot adds a few seconds.
constexpr uint32_t POWER_DOWN_MAX_MS = 11000;
constexpr uint32_t BOOT_MAX_MS = 10000;

// Minimum firmware: 7.12.0 drives P0.08 high into the unpowered Teensy.
constexpr int MIN_MAJOR = 7, MIN_MINOR = 12, MIN_PATCH = 1;

constexpr uint32_t PERSIST_MAGIC = 0x50535431;  // "PST1"

// What survives the Teensy losing power. Stored in EEPROM by the sketch.
struct Persist {
  uint32_t magic;
  uint8_t pending;      // 1: a sleep was acknowledged; verify after the reboot
  uint32_t seq;         // test number, counts up
  uint32_t rtc_before;  // RTC seconds at the acknowledgement, 0 if no RTC
  uint32_t passes;
  uint32_t fails;
  char last[128];       // last result, printed at boot
};

class Hal {
 public:
  virtual ~Hal() {}
  virtual uint32_t ms() = 0;                 // milliseconds since this boot
  virtual int uartRead() = 0;                // next byte from the accelerator, -1 if none
  virtual void uartWrite(const char* s) = 0; // to the accelerator
  virtual bool buttonDown() = 0;             // true while pressed
  virtual void led(bool on) = 0;
  virtual void log(const char* s) = 0;       // one line, to USB Serial
  virtual void load(Persist& p) = 0;
  virtual void save(const Persist& p) = 0;
  virtual uint32_t rtc() = 0;                // seconds; must keep counting across power loss to be useful
};

// What the accelerator's sys_info said.
struct Info {
  bool ok;
  int major, minor, patch;
  long uptime_ms;
  char reset_cause[24];
  char board_rev[4];
};

class SleepTest {
 public:
  explicit SleepTest(Hal& hal) : hal_(hal) {}

  void begin();  // call once at boot
  void poll();   // call from loop()

  // For tests and the status line.
  enum Phase {
    BOOT_SYNC, BOOT_INFO,             // after a reboot with a pending test
    IDLE,
    REQ_SYNC, REQ_INFO, REQ_ACK,      // button pressed
    ASLEEP,                           // acknowledged; waiting to lose power or see the banner
    LIVE_SYNC, LIVE_INFO,             // still powered and the accelerator woke
  };
  Phase phase() const { return phase_; }
  const Persist& persist() const { return p_; }

  static bool parseInfo(const char* const* lines, size_t n, Info& out);
  static bool versionOk(const Info& i);

 private:
  // Line assembly: a prompt has no newline after it, so it ends a "line" too.
  void feed(char c);
  void startCommand(const char* cmd, uint32_t timeout_ms);
  void startSync(uint32_t timeout_ms);
  enum CmdState { CMD_NONE, CMD_BUSY, CMD_DONE, CMD_TIMEOUT };
  CmdState cmdPoll();

  void enter(Phase p);
  void finish(bool pass, const char* fmt, ...);
  bool checkWoke(const Info& i, char* why, size_t len);
  bool buttonPressedEdge();

  Hal& hal_;
  Persist p_{};
  Phase phase_ = IDLE;

  // Receive buffer
  static constexpr size_t MAX_LINES = 24, LINE_LEN = 96;
  char lines_[MAX_LINES][LINE_LEN];
  size_t nlines_ = 0;
  char cur_[LINE_LEN];
  size_t curlen_ = 0;
  bool prompt_ = false;
  bool banner_ = false;
  uint32_t banner_ms_ = 0;
  bool ack_ = false;

  // Command in flight
  CmdState cmd_ = CMD_NONE;
  bool syncing_ = false;
  uint32_t cmd_start_ = 0, cmd_timeout_ = 0, last_cr_ = 0;

  // Timing
  uint32_t ack_ms_ = 0;
  uint32_t phase_ms_ = 0;
  bool announced_still_powered_ = false;

  // Button debounce
  bool btn_stable_ = false, btn_last_ = false;
  uint32_t btn_change_ms_ = 0;
};

}  // namespace pst

#endif
