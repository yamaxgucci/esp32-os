/*
 * cchost - run the board's own compiler on this PC.
 *
 * CC.AXE and this tool are the same cc_compile.c.  Having it on the host is
 * not a convenience: it is how a change to the code generator is shown to have
 * changed nothing it was not meant to.  Compile every example before a refactor
 * and after it, and compare the bytes - a back end that has been taken apart
 * and put back together either produces the same image or it does not, and no
 * amount of reading the diff answers that as well.
 *
 *   build-host/cchost in.c out.axe        compile
 *   build-host/cchost in.c -              compile, write nothing, report size
 *   build-host/cchost in.c out.axe -v     and print the header
 *   build-host/cchost in.c out.axe -riscv for the other machine
 *   build-host/cchost in.c out.bc  -bc    for the one with no processor
 *   build-host/cchost in.c out.bin -code  just the code part, to disassemble
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc_compile.h"

#define SRC_CAP (256u * 1024u)

static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cchost: cannot open %s\n", path);
        return NULL;
    }
    char *buf = (char *)malloc(SRC_CAP);
    if (buf == NULL) {
        fclose(f);
        fprintf(stderr, "cchost: out of memory\n");
        return NULL;
    }
    const size_t n = fread(buf, 1, SRC_CAP - 1u, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/*
 * `#include "x.h"` beside the source, which is what the guest does too - the
 * examples that use one (tile_demo, wetspot) would otherwise not compile here
 * and would silently drop out of any comparison.
 */
static char g_dir[512];

static int read_include(void *ctx, const char *path, char **out_text,
                        size_t *out_len)
{
    (void)ctx;
    char full[1024];
    if (strchr(path, '/') != NULL || strchr(path, '\\') != NULL ||
        strchr(path, ':') != NULL || g_dir[0] == '\0') {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), "%s%s", g_dir, path);
    }
    char *text = slurp(full, out_len);
    if (text == NULL) {
        return -1;
    }
    *out_text = text;
    return 0;
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/*
 * FNV-1a over the whole image.  A hash rather than a byte-for-byte diff because
 * what is wanted is one line per example that either matches the line from
 * before the change or does not; the file is there to look at when it does not.
 */
static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cchost <in.c> <out.axe|-> [-v]\n");
        return 2;
    }
    const char *in = argv[1];
    const char *out = argv[2];
    int         verbose = 0;
    int         arch = CC_ARCH_XTENSA;
    int         code_only = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-riscv") == 0) {
            arch = CC_ARCH_RV32;
        } else if (strcmp(argv[i], "-xtensa") == 0) {
            arch = CC_ARCH_XTENSA;
        } else if (strcmp(argv[i], "-bc") == 0) {
            arch = CC_ARCH_BC;
        } else if (strcmp(argv[i], "-code") == 0) {
            /* The code part alone, so a real disassembler can be pointed at
             * it: objdump -D -b binary -m riscv:rv32 is the only independent
             * reader of these instructions this project has. */
            code_only = 1;
        } else {
            fprintf(stderr, "cchost: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    /* The directory the source sits in, for the include reader above. */
    const char *slash = strrchr(in, '/');
    const char *back = strrchr(in, '\\');
    const char *cut = (back != NULL && (slash == NULL || back > slash)) ? back
                                                                       : slash;
    g_dir[0] = '\0';
    if (cut != NULL) {
        const size_t n = (size_t)(cut - in) + 1u;
        if (n < sizeof(g_dir)) {
            memcpy(g_dir, in, n);
            g_dir[n] = '\0';
        }
    }

    size_t src_len = 0;
    char  *src = slurp(in, &src_len);
    if (src == NULL) {
        return 1;
    }

    cc_result_t res;
    const int   rc = cc_compile_to_axe_for(src, src_len, read_include, NULL,
                                           arch, &res);
    free(src);
    if (rc != 0) {
        fprintf(stderr, "cchost: %s: %s\n", in, res.err);
        return 1;
    }

    printf("%s %u bytes fnv %08x\n", in, (unsigned)res.axe_len,
           (unsigned)fnv1a(res.axe, res.axe_len));

    if (verbose) {
        const uint8_t *h = res.axe;
        printf("  abi %u.%u arch %u flags %08x\n", rd_u16(h + 4), rd_u16(h + 6),
               rd_u16(h + 8), (unsigned)rd_u32(h + 12));
        printf("  code base %08x size %u file %u at %u\n", rd_u32(h + 16),
               (unsigned)rd_u32(h + 20), (unsigned)rd_u32(h + 24),
               (unsigned)rd_u32(h + 28));
        printf("  data base %08x size %u file %u at %u\n", rd_u32(h + 32),
               (unsigned)rd_u32(h + 36), (unsigned)rd_u32(h + 40),
               (unsigned)rd_u32(h + 44));
        printf("  entry %08x api_slot %08x relocs %u at %u\n", rd_u32(h + 48),
               rd_u32(h + 52), (unsigned)rd_u32(h + 60),
               (unsigned)rd_u32(h + 56));
        if (rd_u16(h + 10) >= 184) {
            printf("  irelocs %u at %u\n", (unsigned)rd_u32(h + 180),
                   (unsigned)rd_u32(h + 176));
        }
        printf("  name %s version %s\n", (const char *)h + 72,
               (const char *)h + 104);
    }

    if (strcmp(out, "-") != 0) {
        FILE *f = fopen(out, "wb");
        if (f == NULL) {
            fprintf(stderr, "cchost: cannot write %s\n", out);
            cc_result_free(&res);
            return 1;
        }
        if (code_only) {
            fwrite(res.axe + rd_u32(res.axe + 28), 1, rd_u32(res.axe + 24), f);
        } else {
            fwrite(res.axe, 1, res.axe_len, f);
        }
        fclose(f);
    }
    cc_result_free(&res);
    return 0;
}
