# `argon_port` — what the OS requires of the machine under it

The ArgonOS kernel does not call ESP-IDF. It calls a **port**: the contract
headers in `include/argon/port/`, behind which stands one of the
implementations in `idf/`, `host/` or `bare/`. Which one is chosen by a single
line in this component's `CMakeLists.txt`.

## Why

Three reasons, ordered by what they give immediately:

1. **A specification.** The `bare` port is the list of what the OS needs from
   below, and it compiles. Today it is `#error` most of the way down — but every
   `#error` is a concrete piece of work rather than a wish, and `bare/README.md`
   puts them in the order they have to be done with an honest size against each.
2. **A second chip.** RISC-V (C6, P4) differs from Xtensa in exactly what lives
   in `<port>/src/fault_hw.c`, not in the whole kernel.
3. **Verifiability, next.** A `host` port over pthreads and the host allocator
   would let the parts of the kernel that can only be exercised in QEMU today —
   processes, threads, the supervisor — build and run on the PC in milliseconds.
   That port is not written yet; the boundary it needs now exists.

## The rule that matters more than the rest

**The layer is not allowed to cost a byte.** Anything that maps one-to-one onto
a function below is a `static inline` in the port header, not a call across a
translation unit. This is not checked by eye: the per-section size of every
kernel object file is compared before and after.

The same rule says what the layer must not turn into: a better interface. The
port repeats what is already there with exactly the same semantics — including
units (ticks, not milliseconds, wherever the caller counts in ticks) and
including the parts that look wrong. Improving behaviour is allowed, but in its
own commit, where it can be seen.

## Layout

| Directory | What it is |
|---|---|
| `include/argon/port/*.h` | the contract: what a port must supply, and with what semantics |
| `idf/` | the ESP-IDF 5.5 implementation — what the system runs on today |
| `bare/` | the skeleton: everything bare metal does not have, with `#error` and an explanation |

The contract is ten headers and it is the whole of what ArgonOS requires below
itself:

| Header | What it asks for |
|---|---|
| `mem.h` | an allocator with capability classes, and a heap over a supplied block |
| `time.h` | microseconds since boot, and a cycle counter that really counts cycles |
| `task.h` | pre-emptive tasks with priorities, pinned to a core |
| `sync.h` | mutexes that invert priority, semaphores, queues |
| `sys.h` | cores, clock, external RAM, reset reason, restart, memory that survives one |
| `log.h` | capturing what the layer below prints |
| `config.h` | where build-time options come from |
| `uart.h` | serial ports |
| `flash.h` | named flash regions, and mapping one to fetch instructions from |
| `storage.h` | a filesystem on flash, and one on a card |
| `io.h` | GPIO, I2C, SPI, PWM, ADC |
| `fault.h` | catching a fault so it costs one process rather than the machine |
| `panel.h` | a screen, if there is one |
| `net.h` | an interface and TCP, if there is one |

A contract header ends by including `argon/port/impl/<name>.h`, and the chosen
port directory is what supplies that path. So the contract has one home, each
port has its own, and swapping them touches no kernel source at all.

## Adding a port

1. Copy `bare/` to `myport/`.
2. Replace `#error` with code until it links.
3. Set `set(ARGON_PORT myport)` in this component's `CMakeLists.txt`.

The order to do it in, and what each step actually costs, is in
[`bare/README.md`](bare/README.md). The short version: memory and time first
because nothing compiles without them, then a scheduler — which is the one item
on the list that is a kernel rather than a driver — then the serial port, and
after that it is peripherals in whatever order the board needs them.
