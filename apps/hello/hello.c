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
 * It also reads its own constants and prints where they are, because they live in
 * the data part now rather than in the arena: the addresses that come out say
 * which window each kind of thing ended up in.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("HELLO", "1.0", "argon", 0);

/* Bigger than the arena the code lives in, and not stored in the file: bss is
 * described by the image and zeroed by the loader. */
static unsigned char s_big[512 * 1024];

#define STRIDE 4096u

/*
 * Constants travel with the data, which is what makes fonts and bitmaps possible
 * at all: they are read, never executed, and the arena is tens of kilobytes while
 * PSRAM is megabytes.  The code reaches this table through a literal holding its
 * absolute address, and the loader relocates that literal by the data bias - by
 * the wrong bias, or by none, the checksum below would not come out.
 */
static const unsigned char k_ramp[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};
#define RAMP_SUM 0x7f8u /* 0x11 * (0 + 1 + ... + 15) */

/* A table of pointers to constants: words in the data part pointing into it. */
static const char *const k_lines[] = {
    "constants live with the data",
    "and cost PSRAM, not arena",
};

/* And the exception: this one stays in the code part, so its address comes out in
 * the instruction window while k_ramp comes out in the data window. */
AG_HOT_RODATA const unsigned char k_hot[16] = {
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};

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

    /*
     * Read every constant through the relocated literals, and print where each
     * kind ended up: k_ramp in the data window with the rest of the data, k_hot
     * in the instruction window with the code, because it asked to stay there.
     */
    unsigned sum = 0, hot = 0;
    for (unsigned i = 0; i < sizeof(k_ramp); i++) {
        sum += k_ramp[i];
        hot += k_hot[i];
    }
    ag_printf("constants at %p sum %#x (want %#x), hot at %p sum %#x\n",
              (const void *)k_ramp, sum, RAMP_SUM, (const void *)k_hot, hot);
    for (unsigned i = 0; i < sizeof(k_lines) / sizeof(k_lines[0]); i++) {
        ag_printf("  %s\n", k_lines[i]);
    }

    /*
     * The arena belongs to this process: what it allocates comes out of here,
     * and all of it goes back when the process ends, however it ends.
     */
    ag_meminfo_t mem;
    ag_meminfo(&mem);
    ag_printf("pid %d, arena %u KB with %u KB free\n", (int)ag_getpid(),
              (unsigned)(mem.arena_total / 1024),
              (unsigned)(mem.arena_free / 1024));

    return (dirty == 0 && bad == 0 && sum == RAMP_SUM && hot == RAMP_SUM)
               ? 0
               : 1;
}
