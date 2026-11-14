/*
 * sim.c -- see sim.h.
 */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- accelerator */
static void accel_say(sim_t *s, const char *str)
{
    for (; *str != '\0'; str++) {
        size_t next = (s->tx_head + 1) % SIM_TX_SIZE;
        if (next == s->tx_tail) {
            return; /* full: drop, as a UART would */
        }
        s->tx[s->tx_head] = *str;
        s->tx_head        = next;
    }
}

static void accel_handle(sim_t *s, const char *line)
{
    char buf[256];

    if (line[0] == '\0') {
        /* empty line: just a prompt */
    } else if (strcmp(line, "sys_info") == 0) {
        snprintf(buf, sizeof buf,
                 "- System Information -\r\nBoard Rev.: B\r\nUptime: %ld ms\r\nApp. Version: v-%s\r\n"
                 "Reset Cause: %s\r\nImage: confirmed\r\nSIM Check: none\r\n",
                 s->now - s->boot_at, s->o.version, s->reset_cause);
        accel_say(s, buf);
    } else if (strncmp(line, "sys_sleep", 9) == 0) {
        long  secs = -1;
        char *end;

        if (line[9] == ' ' && line[10] != '\0') {
            secs = strtol(line + 10, &end, 10);
            if (*end != '\0') {
                secs = -1;
            }
        }
        if (secs < 1 || secs > 86400) {
            accel_say(s, "Sleep time must be a number of seconds in 1..86400\r\n");
        } else if (s->o.refuse) {
            accel_say(s, "sys_sleep: refused, DFU session active\r\n");
        } else {
            if (s->o.ack) {
                snprintf(buf, sizeof buf, "Sleep for %ld s\r\n", secs);
                accel_say(s, buf);
            }
            s->sleeps++;
            s->shell   = false;
            s->down_at = s->now + s->o.power_down_ms;
            s->wake_at = s->down_at + (s->o.wake_early ? secs * 500 : secs * 1000);
            return; /* no prompt */
        }
    } else {
        snprintf(buf, sizeof buf, "%s: command not found\r\n", line);
        accel_say(s, buf);
    }
    accel_say(s, "ptsa:~$ ");
}

static void accel_rx(sim_t *s, char c)
{
    if (!s->rails || !s->shell || s->now < s->ready_at) {
        return; /* asleep, or still booting */
    }
    if (c == '\r' || c == '\n') {
        s->in[s->inlen] = '\0';
        accel_say(s, s->in); /* echo */
        accel_say(s, "\r\n");
        accel_handle(s, s->in);
        s->inlen = 0;
    } else if (s->inlen < sizeof s->in - 1) {
        s->in[s->inlen++] = c;
    }
}

static void accel_tick(sim_t *s)
{
    if (s->down_at >= 0 && s->now >= s->down_at) {
        s->rails   = false; /* buck + e-fuse off; a Teensy on the rail goes dark */
        s->down_at = -1;
    }
    if (s->wake_at >= 0 && s->now >= s->wake_at) {
        /* Cold reset: rails back at the power-on defaults. */
        s->wake_at  = -1;
        s->rails    = true;
        s->shell    = true;
        s->boot_at  = s->now;
        s->ready_at = s->now + s->o.boot_ms;
        s->tx_head = s->tx_tail = 0;
        snprintf(s->reset_cause, sizeof s->reset_cause, "%s", s->o.reset_cause_after_wake);
    }
    if (s->ready_at >= 0 && s->now == s->ready_at) {
        char buf[96];
        snprintf(buf, sizeof buf, "*** Booting Power Tree Sim Adapter (PTSA) v%s-0000000 ***\r\n", s->o.version);
        accel_say(s, buf);
        accel_say(s, "ptsa:~$ ");
    }
}

/* -------------------------------------------------------------- Teensy HAL */
static uint32_t hal_ms(void *ctx)
{
    sim_t *s = ctx;
    return (uint32_t)(s->now - s->teensy_boot_at);
}

static int hal_uart_read(void *ctx)
{
    sim_t        *s = ctx;
    unsigned char c;

    if (s->tx_tail == s->tx_head) {
        return -1;
    }
    c          = (unsigned char)s->tx[s->tx_tail];
    s->tx_tail = (s->tx_tail + 1) % SIM_TX_SIZE;
    return c;
}

static void hal_uart_write(void *ctx, const char *str)
{
    for (; *str != '\0'; str++) {
        accel_rx(ctx, *str);
    }
}

static bool hal_button_down(void *ctx) { return ((sim_t *)ctx)->button; }
static void hal_led(void *ctx, bool on) { (void)ctx; (void)on; }

static void hal_log(void *ctx, const char *line)
{
    sim_t *s = ctx;
    if (s->nlog < SIM_LOG_MAX) {
        snprintf(s->log[s->nlog++], sizeof s->log[0], "%s", line);
    }
}

static void hal_load(void *ctx, pst_persist_t *p) { *p = ((sim_t *)ctx)->eeprom; }
static void hal_save(void *ctx, const pst_persist_t *p) { ((sim_t *)ctx)->eeprom = *p; }

/* With a coin cell the RTC counts through power loss. Without one it restarts
 * at the Teensy core's default date, which is earlier than any saved value. */
static uint32_t hal_rtc(void *ctx)
{
    sim_t *s = ctx;
    if (s->rtc_battery) {
        return 1794700000u + (uint32_t)(s->now / 1000); /* 2026-11 */
    }
    return 1546300800u + (uint32_t)((s->now - s->teensy_boot_at) / 1000); /* 2019-01-01 + uptime */
}

static void teensy_boot(sim_t *s)
{
    s->teensy_boot_at = s->now;
    s->teensy_alive   = true;
    s->teensy_boots++;
    pst_init(&s->test, &s->hal);
    pst_begin(&s->test);
}

/* ------------------------------------------------------------------- world */
accel_opts_t sim_default_opts(void)
{
    accel_opts_t o;

    memset(&o, 0, sizeof o);
    snprintf(o.version, sizeof o.version, "7.12.1");
    snprintf(o.reset_cause_after_wake, sizeof o.reset_cause_after_wake, "software");
    o.ack           = true;
    o.power_down_ms = 6000;
    o.boot_ms       = 1500;
    return o;
}

void sim_init(sim_t *s, const accel_opts_t *o)
{
    memset(s, 0, sizeof *s);
    s->o              = *o;
    s->teensy_on_rail = true;
    s->rails = s->shell = true;
    s->down_at = s->wake_at = s->ready_at = -1;
    snprintf(s->reset_cause, sizeof s->reset_cause, "power-on");

    s->hal.ctx         = s;
    s->hal.ms          = hal_ms;
    s->hal.uart_read   = hal_uart_read;
    s->hal.uart_write  = hal_uart_write;
    s->hal.button_down = hal_button_down;
    s->hal.led         = hal_led;
    s->hal.log         = hal_log;
    s->hal.load        = hal_load;
    s->hal.save        = hal_save;
    s->hal.rtc         = hal_rtc;
}

void sim_step(sim_t *s)
{
    bool powered;

    s->now++;
    accel_tick(s);
    powered = !s->teensy_on_rail || s->rails;
    if (!powered && s->teensy_alive) {
        s->teensy_alive = false; /* the Teensy loses power; RAM state is gone */
        memset(&s->test, 0, sizeof s->test);
    } else if (powered && !s->teensy_alive) {
        teensy_boot(s);
    }
    if (s->teensy_alive) {
        pst_poll(&s->test);
    }
}

void sim_run(sim_t *s, long ms)
{
    long i;
    for (i = 0; i < ms; i++) {
        sim_step(s);
    }
}

void sim_press(sim_t *s, long hold_ms)
{
    s->button = true;
    sim_run(s, hold_ms);
    s->button = false;
}

bool sim_logged(const sim_t *s, const char *needle)
{
    int i;
    for (i = 0; i < s->nlog; i++) {
        if (strstr(s->log[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}
