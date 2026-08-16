# `bare` — the list of what a bare-metal ArgonOS needs

Not a working port. A skeleton whose every `#error` is a piece of work, in the
order it has to be done. Set `set(ARGON_PORT bare)` in the component's
`CMakeLists.txt` and the compiler will read the list out loud.

Nothing here is guesswork about what *might* be needed: it is what the kernel
actually calls, because the kernel calls nothing else.

| # | File | What it is | Size |
|---|---|---|---|
| 1 | `impl/mem.h` | allocator with capability classes, plus a heap over a supplied block | a day |
| 2 | `impl/time.h` | a free-running microsecond counter and a cycle counter | an hour |
| 3 | `impl/sys.h` | core count, clock, external RAM, reset reason, restart, non-initialised memory | an hour |
| 4 | `impl/log.h` | already complete — two no-ops | done |
| 5 | `impl/config.h` | already complete — every option has a default | done |
| 6 | `impl/task.h`, `impl/sync.h` | **a pre-emptive scheduler with priorities**, and mutexes that invert priority | weeks |
| 7 | `impl/uart.h` + `src/uart_hw.c` | the serial port everything is brought up over | a day |
| 8 | `src/fault_hw.c` | the exception table, so an application that faults costs one process | days |
| 9 | `src/flash_hw.c` | named flash regions, and mapping one so instructions can be fetched from it | days |
| 10 | `impl/io.h` + `src/io_hw.c` | GPIO, I2C, SPI, PWM, ADC | a week |
| 11 | `src/storage_hw.c` | a power-fail-safe filesystem on flash, and FAT on a card | weeks |
| 12 | `src/panel_hw.c` | optional — a display | — |
| 13 | `src/net_hw.c` | optional — an interface and TCP | — |

Items 1–7 get to a prompt on a serial console. 8–11 get an operating system.

Two of these are not drivers and are worth knowing about before starting.

**Item 6 is a kernel, not a shim.** ArgonOS does not bring a scheduler; it asks
the machine below for pre-emptive tasks and builds processes, threads and the
supervisor on those. Writing one is entirely possible and it is also the bulk of
the work on this page. Worth considering first: FreeRTOS is not ESP-IDF. Its
kernel is a handful of files with a portable core and a small per-architecture
layer, and using it without the rest of ESP-IDF is a legitimate answer to items
6 and nothing else.

**Item 11 is somebody else's code.** ESP-IDF supplies littlefs, FAT, wear
levelling and the SD host, and this project deliberately does not reimplement
them — see `argon/port/storage.h`. Bringing littlefs and FatFs to a new machine
is mostly writing their block-device hooks, which is small; bringing up the SD
host underneath is not.

What is *not* on this list is as informative as what is: no VFS, no path
handling, no console, no terminal codecs, no process model, no loader, no
resource accounting, no shell, no file manager, no editor. Those are 25 000
lines that do not change.
