/*
 * PtsaSleepTest -- press the Teensy's button to put the PTSA accelerator to
 * sleep for 15 s, then check that it woke up properly.
 *
 * Board: Teensy 4.1 (Arduino IDE + Teensyduino). USB Type: Serial.
 * Open the Serial Monitor at any baud rate to see the log and results.
 *
 * Wiring: as on the Main Board.
 *   Serial1  pins 0 (RX1) / 1 (TX1)  ->  accelerator console, 115200 8N1
 *   pin 3    button to GND (INPUT_PULLUP, active low)
 *   pin 13   LED: on while a test runs
 *
 * All the test logic is C, in src/sleep_test_core.c, so it also runs in the PC
 * simulation (test/). This file only fills in the HAL. Arduino compiles a
 * .ino as C++ and Serial/Serial1 are C++ objects, so those calls cannot be
 * avoided here -- everything else is plain C.
 */
#include <avr/eeprom.h>
#include <TimeLib.h>

#include "src/sleep_test_core.h"

#define BUTTON_PIN  3
#define LED_PIN     13
#define EEPROM_ADDR 0

static uint32_t hal_ms(void *ctx) { (void)ctx; return millis(); }

static int hal_uart_read(void *ctx)
{
    (void)ctx;
    return Serial1.available() ? Serial1.read() : -1;
}

static void hal_uart_write(void *ctx, const char *s) { (void)ctx; Serial1.print(s); }
static bool hal_button_down(void *ctx) { (void)ctx; return digitalRead(BUTTON_PIN) == LOW; }
static void hal_led(void *ctx, bool on) { (void)ctx; digitalWrite(LED_PIN, on ? HIGH : LOW); }
static void hal_log(void *ctx, const char *line) { (void)ctx; Serial.println(line); }

static void hal_load(void *ctx, pst_persist_t *p)
{
    (void)ctx;
    eeprom_read_block(p, (const void *)EEPROM_ADDR, sizeof *p);
}

static void hal_save(void *ctx, const pst_persist_t *p)
{
    (void)ctx;
    eeprom_write_block(p, (void *)EEPROM_ADDR, sizeof *p);
}

/* Keeps counting while the Teensy is unpowered only with a coin cell on VBAT.
 * Without one the test logic sees it restart and skips the timing check. */
static uint32_t hal_rtc(void *ctx) { (void)ctx; return Teensy3Clock.get(); }

static const pst_hal_t hal = {
    NULL,
    hal_ms,
    hal_uart_read,
    hal_uart_write,
    hal_button_down,
    hal_led,
    hal_log,
    hal_load,
    hal_save,
    hal_rtc,
};

static pst_test_t test;

void setup(void)
{
    uint32_t t;

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    Serial1.begin(115200);

    /* Give a Serial Monitor a moment to attach, but never wait for one: on the
     * accelerator's rail this boots unattended when the accelerator wakes. */
    t = millis();
    while (!Serial && millis() - t < 1500) {
    }

    pst_init(&test, &hal);
    pst_begin(&test);
}

void loop(void)
{
    pst_poll(&test);

    /* Type 'c' in the Serial Monitor to clear the stored results. */
    if (Serial.available() && Serial.read() == 'c') {
        pst_persist_t blank;

        memset(&blank, 0, sizeof blank);
        hal_save(NULL, &blank);
        Serial.println("PST: results cleared");
        pst_init(&test, &hal);
        pst_begin(&test);
    }
}
