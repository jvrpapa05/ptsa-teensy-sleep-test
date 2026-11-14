/*
 * sleep_test_core.c -- see sleep_test_core.h.
 */
#include "sleep_test_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char PROMPT[] = "ptsa:~$";
static const char BANNER[] = "*** Booting Power Tree Sim Adapter (PTSA)";

#define PROMPT_LEN (sizeof PROMPT - 1)
#define BANNER_LEN (sizeof BANNER - 1)

/* ------------------------------------------------------------ HAL shortcuts */
static uint32_t now_ms(const pst_test_t *t) { return t->hal->ms(t->hal->ctx); }
static void     say(const pst_test_t *t, const char *s) { t->hal->uart_write(t->hal->ctx, s); }
static void     log_line(const pst_test_t *t, const char *s) { t->hal->log(t->hal->ctx, s); }
static void     save(const pst_test_t *t) { t->hal->save(t->hal->ctx, &t->p); }

/* --------------------------------------------------------------- lifecycle */
static void enter(pst_test_t *t, pst_phase_t p)
{
    t->phase = p;
    t->hal->led(t->hal->ctx, p != PST_IDLE);
}

void pst_init(pst_test_t *t, const pst_hal_t *hal)
{
    memset(t, 0, sizeof *t);
    t->hal   = hal;
    t->phase = PST_IDLE;
}

void pst_begin(pst_test_t *t)
{
    char line[200];

    t->hal->load(t->hal->ctx, &t->p);
    if (t->p.magic != PST_PERSIST_MAGIC) {
        memset(&t->p, 0, sizeof t->p);
        t->p.magic = PST_PERSIST_MAGIC;
        snprintf(t->p.last, sizeof t->p.last, "none yet");
        save(t);
    }

    snprintf(line, sizeof line, "PST: sys_sleep button test. passes=%lu fails=%lu last: %s",
             (unsigned long)t->p.passes, (unsigned long)t->p.fails, t->p.last);
    log_line(t, line);

    if (t->p.pending) {
        log_line(t, "PST: test pending from before power loss -- checking the accelerator woke properly");
        /* The accelerator is booting at the same moment we are: MCUboot, then
         * its shell. Give it a generous window. */
        t->syncing = false;
        enter(t, PST_BOOT_SYNC);
        /* started in the first poll, once the HAL is fully running */
        t->cmd = PST_CMD_NONE;
    } else {
        log_line(t, "PST: press the button (pin 3) to put the accelerator to sleep for 15 s");
        enter(t, PST_IDLE);
    }
}

static void finish(pst_test_t *t, bool pass, const char *fmt, ...)
{
    char    why[104];
    char    line[200];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);

    if (pass) {
        t->p.passes++;
    } else {
        t->p.fails++;
    }
    t->p.pending = 0;
    snprintf(t->p.last, sizeof t->p.last, "#%lu %s: %s", (unsigned long)t->p.seq, pass ? "PASS" : "FAIL", why);
    save(t);

    snprintf(line, sizeof line, "PST: RESULT %s (passes=%lu fails=%lu)", t->p.last,
             (unsigned long)t->p.passes, (unsigned long)t->p.fails);
    log_line(t, line);
    enter(t, PST_IDLE);
}

/* ----------------------------------------------------------------- receive */
static void feed(pst_test_t *t, char c)
{
    char want[24];
    bool is_prompt;

    if (c == '\r') {
        return;
    }
    if (c != '\n' && t->curlen < PST_LINE_LEN - 1) {
        t->cur[t->curlen++] = c;
    }
    t->cur[t->curlen] = '\0';

    is_prompt = t->curlen >= PROMPT_LEN && strcmp(t->cur + t->curlen - PROMPT_LEN, PROMPT) == 0;
    if (c != '\n' && !is_prompt) {
        return;
    }

    if (is_prompt) {
        t->cur[t->curlen - PROMPT_LEN] = '\0'; /* text before the prompt, if any */
        t->prompt = true;
    }
    if (t->cur[0] != '\0') {
        if (strncmp(t->cur, BANNER, BANNER_LEN) == 0) {
            t->banner    = true;
            t->banner_ms = now_ms(t);
        }
        snprintf(want, sizeof want, "Sleep for %lu s", (unsigned long)PST_SLEEP_S);
        if (strstr(t->cur, want) != NULL) {
            t->ack = true;
        }
        if (t->nlines < PST_MAX_LINES) {
            snprintf(t->lines[t->nlines], PST_LINE_LEN, "%s", t->cur);
            t->nlines++;
        }
    }
    t->curlen = 0;
    t->cur[0] = '\0';
}

static void start_command(pst_test_t *t, const char *cmd, uint32_t timeout_ms)
{
    t->nlines      = 0;
    t->prompt      = false;
    t->syncing     = false;
    t->cmd         = PST_CMD_BUSY;
    t->cmd_start   = now_ms(t);
    t->cmd_timeout = timeout_ms;
    say(t, cmd);
    say(t, "\r"); /* the accelerator's line ending, as the Main Board firmware sends it */
}

/* Send bare CRs until a prompt comes back: the accelerator may still be booting. */
static void start_sync(pst_test_t *t, uint32_t timeout_ms)
{
    start_command(t, "", timeout_ms);
    t->syncing = true;
    t->last_cr = now_ms(t);
}

static pst_cmd_t cmd_poll(pst_test_t *t)
{
    uint32_t now;

    if (t->cmd != PST_CMD_BUSY) {
        return t->cmd;
    }
    if (t->prompt) {
        return t->cmd = PST_CMD_DONE;
    }
    now = now_ms(t);
    if (t->syncing && now - t->last_cr >= 500) {
        say(t, "\r");
        t->last_cr = now;
    }
    if (now - t->cmd_start >= t->cmd_timeout) {
        return t->cmd = PST_CMD_TIMEOUT;
    }
    return PST_CMD_BUSY;
}

/* ------------------------------------------------------------------ checks */
static void copy_value(char *dst, size_t len, const char *src)
{
    while (*src == ' ') {
        src++;
    }
    snprintf(dst, len, "%s", src);
}

bool pst_parse_info(const char lines[][PST_LINE_LEN], size_t n, pst_info_t *out)
{
    bool   have_ver = false;
    size_t i;

    memset(out, 0, sizeof *out);
    out->uptime_ms = -1;

    for (i = 0; i < n; i++) {
        const char *l = lines[i];
        const char *v;

        if ((v = strstr(l, "App. Version:")) != NULL) {
            const char *d = v + 13;
            while (*d != '\0' && (*d < '0' || *d > '9')) {
                d++;
            }
            have_ver = sscanf(d, "%d.%d.%d", &out->major, &out->minor, &out->patch) == 3;
        } else if ((v = strstr(l, "Uptime:")) != NULL) {
            out->uptime_ms = strtol(v + 7, NULL, 10);
        } else if ((v = strstr(l, "Reset Cause:")) != NULL) {
            copy_value(out->reset_cause, sizeof out->reset_cause, v + 12);
        } else if ((v = strstr(l, "Board Rev.:")) != NULL) {
            copy_value(out->board_rev, sizeof out->board_rev, v + 11);
        }
    }
    out->ok = have_ver && out->uptime_ms >= 0;
    return out->ok;
}

bool pst_version_ok(const pst_info_t *i)
{
    if (i->major != PST_MIN_MAJOR) {
        return i->major > PST_MIN_MAJOR;
    }
    if (i->minor != PST_MIN_MINOR) {
        return i->minor > PST_MIN_MINOR;
    }
    return i->patch >= PST_MIN_PATCH;
}

static bool check_woke(const pst_info_t *i, char *why, size_t len)
{
    if (!i->ok) {
        snprintf(why, len, "sys_info unreadable after wake");
        return false;
    }
    if (strcmp(i->reset_cause, "software") != 0) {
        snprintf(why, len, "Reset Cause '%s', expected 'software'", i->reset_cause);
        return false;
    }
    if (i->uptime_ms > 60000) {
        snprintf(why, len, "accelerator uptime %ld ms: it did not just reboot", i->uptime_ms);
        return false;
    }
    return true;
}

/* True once per press, after the input has been stable for 50 ms. */
static bool button_pressed_edge(pst_test_t *t)
{
    bool     raw = t->hal->button_down(t->hal->ctx);
    uint32_t now = now_ms(t);

    if (raw != t->btn_last) {
        t->btn_last      = raw;
        t->btn_change_ms = now;
    }
    if (now - t->btn_change_ms >= 50 && raw != t->btn_stable) {
        t->btn_stable = raw;
        return raw;
    }
    return false;
}

/* --------------------------------------------------------- phase handlers */
static void on_idle(pst_test_t *t, bool pressed)
{
    char line[64];

    if (!pressed) {
        return;
    }
    t->p.seq++;
    snprintf(line, sizeof line, "PST: test #%lu: button pressed", (unsigned long)t->p.seq);
    log_line(t, line);
    start_sync(t, 3000);
    enter(t, PST_REQ_SYNC);
}

static void on_req_info(pst_test_t *t)
{
    pst_info_t info;
    char       cmd[24];

    if (!pst_parse_info((const char (*)[PST_LINE_LEN])t->lines, t->nlines, &info)) {
        finish(t, false, "sys_info unreadable");
        return;
    }
    if (!pst_version_ok(&info)) {
        finish(t, false, "firmware %d.%d.%d older than %d.%d.%d, not sleeping", info.major, info.minor,
               info.patch, PST_MIN_MAJOR, PST_MIN_MINOR, PST_MIN_PATCH);
        return;
    }
    snprintf(cmd, sizeof cmd, "sys_sleep %lu", (unsigned long)PST_SLEEP_S);
    t->ack    = false;
    t->banner = false;
    start_command(t, cmd, 3000);
    enter(t, PST_REQ_ACK);
}

static void on_req_ack(pst_test_t *t)
{
    if (t->ack) {
        t->ack_ms = now_ms(t);
        /* Saved NOW: within a few seconds this Teensy may have no power. */
        t->p.pending    = 1;
        t->p.rtc_before = t->hal->rtc(t->hal->ctx);
        save(t);
        log_line(t, "PST: accelerator acknowledged 'Sleep for 15 s' -- power should drop within ~11 s");
        t->announced_still_powered = false;
        enter(t, PST_ASLEEP);
    } else if (t->prompt) {
        finish(t, false, "sys_sleep refused: %s", t->nlines > 1 ? t->lines[t->nlines - 1] : "(no text)");
    } else if (now_ms(t) - t->cmd_start >= 3000) {
        finish(t, false, "no 'Sleep for 15 s' acknowledgement within 3 s");
    }
}

static void on_asleep(pst_test_t *t)
{
    const uint32_t limit = PST_SLEEP_S * 1000u + PST_POWER_DOWN_MAX_MS + PST_BOOT_MAX_MS;
    uint32_t       since = now_ms(t) - t->ack_ms;

    if (!t->announced_still_powered && since > PST_POWER_DOWN_MAX_MS + 1000u) {
        t->announced_still_powered = true;
        log_line(t, "PST: still powered after the power-down window -- Teensy is not on the accelerator's "
                    "rail (USB power?). Measuring the sleep live instead.");
    }
    if (t->banner) {
        start_sync(t, PST_BOOT_MAX_MS);
        enter(t, PST_LIVE_SYNC);
    } else if (since > limit) {
        finish(t, false, "no wake-up banner within %lu s of the acknowledgement", (unsigned long)(limit / 1000u));
    }
}

static void on_live_info(pst_test_t *t)
{
    pst_info_t info;
    char       why[104];
    uint32_t   slept = t->banner_ms - t->ack_ms;

    pst_parse_info((const char (*)[PST_LINE_LEN])t->lines, t->nlines, &info);
    if (slept < PST_SLEEP_S * 1000u) {
        finish(t, false, "woke after %lu ms, before 15 s", (unsigned long)slept);
    } else if (!check_woke(&info, why, sizeof why)) {
        finish(t, false, "%s", why);
    } else {
        finish(t, true, "Teensy stayed powered; slept %lu.%lu s, clean software reset",
               (unsigned long)(slept / 1000u), (unsigned long)(slept % 1000u / 100u));
    }
}

static void on_boot_info(pst_test_t *t)
{
    pst_info_t info;
    char       why[104];
    uint32_t   now = t->hal->rtc(t->hal->ctx);

    pst_parse_info((const char (*)[PST_LINE_LEN])t->lines, t->nlines, &info);
    if (!check_woke(&info, why, sizeof why)) {
        finish(t, false, "%s", why);
        return;
    }

    /* With an RTC coin cell the clock kept counting while we were off. */
    if (t->p.rtc_before != 0 && now > t->p.rtc_before) {
        uint32_t off_s = now - t->p.rtc_before;

        if (off_s < PST_SLEEP_S) {
            finish(t, false, "powered off only %lu s, less than 15 s", (unsigned long)off_s);
        } else if (off_s > PST_SLEEP_S + (PST_POWER_DOWN_MAX_MS + PST_BOOT_MAX_MS) / 1000u + 30u) {
            finish(t, false, "off for %lu s, far longer than 15 s", (unsigned long)off_s);
        } else {
            finish(t, true, "Teensy lost power and came back; off ~%lu s; clean software reset",
                   (unsigned long)off_s);
        }
    } else {
        finish(t, true, "Teensy lost power and came back; clean software reset (no RTC battery, length not measured)");
    }
}

/* --------------------------------------------------------------- main loop */
void pst_poll(pst_test_t *t)
{
    int  c;
    bool pressed;

    while ((c = t->hal->uart_read(t->hal->ctx)) >= 0) {
        feed(t, (char)c);
    }
    pressed = button_pressed_edge(t);

    switch (t->phase) {
    case PST_IDLE:
        on_idle(t, pressed);
        break;

    case PST_REQ_SYNC:
        if (cmd_poll(t) == PST_CMD_TIMEOUT) {
            finish(t, false, "accelerator shell not answering on Serial1");
        } else if (t->cmd == PST_CMD_DONE) {
            start_command(t, "sys_info", 3000);
            enter(t, PST_REQ_INFO);
        }
        break;

    case PST_REQ_INFO:
        if (cmd_poll(t) == PST_CMD_TIMEOUT) {
            finish(t, false, "no reply to sys_info");
        } else if (t->cmd == PST_CMD_DONE) {
            on_req_info(t);
        }
        break;

    case PST_REQ_ACK:
        on_req_ack(t);
        break;

    case PST_ASLEEP:
        on_asleep(t);
        break;

    case PST_LIVE_SYNC:
        if (cmd_poll(t) == PST_CMD_TIMEOUT) {
            finish(t, false, "shell did not come back after the banner");
        } else if (t->cmd == PST_CMD_DONE) {
            start_command(t, "sys_info", 3000);
            enter(t, PST_LIVE_INFO);
        }
        break;

    case PST_LIVE_INFO:
        if (cmd_poll(t) == PST_CMD_TIMEOUT) {
            finish(t, false, "no reply to sys_info after wake");
        } else if (t->cmd == PST_CMD_DONE) {
            on_live_info(t);
        }
        break;

    case PST_BOOT_SYNC:
        if (t->cmd == PST_CMD_NONE) {
            start_sync(t, PST_BOOT_MAX_MS + 10000u);
        } else if (cmd_poll(t) == PST_CMD_TIMEOUT) {
            finish(t, false, "accelerator shell not answering after the Teensy's reboot");
        } else if (t->cmd == PST_CMD_DONE) {
            start_command(t, "sys_info", 3000);
            enter(t, PST_BOOT_INFO);
        }
        break;

    case PST_BOOT_INFO:
        if (cmd_poll(t) == PST_CMD_TIMEOUT) {
            finish(t, false, "no reply to sys_info after reboot");
        } else if (t->cmd == PST_CMD_DONE) {
            on_boot_info(t);
        }
        break;
    }
}
