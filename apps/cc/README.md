# Argon CC — tiny C → `.AXE` on the device

**Argon CC** compiles a small subset of C to a runnable Xtensa `.AXE` without a
host toolchain (image name `CC.AXE`).

## Language

- Types: `int`, `void` (return / empty params)
- Entry: `ag_main` (required). Other functions allowed; **define before use**
- Functions: up to 4 `int` params; calls use windowed `callx8`
- Globals (before functions): `int x;`, `int arr[N];` (int arrays only)
- Locals: `int x`, `int arr[N]`, assign, `return`, `if`/`else`, `while`, `for`, blocks
- `for (init; cond; step)` — init may be `int name = expr`, assignment, or empty
- Arrays: `a[i]`, `a[i] = expr` (locals and globals)
- Expressions: `+ - * / %`, compares, `&& || !`, parentheses, calls
- String literals `"..."` → address (`int`), usable as builtin args
- Result of `ag_main` is the process exit code (`errorlevel`)

No preprocessor, floats, structs, or general pointers yet.

Example game written in this subset: [`examples/asteroids.c`](examples/asteroids.c).

## Builtins (ABI)

Hardcoded calls through `g_ag_api` (data slot + relocations). Using any `ag_gfx_*`
sets `AG_AXE_NEEDS_GFX` and requires ABI minor 9.

| Builtin | Notes |
|---------|--------|
| `ag_delay(ms)` | `time->delay_ms` |
| `ag_key(code)` | sticky `key_pressed` (serial; imperfect for chords) |
| `ag_btn(id)` | **live pad** (HostFS PADPUSH): 0=up…3=right, 4=b1, 5=b2, 6=pause, 7=quit |
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

## Build

```text
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc ^
    --include sdk/include --include apps/cc -o build\sd_card\CC.AXE ^
    apps/cc/cc_main.c apps/cc/cc_compile.c
```

## Use (QEMU)

```text
argon run -Gfx -HostFs build\sms_share
```

Guest (`H:` = share with `CC.AXE`, `asteroids.c`, optional `ASTEROIDS.AXE`):

```text
run h:\cc.axe h:\asteroids.c h:\asteroids.axe
run h:\asteroids.axe
```

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

Example source:

```c
int add(int a, int b)
{
    return a + b;
}

int ag_main(void)
{
    int s = 0;
    int i;
    for (i = 0; i < 10; i = i + 1) {
        s = s + add(i, 1);
    }
    return s;
}
```
