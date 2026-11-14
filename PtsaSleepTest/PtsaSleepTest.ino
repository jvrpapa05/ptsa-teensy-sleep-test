// PtsaSleepTest -- press the Teensy's button to put the PTSA accelerator to
// sleep for 15 s, then check that it woke up properly.
//
// Board: Teensy 4.1 (Arduino IDE + Teensyduino). USB Type: Serial.
// Open the Serial Monitor at any baud rate to see the log and results.
//
// Wiring: as on the Main Board.
//   Serial1  pins 0 (RX1) / 1 (TX1)  ->  accelerator console, 115200 8N1
//   pin 3    button to GND (INPUT_PULLUP, active low)
//   pin 13   LED: on while a test runs
//
// The test logic is in src/sleep_test_core.cpp, which has no Arduino code so
// it can also run in the PC simulation (test/). This file only connects
// it to the hardware.
#include <EEPROM.h>
#include <TimeLib.h>

#include "src/sleep_test_core.h"

static const int BUTTON_PIN = 3;
static const int LED_PIN = 13;
static const int EEPROM_ADDR = 0;

class TeensyHal : public pst::Hal {
 public:
  uint32_t ms() override { return millis(); }
  int uartRead() override { return Serial1.available() ? Serial1.read() : -1; }
  void uartWrite(const char* s) override { Serial1.print(s); }
  bool buttonDown() override { return digitalRead(BUTTON_PIN) == LOW; }
  void led(bool on) override { digitalWrite(LED_PIN, on ? HIGH : LOW); }
  void log(const char* s) override { Serial.println(s); }
  void load(pst::Persist& p) override { EEPROM.get(EEPROM_ADDR, p); }
  void save(const pst::Persist& p) override { EEPROM.put(EEPROM_ADDR, p); }
  // Keeps counting while the Teensy is unpowered only with a coin cell on VBAT.
  // Without one the core test logic sees it restart and skips the timing check.
  uint32_t rtc() override { return Teensy3Clock.get(); }
};

static TeensyHal hal;
static pst::SleepTest test(hal);

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  Serial1.begin(115200);

  // Give a Serial Monitor a moment to attach, but never wait for one: on the
  // buck rail this boots unattended when the accelerator wakes.
  uint32_t t = millis();
  while (!Serial && millis() - t < 1500) {
  }

  test.begin();
}

void loop() {
  test.poll();

  // Type 'c' in the Serial Monitor to clear the stored results.
  if (Serial.available() && Serial.read() == 'c') {
    pst::Persist blank{};
    hal.save(blank);
    Serial.println("PST: results cleared");
    test.begin();
  }
}
