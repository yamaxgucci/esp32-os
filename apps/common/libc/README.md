# `apps/common/libc` — the freestanding C library for `.AXE` apps

Applications link `-nostdlib`: the only thing they get from the toolchain is
`libgcc`. That is deliberate — newlib would pull in a heap, file descriptors and
locale machinery that duplicate what the OS already provides through `ag_*`. So
anything a vendored emulator core expects from `<stdio.h>` or `<string.h>` lives
here instead, forwarded to the ABI.

- `libc_shim.c` — `malloc`/`free`, `printf`/`snprintf` (small formatter → `ag_print`), `FILE*` over `ag_open`,
  the `mem*`/`str*` family, ctype/strings, a few `math.h` entries, `abort`, `__assert_func`
- `stdio.h`, `stdlib.h`, `string.h`, `math.h`, `signal.h` — headers just wide
  enough for the cores that need them. Put `--include apps/common/libc` before
  the app's own include directories

Ports were vendored one per emulator at first, and both copies drifted; there is
one now. Anything genuinely specific to an emulator (its `platform.c`, its sound
sink) stays in that app's own `port/`.

If a core needs something real from newlib — `setjmp` is the case that came up —
ask for it explicitly with `mkaxe.py --libs c` rather than reimplementing it. The
linker takes only the objects that resolve a still-undefined symbol, so the
shim's own `malloc` still wins.

Used by: `apps/sms`, `apps/md`, `apps/doom`.
