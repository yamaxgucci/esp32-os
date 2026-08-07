/*
 * ArgonOS - forces the flash XIP (R-1) load path.
 *
 * The body is thousands of tiny functions so .text exceeds the 64 KB code arena.
 * A successful run means the loader programmed appfs and executed from mmap.
 *
 *   python tools/gen_bigcode.py
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o BIGCODE.AXE apps/bigcode/bigcode.c \
 *       apps/bigcode/bigcode_body.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("BIGCODE", "1.0", "argon", 0);

int bigcode_run(void);

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const int rc = bigcode_run();
    ag_printf("bigcode %s (this code at %p)\n", rc == 0 ? "ok" : "FAIL",
              (void *)(uintptr_t)&ag_main);
    return rc;
}
