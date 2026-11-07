# Building ArgonOS, board by board

**English** | [Русский](building.ru.md)

This page takes you from a fresh checkout to a board showing the `C:\>` prompt.
The first part is common to every board; then each board has its own short
list of steps.

## What you need

| | |
|---|---|
| Host | **Windows 10/11.** The task runner is PowerShell; nothing else is supported as a build host today. |
| ESP-IDF | **5.5 or newer**, installed with Espressif's installer. The `argon` script finds it in the usual places, or you tell it where (below). |
| Python | 3.10 or newer, with `pip`. |
| A host C compiler | Only for `argon tests` (the host unit tests). MinGW-w64 will do. |
| A USB cable | A data cable. Many cables carry power only. |

Machine-specific paths go in `tools\local-env.ps1`, which is not committed:

```powershell
$env:ARGON_IDF_PATH       = 'D:\Espressif\esp-idf'
$env:ARGON_IDF_TOOLS_PATH = 'D:\Espressif\tools'
$env:ARGON_PYTHON         = 'D:\dev\python312\python.exe'
$env:ARGON_HOST_CC_BIN    = 'D:\dev\mingw64\bin'
```

If the file is absent, `tools\idf-env.ps1` guesses.

```powershell
git clone https://github.com/yamaxgucci/esp32-os.git
cd esp32-os
.\argon.cmd help
```

## The common recipe

Every board goes through the same five steps. Only the *target* and the
*board pack* change.

```powershell
# 1. Pick the board. This wipes build\ and reconfigures for the chip.
.\argon.cmd target <target>

# 2. Build the firmware.
.\argon.cmd build

# 3. Flash it. Use your own COM port.
.\argon.cmd flash -port COM5 -NoMonitor

# 4. Build the programs and drivers that go on the board's own flash (C:).
.\argon.cmd apps

# 5. Write C:, which holds the board's pin map, its boot configuration and the
#    drivers it loads at start.
python tools\mksysfs.py --board boards\<board-pack> --apps build\apps `
    --sdkconfig sdkconfig --flash -p COM5

# Look at it.
.\argon.cmd monitor -port COM5
```

* The console is **115200 8N1**. Use the ESP-IDF monitor (exit with `Ctrl+]`),
  PuTTY, or anything that understands ANSI escape sequences: the screen is drawn
  with them.
* **`argon target` deletes `build\` and `sdkconfig`.** Switching boards is a full
  rebuild. Run `argon target` with no argument to see what you are set to.
* `C:` is reformatted when the firmware or the partition table changes, so step 5
  comes after step 3 every time.
* Step 5 is what tells the system which pins its screen and SD card use. A board
  flashed without it boots to a prompt but has a dark screen.

What you should see:

```
ArgonOS 0.1.0 (…)  esp32s3  profile=full
… KB conventional memory, 8192 KB extended
ABI 0.xx  cores=2  app core=1
…
Type 'help' for a list of commands.
C:\>
```

The second line is the first hardware check. *Extended* is PSRAM: if it reads
zero on a board that has some, the mode is wrong (octal against quad). When
something does not come up, see
[docs/user/02-board-setup.md](user/02-board-setup.md) (Russian), section 8.

## No board: QEMU

```powershell
.\argon.cmd target esp32s3
.\argon.cmd build
.\argon.cmd run                       # console in this window; Ctrl+A then X quits
.\argon.cmd test ver mem              # scripted boot, prints the resulting screen
.\argon.cmd tests                     # host unit tests
.\argon.cmd check                     # all of it, plus every application
```

QEMU runs the ESP32-S3 target (and an ESP32-C3 one, `esp32c3-qemu`). It does not
run the C6 or the P4. Anything that depends on the real clock, real pins or a real panel needs
a board.

## Boards

| Board | Target | Board pack | Needs |
|---|---|---|---|
| [ESP32-S3-WROOM CAM (N16R8)](#esp32-s3-cam) | `esp32s3-board` | `esp32s3-cam` | 8 MB octal PSRAM |
| [ESP32 "CYD" 2432S028R / S024](#cyd) | `esp32-cyd` | `esp32-cyd` | — |
| [Waveshare ESP32-S3-Touch-LCD-2](#touch-lcd-2) | `esp32s3-lcd2` | `esp32s3-touch-lcd-2` | — |
| [Waveshare ESP32-S3-Zero](#s3-zero) | `esp32s3-zero` | `esp32s3-zero` | an I2S DAC for sound |
| [Waveshare ESP32-C6-LCD-1.47](#c6) | `esp32c6` | `esp32-c6-lcd-1.47` | — |
| [ESP32-C3 SuperMini](#c3) | `esp32c3` | `esp32c3-supermini` | — |
| [M5Stack Stamp-P4](#p4) | `esp32p4` | `m5stack-stamp-p4` | — |
| [ESP32-A1S Audio Kit](#a1s) | `esp32-a1s` | `esp32-a1s` | — |
| [USB KVM dongle (S3-CAM or T-Dongle-S3)](#kvm) | `esp32s3-kvm` | `esp32s3-kvm` / `esp32s3-dongle` | — |

The pin maps in the board packs are the boards that were on the author's desk.
Clone boards of the same family differ — the 2.8" CYD has its backlight on one
pin and the 2.4" on another — so **check a pack's `BOARD.CFG` against your
board's schematic before trusting it**. A wrong SD chip select is a card that
never mounts while everything else looks healthy.

<a id="esp32-s3-cam"></a>
### ESP32-S3-WROOM CAM (N16R8)

The main development board: 16 MB flash, 8 MB octal PSRAM, a camera socket, a
microSD slot. The screen is a separate ST7789 module on the camera header.
Fallout 2 and the Mega Drive run here. More: [14-esp32-s3-cam.md](14-esp32-s3-cam.md).

```powershell
.\argon.cmd target esp32s3-board
.\argon.cmd build
.\argon.cmd flash -port COM4 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-cam --apps build\apps --sdkconfig sdkconfig --flash -p COM4
```

* `esp32s3-board` is the S3 without the two settings that exist only for QEMU.
  `esp32s3-cam` is the same with the camera built in; the camera and the SPI panel
  share header pins, so you cannot have both.
* A module with **32 MB** PSRAM does not work (see the README). **Quad** PSRAM
  needs `CONFIG_SPIRAM_MODE_QUAD` in place of `…_OCT`, or no PSRAM is found.

<a id="cyd"></a>
### ESP32 "CYD" (ESP32-2432S028R / S024)

ESP32-WROOM-32, 4 MB flash, **no PSRAM**, an ILI9341 320×240 screen on SPI, a
resistive touch panel, an SD slot on SPI. The cheapest way to see the system
running. More: [09-esp32-cyd.md](09-esp32-cyd.md).

The easy way: download the image from the
[Releases page](https://github.com/yamaxgucci/esp32-os/releases) and follow
[Flashing a release image](#flashing-a-release-image) below.

Building it yourself:

```powershell
.\argon.cmd target esp32-cyd
.\argon.cmd build
.\argon.cmd flash -port COM3 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-cyd --apps build\apps --sdkconfig sdkconfig --flash -p COM3
```

* Variants of the target: `esp32-cyd` has **no radio** and the most memory;
  `esp32-cyd-net` adds Wi-Fi and the shop client; `esp32-cyd-md` is `-net` with
  more instruction RAM for the Mega Drive. Mega Drive runs on the plain
  `esp32-cyd`.
* The pack's `BOARD.CFG` is for the 2.4" model. If your board is the 2.8" (S028),
  check the backlight and SD pins first.

<a id="touch-lcd-2"></a>
### Waveshare ESP32-S3-Touch-LCD-2

ESP32-S3R8: 16 MB flash, 8 MB octal PSRAM, a 2-inch ST7789 240×320 panel,
capacitive touch, a TF card. Screen and card share one SPI bus.

```powershell
.\argon.cmd target esp32s3-lcd2
.\argon.cmd build
.\argon.cmd flash -port COM6 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-touch-lcd-2 --apps build\apps --sdkconfig sdkconfig --flash -p COM6
```

Read the notes at the bottom of `boards\esp32s3-touch-lcd-2\BOARD.CFG` first:
they describe how the single USB-C socket is wired. See also
[user/02-board-setup.md](user/02-board-setup.md).

<a id="s3-zero"></a>
### Waveshare ESP32-S3-Zero

4 MB flash, no screen, no card slot, USB-JTAG console. With a CS4344 I2S DAC
on the pins in `BOARD.CFG` it makes sound. It has its own partition table
(`partitions_4mb_zero.csv`). Usually a node in a [cluster](user/07-cluster.md).

```powershell
.\argon.cmd target esp32s3-zero
.\argon.cmd build
.\argon.cmd flash -port COM6 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-zero --apps build\apps --sdkconfig sdkconfig --flash -p COM6
```

<a id="c6"></a>
### Waveshare ESP32-C6-LCD-1.47

RISC-V, one core at 160 MHz, 4 MB flash, no PSRAM, an ST7789 172×320 screen and
a microSD slot. The USB-C socket goes straight to the chip, so the console is the
chip's own USB serial. Wi-Fi 6 and BLE both work. More:
[12-esp32-c6-lcd.md](12-esp32-c6-lcd.md).

```powershell
.\argon.cmd target esp32c6
.\argon.cmd build
.\argon.cmd flash -port COM5 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-c6-lcd-1.47 --apps build\apps `
    --sdkconfig sdkconfig --add boards\esp32-c6-lcd-1.47\AUTOEXEC.BAT --flash -p COM5
```

* **QEMU cannot run this chip.** Before committing anything or running the
  tests, go back with `argon target esp32s3`.
* To flash a bare board you may have to hold **BOOT** (GPIO 9) while plugging it
  in, so the chip enters its ROM loader.

<a id="c3"></a>
### ESP32-C3 SuperMini

RISC-V, one core, about 400 KB of RAM, thirteen pins, no screen, no card, no
sound. A good second node and a bad first one: it holds a program and a wire,
and borrows a screen from a neighbour. Console on the chip's USB.

```powershell
.\argon.cmd target esp32c3
.\argon.cmd build
.\argon.cmd flash -port COM8 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32c3-supermini --apps build\apps --sdkconfig sdkconfig --flash -p COM8
```

For the two-board desk see [user/07-cluster.md](user/07-cluster.md).

<a id="p4"></a>
### M5Stack Stamp-P4

ESP32-P4 (rev 1.3): two RISC-V cores at 360 MHz, 32 MB PSRAM, 16 MB flash,
**no radio**, and nothing else on the module — no screen, no card, no buttons.
The USB-C socket is the chip's USB-Serial/JTAG. More:
[17-esp32-p4-stamp.md](17-esp32-p4-stamp.md).

```powershell
.\argon.cmd target esp32p4
.\argon.cmd build
.\argon.cmd flash -port COM21 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\m5stack-stamp-p4 --apps build\apps --sdkconfig sdkconfig --flash -p COM21
```

QEMU cannot run this chip. Return to `esp32s3` before running the tests.

<a id="a1s"></a>
### ESP32-A1S Audio Kit

AI-Thinker's audio board (marked V2.2) with an ES8388 codec, a guitar jack and
a headphone jack, and no screen. It is the board the valve-amplifier emulator is
meant to live in. Plug the cable into the micro-USB marked **UART** — the one
marked `power` carries no data. Windows has no driver for the CP2102 on it; install
Silicon Labs' VCP driver. Auto-reset works, so no BOOT button. More:
[15-esp32-a1s.md](15-esp32-a1s.md).

```powershell
.\argon.cmd target esp32-a1s          # the pedal: no radios, the emulator at full speed
# .\argon.cmd target esp32-a1s-net    # with Wi-Fi, so a phone can be its screen
.\argon.cmd build
.\argon.cmd flash -port COM12 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-a1s --apps build\apps --sdkconfig sdkconfig --flash -p COM12
```

Boards sold under the one "V2.2" marking come with at least two different codecs
and pin maps. The pack's `BOARD.CFG` is deliberately almost empty; read the
document above before filling it in.

<a id="kvm"></a>
### USB KVM dongle

The board is plugged into another PC's USB port and presents itself as a
keyboard, a mouse and a flash drive; the screen of that PC is shown on a phone,
and the phone's input is sent back. The PC runs no software and sees a
keyboard. Works at the BIOS. The firmware runs on an S3-CAM or on a LilyGo
T-Dongle-S3 (only the small screen differs). More:
[14-usb-kvm.md](14-usb-kvm.md) and [apps/kvm/README.md](../apps/kvm/README.md).

```powershell
.\argon.cmd target esp32s3-kvm
.\argon.cmd build
.\argon.cmd flash -port COM3 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-dongle --apps build\apps --sdkconfig sdkconfig --flash -p COM3
```

Use `boards\esp32s3-kvm` instead of `esp32s3-dongle` for the S3-CAM. The KVM
build has its own partition table (`partitions_kvm.csv`).

## Flashing a release image

A release image is the whole flash in one file: bootloader, partition table,
firmware and the `C:` drive, ready to be written at offset 0. You do not need
ESP-IDF, only Python.

```powershell
pip install esptool
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 argonos-esp32-cyd.bin
```

Use your own COM port; on Linux or macOS the port is `/dev/ttyUSB0` or similar.
Each release lists the SHA-256 of the file. A `write_flash 0x0` replaces
**everything** in the board's flash, including whatever was on `C:`. After the
reset the console is 115200 8N1 on the USB-serial port.

## When it does not work

| Symptom | Look at |
|---|---|
| `argon` says Python is not found | `ARGON_PYTHON` in `tools\local-env.ps1`; the Microsoft Store stub is not Python |
| `idf.py` not found | `ARGON_IDF_PATH`, and that ESP-IDF's own installer finished |
| Flashing hangs at "Connecting…" | hold BOOT while it starts; try another cable and another port |
| Prompt appears, screen is dark | step 5 was skipped; write `C:` with `mksysfs` |
| `extended` is 0 on a board with PSRAM | octal against quad PSRAM mode |
| `media FAILED` with a card in | the SD pins in `BOARD.CFG`; try `width = 1` and a lower `max_khz` |
| Garbage on the console | speed 115200, and a terminal that understands ANSI |
| `argon target` fails with access denied | something under `build\` is read-only; the script removes `build\` itself and retries |

More: [user/02-board-setup.md](user/02-board-setup.md) (Russian).
