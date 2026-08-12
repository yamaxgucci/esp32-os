# Argon CC — tiny C → `.AXE` on the device

**Argon CC** compiles a small subset of C to a runnable Xtensa `.AXE` without a
host toolchain (image name `CC.AXE`).

## Language

- Types: `int` (32-bit), `char` (one **unsigned** byte), `struct`, a pointer to
  any of those, `void` (return / empty params)
- Entry: `ag_main` (app) or `ag_driver_init` (`.SYS` driver). Exactly one.
  Other functions allowed; **define before use**
- Drivers: `#pragma drv "NAME" "VER" "AUTHOR"` (required) sets the image
  header; the compiler stamps `AG_AXE_DRIVER`. `&func` yields a code address
  for filling an ops table
- App sizes: `#pragma appstack N` / `#pragma appheap N` write non-zero
  `stack_size` / `heap_size` into the AXE header (kernel defaults if omitted;
  `N` is decimal or `0x` hex bytes)
- Functions: up to 6 params of any of those types; calls use windowed `callx8`
- Globals (before functions): `int x;`, `char c;`, `int a[N];`, `char b[N];`,
  `char *p;`, `struct v s;`, `struct v a[N];`, `struct v *p;`
- Locals: the same, plus assign, `return`, `if`/`else`, `while`, `for`, `do`,
  `break`/`continue`, `switch`/`case`/`default`, blocks
- `for (init; cond; step)` — init may be a declaration, an assignment, `++`/`+=`,
  or empty; step the same (no decl)
- `switch` is an if-chain with fall-through; `break` leaves it (no `continue`)
- `typedef` and `enum { A, B = 3 }` (enumerators are int constants); `const` /
  `volatile` are accepted and ignored; `sizeof(type)` for known types
- Arrays and pointers: `a[i]`, `a[i] = expr`, `*p`, `*p = expr`, `&x`, `&a[i]`,
  `&func` (code address of a function already defined)
- Structs: `struct v { int x; char tag; int a[4]; struct v *next; };` then
  `s.x`, `p->x`, `a[i].x`, `p[i].x`, `&s` — in any combination, on either side
  of an `=`
- Pointer arithmetic steps by the element: `p + 1` moves 1 byte for `char *`,
  4 for `int *` and the whole struct for a `struct v *`; an array name is the
  address of its first element
- Expressions: `+ - * / %`, compares, `& | ^ ~ << >>`, `&& || !`, `?:`,
  `++`/`--` (prefix/postfix on names), `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=`
  `<<=` `>>=`, parentheses, calls — at C's precedence, so `(x & 3) == 0` needs
  its parentheses
- `>>` is arithmetic (sign-keeping); a shift count belongs in 0…31
- Integer constants: decimal, `0x` hex, `0` octal; character constants `'a'`,
  `'\n'`; string literals `"..."` are `char *`
- Preprocessor: `#define` / `#undef` (object- and function-like), `#ifdef` /
  `#ifndef` / `#else` / `#endif`, `#include "file"` (resolved next to the
  source being compiled). No `#if` expressions, no `##`, no angle-bracket
  includes. A header with a function body ends the globals window — keep
  function definitions in `.c` files
- Result of `ag_main` is the process exit code (`errorlevel`)
- Errors carry the source line: `expected ';' at line 42`

No floats, unions, arrays of pointers, or pointers to pointers. Functions return
`int` or `void` — an address is an `int` like any other, so a function that hands
one back is declared `int`.

A struct is only ever handled by address: no struct assignment (`a = b`), no
struct parameter or return value — pass `&s` and take a `struct v *`. Fields go
in declaration order, each on its natural alignment (a word for everything but
a `char`), and the struct rounds up to a word — the same bytes the host C
compiler lays down for the same fields, which is what makes it safe to hand a
pointer to one across the ABI. A struct local starts out holding whatever the
frame held, as in C.

A value is always a 32-bit word in a register; `char` describes *storage*, so it
is what a load or a store touches and what an index is scaled by. `char c = 300`
keeps 44, and reads back positive.

Limits per program: 512 functions, 512 globals, 256 macros, 96 locals
(224 words) per function, 24 structs of 24 fields, 512 KB of static data
(PSRAM), and code up to 512 KB. Code that fits the IRAM arena (192 KB on S3)
runs from there; larger images use flash XIP. Guest `CC.AXE` reads up to
128 KB of source per file — split large games with `#include`.

Examples: [`examples/asteroids.c`](examples/asteroids.c) (a game),
[`examples/selftest.c`](examples/selftest.c) (checks the generated code,
including `#include "ppinc.h"`, and exits with the number of failures),
[`examples/fsmem.c`](examples/fsmem.c) (phase D: file + mute PCM chunk),
[`examples/dx7nofx.c`](examples/dx7nofx.c) (phase E: structural DX7 nofx on CC),
[`examples/echo.c`](examples/echo.c) (guest-built `.SYS`: `drv load` echo device).

## Builtins (ABI)

Hardcoded calls through `g_ag_api` (data slot + relocations). The offsets live
in `cc_compile.h` and are checked against `offsetof()` on the real `abi.h` by
`host-tests/test_cc.c`, so a table reordered in the kernel fails a build instead
of a guest.

The image demands the highest ABI minor of the features it uses: 8 plain, 9 for
`ag_gfx_*` (which also sets `AG_AXE_NEEDS_GFX`), 10 for `ag_btn`, 14 for
`ag_audio_*` (`AG_AXE_NEEDS_AUDIO`), 16 for clip/round-rect, 17 for
`ag_gfx_blit_bind` / `blit_copy` / `blit_keyed`, 20 for `ag_focused`.

| Builtin | Notes |
|---------|--------|
| `ag_delay(ms)` | `time->delay_ms` |
| `ag_micros()` / `ag_millis()` | boot time; micros is low 32 bits of `time->us` |
| `ag_key(code)` | sticky `key_pressed` (serial; imperfect for chords) |
| `ag_btn(id)` | **live pad** (HostFS PADPUSH): 0=up…3=right, 4=b1, 5=b2, 6=pause, 7=quit |
| `ag_poll_event(ev, ms)` | `inp->poll`; `ev` is a buffer/`struct` ≥ `ag_event_t` (type at offset 0) |
| `ag_heartbeat()` | `sys->heartbeat` (call while backgrounded / long waits) |
| `ag_focused()` | true while this process owns the focused session slot (ABI 0.20) |
| `ag_print(s)` | `con->puts`; `s` is a string literal or any address |
| `ag_printf(fmt, ...)` | `con->printf`, up to 6 arguments including the format |
| `ag_print_int(n)` / `ag_print_hex(n)` | the same with a format the compiler supplies |
| `ag_cls()` / `ag_gotoxy(x, y)` | text screen |
| `ag_gpio_config(pin, mode)` | mode: 0 in, 1 out, 2 out-OD, 3 in-pullup, 4 in-pulldown |
| `ag_gpio_write(pin, level)` / `ag_gpio_read(pin)` | |
| `ag_adc_read(channel)` | |
| `ag_gfx_acquire()` | `gfx->acquire(NULL)` |
| `ag_gfx_release()` | |
| `ag_gfx_clear(c)` | |
| `ag_gfx_flush(x,y,w,h)` | |
| `ag_gfx_swap()` | |
| `ag_gfx_fill_rect(x,y,w,h,color)` | |
| `ag_gfx_text(x,y,s,fg,bg)` | `s` string literal or int pointer |
| `ag_gfx_pixel(x,y,color)` | |
| `ag_gfx_line(x0,y0,x1,y1,color)` | |
| `ag_gfx_circle` / `ag_gfx_fill_circle` | |
| `ag_gfx_poly_begin` / `vertex` / `fill` / `stroke` | Convex poly helpers |
| `ag_gfx_clip` / `ag_gfx_clip_reset` | Soft-draw clip (ABI 0.16) |
| `ag_gfx_stroke_rect` / `ag_gfx_fill_round_rect` | Rect helpers (ABI 0.16) |
| `ag_gfx_blit_bind(src, stride)` | RGB565 LE source for stateful blit (ABI 0.17) |
| `ag_gfx_blit_copy(x,y,w,h)` | Opaque copy from bound source |
| `ag_gfx_blit_keyed(x,y,w,h,key)` | Chroma-key blit; `key` is `0x00RRGGBB` |
| `ag_audio_present()` | 1 if `api->audio` exists |
| `ag_audio_is_hw()` | 1 when I2S pins live |
| `ag_audio_open()` | default 22050 Hz stereo s16 (`fmt` NULL) |
| `ag_audio_close()` | |
| `ag_audio_write(buf, frames)` | `buf` is `int[]` address; kernel reads interleaved s16 (see `fmtoy.c`) |
| `ag_audio_space()` | free frames (best-effort) |
| `ag_malloc(n)` / `ag_realloc(p, n)` / `ag_free(p)` | process arena in PSRAM |
| `ag_open(path, flags)` / `ag_close(h)` | `flags`: `#define AG_O_RDONLY 0`, `WRONLY 1`, `RDWR 2`, `CREATE 4`, `TRUNC 8` |
| `ag_read(h, buf, n)` / `ag_write(h, buf, n)` | returns byte count or negative error |
| `ag_opendir(path)` / `ag_readdir(h, &ent)` / `ag_closedir(h)` | `ent` is a buffer/`struct` the size of `ag_dirent_t` |
| `ag_dev_open(name)` / `ag_dev_close(h)` | open by bare device name |
| `ag_dev_read` / `ag_dev_write` / `ag_dev_ioctl` | same shapes as the FS calls |
| `ag_dev_add(desc)` / `ag_dev_remove(name)` | publish / withdraw (only in `ag_driver_init`) |
| `ag_dev_priv(dev)` | driver's `priv` from inside ops callbacks |

No `ag_seek`: the kernel API is `int64_t`, and every value in this language is a
32-bit word. Open flags are ordinary `#define`s (see above), not keywords.

Using any `ag_gfx_*` sets `AG_AXE_NEEDS_GFX`. Using any `ag_audio_*` sets
`AG_AXE_NEEDS_AUDIO` (ABI minor 14).

Audio toy (2-op FM, not DX7): [`examples/fmtoy.c`](examples/fmtoy.c).
Phase D smoke (file read + mute PCM chunk): [`examples/fsmem.c`](examples/fsmem.c).

Phase E — structural DX7 **nofx** written in the CC dialect (6 op, 32 alg,
8 voices, EG, LFO shapes): [`examples/dx7nofx.c`](examples/dx7nofx.c).
It is a reimplementation for Mini-C, not a compile of host
[`apps/common/dx7`](../common/dx7). The host-built [`apps/dx7`](../dx7)
(`DX7.AXE` via `mkaxe`) stays the line for FX / WAV / richer UI.

Same virt devices as host DX7 (not mute `ag_audio_*` → pcmnull):

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
# guest:
drv install h:\pcmvirt.sys
drv install h:\midivirt.sys
run h:\cc.axe h:\dx7nofx.c h:\dx7nofx.axe
run h:\dx7nofx.axe
# host (IDF Python):
pcmplay.py --reconnect
midikbd.py --reconnect
```

Focus the **midikbd** window for poly notes (`Z..M` / `Q..I`). Console
sticky keys remain a fallback. `[` `]` algorithm, `,` `.` preset, `-` `=`
feedback, Space panic, Esc quit. The live UI shows `Perf` / `Stream`
(render load %, late, drop, resync) like host DX7. Smoke prints
`dx7nofx: smoke ok`.

## Build

```text
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc ^
    --include sdk/include --include apps/cc -o build\apps\CC.AXE ^
    apps/cc/cc_main.c apps/cc/cc_compile.c
```

`mkaxe` also stages into `build/sd_card/` (see
[`docs/user/03-host-share.md`](../../docs/user/03-host-share.md)).

## Drivers (`.SYS`) on the guest

```c
#pragma drv "ECHO" "1.0" "cc"

struct ops { int open; int close; int read; int write; int ioctl; int size; };
/* … fill ops.read = &echo_read; … */

int ag_driver_init(void)
{
    return ag_dev_add(&g_desc);
}
```

```text
run h:\cc.axe h:\echo.c h:\echo.sys
drv load h:\echo.sys
dev
drv unload ECHO
```

## Use (QEMU)

```text
argon run -Gfx -HostFs build\sd_card
```

Guest (`H:` = `build\sd_card` with `CC.AXE`, examples, and `g2d_*.h`):

```text
run h:\cc.axe h:\asteroids.c h:\asteroids.axe
run h:\asteroids.axe

run h:\cc.axe h:\tile_demo.c h:\tile_demo.axe
run h:\tile_demo.axe

run h:\cc.axe h:\wetspot.c h:\wetspot.axe
run h:\wetspot.axe

run h:\cc.axe h:\harbor.c h:\harbr.axe
run h:\harbr.axe
```

RPG demo: [`games/harbor`](games/harbor) (Harbor Quest). Stage `harbor.c`,
`hq_data.h`, `atlas.bin`, and the g2d headers on HostFS.

2D games: Mini-C library [`lib/g2d`](lib/g2d) (copy `g2d_globals.h` /
`g2d_impl.h` beside your `.c`). Needs ABI 0.17 `ag_gfx_blit_bind` /
`blit_copy` / `blit_keyed`.

Asteroids input is the **same HostFS PADPUSH path as SMS** (`H:\sms.pad`,
level keys ~60 Hz). Controls (defaults in `sms.cfg`): Left/Right rotate, Up
thrust, **Z** fire, **Esc** quit; after Game Over Z restarts. Focus the SDL
window. Do not rely on `ag_key` sticky serial keys for play.

Smaller smoke:

```text
edit t:\hi.c
run t:\cc.axe t:\hi.c t:\hi.axe
run t:\hi.axe
errorlevel
```

The self-test is the one that checks the *generated code* rather than the
parser, which host tests cannot do — they have no Xtensa to run it on:

```text
argon test -Put "build\sd_card\CC.AXE=t:\cc.axe" ^
    -Put "apps\cc\examples\selftest.c=t:\st.c" ^
    "run t:\cc.axe t:\st.c t:\st.axe" "run t:\st.axe" errorlevel
```

`errorlevel 0` is the pass; anything else is the number of checks that failed,
each printed with what it got and what it wanted.

Example source:

```c
struct pt {
    int x;
    int y;
};

int add(int a, int b)
{
    return a + b;
}

/* A struct travels by address: the callee takes a pointer and writes through it. */
int move(struct pt *p, int dx, int dy)
{
    p->x = p->x + dx;
    p->y = p->y + dy;
    return p->x * p->x + p->y * p->y;
}

/* A string is a char pointer, and walking one is the ordinary C loop. */
int upper(char *s, char *out)
{
    int n = 0;
    while (*s != 0) {
        char c = *s;
        if (c >= 'a' && c <= 'z') {
            c = c & ~32;
        }
        out[n] = c;
        n = n + 1;
        s = s + 1;
    }
    out[n] = 0;
    return n;
}

int ag_main(void)
{
    char      buf[32];
    struct pt p;
    int       s = 0;
    int       i;
    for (i = 0; i < 10; i = i + 1) {
        s = s + add(i, 1);
    }
    p.x = 0;
    p.y = 0;
    upper("argon cc", buf);
    ag_printf("%s %d %d\n", buf, s, move(&p, 3, 4));
    return s;
}
```
