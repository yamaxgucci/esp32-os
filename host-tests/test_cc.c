/*
 * ArgonOS - tiny C compiler host tests (expressions + .AXE shape).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/abi.h>

#include "cc_compile.h"
#include "test.h"

/*
 * The compiler reaches the syscall table by arithmetic, so its offsets have to
 * agree with abi.h - and the host is 64-bit, where the same struct lays out
 * differently.  What survives the difference is the position of a field: every
 * member of ag_api_t past the version words is a pointer, and so is every
 * member of a sub-table past its size word.  Counting positions and multiplying
 * by the target's pointer size gives the offset the guest will see.
 *
 * A failure here means the ABI changed in a way the rules forbid - a field
 * inserted or reordered rather than appended - and every .AXE the compiler has
 * ever written now calls the wrong function.
 */
#define API_SLOT(field) \
    (8u + 4u * (unsigned)((offsetof(ag_api_t, field) - 8u) / sizeof(void *)))
#define SUB_SLOT(type, field)             \
    (4u + 4u * (unsigned)((offsetof(type, field) - sizeof(void *)) / \
                          sizeof(void *)))

_Static_assert(API_SLOT(mem) == API_OFF_MEM, "ag_api_t.mem moved");
_Static_assert(API_SLOT(fs) == API_OFF_FS, "ag_api_t.fs moved");
_Static_assert(API_SLOT(con) == API_OFF_CON, "ag_api_t.con moved");
_Static_assert(API_SLOT(inp) == API_OFF_INP, "ag_api_t.inp moved");
_Static_assert(API_SLOT(gfx) == API_OFF_GFX, "ag_api_t.gfx moved");
_Static_assert(API_SLOT(dev) == API_OFF_DEV, "ag_api_t.dev moved");
_Static_assert(API_SLOT(io) == API_OFF_IO, "ag_api_t.io moved");
_Static_assert(API_SLOT(time) == API_OFF_TIME, "ag_api_t.time moved");
_Static_assert(API_SLOT(audio) == API_OFF_AUDIO, "ag_api_t.audio moved");

_Static_assert(SUB_SLOT(ag_mem_api_t, alloc) == MEM_OFF_ALLOC,
               "mem.alloc moved");
_Static_assert(SUB_SLOT(ag_mem_api_t, realloc) == MEM_OFF_REALLOC,
               "mem.realloc moved");
_Static_assert(SUB_SLOT(ag_mem_api_t, free) == MEM_OFF_FREE, "mem.free moved");

_Static_assert(SUB_SLOT(ag_fs_api_t, open) == FS_OFF_OPEN, "fs.open moved");
_Static_assert(SUB_SLOT(ag_fs_api_t, close) == FS_OFF_CLOSE, "fs.close moved");
_Static_assert(SUB_SLOT(ag_fs_api_t, read) == FS_OFF_READ, "fs.read moved");
_Static_assert(SUB_SLOT(ag_fs_api_t, write) == FS_OFF_WRITE, "fs.write moved");
_Static_assert(SUB_SLOT(ag_fs_api_t, opendir) == FS_OFF_OPENDIR,
               "fs.opendir moved");
_Static_assert(SUB_SLOT(ag_fs_api_t, readdir) == FS_OFF_READDIR,
               "fs.readdir moved");
_Static_assert(SUB_SLOT(ag_fs_api_t, closedir) == FS_OFF_CLOSEDIR,
               "fs.closedir moved");

_Static_assert(SUB_SLOT(ag_dev_api_t, open) == DEV_OFF_OPEN, "dev.open moved");
_Static_assert(SUB_SLOT(ag_dev_api_t, close) == DEV_OFF_CLOSE,
               "dev.close moved");
_Static_assert(SUB_SLOT(ag_dev_api_t, read) == DEV_OFF_READ, "dev.read moved");
_Static_assert(SUB_SLOT(ag_dev_api_t, write) == DEV_OFF_WRITE,
               "dev.write moved");
_Static_assert(SUB_SLOT(ag_dev_api_t, ioctl) == DEV_OFF_IOCTL,
               "dev.ioctl moved");

_Static_assert(SUB_SLOT(ag_con_api_t, puts) == CON_OFF_PUTS, "con.puts moved");
_Static_assert(SUB_SLOT(ag_con_api_t, printf) == CON_OFF_PRINTF,
               "con.printf moved");
_Static_assert(SUB_SLOT(ag_con_api_t, cls) == CON_OFF_CLS, "con.cls moved");
_Static_assert(SUB_SLOT(ag_con_api_t, gotoxy) == CON_OFF_GOTOXY,
               "con.gotoxy moved");

_Static_assert(SUB_SLOT(ag_inp_api_t, key_pressed) == INP_OFF_KEY_PRESSED,
               "inp.key_pressed moved");
_Static_assert(SUB_SLOT(ag_inp_api_t, pad) == INP_OFF_PAD, "inp.pad moved");
_Static_assert(SUB_SLOT(ag_inp_api_t, btn) == INP_OFF_BTN, "inp.btn moved");

_Static_assert(SUB_SLOT(ag_time_api_t, us) == TIME_OFF_US, "time.us moved");
_Static_assert(SUB_SLOT(ag_time_api_t, ms) == TIME_OFF_MS, "time.ms moved");
_Static_assert(SUB_SLOT(ag_time_api_t, delay_ms) == TIME_OFF_DELAY_MS,
               "time.delay_ms moved");

_Static_assert(SUB_SLOT(ag_io_api_t, gpio_config) == IO_OFF_GPIO_CONFIG,
               "io.gpio_config moved");
_Static_assert(SUB_SLOT(ag_io_api_t, gpio_write) == IO_OFF_GPIO_WRITE,
               "io.gpio_write moved");
_Static_assert(SUB_SLOT(ag_io_api_t, gpio_read) == IO_OFF_GPIO_READ,
               "io.gpio_read moved");
_Static_assert(SUB_SLOT(ag_io_api_t, adc_read) == IO_OFF_ADC_READ,
               "io.adc_read moved");

_Static_assert(SUB_SLOT(ag_gfx_api_t, acquire) == GFX_OFF_ACQUIRE,
               "gfx.acquire moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, release) == GFX_OFF_RELEASE,
               "gfx.release moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, flush) == GFX_OFF_FLUSH,
               "gfx.flush moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, swap) == GFX_OFF_SWAP, "gfx.swap moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, clear) == GFX_OFF_CLEAR,
               "gfx.clear moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, fill_rect) == GFX_OFF_FILL_RECT,
               "gfx.fill_rect moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, text) == GFX_OFF_TEXT, "gfx.text moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, pixel) == GFX_OFF_PIXEL,
               "gfx.pixel moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, line) == GFX_OFF_LINE, "gfx.line moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, circle) == GFX_OFF_CIRCLE,
               "gfx.circle moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, fill_circle) == GFX_OFF_FILL_CIRCLE,
               "gfx.fill_circle moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, poly_begin) == GFX_OFF_POLY_BEGIN,
               "gfx.poly_begin moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, poly_vertex) == GFX_OFF_POLY_VERTEX,
               "gfx.poly_vertex moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, poly_fill) == GFX_OFF_POLY_FILL,
               "gfx.poly_fill moved");
_Static_assert(SUB_SLOT(ag_gfx_api_t, poly_stroke) == GFX_OFF_POLY_STROKE,
               "gfx.poly_stroke moved");

_Static_assert(SUB_SLOT(ag_audio_api_t, present) == AUDIO_OFF_PRESENT,
               "audio.present moved");
_Static_assert(SUB_SLOT(ag_audio_api_t, is_hw) == AUDIO_OFF_IS_HW,
               "audio.is_hw moved");
_Static_assert(SUB_SLOT(ag_audio_api_t, open) == AUDIO_OFF_OPEN,
               "audio.open moved");
_Static_assert(SUB_SLOT(ag_audio_api_t, close) == AUDIO_OFF_CLOSE,
               "audio.close moved");
_Static_assert(SUB_SLOT(ag_audio_api_t, write) == AUDIO_OFF_WRITE,
               "audio.write moved");
_Static_assert(SUB_SLOT(ag_audio_api_t, space) == AUDIO_OFF_SPACE,
               "audio.space moved");

static void check_expr(const char *expr, int32_t want)
{
    int32_t got = 0;
    char err[160];
    const int rc = cc_eval_expr(expr, &got, err, sizeof(err));
    AG_CHECK(rc == 0);
    if (rc != 0) {
        printf("     expr \"%s\": %s\n", expr, err);
        return;
    }
    AG_CHECK_INT(got, want);
}

/*
 * Stand-in for a filesystem: `#include "a.h"` finds what the test put in this
 * table.  The compiler frees what the reader hands back, so it comes from the
 * same malloc the compiler uses.
 */
typedef struct {
    const char *path;
    const char *text;
} fake_file_t;

static const fake_file_t *g_fake_files;
static int                g_fake_nfiles;

static int fake_reader(void *ctx, const char *path, char **out_text,
                       size_t *out_len)
{
    (void)ctx;
    for (int i = 0; i < g_fake_nfiles; i++) {
        if (strcmp(g_fake_files[i].path, path) != 0) {
            continue;
        }
        const size_t n = strlen(g_fake_files[i].text);
        char        *buf = (char *)malloc(n + 1);
        if (buf == NULL) {
            return -1;
        }
        memcpy(buf, g_fake_files[i].text, n + 1);
        *out_text = buf;
        *out_len = n;
        return 0;
    }
    return -1;
}

static void check_prog_inc(const char *src, int expect_ok,
                           const fake_file_t *files, int nfiles)
{
    g_fake_files = files;
    g_fake_nfiles = nfiles;
    cc_result_t res;
    const int   rc = cc_compile_to_axe_inc(src, strlen(src), fake_reader, NULL,
                                           &res);
    g_fake_files = NULL;
    g_fake_nfiles = 0;
    if (expect_ok) {
        AG_CHECK(rc == 0);
        if (rc != 0) {
            printf("     compile: %s\n", res.err);
            return;
        }
    } else {
        AG_CHECK(rc != 0);
    }
    cc_result_free(&res);
}

static void check_prog(const char *src, int expect_ok)
{
    cc_result_t res;
    const int rc = cc_compile_to_axe(src, strlen(src), &res);
    if (expect_ok) {
        AG_CHECK(rc == 0);
        if (rc != 0) {
            printf("     compile: %s\n", res.err);
            return;
        }
        AG_CHECK(res.axe_len >= 176);
        AG_CHECK(memcmp(res.axe, "AXE1", 4) == 0);
        /* arch = 1 (xtensa) at offset 8 */
        AG_CHECK(res.axe[8] == 1 && res.axe[9] == 0);
        /* code size > 0 at offset 20 */
        const uint32_t code_size = (uint32_t)res.axe[20] | ((uint32_t)res.axe[21] << 8) |
                                   ((uint32_t)res.axe[22] << 16) |
                                   ((uint32_t)res.axe[23] << 24);
        AG_CHECK(code_size > 0);
        cc_result_free(&res);
    } else {
        AG_CHECK(rc != 0);
        cc_result_free(&res);
    }
}

/*
 * The minor a program demands is the highest of the features it uses, so that a
 * program which only prints still runs on a kernel that predates audio.
 */
static void check_minor(const char *src, int want)
{
    cc_result_t res;
    const int rc = cc_compile_to_axe(src, strlen(src), &res);
    AG_CHECK(rc == 0);
    if (rc != 0) {
        printf("     compile: %s\n", res.err);
        return;
    }
    AG_CHECK_INT(res.axe[6] | (res.axe[7] << 8), want);
    cc_result_free(&res);
}

static int axe_contains(const uint8_t *axe, size_t len, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return 0;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(axe + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static void check_example_file(const char *label, const char *rel_a, const char *rel_b,
                               size_t max_src, uint32_t want_flags)
{
    const char *paths[] = {rel_a, rel_b, NULL};
    FILE *f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "rb");
        if (f != NULL) {
            break;
        }
    }
    AG_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char *buf = (char *)malloc(max_src);
    AG_CHECK(buf != NULL);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    const size_t n = fread(buf, 1, max_src - 1, f);
    fclose(f);
    buf[n] = '\0';
    cc_result_t res;
    const int rc = cc_compile_to_axe(buf, n, &res);
    AG_CHECK(rc == 0);
    if (rc != 0) {
        printf("     %s: %s\n", label, res.err);
    } else {
        const uint32_t flags = (uint32_t)res.axe[12] | ((uint32_t)res.axe[13] << 8) |
                               ((uint32_t)res.axe[14] << 16) |
                               ((uint32_t)res.axe[15] << 24);
        AG_CHECK((flags & want_flags) == want_flags);
        if ((flags & want_flags) != want_flags) {
            printf("     %s: flags=0x%x want=0x%x\n", label, (unsigned)flags,
                   (unsigned)want_flags);
        }
        cc_result_free(&res);
    }
    free(buf);
}

static void check_asteroids_example(void)
{
    /* Path relative to repo when tests run from build-host via argon.ps1. */
    check_example_file("asteroids", "../apps/cc/examples/asteroids.c",
                       "apps/cc/examples/asteroids.c", 64 * 1024, 2u /* NEEDS_GFX */);
}

static void check_dx7nofx_example(void)
{
    /* pcmvirt/midivirt via ag_dev_* (no AG_AXE_NEEDS_AUDIO; not ag_audio_*). */
    check_example_file("dx7nofx", "../apps/cc/examples/dx7nofx.c",
                       "apps/cc/examples/dx7nofx.c", 128 * 1024, 0u);
}

void run_cc_tests(void)
{
    printf("cc\n");

    check_expr("1+2*3", 7);
    check_expr("(1+2)*3", 9);
    check_expr("10-3-2", 5);
    check_expr("20/3", 6);
    check_expr("20%3", 2);
    check_expr("-5+2", -3);
    check_expr("!0", 1);
    check_expr("!2", 0);
    check_expr("1==1", 1);
    check_expr("1!=2", 1);
    check_expr("3<4", 1);
    check_expr("4<=4", 1);
    check_expr("5>3", 1);
    check_expr("5>=6", 0);
    check_expr("1&&0", 0);
    check_expr("0||2", 1);
    check_expr("1+2*3-4/2", 5);

    /* Phase B: bits and shifts, at C's precedence rather than a convenient one. */
    check_expr("6&3", 2);
    check_expr("6|3", 7);
    check_expr("6^3", 5);
    check_expr("~0", -1);
    check_expr("~5&255", 250);
    check_expr("1<<4", 16);
    check_expr("256>>3", 32);
    check_expr("-16>>2", -4);
    check_expr("1<<2+1", 8);        /* + binds tighter than << */
    check_expr("3&1==1", 1);        /* == binds tighter than & */
    check_expr("1|2^3&1", 3);       /* & then ^ then | */
    check_expr("0|0&&1", 0);        /* | binds tighter than && */
    check_expr("'A'", 65);
    check_expr("'a'-'A'", 32);
    check_expr("'\\n'", 10);

    check_prog("int ag_main(void) { return 42; }\n", 1);
    check_prog("int ag_main(void) {\n"
               "  int x = 1;\n"
               "  int y = 2;\n"
               "  while (x < 10) { x = x + y; }\n"
               "  if (x > 5) return x; else return 0;\n"
               "}\n",
               1);
    check_prog("int foo(void) { return 1; }\n", 0);
    check_prog("int ag_main(void) { return unknown; }\n", 0);

    /* Multiple functions + call */
    check_prog("int add(int a, int b) { return a + b; }\n"
               "int ag_main(void) { return add(20, 22); }\n",
               1);

    /* Globals + array access */
    check_prog("int g;\n"
               "int a[4];\n"
               "int ag_main(void) {\n"
               "  g = 1;\n"
               "  a[0] = 2;\n"
               "  a[1] = a[0] + g;\n"
               "  return a[1];\n"
               "}\n",
               1);

    /* for loop */
    check_prog("int ag_main(void) {\n"
               "  int s = 0;\n"
               "  int i;\n"
               "  for (i = 0; i < 5; i = i + 1) { s = s + i; }\n"
               "  return s;\n"
               "}\n",
               1);
    check_prog("int ag_main(void) {\n"
               "  int s = 0;\n"
               "  for (int i = 0; i < 3; i = i + 1) { s = s + 1; }\n"
               "  return s;\n"
               "}\n",
               1);

    /* String literal present in .AXE data */
    {
        const char *src =
            "int ag_main(void) {\n"
            "  ag_gfx_acquire();\n"
            "  ag_gfx_text(0, 0, \"HiCC\", 1, 0);\n"
            "  ag_gfx_release();\n"
            "  return 0;\n"
            "}\n";
        cc_result_t res;
        const int rc = cc_compile_to_axe(src, strlen(src), &res);
        AG_CHECK(rc == 0);
        if (rc == 0) {
            AG_CHECK(axe_contains(res.axe, res.axe_len, "HiCC"));
            /* NEEDS_GFX flag at offset 12 */
            const uint32_t flags = (uint32_t)res.axe[12] | ((uint32_t)res.axe[13] << 8) |
                                   ((uint32_t)res.axe[14] << 16) |
                                   ((uint32_t)res.axe[15] << 24);
            AG_CHECK((flags & 2u) != 0);
            AG_CHECK(res.axe[6] == ABI_MINOR_GFX && res.axe[7] == 0);
            cc_result_free(&res);
        } else {
            printf("     string/gfx compile: %s\n", res.err);
            cc_result_free(&res);
        }
    }

    /*
     * Precedence across a spill-free operand.  A shortcut that takes `b` for
     * the addition leaves `/ 2` with nothing to bind to, which the parser then
     * reports as a missing comma - two statements away from the real cause.
     */
    check_prog("int f(int a, int b) { return a + b / 2; }\n"
               "int ag_main(void) { return f(10, 4); }\n",
               1);
    check_prog("int g;\n"
               "int ag_main(void) {\n"
               "  int a = 8;\n"
               "  g = 3;\n"
               "  return a * g + a / g - a % g;\n"
               "}\n",
               1);
    check_prog("int h(int a, int b, int c) { return a; }\n"
               "int ag_main(void) {\n"
               "  int x = 2;\n"
               "  return h(x + 1 * 2, x - 1, x * 2 + 1);\n"
               "}\n",
               1);

    /* Six parameters: the sixth arrives in a7, which becomes the frame
     * pointer, so the prologue has to store it before it is overwritten. */
    check_prog("int s6(int a, int b, int c, int d, int e, int f) {\n"
               "  return a + b + c + d + e + f;\n"
               "}\n"
               "int ag_main(void) { return s6(1, 2, 3, 4, 5, 6); }\n",
               1);
    check_prog("int s7(int a, int b, int c, int d, int e, int f, int g) {\n"
               "  return a;\n"
               "}\n"
               "int ag_main(void) { return 0; }\n",
               0);

    /* Console and pins: phase A of the language backlog. */
    check_prog("int ag_main(void) {\n"
               "  ag_print(\"hi\\n\");\n"
               "  ag_print_int(42);\n"
               "  ag_printf(\"%d %d\\n\", 1, 2);\n"
               "  ag_cls();\n"
               "  ag_gotoxy(0, 0);\n"
               "  return 0;\n"
               "}\n",
               1);
    check_prog("int ag_main(void) {\n"
               "  ag_gpio_config(2, 1);\n"
               "  ag_gpio_write(2, 1);\n"
               "  return ag_gpio_read(2);\n"
               "}\n",
               1);
    check_prog("int ag_main(void) { ag_printf(); return 0; }\n", 0);

    check_minor("int ag_main(void) { return 1; }\n", ABI_MINOR_BASE);
    check_minor("int ag_main(void) { ag_print(\"x\"); return 0; }\n",
                ABI_MINOR_BASE);
    check_minor("int ag_main(void) { ag_gfx_clear(0); return 0; }\n",
                ABI_MINOR_GFX);
    check_minor("int ag_main(void) { return ag_btn(4); }\n", ABI_MINOR_BTN);
    check_minor("int ag_main(void) { ag_audio_open(); return 0; }\n",
                ABI_MINOR_AUDIO);
    check_minor("int ag_main(void) {\n"
                "  ag_gfx_clear(0);\n"
                "  ag_audio_close();\n"
                "  return 0;\n"
                "}\n",
                ABI_MINOR_AUDIO);

    /* Phase B: char storage, and a string walked through a pointer. */
    check_prog("int ag_main(void) {\n"
               "  int x = 6;\n"
               "  int m = x & 3;\n"
               "  return (m | 8) ^ (x >> 1);\n"
               "}\n",
               1);
    check_prog("char c;\n"
               "char buf[8];\n"
               "int ag_main(void) {\n"
               "  c = 'A';\n"
               "  buf[0] = c;\n"
               "  buf[1] = 0;\n"
               "  return buf[0];\n"
               "}\n",
               1);
    check_prog("int slen(char *s) {\n"
               "  int n = 0;\n"
               "  while (*s != 0) { n = n + 1; s = s + 1; }\n"
               "  return n;\n"
               "}\n"
               "int ag_main(void) { return slen(\"hello\"); }\n",
               1);
    check_prog("int ag_main(void) {\n"
               "  char *s;\n"
               "  int n = 0;\n"
               "  s = \"abc\";\n"
               "  while (s[n] != 0) { n = n + 1; }\n"
               "  return n;\n"
               "}\n",
               1);
    /* A pointer steps by its element, so an int pointer walks four bytes. */
    check_prog("int a[4];\n"
               "int ag_main(void) {\n"
               "  int *p;\n"
               "  a[1] = 7;\n"
               "  p = a;\n"
               "  p = p + 1;\n"
               "  return *p;\n"
               "}\n",
               1);
    check_prog("int ag_main(void) {\n"
               "  int n = 0;\n"
               "  int *p;\n"
               "  p = &n;\n"
               "  *p = 42;\n"
               "  return n;\n"
               "}\n",
               1);
    check_prog("char b[4];\n"
               "int ag_main(void) {\n"
               "  char *p;\n"
               "  p = &b[2];\n"
               "  *p = 'z';\n"
               "  return b[2];\n"
               "}\n",
               1);
    check_prog("int put(char c) { return c; }\n"
               "int ag_main(void) { return put('x'); }\n",
               1);
    /* An indexed pointer as the left side of an operator, not just alone. */
    check_prog("int ag_main(void) {\n"
               "  char *s;\n"
               "  s = \"ab\";\n"
               "  return s[0] + s[1] * 2;\n"
               "}\n",
               1);

    /* The example in apps/cc/README.md, so that it cannot rot unnoticed. */
    check_prog("struct pt { int x; int y; };\n"
               "int add(int a, int b) { return a + b; }\n"
               "int move(struct pt *p, int dx, int dy) {\n"
               "  p->x = p->x + dx;\n"
               "  p->y = p->y + dy;\n"
               "  return p->x * p->x + p->y * p->y;\n"
               "}\n"
               "int upper(char *s, char *out) {\n"
               "  int n = 0;\n"
               "  while (*s != 0) {\n"
               "    char c = *s;\n"
               "    if (c >= 'a' && c <= 'z') { c = c & ~32; }\n"
               "    out[n] = c;\n"
               "    n = n + 1;\n"
               "    s = s + 1;\n"
               "  }\n"
               "  out[n] = 0;\n"
               "  return n;\n"
               "}\n"
               "int ag_main(void) {\n"
               "  char buf[32];\n"
               "  struct pt p;\n"
               "  int s = 0;\n"
               "  int i;\n"
               "  for (i = 0; i < 10; i = i + 1) { s = s + add(i, 1); }\n"
               "  p.x = 0;\n"
               "  p.y = 0;\n"
               "  upper(\"argon cc\", buf);\n"
               "  ag_printf(\"%s %d %d\\n\", buf, s, move(&p, 3, 4));\n"
               "  return s;\n"
               "}\n",
               1);

    /* Structures: a field is read and written wherever a variable would be. */
    check_prog("struct pt { int x; int y; };\n"
               "int ag_main(void) {\n"
               "  struct pt p;\n"
               "  p.x = 3;\n"
               "  p.y = 4;\n"
               "  return p.x * p.x + p.y * p.y;\n"
               "}\n",
               1);
    check_prog("struct pt { int x; int y; };\n"
               "struct pt gp;\n"
               "int ag_main(void) { gp.x = 7; return gp.x; }\n",
               1);
    check_prog("struct pt { int x; int y; };\n"
               "int sum(struct pt *p) { return p->x + p->y; }\n"
               "int ag_main(void) {\n"
               "  struct pt p;\n"
               "  p.x = 1;\n"
               "  p.y = 2;\n"
               "  return sum(&p);\n"
               "}\n",
               1);
    check_prog("struct pt { int x; int y; };\n"
               "int ag_main(void) {\n"
               "  struct pt a[4];\n"
               "  int i;\n"
               "  int s = 0;\n"
               "  for (i = 0; i < 4; i = i + 1) { a[i].x = i; a[i].y = i * 2; }\n"
               "  for (i = 0; i < 4; i = i + 1) { s = s + a[i].x + a[i].y; }\n"
               "  return s;\n"
               "}\n",
               1);
    check_prog("struct pt { int x; int y; };\n"
               "struct pt ga[3];\n"
               "int ag_main(void) {\n"
               "  struct pt *p;\n"
               "  p = &ga[1];\n"
               "  p->x = 5;\n"
               "  p = p + 1;\n"
               "  p->x = 6;\n"
               "  return ga[1].x + ga[2].x;\n"
               "}\n",
               1);
    /* Mixed field sizes, an array inside a struct, and a struct inside one. */
    check_prog("struct rec { char tag; int n; char name[8]; };\n"
               "struct box { struct rec r; int k; };\n"
               "int ag_main(void) {\n"
               "  struct box b;\n"
               "  b.r.tag = 'a';\n"
               "  b.r.n = 9;\n"
               "  b.r.name[0] = 'x';\n"
               "  b.k = 1;\n"
               "  return b.r.tag + b.r.n + b.r.name[0] + b.k;\n"
               "}\n",
               1);
    /* A list node points at its own type, which the size is not known for yet. */
    check_prog("struct node { int v; struct node *next; };\n"
               "int ag_main(void) {\n"
               "  struct node a;\n"
               "  struct node b;\n"
               "  a.v = 1;\n"
               "  b.v = 2;\n"
               "  a.next = &b;\n"
               "  b.next = 0;\n"
               "  return a.next->v;\n"
               "}\n",
               1);
    /* An array of structs reached through a pointer, and a field of a field. */
    check_prog("struct pt { int x; int y; };\n"
               "struct pt ga[4];\n"
               "int ag_main(void) {\n"
               "  struct pt *p;\n"
               "  p = ga;\n"
               "  p[2].y = 11;\n"
               "  return ga[2].y;\n"
               "}\n",
               1);

    /* What the one-field type system cannot say, it refuses to guess. */
    check_prog("int ag_main(void) { int x = 1; return *x; }\n", 0);
    check_prog("int ag_main(void) { char *p; p = \"a\"; return **p; }\n", 0);
    check_prog("int ag_main(void) { char *p; p = \"a\"; return &p; }\n", 0);
    check_prog("char *g[4];\nint ag_main(void) { return 0; }\n", 0);
    check_prog("char *f(void) { return 0; }\nint ag_main(void) { return 0; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "int ag_main(void) { struct pt a; struct pt b; a = b; return 0; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "int f(struct pt p) { return p.x; }\n"
               "int ag_main(void) { return 0; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "struct pt f(void) { struct pt p; return p; }\n"
               "int ag_main(void) { return 0; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "int ag_main(void) { struct pt p; return p.z; }\n",
               0);
    check_prog("int ag_main(void) { int n = 1; return n.x; }\n", 0);
    check_prog("struct pt { int x; };\n"
               "int ag_main(void) { struct pt p; return p->x; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "int ag_main(void) { struct pt *p; return p.x; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "int ag_main(void) { struct pt p; return p; }\n",
               0);
    check_prog("struct pt { struct pt inner; int x; };\n"
               "int ag_main(void) { return 0; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "struct pt { int y; };\n"
               "int ag_main(void) { return 0; }\n",
               0);
    check_prog("int ag_main(void) { struct nope p; return 0; }\n", 0);
    check_prog("struct pt { int x; int x; };\n"
               "int ag_main(void) { return 0; }\n",
               0);
    check_prog("struct pt { int x; };\n"
               "int ag_main(void) { struct pt *p; return *p; }\n",
               0);

    /* Preprocessor: a name that stands for text, and text that is not there. */
    check_prog("#define N 4\n"
               "int a[N];\n"
               "int ag_main(void) { a[N - 1] = 7; return a[3]; }\n",
               1);
    check_prog("int a[N];\nint ag_main(void) { return 0; }\n", 0);
    check_prog("#define W 320\n"
               "#define H 240\n"
               "#define AREA (W * H)\n"
               "int ag_main(void) { return AREA / 100; }\n",
               1);
    /* An array size is a constant expression, which is what makes macros useful
     * there: `int b[W * H]` has to mean what it says. */
    check_prog("#define W 8\n"
               "#define H 4\n"
               "int buf[W * H];\n"
               "int ag_main(void) { buf[W * H - 1] = 1; return buf[31]; }\n",
               1);
    check_prog("int a[2 + 2];\nint ag_main(void) { return a[3]; }\n", 1);
    check_prog("int ag_main(void) { int a[(1 + 1) * 3]; a[5] = 1; return a[5]; }\n",
               1);
    check_prog("int a[0];\nint ag_main(void) { return 0; }\n", 0);
    check_prog("int a[2 - 5];\nint ag_main(void) { return 0; }\n", 0);
    check_prog("int n;\nint a[n];\nint ag_main(void) { return 0; }\n", 0);
    check_prog("struct s { int v[2 * 2]; };\n"
               "int ag_main(void) { struct s x; x.v[3] = 1; return x.v[3]; }\n",
               1);

    check_prog("#define MSG \"hello\"\n"
               "int ag_main(void) { ag_print(MSG); return 0; }\n",
               1);
    check_prog("#define EMPTY\n"
               "int ag_main(void) { EMPTY return 1; }\n",
               1);
    check_prog("#define N 4\n"
               "#undef N\n"
               "int a[N];\n"
               "int ag_main(void) { return 0; }\n",
               0);
    /* A macro with parameters, including one nested in its own argument. */
    check_prog("#define SQ(x) ((x) * (x))\n"
               "int ag_main(void) { return SQ(3) + SQ(SQ(2)); }\n",
               1);
    check_prog("#define ADD(a, b) ((a) + (b))\n"
               "int ag_main(void) { int n = ADD(1, ADD(2, 3)); return n; }\n",
               1);
    /* The argument is parenthesised, so precedence cannot leak either way. */
    check_prog("#define DBL(x) (x) + (x)\n"
               "int ag_main(void) { return DBL(1 + 1) * 2; }\n",
               1);
    check_prog("#define SQ(x) ((x) * (x))\n"
               "int ag_main(void) { return SQ(1, 2); }\n",
               0);
    check_prog("#define SQ(x) ((x) * (x))\n"
               "int ag_main(void) { return SQ; }\n",
               0);
    /* A macro that names itself expands once and then is only a name. */
    check_prog("int X;\n"
               "#define X X + 1\n"
               "int ag_main(void) { return X; }\n",
               1);
    check_prog("#define F(x) F(x) + 1\n"
               "int F;\n"
               "int ag_main(void) { return 0; }\n",
               1);
    check_prog("#define A B\n"
               "#define B A\n"
               "int A;\n"
               "int ag_main(void) { return A; }\n",
               1);

    /* Conditionals: the branch not taken is never seen by the parser. */
    check_prog("#define FEATURE\n"
               "#ifdef FEATURE\n"
               "int ag_main(void) { return 1; }\n"
               "#else\n"
               "this is not C at all ]]]\n"
               "#endif\n",
               1);
    check_prog("#ifdef FEATURE\n"
               "this is not C at all ]]]\n"
               "#else\n"
               "int ag_main(void) { return 2; }\n"
               "#endif\n",
               1);
    check_prog("#ifndef FEATURE\n"
               "int ag_main(void) { return 3; }\n"
               "#endif\n",
               1);
    check_prog("#define A\n"
               "#ifdef A\n"
               "#ifdef B\n"
               "junk ]]]\n"
               "#else\n"
               "int ag_main(void) { return 4; }\n"
               "#endif\n"
               "#endif\n",
               1);
    /* A conditional inside a skipped branch must not end the skip early. */
    check_prog("#ifdef NOPE\n"
               "#ifdef ALSO_NOPE\n"
               "#endif\n"
               "junk ]]]\n"
               "#endif\n"
               "int ag_main(void) { return 5; }\n",
               1);
    check_prog("int ag_main(void) { return 0; }\n#endif\n", 0);
    check_prog("#ifdef A\nint ag_main(void) { return 0; }\n", 0);
    check_prog("#else\nint ag_main(void) { return 0; }\n", 0);
    check_prog("#nonsense\nint ag_main(void) { return 0; }\n", 0);
    check_prog("#define\nint ag_main(void) { return 0; }\n", 0);

    /* #include, against a filesystem that is a table in this test. */
    {
        static const fake_file_t files[] = {
            {"pt.h", "#ifndef PT_H\n"
                     "#define PT_H\n"
                     "struct pt { int x; int y; };\n"
                     "#define ORIGIN 0\n"
                     "#endif\n"},
            {"sub.h", "#include \"pt.h\"\n"
                      "int subhelper(struct pt *p) { return p->x; }\n"},
        };
        const int n = (int)(sizeof(files) / sizeof(files[0]));
        check_prog_inc("#include \"pt.h\"\n"
                       "int ag_main(void) {\n"
                       "  struct pt p;\n"
                       "  p.x = ORIGIN;\n"
                       "  p.y = 1;\n"
                       "  return p.y;\n"
                       "}\n",
                       1, files, n);
        /* Included twice, directly and through another file: the guard holds. */
        check_prog_inc("#include \"pt.h\"\n"
                       "#include \"sub.h\"\n"
                       "int ag_main(void) {\n"
                       "  struct pt p;\n"
                       "  p.x = 2;\n"
                       "  return subhelper(&p);\n"
                       "}\n",
                       1, files, n);
        check_prog_inc("#include \"missing.h\"\n"
                       "int ag_main(void) { return 0; }\n",
                       0, files, n);
        check_prog_inc("#include pt.h\n"
                       "int ag_main(void) { return 0; }\n",
                       0, files, n);
    }
    /* Without a reader there is no filesystem to reach, and it says so. */
    check_prog("#include \"pt.h\"\nint ag_main(void) { return 0; }\n", 0);

    /* An error inside an included file names that file, not the main one. */
    {
        static const fake_file_t files[] = {
            {"bad.h", "int oops(void) { int x; x = ; return x; }\n"},
        };
        g_fake_files = files;
        g_fake_nfiles = 1;
        cc_result_t res;
        const char *src = "#include \"bad.h\"\n"
                          "int ag_main(void) { return 0; }\n";
        AG_CHECK(cc_compile_to_axe_inc(src, strlen(src), fake_reader, NULL,
                                       &res) != 0);
        AG_CHECK(strstr(res.err, "bad.h") != NULL);
        cc_result_free(&res);
        g_fake_files = NULL;
        g_fake_nfiles = 0;
    }

    /* `||` / `&&` must not leave a9 dirty after the right side (fast path). */
    check_expr("0 || 1 != 2", 1);
    check_expr("1 && 2 != 3", 1);
    check_expr("0 || 5 == 5", 1);
    check_expr("1 && 5 == 6", 0);

    /* Phase D: mem / fs / dev builtins compile (offsets checked above). */
    check_prog("#define AG_O_RDONLY 0\n"
               "int ag_main(void) {\n"
               "  char *p;\n"
               "  int h;\n"
               "  int n;\n"
               "  char buf[8];\n"
               "  p = ag_malloc(16);\n"
               "  if (p == 0) { return 1; }\n"
               "  p = ag_realloc(p, 32);\n"
               "  ag_free(p);\n"
               "  h = ag_open(\"t:\\\\msg.txt\", AG_O_RDONLY);\n"
               "  if (h < 0) { return 2; }\n"
               "  n = ag_read(h, buf, 8);\n"
               "  ag_close(h);\n"
               "  return n;\n"
               "}\n",
               1);
    check_prog("int ag_main(void) {\n"
               "  int h;\n"
               "  char buf[4];\n"
               "  h = ag_dev_open(\"pcmnull\");\n"
               "  if (h < 0) { return 1; }\n"
               "  ag_dev_write(h, buf, 4);\n"
               "  ag_dev_close(h);\n"
               "  return 0;\n"
               "}\n",
               1);
    check_prog("struct dent { char name[256]; int size_lo; int size_hi; "
               "int mtime_lo; int mtime_hi; int attr; };\n"
               "int ag_main(void) {\n"
               "  int d;\n"
               "  struct dent e;\n"
               "  d = ag_opendir(\"t:\\\\\");\n"
               "  if (d < 0) { return 1; }\n"
               "  ag_readdir(d, &e);\n"
               "  ag_closedir(d);\n"
               "  return 0;\n"
               "}\n",
               1);

    /* An error says where it is; a compiler with no host behind it must. */
    {
        cc_result_t res;
        const char *src = "int ag_main(void) {\n"
                          "  int x = 1;\n"
                          "  x = ;\n"
                          "  return x;\n"
                          "}\n";
        AG_CHECK(cc_compile_to_axe(src, strlen(src), &res) != 0);
        AG_CHECK(strstr(res.err, "line 3") != NULL);
        cc_result_free(&res);
    }

    check_asteroids_example();
    check_dx7nofx_example();
}
