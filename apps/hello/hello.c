/*
 * ArgonOS - the example application, and the loader's end-to-end test.
 *
 * Build it with the SDK's image tool and run it from the shell:
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o HELLO.AXE apps/hello/hello.c
 *   run t:\hello.axe one two
 *
 * It carries a static buffer far larger than the whole executable arena, on
 * purpose: an image is loaded in two parts, code into the small executable arena
 * and data into extended memory, so this is what a working split looks like from
 * the application's side.  If the two parts were relocated with one bias, or the
 * data went somewhere that is not really there, the check below fails or the run
 * ends in a fault rather than a number.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("HELLO", "1.0", "argon", 0);

/* Bigger than the arena the code lives in, and not stored in the file: bss is
 * described by the image and zeroed by the loader. */
static unsigned char s_big[512 * 1024];

#define STRIDE 4096u

int ag_main(int argc, char **argv)
{
    ag_printf("hello from ArgonOS, %d argument(s)\n", argc);
    for (int i = 0; i < argc; i++) {
        ag_printf("  argv[%d] = %s\n", i, argv[i]);
    }

    /*
     * bss has to arrive zeroed, whatever was in that memory before.
     */
    unsigned dirty = 0;
    for (unsigned i = 0; i < sizeof(s_big); i += STRIDE) {
        if (s_big[i] != 0) {
            dirty++;
        }
    }

    /* Written and read back a page at a time, so that memory that is not really
     * there shows up as a mismatch rather than as a plausible-looking run. */
    for (unsigned i = 0; i < sizeof(s_big); i += STRIDE) {
        s_big[i] = (unsigned char)(i / STRIDE);
    }
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof(s_big); i += STRIDE) {
        if (s_big[i] != (unsigned char)(i / STRIDE)) {
            bad++;
        }
    }

    ag_printf("%u KB of bss at %p: %u dirty, %u wrong\n",
              (unsigned)(sizeof(s_big) / 1024), (void *)s_big, dirty, bad);
    ag_printf("this code is at %p\n", (void *)(uintptr_t)&ag_main);

    ag_meminfo_t mem;
    ag_meminfo(&mem);
    ag_printf("%u KB of extended memory still free\n",
              (unsigned)(mem.arena_free / 1024));

    return (dirty == 0 && bad == 0) ? 0 : 1;
}
