# ESP8266 RLINK coprocessor firmware

The real far end of `EXTRADIO.SYS`: an ESP8266 (e.g. an ESP-07) wired to an
ArgonOS board's UART1, running **our** RLINK protocol instead of the stock AT
firmware. It is the on-metal twin of `tools/rlinkd.py` (the QEMU fake) — same
wire format (`apps/common/rlink/ag_rlink.h`), same request/reply/push shape —
but the sockets are lwIP sockets on a real Wi-Fi join.

`main/rlink_main.c` ports the `rlinkd.py` logic to C over esp_wifi + lwIP; the
codec `ag_rlink.c` is shared verbatim with the guest and the host tests (copied
into `main/` at build time — see below).

## What it does

- UART0 at 115200 8N1 carries raw RLINK frames (logs are silenced so nothing
  else touches the wire; the boot-ROM banner is drained by the guest's magic
  resync before the first HELLO reply).
- HELLO answers proto **v2** + `RL_CAP_SOCKETS`.
- START carries the Wi-Fi credentials (ssid + passphrase, `a0` = ssid length);
  the firmware joins there and pushes `RL_EV_GOTIP` when DHCP lands. Nothing is
  stored in flash — the network is named by the guest's `radio.ssid`/`radio.pass`.
- RESOLVE / CONNECT / LISTEN / ACCEPT / SEND / CLOSE / NONBLOCK map to lwIP BSD
  sockets; one task per connected socket pushes arriving bytes as `RL_OP_DATA`.

## Building (ESP8266_RTOS_SDK, not ESP-IDF)

Wi-Fi on the ESP8266 lives in closed SDK blobs, so this needs the
**ESP8266_RTOS_SDK** (v3.4) and its `xtensa-lx106-elf` toolchain, not the ESP32
IDF.

Two traps on Windows, both real here:

1. **A space in the project path breaks the SDK's component discovery** (the
   `main` component silently compiles nothing). Build from a space-free copy of
   this directory, e.g. `D:\rl8266fw`.
2. The v3.4 CMake build wants `mconf-idf`; having any `gcc` on PATH (e.g.
   `D:\dev\mingw64\bin`) satisfies the configure check (the actual `mconf` build
   only runs for `menuconfig`, which we do not use).

```
set IDF_PATH=D:\Espressif\ESP8266_RTOS_SDK
set IDF_TOOLS_PATH=D:\Espressif\tools8266
REM PATH += lx106 bin, cmake, ninja, a native gcc, the SDK python venv
python %IDF_PATH%\tools\idf.py build
```

Output at `build/`: `bootloader/bootloader.bin` (0x0),
`partition_table/partition-table.bin` (0x8000), `rlink8266.bin` (0x10000).

The codec copy `main/ag_rlink.c`/`.h` is gitignored; refresh it from
`apps/common/rlink` if that changes (the SDK rejects component sources outside
the component directory, so a copy is kept here).

## Flashing it with the board itself

The ArgonOS board flashes the wired module from these `.bin`s with
`ESPFLASH.AXE` (see `docs/user/06-flashing-a-coprocessor.md`). A multi-region
image is several `write`s; keep the module in the loader between them with
`-keep`, because sending FLASH_END ends the ESP8266 loader session:

```
run c:\espflash.axe write c:\fw\boot.bin  -a 0x0     -keep
run c:\espflash.axe write c:\fw\part.bin  -a 0x8000  -keep
run c:\espflash.axe write c:\fw\rlink.bin -a 0x10000 -keep
```

Enter download mode with GPIO0 tied low at reset; tie GPIO0 high (or move its
strap to 3.3 V) and power-cycle to run the firmware. Then on the board:
`drv load c:\extradio.sys` → `net use extradio` (sends the credentials) →
`net` / `wget`.

Board-verified 2026-09-13 on an ESP-07 + CYD: join, DHCP, DNS, TCP, HTTP 200
all through RLINK.
