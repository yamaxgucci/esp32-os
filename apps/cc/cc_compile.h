/*
 * ArgonOS - Tiny C → .AXE compiler (shared host / guest).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_CC_COMPILE_H
#define AG_CC_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Offsets into ag_api_t and its sub-tables, as the generated code reaches them:
 * this compiler has no headers and no linker, so a builtin call is a load of
 * the api pointer, a load of the sub-table and a load of the function.
 *
 * They are here rather than in the .c so that host-tests/test_cc.c can compare
 * every one of them against offsetof() in the real abi.h.  The ABI rule is that
 * a table only ever grows at its end; that test is what turns a violation of it
 * into a failed build instead of a guest jumping through the wrong slot.
 */
#define API_OFF_SYS   8
#define API_OFF_MEM   12
#define API_OFF_FS    16
#define API_OFF_CON   20
#define API_OFF_INP   24
#define API_OFF_GFX   28
#define API_OFF_DEV   32
#define API_OFF_IO    36
#define API_OFF_TIME  40
#define API_OFF_PROC  48
#define API_OFF_AUDIO 60

#define MEM_OFF_ALLOC   4
#define MEM_OFF_REALLOC 12
#define MEM_OFF_FREE    16

#define FS_OFF_OPEN     4
#define FS_OFF_CLOSE    8
#define FS_OFF_READ     12
#define FS_OFF_WRITE    16
#define FS_OFF_OPENDIR  52
#define FS_OFF_READDIR  56
#define FS_OFF_CLOSEDIR 60

#define DEV_OFF_OPEN     8
#define DEV_OFF_CLOSE    12
#define DEV_OFF_READ     16
#define DEV_OFF_WRITE    20
#define DEV_OFF_IOCTL    24
#define DEV_OFF_ADD      32
#define DEV_OFF_REMOVE   36
#define DEV_OFF_GET_PRIV 40

#define CON_OFF_PUTS   8
#define CON_OFF_PRINTF 12
#define CON_OFF_CLS    32
#define CON_OFF_GOTOXY 36

#define INP_OFF_POLL        4
#define INP_OFF_KEY_PRESSED 12
#define INP_OFF_PAD         20
#define INP_OFF_BTN         24

#define SYS_OFF_HEARTBEAT 32

#define PROC_OFF_FOCUSED 48

#define TIME_OFF_US       4
#define TIME_OFF_MS       8
#define TIME_OFF_DELAY_MS 16

#define IO_OFF_GPIO_CONFIG 4
#define IO_OFF_GPIO_WRITE  8
#define IO_OFF_GPIO_READ   12
#define IO_OFF_ADC_READ    56

#define GFX_OFF_ACQUIRE     4
#define GFX_OFF_RELEASE     8
#define GFX_OFF_FLUSH       12
#define GFX_OFF_SWAP        16
#define GFX_OFF_CLEAR       20
#define GFX_OFF_FILL_RECT   24
#define GFX_OFF_TEXT        32
#define GFX_OFF_PIXEL       40
#define GFX_OFF_LINE        44
#define GFX_OFF_CIRCLE      48
#define GFX_OFF_FILL_CIRCLE 52
#define GFX_OFF_POLY_BEGIN  56
#define GFX_OFF_POLY_VERTEX 60
#define GFX_OFF_POLY_FILL   64
#define GFX_OFF_POLY_STROKE 68
#define GFX_OFF_CLIP            80
#define GFX_OFF_CLIP_RESET      84
#define GFX_OFF_STROKE_RECT     88
#define GFX_OFF_FILL_ROUND_RECT 92
#define GFX_OFF_BLIT_KEY        96
#define GFX_OFF_BLIT_BIND       100
#define GFX_OFF_BLIT_COPY       104
#define GFX_OFF_BLIT_KEYED      108
#define GFX_OFF_TEXT_FIT        112
#define GFX_OFF_BLIT_SRC_RECT   116
#define GFX_OFF_BLIT_SCALED     120
#define GFX_OFF_BLIT_TILED      124
#define GFX_OFF_POLY_UV         128
#define GFX_OFF_POLY_FILL_TEX   132

#define AUDIO_OFF_PRESENT 4
#define AUDIO_OFF_IS_HW   8
#define AUDIO_OFF_OPEN    12
#define AUDIO_OFF_CLOSE   16
#define AUDIO_OFF_WRITE   20
#define AUDIO_OFF_SPACE   24

/*
 * The minor an image demands of the kernel, per feature used.  Stamping the
 * highest of the ones a program actually uses, rather than a constant, is what
 * lets a program that only draws run on a kernel that has no audio.
 */
#define ABI_MINOR_BASE   8
#define ABI_MINOR_GFX    9
#define ABI_MINOR_BTN    10
#define ABI_MINOR_AUDIO  14
#define ABI_MINOR_GFX16  16
#define ABI_MINOR_GFX17  17
#define ABI_MINOR_GFX25  25
#define ABI_MINOR_GFX26  26
#define ABI_MINOR_FOCUS  20

typedef struct {
    uint8_t *axe;
    size_t   axe_len;
    char     err[160];
} cc_result_t;

/*
 * How `#include "file"` reaches a file, which the compiler itself has no way to
 * do: it is the same code on a host with stdio and on a guest with the Argon FS.
 * Returns 0 and a NUL-terminated buffer that the compiler then owns and frees -
 * so it must come from the allocator the compiler uses (malloc on the host,
 * ag_malloc on the guest).
 */
typedef int (*cc_read_file_fn)(void *ctx, const char *path, char **out_text,
                               size_t *out_len);

/* Compile source to a complete Xtensa .AXE image. 0 on success. */
int cc_compile_to_axe(const char *src, size_t src_len, cc_result_t *out);

/* The same, for a source that may include files. A NULL reader refuses them. */
int cc_compile_to_axe_inc(const char *src, size_t src_len,
                          cc_read_file_fn reader, void *ctx, cc_result_t *out);

void cc_result_free(cc_result_t *out);

/*
 * Evaluate a pure integer expression with the same operators as the compiler
 * (+ - * / % < > <= >= == != && || ! and parentheses). Used by host tests.
 */
int cc_eval_expr(const char *expr, int32_t *out_value, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* AG_CC_COMPILE_H */
