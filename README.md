# ArgonOS

A small, fast operating system for ESP32-class microcontrollers.

ArgonOS follows the MS-DOS execution model — **the application owns the
machine** — while adding the few things that make a system usable in
industrial products: a stable ABI for third-party binaries, a real device and
driver model, per-process resource accounting, and a supervisor that can kill
a hung application without rebooting the board.

It is not a Linux clone. There is no MMU-backed process isolation, no virtual
memory, no POSIX emulation layer. There is a thin kernel, a syscall table, and
your code running on a dedicated CPU core with almost all of the RAM.

## Status

**Early development.** Nothing is usable yet. See
[docs/04-roadmap.md](docs/04-roadmap.md) for the plan and
[docs/00-architecture.md](docs/00-architecture.md) for the design.

## Design in one screen

```
  Applications (.AXE)   native code, loaded from SD, runs on core 1
  ─────────────────────────────────────────────────────────────────
  libargon              inline wrappers over a versioned syscall table
  ─────────────────────────────────────────────────────────────────
  ArgonOS kernel        loader · processes · memory arenas · VFS
                        console multiplexer · device manager · shell
                        supervisor (watchdog, Ctrl-Alt-Del, recovery)
  ─────────────────────────────────────────────────────────────────
  board pack            BOARD.CFG: pins, buses, display, SD mode
  ─────────────────────────────────────────────────────────────────
  ESP-IDF               FreeRTOS · drivers · Wi-Fi · USB · lwIP
```

* **Applications** are relocatable ELF images loaded into internal SRAM or
  into PSRAM mapped executable. No interpreter, no JIT, no sandbox — native
  speed after load.
* **The console** is a virtual text screen rendered simultaneously to UART,
  telnet and a local display, so the same build works headless or with a
  screen and a USB keyboard.
* **Graphics** are opt-in: text by default, an application can take the
  display and draw, exactly like a DOS video mode switch.
* **Drivers** are either linked in or loaded from `.SYS` modules at runtime.

## Target hardware

Primary: **ESP32-S3** with at least 4 MB PSRAM and 8 MB flash.
Planned: ESP32-P4. Reduced profiles for ESP32 / C3 / C6.

## Building and running

Requires ESP-IDF 5.5 or newer. Everything goes through one entry point:

```
argon build              build the firmware
argon run                run in QEMU, console attached to this window
argon run -tcp           run in QEMU, console on 127.0.0.1:5556
argon test ver mem       boot in QEMU, type commands, print the screen
argon tests              host unit tests, no hardware needed
argon flash -port COM5   flash a real board and open the monitor
```

`argon` is a batch file rather than a PowerShell script so that it works on a
stock Windows install, where running `.ps1` files is disabled by default.

Machine specific paths (where ESP-IDF and a host compiler live) go in
`tools\local-env.ps1`, which is not committed; `tools\idf-env.ps1` guesses the
usual locations when it is absent.

No board is required to work on this: ArgonOS boots in Espressif's QEMU, and
`argon test` drives its console and prints the resulting screen. Everything
that has no dependency on the chip - path handling, the config parser, the text
screen, the terminal codecs, line editing, the VFS and the RAM disk - also
builds and runs on the host.

## License

Apache-2.0. See [LICENSE](LICENSE).
