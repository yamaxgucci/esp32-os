# A Sega Mega Drive pad on an ArgonOS board

**English** | [Русский](sega-pad.ru.md)

How to connect a real Mega Drive pad, 3- or 6-button, to an ArgonOS board:
the parts, what to solder where, how to set the system up and how to check
it. Built and tested on a CYD (ESP32-2432S028R) with a 6-button pad and a red
PCF8574 module. In the Mega Drive emulator (MD) every button works, X, Y, Z
and Mode included. Running the games themselves is in
[mega-drive.md](mega-drive.md).

- [1. How it works](#1-how-it-works)
- [2. Parts](#2-parts)
- [3. Soldering](#3-soldering)
- [4. Connecting to the board](#4-connecting-to-the-board)
- [5. Setting up the system](#5-setting-up-the-system)
- [6. Checking](#6-checking)
- [7. Buttons](#7-buttons)
- [8. When it does not work](#8-when-it-does-not-work)

## 1. How it works

A Mega Drive pad has seven signal lines: six outputs (the d-pad and the
buttons) and one input, SELECT, with which the console picks which buttons
the pad shows at the moment. The CYD has only three free pins, so the pad
goes through a **PCF8574 port expander**: a chip on the I2C bus that needs
only two wires (SDA and SCL) and gives eight lines.

The `SEGAPAD.SYS` driver reads the pad through the PCF8574 every 8 ms and
turns the presses into ordinary keys. So the pad works in any program that a
keyboard is enough for, not only in MD.

**A 6-button pad** answers in turns: the console toggles SELECT several
times, and at a certain step the pad shows X, Y, Z and Mode. If SELECT stays
still for too long, the pad starts counting again. Sega's figure is ~1.5 ms
at 5 V, but the pad tested here, at 3.3 V, started again after only ~400 µs.
That is where the two requirements below come from: a 400 kHz bus and recent
firmware.

## 2. Parts

| What | Why | Notes |
|---|---|---|
| A Mega Drive / Genesis pad, 3- or 6-button | | Tested with a 6-button one |
| A PCF8574 module | the port expander | Tested: **a red module with three DIP address switches**. Its bus pull-ups and capacitor are already on it. A PCF8574A works too |
| A **male DB9** connector, solder type | the pad's plug goes into it | The same as on the console. If you do not mind cutting the pad's cable, you can do without it and solder the pad's wires straight to the module |
| Wires | | For the CYD's CN1 you need a matching cable. On the CYD it is a small 4-pin connector, and a cable for it usually comes with the board |
| Soldering iron, multimeter | | The multimeter is for tracing the wires if you cut the pad's cable |

A bare PCF8574 chip (DIP-16) works too, but it needs two 4.7–10 kΩ resistors
and a 100 nF capacitor (see [section 3](#3-soldering)).

## 3. Soldering

### PCF8574 module → DB9 connector

The module has two identical VCC/GND/SDA/SCL headers: one is the input, the
one with an INT pin, the other is for chaining modules. P0–P7 have pins of
their own.

| Module | DB9 | Pad signal |
|---|---|---|
| P0 | 1 | up |
| P1 | 2 | down |
| P2 | 3 | left |
| P3 | 4 | right |
| P4 | 6 | A / B |
| P5 | 9 | Start / C |
| P6 | 7 | SELECT (the only output to the pad) |
| P7 | — | not connected |
| VCC | 5 | pad power |
| GND | 8 | ground |

**DB9 pin numbers** are usually moulded next to the solder cups. Looking at
the male pins from the front, wide side up, the top row is 1, 2, 3, 4, 5 from
left to right and the bottom row 6, 7, 8, 9. From the solder side the order
is mirrored: go by the moulded numbers.

If you cut the pad's cable instead of using a connector: wire colours differ
from one pad to another, so trace each wire with a multimeter from the plug's
pin.

**The DIP address switches** can be in any position: the driver tries every
address 0x20–0x27 (0x38–0x3F for a PCF8574A) by itself.

### A bare chip instead of a module

| PCF8574 (DIP-16) | Where |
|---|---|
| 16 VDD | 3.3 V |
| 8 VSS | ground |
| 15 SDA | the board's SDA |
| 14 SCL | the board's SCL |
| 1, 2, 3 (A0–A2) | to ground, address 0x20 |
| 4–7 (P0–P3) | DB9 1–4 |
| 9, 10, 11 (P4–P6) | DB9 6, 9, 7 |
| 12 P7, 13 INT | not connected |

And you must add:
- 4.7–10 kΩ resistors from SDA to 3.3 V and from SCL to 3.3 V;
- a 100 nF capacitor between pins 16 and 8, as close to the chip as you can.

### Power: 3.3 V only

The module and the pad run on **3.3 V, not 5 V**. The pad's outputs go
straight into the PCF8574, and its SDA and SCL go to ESP32 pins that do not
tolerate 5 V.

## 4. Connecting to the board

### The CYD

Header **CN1**: GND, IO22, IO21 and 3.3 V in a row.

| Module | CYD CN1 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | IO21 |
| SCL | IO22 |
| INT | not connected |

Connect the module's input header, the one with INT. There must not be a
second device at the same address on the bus. For example, the ESP-07
stand-in also answers at 0x20, so unplug it meanwhile.

### Another board

Any two free pins will do. Describe them in `C:\SYSTEM.CFG`
([section 5](#5-setting-up-the-system)). The driver has been tested on the
CYD only.

## 5. Setting up the system

### Bus speed: 400 kHz

At 100 kHz one step of a read takes about 400 µs, exactly as long as a
6-button pad keeps its count at 3.3 V. The result is a pad that answers as a
3-button one, and X, Y, Z and Mode do nothing. At 400 kHz the PCF8574 works
without errors (2000 reads of 2000), although its datasheet says 100 kHz.

- **A CYD built from recent sources:** the board pack already says 400.
- **A CYD from the shop, or built earlier:** add this to the end of
  `C:\SYSTEM.CFG` from the board's console:

  ```
  echo [i2c0] >> c:\system.cfg
  echo khz = 400 >> c:\system.cfg
  ```

  When a section is repeated, the last value wins. Then restart the board.
- **Another board:** describe the whole bus, with your pins:

  ```
  [i2c0]
  sda = 21
  scl = 22
  khz = 400
  pullups = true
  ```

### Firmware: ABI 0.78 or newer

From ABI 0.78 the driver reads the pad in two I2C transactions instead of
eight, with no gap of the program's between the steps (the kernel's
`i2c_steps` call). On older firmware the driver reads the old way. That works
too, but there 5–10% of the reads during a game lose X, Y, Z and Mode, and
the driver takes them from the read before. `ver` shows the version, on the
`ABI 0.xx` line.

Firmware with ABI 0.78 is built from source ([building.md](building.md)).
For the CYD use the **`esp32-cyd`** target, without the radio: with Wi-Fi
plus the pad's driver a Mega Drive game does not have enough memory
([mega-drive.md, section 6](mega-drive.md#6-memory-making-the-game-start)).

### The driver

- **From the shop:** the driver comes with the MD program, as
  `A:\APPS\MD\SEGAPAD.SYS`.
- **From source:** `.\argon.cmd apps --only SEGAPAD.SYS` builds it for the
  current target's chip (`argon target`) into `build\apps\SEGAPAD.SYS`. A
  driver built for the ESP32-S3 crashes on an ESP32 (the CYD), and the other
  way round.

Load it:

```
drv load a:\apps\md\segapad.sys        until the next restart
drv install a:\apps\md\segapad.sys     at every start
```

`drv install` copies the driver to `C:\drv` and adds it to `C:\SYSTEM.CFG`.

## 6. Checking

1. **The bus sees the expander:**

   ```
   io i2c 0
   ```

   It should say `0x20 answers` (or another address, set by the switches;
   0x38 and up for a PCF8574A).
2. **The driver found it:** when it loads, the driver prints
   `SEGAPAD: PCF8574 on i2c0 at 0x20, read every 8 ms`.
3. **The kind of pad:** the journal (`log`) should have a line
   `SEGAPAD: a 6-button pad` or `a 3-button pad`.
4. **No reads are lost:** every 10 seconds the driver writes a tally to the
   journal, for example:

   ```
   SEGAPAD: 1112 reads, 0 with a step over 1500 us, the longest 1636 us,
   1 asked again and 0 without the 6-button step; 0 bus errors so far
   ```

   - `asked again`: reads done a second time because a 6-button pad
     answered as a 3-button one. This is normal.
   - `without the 6-button step`: reads that failed even after that. Should
     be 0 or nearly 0.
   - `bus errors`: bus errors, should be 0 too.
5. **The buttons:** start MD and a game. The Lost Vikings uses every button
   of a 6-button pad, X, Y, Z and Mode included.

## 7. Buttons

The driver turns the pad's buttons into keys:

| Button | Key |
|---|---|
| d-pad | arrows |
| A B C | Z X C |
| Start | Enter |
| X Y Z | A S D |
| Mode | M |
| Start and Mode together | Esc, MD's pause menu |

- Mode goes to the game: The Lost Vikings, for one, switches Vikings with it.
- MD's pause menu opens with Start and Mode together. A 3-button pad has no
  Mode, so there the menu opens with a touch on the screen.
- Start goes out 80 ms late: the driver waits to see whether Mode comes with
  it. A short press of Start still gets through.

## 8. When it does not work

| What you see | What to check |
|---|---|
| `io i2c 0` finds nothing | The module's power (3.3 V on VCC), SDA and SCL not swapped, the pull-ups (on a bare chip). The bus is described in `[i2c0]` |
| `SEGAPAD: no PCF8574 on i2c0 at 0x20..0x27 or 0x38..0x3f` | The same. The module is connected by its input header |
| `a 3-button pad` although the pad has 6 buttons | The bus is at 100 kHz: set 400 ([section 5](#5-setting-up-the-system)). Also, a 6-button pad can start in 3-button mode if Mode is held when it is powered up |
| The pad is found, but the buttons are wrong | P0–P6 and the DB9 pins are mixed up. Most often it is the mirrored order on the solder side |
| Everything pressed at once, or nothing | The pad is not plugged in, or there is no power on DB9 pin 5 |
| X, Y, Z sometimes come late, and the journal shows many `without the 6-button step` | Firmware older than ABI 0.78 ([section 5](#5-setting-up-the-system)) |
| After leaving the pause menu the game "froze" | An old MD: the Start button got through to the game, and the game paused itself. Fixed in MD after 9 November 2026 |
| `md: short of memory` with the pad driver on the CYD | Firmware with Wi-Fi. You need `esp32-cyd` without the radio ([mega-drive.md](mega-drive.md#6-memory-making-the-game-start)) |

For experiments without a real pad there is an ESP-07 module that pretends
to be a PCF8574 with a pad, with the buttons pressed on a phone:
[firmware/esp8266-segapad](../firmware/esp8266-segapad/README.md). It needs
the bus at 50 kHz.
