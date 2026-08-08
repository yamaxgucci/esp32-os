/*
 * ArgonOS - tiny C compiler as a .AXE application.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o CC.AXE apps/cc/cc_main.c apps/cc/cc_compile.c
 *   run t:\cc.axe t:\hi.c t:\hi.axe
 *   run t:\hi.axe
 *   errorlevel
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "cc_compile.h"

AG_APP("CC", "0.1", "argon", 0);

#define SRC_CAP (32u * 1024u)

static int read_all(const char *path, char *buf, size_t cap, size_t *out_len)
{
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("cc: open %s: %s\n", path, ag_strerror(h));
        return -1;
    }
    size_t n = 0;
    for (;;) {
        if (n >= cap) {
            ag_close(h);
            ag_printf("cc: source larger than %u bytes\n", (unsigned)cap);
            return -1;
        }
        const int32_t got = ag_read(h, buf + n, cap - n);
        if (got < 0) {
            ag_close(h);
            ag_printf("cc: read %s: %s\n", path, ag_strerror(got));
            return -1;
        }
        if (got == 0) {
            break;
        }
        n += (size_t)got;
    }
    ag_close(h);
    *out_len = n;
    return 0;
}

static int write_all(const char *path, const void *data, size_t len)
{
    const ag_handle_t h = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        ag_printf("cc: create %s: %s\n", path, ag_strerror(h));
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        const int32_t w = ag_write(h, (const uint8_t *)data + off, len - off);
        if (w < 0) {
            ag_close(h);
            ag_printf("cc: write %s: %s\n", path, ag_strerror(w));
            return -1;
        }
        if (w == 0) {
            ag_close(h);
            ag_printf("cc: short write %s\n", path);
            return -1;
        }
        off += (size_t)w;
    }
    ag_close(h);
    return 0;
}

int ag_main(int argc, char **argv)
{
    if (argc < 3) {
        ag_print("usage: cc <in.c> <out.axe>\n");
        return 1;
    }

    char *src = (char *)ag_malloc(SRC_CAP);
    if (src == NULL) {
        ag_print("cc: out of memory\n");
        return 1;
    }

    size_t src_len = 0;
    if (read_all(argv[1], src, SRC_CAP - 1, &src_len) != 0) {
        ag_free(src);
        return 1;
    }
    src[src_len] = '\0';

    cc_result_t res;
    if (cc_compile_to_axe(src, src_len, &res) != 0) {
        ag_printf("cc: %s\n", res.err);
        ag_free(src);
        return 1;
    }
    ag_free(src);

    if (write_all(argv[2], res.axe, res.axe_len) != 0) {
        cc_result_free(&res);
        return 1;
    }

    ag_printf("cc: wrote %s (%u bytes)\n", argv[2], (unsigned)res.axe_len);
    cc_result_free(&res);
    return 0;
}
