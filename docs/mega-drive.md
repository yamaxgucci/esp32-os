# Sega Mega Drive games on an ESP32

**English** | [Русский](mega-drive.ru.md)

This page covers the whole way to a running Mega Drive (Genesis) game on a
board with ArgonOS. It starts with the easiest way: everything is installed
from the shop in a browser, with no compiler. It ends with building the system
and the emulator from source. A separate section explains how to fit in the
memory of a board without PSRAM. Memory is what most often stops a game from
starting.

The emulator is called **MD** (`MD.AXE`). It lists the games on the card,
pauses a game with save and load state, plays sound and understands 3- and
6-button pads. The core is gwenesis with Musashi (68000). How it works inside
is in [apps/md/README.md](../apps/md/README.md).

- [1. What you need](#1-what-you-need)
- [2. The easy way: the shop](#2-the-easy-way-the-shop)
- [3. Games: which files and where](#3-games-which-files-and-where)
- [4. Running and controls](#4-running-and-controls)
- [5. A pad](#5-a-pad)
- [6. Memory: making the game start](#6-memory-making-the-game-start)
- [7. Building from source](#7-building-from-source)
- [8. When something is wrong](#8-when-something-is-wrong)
- [9. Options](#9-options)

## 1. What you need

**A board.** MD is built for two boards, and these are the ones it was tested on:

| Board | Chip | Memory | How a game runs |
|---|---|---|---|
| **CYD**: ESP32-2432S024 (2.4″) or ESP32-2432S028R (2.8″) | ESP32 | **no PSRAM**, 4 MB flash | 55–60 emulated frames a second; the screen is updated 20–35 times a second, less in heavy scenes |
| **ESP32-S3-CAM** (N16R8) with a 2″ ST7789 240×320 SPI screen | ESP32-S3 | 8 MB PSRAM, 16 MB flash | 59 emulated frames a second; the screen is updated with whole frames 30–58 times a second |

Emulated frames are the speed of the game itself; 60 is the console's own.
Only the lines that changed are sent to the screen, so the screen is updated
less often than the game runs.

Other ESP32 and ESP32-S3 boards may work if they have a screen and a driver
for it. Only these two have been tested.

**You also need:**

- a USB **data** cable (many cables carry power only);
- a microSD card for the CYD, FAT32. The S3-CAM can do without one;
- a computer with Chrome or Edge: the shop page flashes the board over Web
  Serial, which Firefox does not have;
- cartridge images that you own ([section 3](#3-games-which-files-and-where));
- optionally, a Sega pad and a PCF8574 expander ([section 5](#5-a-pad)).

## 2. The easy way: the shop

The ArgonOS program shop: **<https://170-168-150-42.sslip.io>** (the shop
pages are in Russian). To add a board you need an account.

### 2.1. Put ArgonOS on the board

1. Plug the board in and open **"Установить ArgonOS"** (Install ArgonOS) in
   the shop's top bar.
2. Pick the board and the firmware:

   | Board | Which one |
   |---|---|
   | CYD, **with a pad** (PCF8574 or ESP-07) | **"CYD 2.4″ (ESP32-2432S024), без радио"**, the build without the radio |
   | CYD, no pad | either. With Wi-Fi, programs install over the air, but Wi-Fi has to be turned off before playing ([step 2.4](#24-cyd-only-free-the-memory)) |
   | S3-CAM | **"ESP32-S3-CAM + экран SPI"**. The page shows which pins the screen goes to |

   [Section 6](#6-memory-making-the-game-start) explains why. In short: the
   CYD has no PSRAM, and the Wi-Fi libraries alone hold about 40 KB of
   internal memory even with the radio off. A game with a pad does not fit
   with those 40 KB gone.
3. The page checks the chip and the flash size, flashes the board and opens
   its console. On the Wi-Fi firmware you can connect the board to your
   network in the same page, in the Wi-Fi step.

> Flashing erases the board's internal drive `C:`. If the board was already
> in "Мои платы" (My boards), add it again afterwards.

### 2.2. Add the board to "My boards"

"Мои платы" → "Добавить" (Add). The board asks on its own screen whether you
agree to become its owner. Confirm on the board.

### 2.3. The card and installing MD

1. **On the CYD, insert the microSD card before installing.** MD is installed
   on the card; it does not fit on `C:` (512 KB).
2. Open **MD** in the catalogue and press "Установить" (Install). The shop
   picks the image for the board's chip.
   - A board on Wi-Fi downloads MD by itself.
   - A board without the radio installs programs **only over the cable**.
     Keep the page open until it says "установлена" (installed). Over the
     cable MD takes about 7 minutes, and the page shows the percentage done.
3. MD goes to `A:\APPS\MD.AXE` on the card, or `C:\APPS\MD.AXE` without one.
   On the CYD the pad driver `A:\APPS\MD\SEGAPAD.SYS` comes with it.

### 2.4. CYD only: free the memory

- **Firmware without the radio:** nothing to do.
- **Wi-Fi firmware, no pad:** turn Wi-Fi off before playing. Open the board's
  console ("Мои платы" → "Консоль") and type

  ```
  wifi forget
  ```

  Then restart the board with RST. After that programs install only over the
  cable. To bring Wi-Fi back: `wifi connect <network> <password>`.
- **Wi-Fi firmware with a pad:** does not fit even after `wifi forget`.
  Reflash the board with the build without the radio (step 2.1).

### 2.5. Games and running

Copy the games to the root of the card
([section 3](#3-games-which-files-and-where)), insert the card and type in
the console:

```
run a:\apps\md.axe
```

The programs folder is already on the search path, so just `md` also works.
Pick a game with the arrows and press Enter. Controls are in
[section 4](#4-running-and-controls).

## 3. Games: which files and where

- **Format:** a cartridge image `.BIN`, `.MD` or `.GEN`, **unzipped**. MD
  does not open ZIP files. It does not open `.SMD` files either (the Super
  Magic Drive copier format, with a header and interleaved blocks), and ROM
  collections are often full of them. `tools\mdrom.py` converts them (below).
- **Byte swap: the `.SWP` file.** The board's processor reads words in the
  opposite byte order to the 68000, and MD reads the game straight out of
  flash, with no copy in memory. So it needs the image with its bytes swapped:
  a `.SWP` file. There are two ways to make one:
  - **On the computer** (faster, and the only way for `.SMD`):

    ```powershell
    python tools\mdrom.py Sonic.bin "Gunstar Heroes.smd"
    ```

    A `.SWP` appears beside each file; put that on the card. All it needs is
    Python 3 and the file [tools/mdrom.py](../tools/mdrom.py) from the
    repository; there is no need to clone the rest.
  - **On the board:** pick a `.BIN`/`.MD`/`.GEN` in the game list. The first
    time, MD makes the `.SWP` beside the game ("PREPARING...") and opens that
    from then on. It takes noticeable time, and there has to be room beside
    the game for a second copy of it.

  MD recognises a file that is already swapped by its header (`ESAG` instead
  of `SEGA`) and does not swap it a second time. **From the command line**
  (`run md.axe <game>`) MD swaps nothing and takes the file as it is, so give
  it the `.SWP` there.
- **Size:**

  | Board | Largest game |
  |---|---|
  | CYD | **about 1 MB**. The game is copied into flash, into the programs partition (1.5 MB), where the cold part of MD itself (~420 KB) already lies. 2–4 MB games do not start on the CYD |
  | S3-CAM | 4 MB |

- **Where:** on the CYD, on the card, where MD is. On an S3-CAM without a
  card, on `C:` over the network ([docs/user/04-network.md](user/04-network.md),
  in Russian). The S3-CAM's `C:` (1 MB) holds MD and one small game.
- **Save states** (SAVE STATE in the pause menu) go beside the game. On the
  CYD they go to `C:\`, because the card is released while a game runs, to
  free memory.

## 4. Running and controls

```
run a:\apps\md.axe                   the window with the games on the card
run a:\apps\md.axe a:\sonic.swp      straight into a game, a .SWP only
```

In the game list: arrows choose, A, C or Start opens, B goes up a folder, Esc
leaves.

**The pause menu** opens with Esc on a keyboard, Start+Mode on a 6-button pad,
or a touch on the screen (the CYD's screen is a touch screen). It has RESUME,
SAVE STATE, LOAD STATE, LOAD GAME (another game) and EXIT.

| Key | Mega Drive button |
|---|---|
| arrows | d-pad |
| Z X C | A B C |
| Enter | Start |
| A S D | X Y Z |
| M | Mode |
| Esc | pause menu |

**The keyboard** is the console over the cable: a terminal on the computer,
115200 8N1. It is enough for the menus and for checking that things work, but
not for playing: a terminal never says when a key is released, so a held key
stays held only through auto-repeat. For playing you need a pad.

## 5. A pad

### 5.1. A real Sega pad on a PCF8574

A Mega Drive pad connects to the board through a PCF8574 I/O expander on the
I2C bus. The `SEGAPAD.SYS` driver finds the expander by itself at 0x20–0x27
(0x38–0x3F for a PCF8574A) and tells a 3-button pad from a 6-button one.

| PCF8574 | Pad connector (DB9) | Signal |
|---|---|---|
| P0 | 1 | up |
| P1 | 2 | down |
| P2 | 3 | left |
| P3 | 4 | right |
| P4 | 6 | A / B |
| P5 | 9 | Start / C |
| P6 | 7 | SELECT |
| — | 5 | 3.3 V |
| — | 8 | ground |

On the CYD the expander plugs into the **CN1** header, which has GND,
IO22 (SCL), IO21 (SDA) and 3.3 V in a row. The board pack already sets up
that bus (`[i2c0]`, 100 kHz). On another board, add it to `C:\SYSTEM.CFG`:

```
[i2c0]
sda = 21
scl = 22
khz = 100
pullups = true
```

Load the driver **before starting MD**, with one of two commands:

```
drv load a:\apps\md\segapad.sys         until the next restart
drv install a:\apps\md\segapad.sys      at every start
```

The **Mode** button goes to the game (The Lost Vikings uses it, for one).
The pause menu is **Start and Mode together**. On a pad without Mode, open the
menu by touching the screen. The driver is tested on the CYD and not on the S3.

### 5.2. An ESP-07 instead of a pad: buttons on a phone

Without a pad, an ESP-07 module can stand in for it. The module pretends to be
a PCF8574 with a pad on it, and the buttons are pressed on a page on a phone.
The same page has a KEYS mode with the phone's keyboard: what you type goes to
the console, to programs and to MD's menus. It uses the same driver,
`SEGAPAD.SYS`.

The module's firmware, `SEGAPAD-ESP07.BIN`, is a download on MD's page in the
shop. That page also explains, step by step, how to flash the module from the
CYD itself with the "Прошивальщик ESP" (ESP flasher) program. Building it from
source and how the module works:
[firmware/esp8266-segapad](../firmware/esp8266-segapad/README.md).

What matters:

- Wires: the module's TX to IO21, RX to IO22, 3.3 V and GND. The module needs
  the usual ESP-07 wiring: EN and RST to 3.3 V, GPIO15 to ground.
- The module is slower than a PCF8574, so set the bus to 50 kHz in
  `C:\SYSTEM.CFG`:

  ```
  [i2c0]
  khz = 50
  ```

  When a section is repeated, the last value wins, so the lines can go at the
  end of the file.
- Connect the phone **straight to the module's own network, SEGAPAD**, and
  open `192.168.4.1`. Through a home router every press also goes through the
  router's Wi-Fi, which is a noticeable delay on a busy network. The module and
  the driver themselves add about 60 ms.

### 5.3. A phone as a pad (S3-CAM only)

On a board with Wi-Fi and PSRAM: `drv load c:\phone.sys`, then open
`http://<board address>:8765/#pad` on the phone. Details in
[apps/phone/README.md](../apps/phone/README.md). **This does not work on the
CYD:** Wi-Fi and this driver take about 50 KB, and the game needs nearly all
the memory there is.

## 6. Memory: making the game start

### The CYD: why it is tight

The CYD's ESP32 has no PSRAM. Once the system has booted, without the radio
and with the card released, about 180 KB of internal memory is free, and that
is for everything. The emulator needs the machine's memory (68000 RAM 64 KB,
Z80 RAM 8 KB, video memory 64 KB), a buffer of lines for the screen, and room
for code. Part of the video memory lives in instruction memory (IRAM); the
rest is gathered in 512-byte pages from whatever is left. Once a game runs,
only a couple of kilobytes stay free. So every few kilobytes decide whether a
game starts or not.

What takes memory and how to avoid it:

| What | How much | What to do |
|---|---|---|
| Wi-Fi libraries in the firmware | ~40 KB always, even after `wifi forget` | firmware **without the radio**: from the shop, or your own build with the `esp32-cyd` target |
| The radio switched on (`[wifi]` in `SYSTEM.CFG`) | ~30 KB more | `wifi forget` and a restart |
| A microSD card mounted | ~44 KB, and the largest free block cut in two | nothing: MD releases the card before a game starts and mounts it again when you pick another |
| Drivers you do not need | a few KB each and up | `drv` lists the loaded ones. Only the screen (ILI9341), touch (XPT2046) and optionally the pad are needed. Unload one with `drv unload <name>`, or drop it for good with `drv uninstall <name>` |
| The phone driver `PHONE.SYS` | ~20 KB plus Wi-Fi | not on the CYD while a game runs |
| What the previous game left in the programs partition | flash space | restart the board between games (`reboot`) if needed |

`mem` shows the free memory. In more detail: `mem /t` (task stacks), `mem /i`
(internal memory, piece by piece). MD prints how much memory the game got when
it starts, for example:

```
md: 68000 RAM 64 KB, Z80 RAM 8 KB, VRAM 64 KB (31 KB in IRAM) - 512-byte pages from 11 blocks; 4 rows of ring
```

If it prints `md: short of memory` instead, there was not enough. Look for the
cause in the table above.

Two more limits on the CYD:

- Save states go to `C:\`, and there is not much room on `C:` (512 KB). The
  save file is opened when the game starts: in the middle of a game the CYD
  does not have even 512 contiguous bytes for the file system's buffer.
- Games of at most ~1 MB ([section 3](#3-games-which-files-and-where)).

### The S3-CAM: plenty of room

The S3 build takes from PSRAM whatever internal memory cannot give, and
internal memory stays with the system, the radio and the drivers. Only check
that the PSRAM was found: at boot the second line must show extended memory,
for example `8192 KB extended`. If it says zero, the PSRAM mode is wrong
(octal against quad). Modules with **32 MB** of PSRAM do not work. More in
[docs/building.md](building.md#esp32-s3-cam).

## 7. Building from source

### 7.1. What you need

| | |
|---|---|
| Host | **Windows 10/11**, PowerShell |
| ESP-IDF | **5.5** or newer, from Espressif's installer |
| Python | 3.10 or newer, with `pip` |

Put your machine's tool paths in `tools\local-env.ps1` (the file is not in
git):

```powershell
$env:ARGON_IDF_PATH       = 'D:\Espressif\esp-idf'
$env:ARGON_IDF_TOOLS_PATH = 'D:\Espressif\tools'
$env:ARGON_PYTHON         = 'D:\dev\python312\python.exe'
```

```powershell
git clone https://github.com/yamaxgucci/esp32-os.git
cd esp32-os
.\argon.cmd help
```

The common build recipe for any board is in [docs/building.md](building.md).
Below is what matters for MD.

### 7.2. Firmware: the CYD

```powershell
.\argon.cmd target esp32-cyd                 # wipes build\ and sdkconfig
.\argon.cmd build
.\argon.cmd flash -port COM3 -NoMonitor      # your COM port
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32-cyd --apps build\apps --sdkconfig sdkconfig --flash -p COM3
```

- **The target is `esp32-cyd`, without the radio.** It leaves the most free
  memory. There are two other variants: `esp32-cyd-net` (Wi-Fi and the shop
  client) and `esp32-cyd-md` (`-net` plus 9 KB more IRAM). MD starts on
  `-net` as long as Wi-Fi stays off and the pad driver is not loaded. A game
  with a pad needs `esp32-cyd`.
- The last command writes `C:`: the screen, touch and card pins, the settings
  and the drivers. Without it the board boots with a dark screen. `C:` is
  reformatted whenever the firmware changes, so `mksysfs` comes after `flash`
  every time.
- **Check `boards\esp32-cyd\BOARD.CFG` against your board's schematic.** The
  pack is written for the 2.4″ model (S024). On the 2.8″ (S028R) the
  backlight, touch and card chip select may be on other pins. A wrong card pin
  looks like this: everything works, and the card never mounts.
- The board pack includes the cluster wire driver (`NODELN32.SYS`). If the
  board is not one half of a pair, unload it to free memory
  ([section 6](#6-memory-making-the-game-start)).

### 7.3. Firmware: the S3-CAM

```powershell
.\argon.cmd target esp32s3-board
.\argon.cmd build
.\argon.cmd flash -port COM4 -NoMonitor
.\argon.cmd apps
python tools\mksysfs.py --board boards\esp32s3-cam --apps build\apps --sdkconfig sdkconfig --flash -p COM4
```

The ST7789 screen goes on the camera header, so this firmware has no camera.
Screen pins: SCL 12, SDA 11, CS 10, DC 13, RES 3, BL 46. More about the board
in [docs/14-esp32-s3-cam.md](14-esp32-s3-cam.md).

### 7.4. MD itself

```powershell
.\apps\md\build.ps1 -Chip esp32      # the CYD and other ESP32 boards
.\apps\md\build.ps1 -Chip esp32s3    # an ESP32-S3 with PSRAM
```

The image is `build\md\<chip>\MD.AXE`, about 480 KB. **The image must match
the chip.** The ESP32 (Xtensa LX6) lacks some of the ESP32-S3's (LX7)
instructions, so an S3 image on a CYD loads and then dies with
`illegal instruction` at the first of them.

Both builds are the same program, built the same way. The second core draws
the picture, the Z80 and the sound chips run on it too, and the cartridge is
read straight out of flash. The S3 build adds `-DMD_PSRAM_PAGES` to take
memory from PSRAM. The `mkaxe.py` command by hand, and other builds (for
example one with the YM2612 sound for comparison), are in
[apps/md/README.md](../apps/md/README.md).

### 7.5. The pad driver

```powershell
.\argon.cmd apps --only SEGAPAD.SYS
```

The driver is built for the current target's chip (`argon target`) and goes
to `build\apps\SEGAPAD.SYS`. Drivers are built for LX6 and LX7 separately as
well: a driver built for the S3 crashes on the CYD.

### 7.6. Putting it on the board and running

The simplest way is a card reader: `MD.AXE`, `SEGAPAD.SYS` and the games in
the root of the card, the card in the board.

```
drv load a:\segapad.sys
run a:\md.axe
```

Without a card reader, files can go over the network if the firmware has Wi-Fi
([docs/user/04-network.md](user/04-network.md)). A small file, such as a
driver, can also go over the cable:

```powershell
python tools\boardput.py -p COM3 build\apps\SEGAPAD.SYS a:\segapad.sys
```

That is about 5 KB/s, so MD and the games are better moved with a card
reader. On the CYD `boardput` sometimes loses the last bytes of a file: check
the size with `dir`.

## 8. When something is wrong

| What you see | Why, and what to do |
|---|---|
| `md: short of memory` | Not enough memory: [section 6](#6-memory-making-the-game-start). Almost always Wi-Fi in the CYD's firmware, or extra drivers |
| `illegal instruction` right after starting | The MD or driver image is built for the other chip ([7.4](#74-md-itself)) |
| A second game does not start (`no space left`) | The previous game left traces in the programs partition. Restart the board (`reboot`) and start the game again |
| A game over 1 MB does not start on the CYD | Expected: the CYD fits games of up to ~1 MB |
| Black screen while the game runs (`stats` shows ~59 frames, nothing goes to the screen) | The game is waiting for the Z80. Try the `sndhere` option, and tell us which game it was |
| An old game reacts oddly to the buttons | Try the `pad3` option, a 3-button pad |
| No card after leaving MD (`file not found`) | Type `mount a:`. If the card does not mount, type `eject`, then `mount a:` |
| The CYD's card stopped answering altogether | Unplug the board for a few seconds. RST is sometimes not enough |
| The pad is silent | When it loads, the driver prints `SEGAPAD: PCF8574 on i2c0 at 0x20` or says why it found no expander. `drv` shows whether the driver is loaded, `log` shows the journal. Check `[i2c0]` in `C:\SYSTEM.CFG` and the wires |
| The board will not flash | A power-only cable, the wrong COM port, or it needs the boot loader: hold BOOT while plugging it in |

## 9. Options

Options go after the game's name, separated by spaces, for example
`run a:\apps\md.axe a:\sonic.swp stats`.

| Option | What it does |
|---|---|
| `stats` | prints every 2 seconds: frames a second, frame time, how much went to the screen |
| `pad3` | a 3-button pad instead of a 6-button one, for old games it confuses |
| `fps30` | draws every second frame; all 60 are emulated |
| `sndhere` | the Z80 and sound on the same core as the game: slower, but the Z80 keeps pace with the game |
| `nosound` | no sound |

The full list is in [apps/md/README.md](../apps/md/README.md#run).

---

Sega's games belong to their owners. Use cartridge images you own. The
licences of the emulator's parts (GPLv3, MIT, BSD) are listed in
[apps/md/README.md](../apps/md/README.md#licensing).
