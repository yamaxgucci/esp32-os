# CC — tiny C → `.AXE` on the device

Compiles a small subset of C to a runnable Xtensa `.AXE` without a host toolchain.

## Now (MVP)

- Types: `int`, `void` (return only)
- Entry: `ag_main` only (no other functions / calls)
- Statements: locals, assign, `return`, `if`/`else`, `while`, blocks
- Expressions: `+ - * / %`, compares, `&& || !`, parentheses
- Result is the process exit code (`errorlevel`)

No preprocessor, floats, structs, pointers, or arrays yet.

**Pins / console / files:** not from CC yet. Generated images have a `g_ag_api`
slot but emit no syscalls. Normal apps built with GCC + `mkaxe` can already use
`ag_gpio_*` from [`argon.h`](../../sdk/include/argon/argon.h); CC needs ABI
calls first (see backlog).

## Later (backlog)

Order when we return to CC (see also [`docs/04-roadmap.md`](../../docs/04-roadmap.md) §2):

1. Multiple functions + calls (`callx`)
2. ABI calls — print, then **GPIO** (`ag_gpio_config` / `write` / `read`)
3. `char`, pointers, arrays
4. `for`, globals
5. Later still: structs, simple preprocessor, RISC-V backend

## Build

```text
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc ^
    --include sdk/include --include apps/cc -o build\sd_card\CC.AXE ^
    apps/cc/cc_main.c apps/cc/cc_compile.c
```

## Use (QEMU)

```text
edit t:\hi.c          # or copy via HostFS / sync
run t:\cc.axe t:\hi.c t:\hi.axe
run t:\hi.axe
errorlevel
```

Example source:

```c
int ag_main(void)
{
    int n = 0;
    while (n < 10) {
        n = n + 1;
    }
    return n;
}
```
