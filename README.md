# ptsa-teensy-sleep-test

A Teensy 4.1 Arduino sketch that tests the `sys_sleep` command of the PTSA
hardware accelerator (an nRF52840 board). Press the Teensy's button and the
accelerator sleeps for **15 seconds**. The sketch then checks that it woke up
properly.

All the test logic is plain C++, so it also runs on a PC against a **simulated
accelerator**, with no hardware needed.

## Wiring

These are the same connections the Teensy uses as the accelerator's Main Board:

| Teensy 4.1 pin | Use |
|---|---|
| 0 (RX1) / 1 (TX1), `Serial1` | Accelerator console, 115200 8N1, commands end with `\r` |
| 3 | Button to GND (`INPUT_PULLUP`, active low) |
| 13 | LED: on while a test runs |
| USB | Log and results, shown in the Serial Monitor |

## What happens when you press the button

1. The Teensy checks that the accelerator's shell answers, and that its
   firmware is **7.12.1 or newer**. If not, it refuses and nothing sleeps.
2. It sends `sys_sleep 15` and waits for `Sleep for 15 s`.
3. It saves a "test pending" record to EEPROM, because it may be about to lose
   power.
4. Then one of two things happens:

| How the Teensy is powered | What happens | How the result is checked |
|---|---|---|
| **From the accelerator's power rail** (USB unplugged) | The Teensy goes dark with the rest of the board, and boots again when the accelerator wakes. | After its reboot, it finds the EEPROM record and asks the accelerator for `sys_info`. It must show a fresh uptime and `Reset Cause: software`. With a **coin cell on VBAT**, the RTC also measures how long the Teensy was off. |
| **From USB** | USB keeps the Teensy powered, so it stays up. | It sees the accelerator's boot banner and measures the sleep with `millis()`: at least 15 s, then a clean software reset. |

Results are printed in the Serial Monitor and kept in EEPROM:

```
PST: RESULT #3 PASS: Teensy lost power and came back; off ~23 s; clean software reset (passes=3 fails=0)
```

The last result is printed again at every boot. Type `c` in the Serial Monitor
to clear the stored results. For an on-rail test, plug USB in afterwards to
read the result line.

## The sys_sleep contract

This is what the sketch expects from the accelerator:

- **Accepted:** `sys_sleep <seconds>`, 1..86400. The reply is `Sleep for <n> s`,
  with **no prompt** after it. The shell stops and the accelerator powers down
  its rails, the Main Board's included.
- **Power-down time:** up to about 11 s, because the cellular modem is shut
  down gracefully first. The timer starts after that.
- **Waking:** when the time runs out, the accelerator does a cold reset. It
  prints `*** Booting Power Tree Sim Adapter (PTSA) v<version>…` and its
  `ptsa:~$ ` prompt returns. `sys_info` then reports `Reset Cause: software`
  and a fresh `Uptime:`.
- **Refused:** out-of-range arguments, and any request made during a Bluetooth
  DFU session. A refusal prints an error and a prompt, and nothing sleeps.

## Flashing it

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) 2.x. Then, in
   *File → Preferences → Additional boards manager URLs*, add
   `https://www.pjrc.com/teensy/package_teensy_index.json`.
2. In *Boards Manager*, install **Teensy** (tested with 1.62.0).
3. Open `PtsaSleepTest/PtsaSleepTest.ino`. Choose *Tools → Board → Teensy 4.1*
   and *Tools → USB Type → Serial*, then upload.

**Linux:** install PJRC's udev rules once, or the upload can't find the board:

```bash
wget https://www.pjrc.com/teensy/00-teensy.rules && sudo cp 00-teensy.rules /etc/udev/rules.d/
```

From the command line:

```bash
arduino-cli config add board_manager.additional_urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core update-index && arduino-cli core install teensy:avr@1.62.0
arduino-cli compile --fqbn teensy:avr:teensy41 PtsaSleepTest
```

## The simulation

`PtsaSleepTest/src/sleep_test_core.cpp` has no Arduino code: all hardware
access goes through a small `Hal` class. On the PC, `test/` connects the same
code to a simulated accelerator and Teensy, with a simulated clock, EEPROM, RTC
coin cell and power rail. It runs 11 scenarios:

- passes on the rail, with and without an RTC battery
- passes on USB power
- fails if the accelerator wakes early
- fails on a watchdog reset
- refuses old firmware
- fails if there's no acknowledgement
- reports a DFU refusal
- starts only one test for a bouncing button
- counts two tests in a row

```bash
make -C test        # needs g++ and make (Linux, macOS, or Windows with MSYS2)
```

CI runs the simulation and compiles the sketch for Teensy 4.1 on every push.

## License

MIT, see [LICENSE](LICENSE).
