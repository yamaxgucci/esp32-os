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

## Building

Requires ESP-IDF 5.3 or newer.

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

Host-side unit tests (no hardware required):

```bash
cmake -S host-tests -B build-host && cmake --build build-host && ctest --test-dir build-host
```

## License

Apache-2.0. See [LICENSE](LICENSE).
