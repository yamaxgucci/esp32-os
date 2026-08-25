# MARAUDER.AXE — ESP32-Marauder as a loadable ArgonOS application

A port of [ESP32-Marauder](https://github.com/justcallmekoko/ESP32Marauder)
(and the [CYD variant](https://github.com/Fr4nkFletcher/ESP32-Marauder-Cheap-Yellow-Display))
to ArgonOS as an ordinary **loadable `.AXE`** — not a builtin kernel module.
It reaches the machine only through the public ABI (`sdk/include/argon`): the
panel through `gfx->present`, the touch screen through the input queue, and the
radios through `api->wifimon`, `api->wifi` and `api->ble`.

Target board: **ESP32-CYD** (ESP32-2432S028R). ILI9341 320×240, XPT2046 touch,
no PSRAM. See [docs/09-esp32-cyd.md](../../docs/09-esp32-cyd.md).

## What works

The **whole menu tree, 1:1 with upstream** (`MenuFunctions.cpp`) — same labels,
colours and order; GPS entries are left out (this board has no GPS). Touch
navigation (tap; the status-bar left edge is Back; a scrollbar and
**swipe-to-scroll** on long lists) **and** keyboard (arrows / Enter / Esc), so
the UI is drivable over a serial console as well as by finger.

Functional screens, each through the app-facing ABI:

**WiFi › Sniffers** (`api->wifimon`, ABI 0.38 — passive capture and injection):
- **Scan AP/STA** — a real station scan via `api->wifi` (ABI 0.39): SSID,
  channel, RSSI and **security** (open/WEP/WPA/WPA2/WPA3); tap an open network
  to join it, a footer shows the link state.
- **Beacons** — sniff beacons/probe responses and list the APs around, sorted
  by signal.
- **Probe Requests** — the SSIDs clients are asking for, with the client MAC.
- **Packet Monitor** — live frame counts by type and a rate bar, while hopping.

**WiFi › Attacks**:
- **Beacon Spam** — inject beacons for a rotating list of obviously-fake SSIDs
  (locally-administered BSSID); they appear in nearby Wi-Fi scans.
- **Evil Portal › Access Points** — bring up an open access point (`ArgonOS-AP`)
  via `api->wifi` (SoftAP); a phone joins it and the screen shows the address
  and client count.

**Bluetooth › Sniffers** (`api->ble`, ABI 0.38 — central/observer):
- **Bluetooth Sniffer** — observe every BLE device in range (name, address,
  signal, whether it is connectable); tap a connectable one to connect and
  discover its GATT services.
- **Detect Card Skimmers** — the same scan, flagging names of the cheap serial
  BLE modules skimmers are built from (HC-05/06/08, JDY-, AT-09, BT05).

**Bluetooth › Attacks** — BLE-spam via `api->ble->adv_raw` (ABI 0.40, raw
advertising injection): **Sour Apple**, **SwiftPair**, **Samsung**, **Google**
and **Spam All** forge the raw advertisements those vendors' pairing pop-ups key
on, under rotating spoofed addresses.

**Device › Info** — chip, board, profile, ABI, memory.

Every other leaf opens a placeholder with the correct title, so the tree is
whole. ESP-NOW is available to apps (`api->wifi`) though no menu leaf drives it
yet.

## How it draws — no framebuffer

With a radio up this board has little RAM free; a 320×240 RGB565 system
framebuffer is 150 KB and does not exist here, so the app never takes the system
surface. It renders the UI into a small **band** (320×1 or ×2) and hands each
band to the panel with `gfx->present` (ABI 0.31), exactly as `apps/midipad`
does. Touch arrives in **console cells**, mapped to a surface pixel by
proportion. Only one screen is live at a time, so their working buffers share
one scratch (`mrd_scratch()`) rather than each spending bss the radio then needs.

The same band code has a second path for development: when a system surface
exists (QEMU `-Gfx`), each band is copied in with `gfx->blit` and flushed, so
the menu, layout and navigation render and can be captured with `gfxdump`
without a board. QEMU models no radio, so the live screens report the radio as
unavailable there.

## Memory and XIP — the radio-first workflow

The code (~20 KB) exceeds the 8 KB IRAM arena, so the loader runs it **from
flash (XIP)** — the app also sets `AG_AXE_WANT_XIP` to make that intentional
rather than incidental. Only the *data* part (~10 KB) lives in RAM, kept lean so
the app fits alongside a running radio (Wi-Fi ~36 KB, BLE ~53 KB, leaving
~17–30 KB).

Because raising a radio needs a large contiguous block the resident app would
otherwise split, **bring the radio up first**, then run the app:

```
wifi on            (or  bt on  for the Bluetooth screens)
run c:\marauder.axe
```

The Wi-Fi and BLE screens require this — they do not raise the radio themselves
(an app-context BLE bring-up is not safe on this chip), and answer with a clear
"run wifi on / bt on first" until it is up. `[wifi] ssid` / `[bt]` in
`SYSTEM.CFG` can raise a radio at boot so a single `run` suffices.

## Build

Registered in `tools/apps.json` (group `core`), so `argon apps` and CI build it.

```powershell
argon apps --only MARAUDER.AXE --warnings           # S3 toolchain (host gate)
argon target esp32; argon apps --only MARAUDER.AXE   # ESP32 board toolchain
```

The board firmware needs `CONFIG_ARGON_NET_WIFI_MON=y` (already set in
`sdkconfig.defaults.esp32`); Wi-Fi AP, ESP-NOW and BLE central/peripheral are on
by default in Kconfig.

## Run on the CYD board

```powershell
argon target esp32
argon build
python -m esptool --chip esp32 -p COM<N> write_flash "@build\flash_args"
python tools\mksysfs.py --display panel --add build\apps\MARAUDER.AXE=marauder.axe --flash -p COM<N>
```

Then on the board's console: `wifi on` (or `bt on`), then `run c:\marauder.axe`.

Headless self-tests print each result over the console (`[mrd] ...`):
`run c:\marauder.axe wifitest` exercises the whole `api->wifi` surface,
`run c:\marauder.axe bttest` the `api->ble` surface (scan / connect / discover /
adv_raw).

## Files

| File | What |
|---|---|
| `marauder.c`  | entry point; `shot` / `dump` / `wifitest` / `bttest` modes |
| `mrd_gfx.c`   | band renderer: acquire, draw primitives, present/blit push, shared scratch |
| `mrd_menu.c`  | the menu tree (1:1) and the navigation that walks it |
| `mrd_scan.c`  | `api->wifimon` screens: beacon/probe capture, packet monitor, beacon spam, device info |
| `mrd_wifi.c`  | `api->wifi` screens: station scan/connect, SoftAP access point |
| `mrd_bt.c`    | `api->ble` screens: BLE sniffer, card-skimmer detector, GATT connect/discover |
| `mrd_spam.c`  | `api->ble->adv_raw` BLE spam: Apple / SwiftPair / Samsung / Google builders |
| `mrd_font.h`  | 8×16 printable-ASCII font (from the kernel's `ag_font8x16`) |
