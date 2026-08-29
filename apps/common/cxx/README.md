# `apps/common/cxx` — C++ in a `.AXE`

An application links `-nostdlib` and gets only `libgcc` from the toolchain.
That was enough for C. C++ needs four things more, and they are here:

| File | What it is |
|---|---|
| `cxx_rt.cpp` | `ag_main`: runs the constructors, calls `ag_cxx_main`, runs the destructors. `operator new`/`delete` over the process arena. `__cxa_atexit`, the local-static guards, `__dso_handle`, `__cxa_pure_virtual` |
| `cxx_throw.cpp` | `std::__throw_*` → panic. Link only with the STL, and **before** `-lstdc++` |
| `cxx_hosted.c` | `pthread_mutex_*` over real ArgonOS mutexes, per-thread keys, `__getreent`. Link only with the STL |
| `ag_cxx.h` | The one-line contract: an app writes `ag_cxx_main`, not `ag_main` |

## Writing one

```cpp
#include <argon/argon.h>
#include "ag_cxx.h"

AG_APP("MYAPP", "1.0", "you", 0);

extern "C" int ag_cxx_main(int argc, char **argv) { ... }
```

A vendored engine keeps its own `main` and the port writes the line that calls
it, the way `apps/doom` already does for doomgeneric.

`mkaxe.py` compiles `.cpp` / `.cc` / `.cxx` with the C++ driver (`--gxx`,
by default the C one with `gcc` replaced by `g++`) and adds `--cxxflags`,
`-fno-exceptions -fno-rtti` by default. Everything else is unchanged, and a
C and a C++ source may sit in the same image.

## Without the STL

Two sources, nothing else:

```
python tools/mkaxe.py --include sdk/include --include apps/common/cxx \
    -o build/apps/MYAPP.AXE apps/myapp/myapp.cpp apps/common/cxx/cxx_rt.cpp
```

`CXXTEST.AXE` is this, and it is the test: constructors and their order,
virtual dispatch through a base, `new`/`delete`, templates, a function-local
static, destructors in reverse at exit.

## With the STL

`std::string`, `std::vector`, `std::map`, `std::sort` all work. Half of each
is out-of-line in `libstdc++.a`, so the image grows by around 25 KB and needs
three more sources and one archive:

```
python tools/mkaxe.py --include sdk/include --include apps/common/cxx \
    --cflags "-Os -ffunction-sections -fdata-sections -fno-builtin" \
    --libs stdc++ -o build/apps/MYAPP.AXE \
    apps/myapp/myapp.cpp apps/common/cxx/cxx_rt.cpp \
    apps/common/cxx/cxx_throw.cpp apps/common/cxx/cxx_hosted.c \
    apps/common/libc/libc_shim.c
```

`CXXSTL.AXE` is this, and it is the test.

Note what is **not** on the include path: `apps/common/libc`. The shim's
`stdlib.h` and `string.h` are narrower than newlib's, and `<cstdlib>` refuses
to compile against them - `div_t has not been declared` and forty lines like
it. The shim reaches its own headers through quoted includes, from its own
directory, so it compiles correctly either way; the C++ sources get the
toolchain's headers and the shim's implementations, which is the combination
that works.

`-fno-builtin` is not optional. Without it GCC recognises the body of the
shim's `strlen` as `strlen` and compiles it into a call to itself; the image
loads, prints nothing, and hangs. This has now cost two people an afternoon.

## What is missing, and why

**Exceptions.** Unwinding needs `.eh_frame` and a personality routine out of
libgcc, and the image discards both - `.eh_frame` and `.gcc_except_table` are
in mkaxe's `/DISCARD/` list. C++ is compiled `-fno-exceptions`, and
`cxx_throw.cpp` turns libstdc++'s own failures into a panic with the message
the exception would have carried. A `throw` reaching an image built this way
does not unwind; it ends the process.

A port that genuinely needs exceptions is a decision to revisit, not a file to
extend: it means keeping the tables, linking the unwinder, and paying for both
in an arena measured in tens of kilobytes.

**RTTI.** `-fno-rtti`, for the same kind of reason. `dynamic_cast` and
`typeid` need typeinfo objects the linker cannot fold.

**iostreams.** Not linked and not wanted: `std::cout` drags in the locale
machinery, which is most of the size of libstdc++. Use `ag_printf`.

**Thread-safe statics across threads.** `__cxa_guard_acquire` does not lock -
see the comment in `cxx_rt.cpp`. Statics initialised on the thread that starts
the program, which is every port so far, are safe. If that is not true of
yours, build with `-fno-threadsafe-statics` and initialise by hand.

## Two traps that are already paid for

**`.ctors` vs `.init_array`.** The Xtensa compiler is built without
`--enable-initfini-array` and emits the 1990s `.ctors`; the RISC-V one emits
`.init_array`. They iterate in opposite directions. `mkaxe.py` merges them
into one ascending range with `SORT_BY_INIT_PRIORITY`, which understands both
spellings, so the runtime walks one array forwards and never knows which
compiler it was.

**`AG_APP` in a C++ file.** `__ag_app_header` is `const` at namespace scope,
which in C++ means internal linkage *and* a mangled name -
`_ZL15__ag_app_header`. `mkaxe.py` looks the symbol up by name, found nothing,
and built an image with an empty name and no stack or heap request, silently.
`AG_APP` now declares both it and `g_ag_api` `extern "C"`.

Both were caught by the orphan-section check `mkaxe.py` grew for this work: it
refuses to build an image with an allocated section the linker script does not
name, because ld puts an orphan wherever it likes and the image simply would
not carry it. That is what would have happened to `.init_array`.
