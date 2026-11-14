// sleep_test_core.cpp -- see sleep_test_core.h.
#include "sleep_test_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace pst {

static const char PROMPT[] = "ptsa:~$";
static const char BANNER[] = "*** Booting Power Tree Sim Adapter (PTSA)";

// ---------------------------------------------------------------- lifecycle
void SleepTest::begin() {
  hal_.load(p_);
  if (p_.magic != PERSIST_MAGIC) {
    memset(&p_, 0, sizeof p_);
    p_.magic = PERSIST_MAGIC;
    strncpy(p_.last, "none yet", sizeof p_.last - 1);
    hal_.save(p_);
  }

  char line[200];
  snprintf(line, sizeof line, "PST: sys_sleep button test. passes=%lu fails=%lu last: %s",
           (unsigned long)p_.passes, (unsigned long)p_.fails, p_.last);
  hal_.log(line);

  if (p_.pending) {
    hal_.log("PST: test #pending from before power loss -- checking the accelerator woke properly");
    // The accelerator is booting at the same moment we are: MCUboot, then its
    // shell. Give it a generous window.
    startSync(BOOT_MAX_MS + 10000);
    enter(BOOT_SYNC);
  } else {
    hal_.log("PST: press the button (pin 3) to put the accelerator to sleep for 15 s");
    enter(IDLE);
  }
}

void SleepTest::enter(Phase p) {
  phase_ = p;
  phase_ms_ = hal_.ms();
  hal_.led(p != IDLE);
}

void SleepTest::finish(bool pass, const char* fmt, ...) {
  char why[104];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, sizeof why, fmt, ap);
  va_end(ap);

  if (pass) p_.passes++; else p_.fails++;
  p_.pending = 0;
  snprintf(p_.last, sizeof p_.last, "#%lu %s: %s", (unsigned long)p_.seq, pass ? "PASS" : "FAIL", why);
  hal_.save(p_);

  char line[200];
  snprintf(line, sizeof line, "PST: RESULT %s (passes=%lu fails=%lu)", p_.last,
           (unsigned long)p_.passes, (unsigned long)p_.fails);
  hal_.log(line);
  enter(IDLE);
}

// ---------------------------------------------------------------- receive
void SleepTest::feed(char c) {
  if (c == '\r') return;
  if (c != '\n' && curlen_ < LINE_LEN - 1) cur_[curlen_++] = c;
  cur_[curlen_] = 0;

  bool is_prompt = curlen_ >= sizeof PROMPT - 1 &&
                   strcmp(cur_ + curlen_ - (sizeof PROMPT - 1), PROMPT) == 0;
  if (c != '\n' && !is_prompt) return;

  if (is_prompt) {
    cur_[curlen_ - (sizeof PROMPT - 1)] = 0;  // text before the prompt, if any
    prompt_ = true;
  }
  if (cur_[0]) {
    if (strncmp(cur_, BANNER, sizeof BANNER - 1) == 0) {
      banner_ = true;
      banner_ms_ = hal_.ms();
    }
    char want[24];
    snprintf(want, sizeof want, "Sleep for %lu s", (unsigned long)SLEEP_S);
    if (strstr(cur_, want)) ack_ = true;
    if (nlines_ < MAX_LINES) {
      snprintf(lines_[nlines_], LINE_LEN, "%s", cur_);
      nlines_++;
    }
  }
  curlen_ = 0;
  cur_[0] = 0;
}

void SleepTest::startCommand(const char* cmd, uint32_t timeout_ms) {
  nlines_ = 0;
  prompt_ = false;
  syncing_ = false;
  cmd_ = CMD_BUSY;
  cmd_start_ = hal_.ms();
  cmd_timeout_ = timeout_ms;
  hal_.uartWrite(cmd);
  hal_.uartWrite("\r");  // the accelerator's line ending, as accel.c sends it
}

// Send bare CRs until a prompt comes back: the accelerator may still be booting.
void SleepTest::startSync(uint32_t timeout_ms) {
  startCommand("", timeout_ms);
  syncing_ = true;
  last_cr_ = hal_.ms();
}

SleepTest::CmdState SleepTest::cmdPoll() {
  if (cmd_ != CMD_BUSY) return cmd_;
  if (prompt_) return cmd_ = CMD_DONE;
  uint32_t now = hal_.ms();
  if (syncing_ && now - last_cr_ >= 500) {
    hal_.uartWrite("\r");
    last_cr_ = now;
  }
  if (now - cmd_start_ >= cmd_timeout_) return cmd_ = CMD_TIMEOUT;
  return CMD_BUSY;
}

// ---------------------------------------------------------------- checks
bool SleepTest::parseInfo(const char* const* lines, size_t n, Info& out) {
  memset(&out, 0, sizeof out);
  out.uptime_ms = -1;
  bool have_ver = false;
  for (size_t i = 0; i < n; i++) {
    const char* l = lines[i];
    const char* v;
    if ((v = strstr(l, "App. Version:"))) {
      const char* d = v + 13;
      while (*d && (*d < '0' || *d > '9')) d++;
      have_ver = sscanf(d, "%d.%d.%d", &out.major, &out.minor, &out.patch) == 3;
    } else if ((v = strstr(l, "Uptime:"))) {
      out.uptime_ms = strtol(v + 7, nullptr, 10);
    } else if ((v = strstr(l, "Reset Cause:"))) {
      v += 12;
      while (*v == ' ') v++;
      strncpy(out.reset_cause, v, sizeof out.reset_cause - 1);
    } else if ((v = strstr(l, "Board Rev.:"))) {
      v += 11;
      while (*v == ' ') v++;
      strncpy(out.board_rev, v, sizeof out.board_rev - 1);
    }
  }
  out.ok = have_ver && out.uptime_ms >= 0;
  return out.ok;
}

bool SleepTest::versionOk(const Info& i) {
  if (i.major != MIN_MAJOR) return i.major > MIN_MAJOR;
  if (i.minor != MIN_MINOR) return i.minor > MIN_MINOR;
  return i.patch >= MIN_PATCH;
}

bool SleepTest::checkWoke(const Info& i, char* why, size_t len) {
  if (!i.ok) { snprintf(why, len, "sys_info unreadable after wake"); return false; }
  if (strcmp(i.reset_cause, "software") != 0) {
    snprintf(why, len, "Reset Cause '%s', expected 'software'", i.reset_cause);
    return false;
  }
  if (i.uptime_ms > 60000) {
    snprintf(why, len, "accelerator uptime %ld ms: it did not just reboot", i.uptime_ms);
    return false;
  }
  return true;
}

bool SleepTest::buttonPressedEdge() {
  bool raw = hal_.buttonDown();
  uint32_t now = hal_.ms();
  if (raw != btn_last_) {
    btn_last_ = raw;
    btn_change_ms_ = now;
  }
  if (now - btn_change_ms_ >= 50 && raw != btn_stable_) {
    btn_stable_ = raw;
    return raw;  // edge to pressed
  }
  return false;
}

static const char* const* asPtrs(char (*lines)[96], size_t n, const char** buf) {
  for (size_t i = 0; i < n; i++) buf[i] = lines[i];
  return buf;
}

// ---------------------------------------------------------------- main loop
void SleepTest::poll() {
  int c;
  while ((c = hal_.uartRead()) >= 0) feed((char)c);

  bool pressed = buttonPressedEdge();
  const char* ptrs[MAX_LINES];
  char why[104];
  Info info;

  switch (phase_) {
    case IDLE:
      if (pressed) {
        p_.seq++;
        char line[64];
        snprintf(line, sizeof line, "PST: test #%lu: button pressed", (unsigned long)p_.seq);
        hal_.log(line);
        startSync(3000);
        enter(REQ_SYNC);
      }
      break;

    case REQ_SYNC:
      if (cmdPoll() == CMD_TIMEOUT) { finish(false, "accelerator shell not answering on Serial1"); break; }
      if (cmd_ == CMD_DONE) { startCommand("sys_info", 3000); enter(REQ_INFO); }
      break;

    case REQ_INFO:
      if (cmdPoll() == CMD_TIMEOUT) { finish(false, "no reply to sys_info"); break; }
      if (cmd_ != CMD_DONE) break;
      if (!parseInfo(asPtrs(lines_, nlines_, ptrs), nlines_, info)) { finish(false, "sys_info unreadable"); break; }
      if (!versionOk(info)) {
        finish(false, "firmware %d.%d.%d older than %d.%d.%d, not sleeping", info.major, info.minor,
               info.patch, MIN_MAJOR, MIN_MINOR, MIN_PATCH);
        break;
      }
      {
        char cmd[24];
        snprintf(cmd, sizeof cmd, "sys_sleep %lu", (unsigned long)SLEEP_S);
        ack_ = false;
        banner_ = false;
        startCommand(cmd, 3000);
        enter(REQ_ACK);
      }
      break;

    case REQ_ACK:
      if (ack_) {
        ack_ms_ = hal_.ms();
        // Saved NOW: within a few seconds this Teensy may have no power.
        p_.pending = 1;
        p_.rtc_before = hal_.rtc();
        hal_.save(p_);
        hal_.log("PST: accelerator acknowledged 'Sleep for 15 s' -- power should drop within ~11 s");
        announced_still_powered_ = false;
        enter(ASLEEP);
      } else if (prompt_) {
        finish(false, "sys_sleep refused: %s", nlines_ > 1 ? lines_[nlines_ - 1] : "(no text)");
      } else if (hal_.ms() - cmd_start_ >= 3000) {
        finish(false, "no 'Sleep for 15 s' acknowledgement within 3 s");
      }
      break;

    case ASLEEP: {
      uint32_t since = hal_.ms() - ack_ms_;
      if (!announced_still_powered_ && since > POWER_DOWN_MAX_MS + 1000) {
        announced_still_powered_ = true;
        hal_.log("PST: still powered after the power-down window -- Teensy is not on the buck rail "
                 "(USB power?). Measuring the sleep live instead.");
      }
      if (banner_) {
        startSync(BOOT_MAX_MS);
        enter(LIVE_SYNC);
      } else if (since > SLEEP_S * 1000 + POWER_DOWN_MAX_MS + BOOT_MAX_MS) {
        finish(false, "no wake-up banner within %lu s of the acknowledgement",
               (unsigned long)((SLEEP_S * 1000 + POWER_DOWN_MAX_MS + BOOT_MAX_MS) / 1000));
      }
      break;
    }

    case LIVE_SYNC:
      if (cmdPoll() == CMD_TIMEOUT) { finish(false, "shell did not come back after the banner"); break; }
      if (cmd_ == CMD_DONE) { startCommand("sys_info", 3000); enter(LIVE_INFO); }
      break;

    case LIVE_INFO: {
      if (cmdPoll() == CMD_TIMEOUT) { finish(false, "no reply to sys_info after wake"); break; }
      if (cmd_ != CMD_DONE) break;
      parseInfo(asPtrs(lines_, nlines_, ptrs), nlines_, info);
      uint32_t slept = banner_ms_ - ack_ms_;
      if (slept < SLEEP_S * 1000) { finish(false, "woke after %lu ms, before 15 s", (unsigned long)slept); break; }
      if (!checkWoke(info, why, sizeof why)) { finish(false, "%s", why); break; }
      finish(true, "Teensy stayed powered; slept %lu.%lu s, clean software reset",
             (unsigned long)(slept / 1000), (unsigned long)(slept % 1000 / 100));
      break;
    }

    case BOOT_SYNC:
      if (cmdPoll() == CMD_TIMEOUT) { finish(false, "accelerator shell not answering after the Teensy's reboot"); break; }
      if (cmd_ == CMD_DONE) { startCommand("sys_info", 3000); enter(BOOT_INFO); }
      break;

    case BOOT_INFO: {
      if (cmdPoll() == CMD_TIMEOUT) { finish(false, "no reply to sys_info after reboot"); break; }
      if (cmd_ != CMD_DONE) break;
      parseInfo(asPtrs(lines_, nlines_, ptrs), nlines_, info);
      if (!checkWoke(info, why, sizeof why)) { finish(false, "%s", why); break; }

      // With an RTC coin cell the clock kept counting while we were off.
      uint32_t now = hal_.rtc();
      if (p_.rtc_before != 0 && now > p_.rtc_before) {
        uint32_t off_s = now - p_.rtc_before;
        if (off_s < SLEEP_S) { finish(false, "powered off only %lu s, less than 15 s", (unsigned long)off_s); break; }
        if (off_s > SLEEP_S + (POWER_DOWN_MAX_MS + BOOT_MAX_MS) / 1000 + 30) {
          finish(false, "off for %lu s, far longer than 15 s", (unsigned long)off_s);
          break;
        }
        finish(true, "Teensy lost power and came back; off ~%lu s; clean software reset", (unsigned long)off_s);
      } else {
        finish(true, "Teensy lost power and came back; clean software reset (no RTC battery, length not measured)");
      }
      break;
    }
  }
}

}  // namespace pst
