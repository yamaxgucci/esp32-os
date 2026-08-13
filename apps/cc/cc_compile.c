/*
 * ArgonOS - Tiny C → Xtensa .AXE (stack-machine codegen).
 *
 * Subset: int/void/char/struct, locals, globals, arrays, return, if/else,
 * while/for/do, break/continue, switch (if-chain), typedef/enum/sizeof,
 * ++/-- and compound assigns, ?: , hex/oct constants, string literals,
 * multiple functions (callx8), and ABI builtins for time/input/gfx/audio.
 * Entry must be named ag_main (app) or ag_driver_init (driver / .SYS).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "cc_compile.h"

#ifdef AG_HOST
#include <stdlib.h>
#include <string.h>

#define CC_ALLOC malloc
#define CC_FREE free
#define CC_STRCMP strcmp
#else
#include <argon/argon.h>
#include <argon/libc.h>

static void *cc_alloc(size_t n)
{
    return ag_malloc(n);
}

static void cc_free(void *p)
{
    ag_free(p);
}

#define CC_ALLOC cc_alloc
#define CC_FREE cc_free
#define CC_STRCMP ag_strcmp
#endif

#define CODE_BASE 0x42000000u
#define DATA_BASE 0x3C000000u
#define AXE_HDR   176u

/*
 * Code may exceed the IRAM arena (CONFIG_ARGON_APP_ARENA_KB, 192 on S3): the
 * loader places oversized images in flash XIP.  Static data and the compiler's
 * own working buffers live in PSRAM.  These ceilings are emit-time caps so a
 * guest program cannot blow the format / host-test budgets by accident.
 */
#define CODE_CAP  (512u * 1024u)
#define DATA_CAP  (512u * 1024u)
#define LIT_CAP   1024
#define MAX_LIT_SITES 8192
#define MAX_GLOBALS 512
#define MAX_FUNCS 512
#define MAX_RELOCS 8192
#define MAX_NAME   32
#define MAX_STR    256
#define MAX_STRUCTS 24
#define MAX_FIELDS  24
#define MAX_ARRAY   (1 << 20)
#define MAX_MACROS  256
#define MAX_MPARAMS 6
#define MAX_INCLUDES 16
#define MAX_FRAMES  12
#define MAX_PATH    64
#define EXP_CAP     4096
#define MAX_TYPEDEFS 32
#define MAX_ENUMS 128
#define MAX_LOOP_DEPTH 16
#define MAX_BREAK_SITES 32
#define MAX_CASES 32

/*
 * Frame layout, from the frame pointer up: spill slots, then locals, then the
 * 16 bytes at the top of every frame where a caller's a4-a7 are saved on window
 * overflow (32 given, for alignment and margin).  Spills come first so that a
 * local's offset is known the moment it is declared - the size of the locals
 * area is not known until the body has been parsed, and offsets are baked into
 * instructions as they are emitted.
 *
 * l32i/s32i reach 1020 bytes, which is what caps the locals area: the highest
 * word must stay addressable from the frame pointer.
 */
#define SPILL_SLOTS 24
#define SPILL_BYTES (SPILL_SLOTS * 4)
#define MAX_LOCALS 96
#define MAX_LOCAL_WORDS 224
#define FRAME_TOP_SAVE 32
#define MAX_PARAMS 6

#define AG_AXE_R_IN_DATA 0x1u
#define AG_AXE_R_TO_DATA 0x2u
#define AG_AXE_NEEDS_GFX (1u << 1)
#define AG_AXE_DRIVER (1u << 3)
#define AG_AXE_NEEDS_AUDIO (1u << 6)

#define RET_VOID 0
#define RET_BOOL 1
#define RET_RAW  2

enum {
    LIT_IMM = 0,
    LIT_CODE_OFF = 1,
    LIT_DATA_OFF = 2
};

static void cpy_err(char *dst, size_t n, const char *msg)
{
    size_t i = 0;
    if (dst == NULL || n == 0) {
        return;
    }
    while (msg[i] != '\0' && i + 1 < n) {
        dst[i] = msg[i];
        i++;
    }
    dst[i] = '\0';
}

static void cpy_num(char *dst, size_t n, int v)
{
    char rev[12];
    int  k = 0;
    size_t at = 0;
    while (dst[at] != '\0' && at + 1 < n) {
        at++;
    }
    if (v <= 0) {
        rev[k++] = '0';
    }
    while (v > 0 && k < (int)sizeof(rev)) {
        rev[k++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (k > 0 && at + 1 < n) {
        dst[at++] = rev[--k];
    }
    dst[at] = '\0';
}

/* ---- lexer ------------------------------------------------------------- */

typedef enum {
    T_EOF = 0,
    T_IDENT,
    T_NUM,
    T_STRING,
    T_INT,
    T_CHAR,
    T_VOID,
    T_RETURN,
    T_IF,
    T_ELSE,
    T_WHILE,
    T_FOR,
    T_DO,
    T_BREAK,
    T_CONTINUE,
    T_SWITCH,
    T_CASE,
    T_DEFAULT,
    T_TYPEDEF,
    T_ENUM,
    T_SIZEOF,
    T_CONST,
    T_VOLATILE,
    T_LPAREN,
    T_RPAREN,
    T_LBRACE,
    T_RBRACE,
    T_LBRACK,
    T_RBRACK,
    T_SEMI,
    T_COMMA,
    T_ASSIGN,
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_PERCENT,
    T_LT,
    T_GT,
    T_LE,
    T_GE,
    T_EQ,
    T_NE,
    T_ANDAND,
    T_OROR,
    T_NOT,
    T_AMP,
    T_PIPE,
    T_CARET,
    T_TILDE,
    T_SHL,
    T_SHR,
    T_PLUSPLUS,
    T_MINUSMINUS,
    T_PLUS_EQ,
    T_MINUS_EQ,
    T_STAR_EQ,
    T_SLASH_EQ,
    T_PERCENT_EQ,
    T_AMP_EQ,
    T_PIPE_EQ,
    T_CARET_EQ,
    T_SHL_EQ,
    T_SHR_EQ,
    T_QUESTION,
    T_COLON,
    T_STRUCT,
    T_DOT,
    T_ARROW,
    T_BAD
} tok_t;

/*
 * The preprocessor, such as it is.  A macro body is not copied anywhere: it is
 * a span of the source (or of an included file, which is kept for the whole
 * compile), and expanding one means reading from that span for a while and then
 * going back.  Only a macro with parameters has to build text, and that goes on
 * a stack in `exp` which unwinds with the frame that used it.
 */
typedef struct {
    char        name[MAX_NAME];
    char        params[MAX_MPARAMS][MAX_NAME];
    int         nparams; /* -1 when the macro takes no argument list */
    int         live;
    const char *body;
    size_t      body_len;
} macro_t;

typedef struct {
    char   path[MAX_PATH];
    char  *text;
    size_t len;
} incl_t;

/*
 * Where the lexer was before it stepped into a macro body or an included file.
 * `mac` is what it stepped *into*, so that a macro cannot expand inside its own
 * expansion, and `exp_mark` is where the argument text for it starts.
 */
typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    size_t      exp_mark;
    int         mac;
    int         incl;
    /*
     * Where the arguments landed in the expansion.  A macro is not expanded
     * inside its own body, but text that came from an argument is the caller's,
     * not the body's: `SQ(SQ(2))` has to work, while `#define X X + 1` still
     * has to stop.  These ranges are what tells the two apart.
     */
    int      nargr;
    uint16_t argr[MAX_MPARAMS][2];
} frame_t;

typedef struct {
    int        nmacros;
    macro_t    macros[MAX_MACROS];
    int        nincls;
    incl_t     incls[MAX_INCLUDES];
    frame_t    frames[MAX_FRAMES];
    char       exp[EXP_CAP];
    cc_read_file_fn reader;
    void      *reader_ctx;
    /* #pragma drv "NAME" "VER" "AUTHOR" — header identity for .SYS images. */
    int        has_drv;
    char       drv_name[32];
    char       drv_ver[16];
    char       drv_author[32];
    /* #pragma appstack N / #pragma appheap N — AXE header sizes (0 = default). */
    uint32_t   app_stack;
    uint32_t   app_heap;
} pp_t;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    tok_t       tok;
    char        text[MAX_NAME];
    char        strbuf[MAX_STR];
    int32_t     num;
    int         err;
    char        errmsg[160];
    /*
     * Preprocessor state that a peek must be able to undo, and so lives here
     * rather than in pp_t: peek_tok saves and restores the whole lexer.  What
     * stays in pp_t is either idempotent (a macro defined twice is the same
     * macro) or a cache, so re-running a directive after a peek changes
     * nothing.
     */
    pp_t       *pp;
    int         nframes;
    int         cond_depth;
    int         cur_incl;
    size_t      exp_top;
} lex_t;

static int is_id0(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_id(char c)
{
    return is_id0(c) || (c >= '0' && c <= '9');
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int same_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * Where the error is, counted when it happens rather than tracked as the lexer
 * runs.  A compiler that lives on the machine it compiles for has no host and
 * no IDE behind it: "expected ';'" without a line is a search by hand.
 */
static void cpy_err_at(char *dst, size_t n, const char *msg, const lex_t *L)
{
    cpy_err(dst, n, msg);
    if (L == NULL || L->src == NULL) {
        return;
    }
    /*
     * A line inside a macro body is no help to anyone: walk back out of any
     * expansion to the text of a file, and count there.  What is left is where
     * the user would look - the line that used the macro.
     */
    const char *src = L->src;
    size_t      pos = L->pos;
    if (L->pp != NULL) {
        int k = L->nframes;
        while (k > 0 && L->pp->frames[k - 1].mac >= 0) {
            k--;
            src = L->pp->frames[k].src;
            pos = L->pp->frames[k].pos;
        }
    }
    int line = 1;
    for (size_t i = 0; i < pos; i++) {
        if (src[i] == '\n') {
            line++;
        }
    }
    size_t at = 0;
    while (dst[at] != '\0' && at + 1 < n) {
        at++;
    }
    const char *tail = " at line ";
    for (size_t i = 0; tail[i] != '\0' && at + 1 < n; i++) {
        dst[at++] = tail[i];
    }
    dst[at] = '\0';
    cpy_num(dst, n, line);
    if (L->pp == NULL || L->cur_incl < 0) {
        return;
    }
    at = 0;
    while (dst[at] != '\0' && at + 1 < n) {
        at++;
    }
    const char *of = " of ";
    for (size_t i = 0; of[i] != '\0' && at + 1 < n; i++) {
        dst[at++] = of[i];
    }
    const char *path = L->pp->incls[L->cur_incl].path;
    for (size_t i = 0; path[i] != '\0' && at + 1 < n; i++) {
        dst[at++] = path[i];
    }
    dst[at] = '\0';
}

static void lex_fail(lex_t *L, const char *msg)
{
    if (L->err) {
        return;
    }
    L->err = 1;
    cpy_err_at(L->errmsg, sizeof(L->errmsg), msg, L);
    L->tok = T_BAD;
}

/* ---- preprocessor ------------------------------------------------------ */

static int pp_find(const lex_t *L, const char *name)
{
    if (L->pp == NULL) {
        return -1;
    }
    for (int i = 0; i < L->pp->nmacros; i++) {
        if (L->pp->macros[i].live &&
            CC_STRCMP(L->pp->macros[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * A macro already being expanded is not expanded again, as C requires - unless
 * the name being looked at came from an argument, which is the caller's text
 * and was never part of the body.
 */
static int pp_active(const lex_t *L, int mi)
{
    for (int i = 0; i < L->nframes; i++) {
        const frame_t *f = &L->pp->frames[i];
        if (f->mac != mi) {
            continue;
        }
        const size_t at = (i + 1 < L->nframes) ? L->pp->frames[i + 1].pos
                                               : L->pos;
        int in_arg = 0;
        for (int k = 0; k < f->nargr; k++) {
            if (at >= f->argr[k][0] && at < f->argr[k][1]) {
                in_arg = 1;
                break;
            }
        }
        if (!in_arg) {
            return 1;
        }
    }
    return 0;
}

static void pp_push(lex_t *L, const char *src, size_t len, int mac,
                    size_t exp_mark)
{
    if (L->nframes >= MAX_FRAMES) {
        lex_fail(L, "includes or macros nested too deep");
        return;
    }
    frame_t *f = &L->pp->frames[L->nframes++];
    f->src = L->src;
    f->len = L->len;
    f->pos = L->pos;
    f->exp_mark = exp_mark;
    f->mac = mac;
    f->incl = L->cur_incl;
    f->nargr = 0;
    L->src = src;
    L->len = len;
    L->pos = 0;
}

static void pp_pop(lex_t *L)
{
    const frame_t *f = &L->pp->frames[--L->nframes];
    L->src = f->src;
    L->len = f->len;
    L->pos = f->pos;
    L->exp_top = f->exp_mark;
    L->cur_incl = f->incl;
}

static void skip_line(lex_t *L)
{
    while (L->pos < L->len && L->src[L->pos] != '\n') {
        L->pos++;
    }
}

static void skip_blanks(lex_t *L)
{
    while (L->pos < L->len &&
           (L->src[L->pos] == ' ' || L->src[L->pos] == '\t')) {
        L->pos++;
    }
}

/* A directive's name or argument; anything that is not one is an error there. */
static int pp_word(lex_t *L, char *out, size_t cap)
{
    skip_blanks(L);
    size_t n = 0;
    while (L->pos < L->len && is_id(L->src[L->pos])) {
        if (n + 1 < cap) {
            out[n++] = L->src[L->pos];
        }
        L->pos++;
    }
    out[n] = '\0';
    return n > 0;
}

/* A quoted string argument of a directive (`#pragma drv "ECHO" …`). */
static int pp_qstr(lex_t *L, char *out, size_t cap)
{
    skip_blanks(L);
    if (L->pos >= L->len || L->src[L->pos] != '"') {
        return 0;
    }
    L->pos++;
    size_t n = 0;
    while (L->pos < L->len && L->src[L->pos] != '"' &&
           L->src[L->pos] != '\n') {
        if (n + 1 < cap) {
            out[n++] = L->src[L->pos];
        }
        L->pos++;
    }
    out[n] = '\0';
    if (L->pos >= L->len || L->src[L->pos] != '"') {
        lex_fail(L, "unterminated string in directive");
        return 0;
    }
    L->pos++;
    return 1;
}

/* Decimal / 0x hex size for `#pragma appheap` / `appstack`. */
static int pp_u32(lex_t *L, uint32_t *out)
{
    skip_blanks(L);
    if (L->pos >= L->len || !is_digit(L->src[L->pos])) {
        return 0;
    }
    uint32_t v = 0;
    if (L->pos + 1 < L->len && L->src[L->pos] == '0' &&
        (L->src[L->pos + 1] == 'x' || L->src[L->pos + 1] == 'X')) {
        L->pos += 2;
        int any = 0;
        while (L->pos < L->len) {
            const char c = L->src[L->pos];
            int d = -1;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10;
            }
            if (d < 0) {
                break;
            }
            v = (v << 4) | (uint32_t)d;
            L->pos++;
            any = 1;
        }
        if (!any) {
            return 0;
        }
    } else {
        while (L->pos < L->len && is_digit(L->src[L->pos])) {
            v = v * 10u + (uint32_t)(L->src[L->pos] - '0');
            L->pos++;
        }
    }
    *out = v;
    return 1;
}

static void pp_define(lex_t *L)
{
    char name[MAX_NAME];
    if (!pp_word(L, name, sizeof(name))) {
        lex_fail(L, "expected a macro name");
        return;
    }
    int mi = pp_find(L, name);
    if (mi < 0) {
        if (L->pp->nmacros >= MAX_MACROS) {
            lex_fail(L, "too many macros");
            return;
        }
        mi = L->pp->nmacros++;
        cpy_err(L->pp->macros[mi].name, MAX_NAME, name);
    }
    macro_t *m = &L->pp->macros[mi];
    m->live = 1;
    m->nparams = -1;
    /*
     * `#define f(x)` takes arguments only when the paren touches the name;
     * with a space it is an object-like macro whose body starts with one.
     */
    if (L->pos < L->len && L->src[L->pos] == '(') {
        L->pos++;
        m->nparams = 0;
        skip_blanks(L);
        if (L->pos < L->len && L->src[L->pos] == ')') {
            L->pos++;
        } else {
            for (;;) {
                if (m->nparams >= MAX_MPARAMS) {
                    lex_fail(L, "too many macro parameters");
                    return;
                }
                if (!pp_word(L, m->params[m->nparams], MAX_NAME)) {
                    lex_fail(L, "expected a macro parameter");
                    return;
                }
                m->nparams++;
                skip_blanks(L);
                if (L->pos < L->len && L->src[L->pos] == ',') {
                    L->pos++;
                    continue;
                }
                if (L->pos < L->len && L->src[L->pos] == ')') {
                    L->pos++;
                    break;
                }
                lex_fail(L, "expected ',' or ')' in macro parameters");
                return;
            }
        }
    }
    skip_blanks(L);
    const size_t start = L->pos;
    skip_line(L);
    size_t end = L->pos;
    while (end > start && (L->src[end - 1] == ' ' || L->src[end - 1] == '\t' ||
                           L->src[end - 1] == '\r')) {
        end--;
    }
    m->body = L->src + start;
    m->body_len = end - start;
}

/*
 * Everything up to the `#else` or `#endif` that closes this one.  Directives
 * inside are not obeyed, only counted, so a conditional nested in a branch that
 * was not taken cannot end the one that skipped it.
 */
static void pp_skip_branch(lex_t *L, int stop_at_else)
{
    int depth = 0;
    for (;;) {
        skip_line(L);
        if (L->pos >= L->len) {
            lex_fail(L, "unterminated #ifdef");
            return;
        }
        L->pos++;
        skip_blanks(L);
        if (L->pos >= L->len || L->src[L->pos] != '#') {
            continue;
        }
        L->pos++;
        char word[MAX_NAME];
        if (!pp_word(L, word, sizeof(word))) {
            continue;
        }
        if (CC_STRCMP(word, "ifdef") == 0 || CC_STRCMP(word, "ifndef") == 0) {
            depth++;
            continue;
        }
        if (CC_STRCMP(word, "endif") == 0) {
            if (depth == 0) {
                skip_line(L);
                return;
            }
            depth--;
            continue;
        }
        if (CC_STRCMP(word, "else") == 0 && depth == 0 && stop_at_else) {
            L->cond_depth++;
            skip_line(L);
            return;
        }
    }
}

static void pp_include(lex_t *L)
{
    skip_blanks(L);
    if (L->pos >= L->len || L->src[L->pos] != '"') {
        lex_fail(L, "expected \"file\" after #include");
        return;
    }
    L->pos++;
    char path[MAX_PATH];
    size_t n = 0;
    while (L->pos < L->len && L->src[L->pos] != '"' && L->src[L->pos] != '\n') {
        if (n + 1 >= sizeof(path)) {
            lex_fail(L, "include path too long");
            return;
        }
        path[n++] = L->src[L->pos++];
    }
    path[n] = '\0';
    if (L->pos >= L->len || L->src[L->pos] != '"') {
        lex_fail(L, "expected \"file\" after #include");
        return;
    }
    L->pos++;
    skip_line(L);
    if (L->pp->reader == NULL) {
        lex_fail(L, "#include is not available here");
        return;
    }
    /*
     * Read once per path: an include guard stops the second copy from being
     * compiled, but nothing else would stop it from being read again - and a
     * peek across the directive would read it a third time.
     */
    for (int i = 0; i < L->pp->nincls; i++) {
        if (CC_STRCMP(L->pp->incls[i].path, path) == 0) {
            pp_push(L, L->pp->incls[i].text, L->pp->incls[i].len, -1,
                    L->exp_top);
            L->cur_incl = i;
            return;
        }
    }
    if (L->pp->nincls >= MAX_INCLUDES) {
        lex_fail(L, "too many included files");
        return;
    }
    char  *text = NULL;
    size_t len = 0;
    if (L->pp->reader(L->pp->reader_ctx, path, &text, &len) != 0 ||
        text == NULL) {
        lex_fail(L, "cannot read the included file");
        return;
    }
    const int idx = L->pp->nincls++;
    incl_t   *inc = &L->pp->incls[idx];
    cpy_err(inc->path, sizeof(inc->path), path);
    inc->text = text;
    inc->len = len;
    pp_push(L, text, len, -1, L->exp_top);
    L->cur_incl = idx;
}

/* One directive, the '#' already behind us.  Returns with the line consumed. */
static void pp_directive(lex_t *L)
{
    L->pos++;
    char word[MAX_NAME];
    if (!pp_word(L, word, sizeof(word))) {
        lex_fail(L, "expected a directive after '#'");
        return;
    }
    if (CC_STRCMP(word, "define") == 0) {
        pp_define(L);
        return;
    }
    if (CC_STRCMP(word, "undef") == 0) {
        char name[MAX_NAME];
        if (!pp_word(L, name, sizeof(name))) {
            lex_fail(L, "expected a macro name");
            return;
        }
        const int mi = pp_find(L, name);
        if (mi >= 0) {
            L->pp->macros[mi].live = 0;
        }
        skip_line(L);
        return;
    }
    if (CC_STRCMP(word, "ifdef") == 0 || CC_STRCMP(word, "ifndef") == 0) {
        char name[MAX_NAME];
        if (!pp_word(L, name, sizeof(name))) {
            lex_fail(L, "expected a macro name");
            return;
        }
        skip_line(L);
        int taken = pp_find(L, name) >= 0;
        if (word[2] == 'n') {
            taken = !taken;
        }
        if (taken) {
            L->cond_depth++;
        } else {
            pp_skip_branch(L, 1);
        }
        return;
    }
    if (CC_STRCMP(word, "else") == 0) {
        if (L->cond_depth == 0) {
            lex_fail(L, "#else without #ifdef");
            return;
        }
        L->cond_depth--;
        pp_skip_branch(L, 0);
        return;
    }
    if (CC_STRCMP(word, "endif") == 0) {
        if (L->cond_depth == 0) {
            lex_fail(L, "#endif without #ifdef");
            return;
        }
        L->cond_depth--;
        skip_line(L);
        return;
    }
    if (CC_STRCMP(word, "include") == 0) {
        pp_include(L);
        return;
    }
    if (CC_STRCMP(word, "pragma") == 0) {
        char kind[MAX_NAME];
        if (!pp_word(L, kind, sizeof(kind))) {
            lex_fail(L, "expected a pragma name");
            return;
        }
        if (CC_STRCMP(kind, "drv") == 0) {
            if (!pp_qstr(L, L->pp->drv_name, sizeof(L->pp->drv_name)) ||
                !pp_qstr(L, L->pp->drv_ver, sizeof(L->pp->drv_ver)) ||
                !pp_qstr(L, L->pp->drv_author, sizeof(L->pp->drv_author))) {
                if (!L->err) {
                    lex_fail(L, "expected #pragma drv \"NAME\" \"VER\" \"AUTHOR\"");
                }
                return;
            }
            if (L->pp->drv_name[0] == '\0') {
                lex_fail(L, "empty driver name in #pragma drv");
                return;
            }
            L->pp->has_drv = 1;
            skip_line(L);
            return;
        }
        if (CC_STRCMP(kind, "appstack") == 0) {
            uint32_t n = 0;
            if (!pp_u32(L, &n) || n == 0) {
                lex_fail(L, "expected #pragma appstack N");
                return;
            }
            L->pp->app_stack = n;
            skip_line(L);
            return;
        }
        if (CC_STRCMP(kind, "appheap") == 0) {
            uint32_t n = 0;
            if (!pp_u32(L, &n) || n == 0) {
                lex_fail(L, "expected #pragma appheap N");
                return;
            }
            L->pp->app_heap = n;
            skip_line(L);
            return;
        }
        lex_fail(L, "unknown pragma");
        return;
    }
    lex_fail(L, "unknown directive");
}

/* A '#' counts as a directive only when nothing but blanks precede it. */
static int at_line_start(const lex_t *L)
{
    size_t i = L->pos;
    while (i > 0) {
        const char c = L->src[i - 1];
        if (c == '\n') {
            return 1;
        }
        if (c != ' ' && c != '\t' && c != '\r') {
            return 0;
        }
        i--;
    }
    return 1;
}

static void lex_skip(lex_t *L)
{
    while (L->pos < L->len) {
        const char c = L->src[L->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            L->pos++;
            continue;
        }
        if (c == '#' && L->pp != NULL && at_line_start(L)) {
            pp_directive(L);
            if (L->err) {
                return;
            }
            continue;
        }
        if (c == '/' && L->pos + 1 < L->len && L->src[L->pos + 1] == '/') {
            L->pos += 2;
            while (L->pos < L->len && L->src[L->pos] != '\n') {
                L->pos++;
            }
            continue;
        }
        if (c == '/' && L->pos + 1 < L->len && L->src[L->pos + 1] == '*') {
            L->pos += 2;
            while (L->pos + 1 < L->len &&
                   !(L->src[L->pos] == '*' && L->src[L->pos + 1] == '/')) {
                L->pos++;
            }
            if (L->pos + 1 >= L->len) {
                lex_fail(L, "unterminated comment");
                return;
            }
            L->pos += 2;
            continue;
        }
        break;
    }
}

static tok_t kw(const char *s)
{
    if (CC_STRCMP(s, "int") == 0) {
        return T_INT;
    }
    if (CC_STRCMP(s, "char") == 0) {
        return T_CHAR;
    }
    if (CC_STRCMP(s, "struct") == 0) {
        return T_STRUCT;
    }
    if (CC_STRCMP(s, "void") == 0) {
        return T_VOID;
    }
    if (CC_STRCMP(s, "return") == 0) {
        return T_RETURN;
    }
    if (CC_STRCMP(s, "if") == 0) {
        return T_IF;
    }
    if (CC_STRCMP(s, "else") == 0) {
        return T_ELSE;
    }
    if (CC_STRCMP(s, "while") == 0) {
        return T_WHILE;
    }
    if (CC_STRCMP(s, "for") == 0) {
        return T_FOR;
    }
    if (CC_STRCMP(s, "do") == 0) {
        return T_DO;
    }
    if (CC_STRCMP(s, "break") == 0) {
        return T_BREAK;
    }
    if (CC_STRCMP(s, "continue") == 0) {
        return T_CONTINUE;
    }
    if (CC_STRCMP(s, "switch") == 0) {
        return T_SWITCH;
    }
    if (CC_STRCMP(s, "case") == 0) {
        return T_CASE;
    }
    if (CC_STRCMP(s, "default") == 0) {
        return T_DEFAULT;
    }
    if (CC_STRCMP(s, "typedef") == 0) {
        return T_TYPEDEF;
    }
    if (CC_STRCMP(s, "enum") == 0) {
        return T_ENUM;
    }
    if (CC_STRCMP(s, "sizeof") == 0) {
        return T_SIZEOF;
    }
    if (CC_STRCMP(s, "const") == 0) {
        return T_CONST;
    }
    if (CC_STRCMP(s, "volatile") == 0) {
        return T_VOLATILE;
    }
    return T_IDENT;
}

/* The character an escape stands for; anything unknown stands for itself. */
static char lex_escape(char e)
{
    if (e == 'n') {
        return '\n';
    }
    if (e == 't') {
        return '\t';
    }
    if (e == 'r') {
        return '\r';
    }
    if (e == '0') {
        return '\0';
    }
    return e;
}

/*
 * The text a macro stands for, with its parameters replaced by the arguments
 * written at the call.  Built on a stack that unwinds when the expansion ends,
 * so a macro used inside an argument of another one costs only its own text.
 */
static void pp_expand_args(lex_t *L, int mi)
{
    const macro_t *m = &L->pp->macros[mi];
    const char *args[MAX_MPARAMS];
    size_t      arglen[MAX_MPARAMS];
    int         nargs = 0;

    L->pos++; /* the '(' */
    if (m->nparams > 0) {
        for (;;) {
            if (nargs >= MAX_MPARAMS) {
                lex_fail(L, "too many macro arguments");
                return;
            }
            int depth = 0;
            const size_t start = L->pos;
            while (L->pos < L->len) {
                const char c = L->src[L->pos];
                if (c == '(') {
                    depth++;
                } else if (c == ')') {
                    if (depth == 0) {
                        break;
                    }
                    depth--;
                } else if (c == ',' && depth == 0) {
                    break;
                }
                L->pos++;
            }
            if (L->pos >= L->len) {
                lex_fail(L, "unterminated macro arguments");
                return;
            }
            args[nargs] = L->src + start;
            arglen[nargs] = L->pos - start;
            nargs++;
            if (L->src[L->pos] == ',') {
                L->pos++;
                continue;
            }
            break;
        }
    }
    if (L->pos >= L->len || L->src[L->pos] != ')') {
        lex_fail(L, "expected ')' after macro arguments");
        return;
    }
    L->pos++;
    if (nargs != m->nparams) {
        lex_fail(L, "wrong number of macro arguments");
        return;
    }

    const size_t mark = L->exp_top;
    size_t       top = L->exp_top;
    size_t       i = 0;
    uint16_t     argr[MAX_MPARAMS][2];
    int          nargr = 0;
    while (i < m->body_len) {
        const char c = m->body[i];
        /* A name inside a literal is text, not a parameter. */
        if (c == '"' || c == '\'') {
            const char quote = c;
            do {
                if (top + 1 >= EXP_CAP) {
                    lex_fail(L, "macro expansion too long");
                    return;
                }
                L->pp->exp[top++] = m->body[i];
                if (m->body[i] == '\\' && i + 1 < m->body_len) {
                    L->pp->exp[top++] = m->body[++i];
                }
                i++;
            } while (i < m->body_len && m->body[i] != quote);
            continue;
        }
        if (is_id0(c)) {
            size_t n = 0;
            while (i + n < m->body_len && is_id(m->body[i + n])) {
                n++;
            }
            int pi = -1;
            for (int k = 0; k < m->nparams; k++) {
                size_t plen = 0;
                while (m->params[k][plen] != '\0') {
                    plen++;
                }
                if (plen == n && same_n(m->params[k], m->body + i, n)) {
                    pi = k;
                    break;
                }
            }
            const char  *from = (pi >= 0) ? args[pi] : m->body + i;
            const size_t flen = (pi >= 0) ? arglen[pi] : n;
            if (top + flen + 2 >= EXP_CAP) {
                lex_fail(L, "macro expansion too long");
                return;
            }
            /*
             * An argument is wrapped: `SQ(a + 1)` has to square the sum, not
             * add 1 to the square, and the same for the expansion as a whole.
             */
            if (pi >= 0) {
                L->pp->exp[top++] = '(';
            }
            const size_t astart = top;
            for (size_t k = 0; k < flen; k++) {
                L->pp->exp[top++] = from[k];
            }
            if (pi >= 0) {
                L->pp->exp[top++] = ')';
                if (nargr < MAX_MPARAMS) {
                    argr[nargr][0] = (uint16_t)(astart - mark);
                    argr[nargr][1] = (uint16_t)(top - mark);
                    nargr++;
                }
            }
            i += n;
            continue;
        }
        if (top + 1 >= EXP_CAP) {
            lex_fail(L, "macro expansion too long");
            return;
        }
        L->pp->exp[top++] = c;
        i++;
    }
    L->exp_top = top;
    pp_push(L, L->pp->exp + mark, top - mark, mi, mark);
    if (L->err) {
        return;
    }
    frame_t *f = &L->pp->frames[L->nframes - 1];
    f->nargr = nargr;
    for (int k = 0; k < nargr; k++) {
        f->argr[k][0] = argr[k][0];
        f->argr[k][1] = argr[k][1];
    }
}

/*
 * An identifier that names a macro reads from the macro instead.  A macro with
 * parameters only does so when a '(' follows it, as C says - the name on its
 * own is then just a name.
 */
static int pp_expand(lex_t *L)
{
    const int mi = pp_find(L, L->text);
    if (mi < 0 || pp_active(L, mi)) {
        return 0;
    }
    const macro_t *m = &L->pp->macros[mi];
    if (m->nparams < 0) {
        pp_push(L, m->body, m->body_len, mi, L->exp_top);
        return 1;
    }
    size_t at = L->pos;
    while (at < L->len && (L->src[at] == ' ' || L->src[at] == '\t' ||
                           L->src[at] == '\n' || L->src[at] == '\r')) {
        at++;
    }
    if (at >= L->len || L->src[at] != '(') {
        return 0;
    }
    L->pos = at;
    pp_expand_args(L, mi);
    return 1;
}

static void lex_next(lex_t *L)
{
    if (L->err) {
        L->tok = T_BAD;
        return;
    }
    lex_skip(L);
    if (L->err) {
        return;
    }
    /* The end of a macro body or of an included file is not the end of input. */
    while (L->pos >= L->len && L->nframes > 0) {
        pp_pop(L);
        lex_skip(L);
        if (L->err) {
            return;
        }
    }
    if (L->pos >= L->len) {
        if (L->cond_depth != 0) {
            lex_fail(L, "unterminated #ifdef");
            return;
        }
        L->tok = T_EOF;
        return;
    }

    const char c = L->src[L->pos];
    if (is_id0(c)) {
        size_t n = 0;
        while (L->pos < L->len && is_id(L->src[L->pos])) {
            if (n + 1 < sizeof(L->text)) {
                L->text[n++] = L->src[L->pos];
            }
            L->pos++;
        }
        L->text[n] = '\0';
        if (L->pp != NULL && kw(L->text) == T_IDENT && pp_expand(L)) {
            lex_next(L);
            return;
        }
        L->tok = kw(L->text);
        return;
    }
    if (is_digit(c)) {
        int32_t v = 0;
        /* 0x… hex, 0… octal, otherwise decimal. */
        if (c == '0' && L->pos + 1 < L->len &&
            (L->src[L->pos + 1] == 'x' || L->src[L->pos + 1] == 'X')) {
            int digits = 0;
            L->pos += 2;
            while (L->pos < L->len) {
                const char h = L->src[L->pos];
                int d;
                if (h >= '0' && h <= '9') {
                    d = h - '0';
                } else if (h >= 'a' && h <= 'f') {
                    d = 10 + (h - 'a');
                } else if (h >= 'A' && h <= 'F') {
                    d = 10 + (h - 'A');
                } else {
                    break;
                }
                if (v > (int32_t)((0x7fffffffu - (uint32_t)d) / 16u)) {
                    lex_fail(L, "integer constant too large");
                    return;
                }
                v = (int32_t)(((uint32_t)v << 4) | (uint32_t)d);
                L->pos++;
                digits++;
            }
            if (digits == 0) {
                lex_fail(L, "bad hex constant");
                return;
            }
        } else if (c == '0') {
            /* Octal (or lone 0). Digits 8/9 after a leading 0 are invalid. */
            L->pos++;
            while (L->pos < L->len && is_digit(L->src[L->pos])) {
                const int d = L->src[L->pos] - '0';
                if (d >= 8) {
                    lex_fail(L, "bad octal constant");
                    return;
                }
                if (v > (2147483647 - d) / 8) {
                    lex_fail(L, "integer constant too large");
                    return;
                }
                v = v * 8 + d;
                L->pos++;
            }
        } else {
            while (L->pos < L->len && is_digit(L->src[L->pos])) {
                const int d = L->src[L->pos] - '0';
                if (v > (2147483647 - d) / 10) {
                    lex_fail(L, "integer constant too large");
                    return;
                }
                v = v * 10 + d;
                L->pos++;
            }
        }
        L->num = v;
        L->tok = T_NUM;
        return;
    }
    if (c == '"') {
        size_t n = 0;
        L->pos++;
        while (L->pos < L->len && L->src[L->pos] != '"') {
            char ch = L->src[L->pos++];
            if (ch == '\\') {
                if (L->pos >= L->len) {
                    lex_fail(L, "unterminated string");
                    return;
                }
                ch = lex_escape(L->src[L->pos++]);
            }
            if (n + 1 >= sizeof(L->strbuf)) {
                lex_fail(L, "string too long");
                return;
            }
            L->strbuf[n++] = ch;
        }
        if (L->pos >= L->len || L->src[L->pos] != '"') {
            lex_fail(L, "unterminated string");
            return;
        }
        L->pos++;
        L->strbuf[n] = '\0';
        L->tok = T_STRING;
        return;
    }
    /*
     * A character constant is a number, not a type of its own: the value domain
     * of this language is the 32-bit int, and 'A' is 65 the moment it is read.
     */
    if (c == '\'') {
        L->pos++;
        if (L->pos >= L->len) {
            lex_fail(L, "unterminated character constant");
            return;
        }
        char ch = L->src[L->pos++];
        if (ch == '\\') {
            if (L->pos >= L->len) {
                lex_fail(L, "unterminated character constant");
                return;
            }
            ch = lex_escape(L->src[L->pos++]);
        }
        if (L->pos >= L->len || L->src[L->pos] != '\'') {
            lex_fail(L, "unterminated character constant");
            return;
        }
        L->pos++;
        L->num = (int32_t)(uint8_t)ch;
        L->tok = T_NUM;
        return;
    }

    L->pos++;
    switch (c) {
    case '(':
        L->tok = T_LPAREN;
        break;
    case ')':
        L->tok = T_RPAREN;
        break;
    case '{':
        L->tok = T_LBRACE;
        break;
    case '}':
        L->tok = T_RBRACE;
        break;
    case '[':
        L->tok = T_LBRACK;
        break;
    case ']':
        L->tok = T_RBRACK;
        break;
    case ';':
        L->tok = T_SEMI;
        break;
    case ',':
        L->tok = T_COMMA;
        break;
    case '+':
        if (L->pos < L->len && L->src[L->pos] == '+') {
            L->pos++;
            L->tok = T_PLUSPLUS;
        } else if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_PLUS_EQ;
        } else {
            L->tok = T_PLUS;
        }
        break;
    case '-':
        if (L->pos < L->len && L->src[L->pos] == '>') {
            L->pos++;
            L->tok = T_ARROW;
        } else if (L->pos < L->len && L->src[L->pos] == '-') {
            L->pos++;
            L->tok = T_MINUSMINUS;
        } else if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_MINUS_EQ;
        } else {
            L->tok = T_MINUS;
        }
        break;
    case '.':
        L->tok = T_DOT;
        break;
    case '*':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_STAR_EQ;
        } else {
            L->tok = T_STAR;
        }
        break;
    case '%':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_PERCENT_EQ;
        } else {
            L->tok = T_PERCENT;
        }
        break;
    case '/':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_SLASH_EQ;
        } else {
            L->tok = T_SLASH;
        }
        break;
    case '?':
        L->tok = T_QUESTION;
        break;
    case ':':
        L->tok = T_COLON;
        break;
    case '!':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_NE;
        } else {
            L->tok = T_NOT;
        }
        break;
    case '=':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_EQ;
        } else {
            L->tok = T_ASSIGN;
        }
        break;
    case '<':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_LE;
        } else if (L->pos < L->len && L->src[L->pos] == '<') {
            L->pos++;
            if (L->pos < L->len && L->src[L->pos] == '=') {
                L->pos++;
                L->tok = T_SHL_EQ;
            } else {
                L->tok = T_SHL;
            }
        } else {
            L->tok = T_LT;
        }
        break;
    case '>':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_GE;
        } else if (L->pos < L->len && L->src[L->pos] == '>') {
            L->pos++;
            if (L->pos < L->len && L->src[L->pos] == '=') {
                L->pos++;
                L->tok = T_SHR_EQ;
            } else {
                L->tok = T_SHR;
            }
        } else {
            L->tok = T_GT;
        }
        break;
    case '&':
        if (L->pos < L->len && L->src[L->pos] == '&') {
            L->pos++;
            L->tok = T_ANDAND;
        } else if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_AMP_EQ;
        } else {
            L->tok = T_AMP;
        }
        break;
    case '|':
        if (L->pos < L->len && L->src[L->pos] == '|') {
            L->pos++;
            L->tok = T_OROR;
        } else if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_PIPE_EQ;
        } else {
            L->tok = T_PIPE;
        }
        break;
    case '^':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_CARET_EQ;
        } else {
            L->tok = T_CARET;
        }
        break;
    case '~':
        L->tok = T_TILDE;
        break;
    default:
        lex_fail(L, "unexpected character");
        break;
    }
}

/*
 * One token of lookahead past the current one.  The lexer holds no state that
 * is not in the struct, so a peek is a copy, a step and a copy back - which is
 * what tells `a` in `x * a` apart from `a` in `x * a[i]` without a second
 * parser pass.
 */
static tok_t peek_tok(lex_t *L)
{
    const lex_t save = *L;
    lex_next(L);
    const tok_t t = L->tok;
    *L = save;
    return t;
}

/* ---- codegen ----------------------------------------------------------- */

/*
 * A type here is a shape, the size of one element, and which struct that
 * element is (or none).  Values live in registers as 32-bit words and nothing
 * else, so this is not a type system for expressions: it exists to answer three
 * questions at the point of a load, a store or an index - how many bytes does
 * this touch, what does an index step by, and does this name have fields.
 *
 * PK_PTR keeps the *pointee* in esize/sidx; the pointer itself is a word.
 * PK_ARRAY and PK_STRUCT are addresses when used as values, which is why an
 * array name and a `&v` are the same kind of thing to everything downstream.
 */
enum { PK_SCALAR = 0, PK_PTR, PK_ARRAY, PK_STRUCT };

typedef struct {
    int kind;
    int esize;
    int sidx;
} ptype_t;

typedef struct {
    char    name[MAX_NAME];
    int     off;
    int     nwords;
    ptype_t t;
} sym_t;

/* A field is a symbol at an offset inside its struct. */
typedef struct {
    char    name[MAX_NAME];
    int     off;
    ptype_t t;
} field_t;

typedef struct {
    char    name[MAX_NAME];
    int     size;
    int     nfields;
    field_t fields[MAX_FIELDS];
} struct_t;

static ptype_t ty_scalar(int esize)
{
    ptype_t t = {PK_SCALAR, esize, -1};
    return t;
}

/* What one element of an array, or the target of a pointer, is. */
static ptype_t ty_elem(const ptype_t *t)
{
    ptype_t e = {PK_SCALAR, t->esize, t->sidx};
    if (t->sidx >= 0) {
        e.kind = PK_STRUCT;
    }
    return e;
}

/* How much memory the place itself takes: a pointer is a word. */
static int ty_store(const ptype_t *t)
{
    return (t->kind == PK_PTR) ? 4 : t->esize;
}


/* Whether the value of this place is its address rather than its contents. */
static int ty_is_addr(const ptype_t *t)
{
    return t->kind == PK_ARRAY || t->kind == PK_STRUCT;
}

typedef struct {
    char   name[MAX_NAME];
    size_t code_off;
    int    nparams;
    int    is_void;
} func_t;

/* A typedef is only a name for an existing type (optional pointer). */
typedef struct {
    char name[MAX_NAME];
    int  esize;
    int  sidx;
    int  is_ptr;
} tdef_t;

typedef struct {
    char    name[MAX_NAME];
    int32_t val;
} enum_t;

typedef struct {
    uint8_t *code;
    size_t   len;
    size_t   cap;
    uint32_t lit[LIT_CAP];
    uint8_t  lit_kind[LIT_CAP];
    uint32_t lit_off[LIT_CAP];
    size_t   lit_site_off[MAX_LIT_SITES];
    uint16_t lit_site_li[MAX_LIT_SITES];
    int      nlit_sites;
    int      nlit;
    int      nlocals;
    sym_t    locals[MAX_LOCALS];
    int      local_words;
    int      nglobals;
    sym_t    globals[MAX_GLOBALS];
    int      nstructs;
    struct_t structs[MAX_STRUCTS];
    int      ntdefs;
    tdef_t   tdefs[MAX_TYPEDEFS];
    int      nenums;
    enum_t   enums[MAX_ENUMS];
    pp_t     pp;
    int      data_next;
    int      data_cap;
    uint8_t *data;
    int      nfuncs;
    func_t   funcs[MAX_FUNCS];
    int      entry_idx; /* ag_main or ag_driver_init */
    int      is_driver;
    uint32_t relocs[MAX_RELOCS];
    int      nrelocs;
    int      needs_gfx;
    int      needs_audio;
    int      min_abi;
    int      spill_top;
    int      err;
    char     errmsg[160];
} gen_t;

static void gfail(gen_t *g, const char *msg)
{
    if (g->err) {
        return;
    }
    g->err = 1;
    cpy_err(g->errmsg, sizeof(g->errmsg), msg);
}

/*
 * The size of one element.  For a struct it is read from the table rather than
 * from the type, because `struct node *next;` inside `struct node` is written
 * while that size is still unknown - and a stale zero there would scale every
 * step through such a list to nothing.
 */
static int ty_size(const gen_t *g, const ptype_t *t)
{
    return (t->sidx >= 0) ? g->structs[t->sidx].size : t->esize;
}

static void emit_raw(gen_t *g, const uint8_t *b, size_t n)
{
    if (g->err) {
        return;
    }
    if (g->len + n > g->cap) {
        gfail(g, "code too large");
        return;
    }
    memcpy(g->code + g->len, b, n);
    g->len += n;
}

static void emit2(gen_t *g, uint8_t a, uint8_t b)
{
    uint8_t x[2] = {a, b};
    emit_raw(g, x, 2);
}

static void emit3(gen_t *g, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t x[3] = {a, b, c};
    emit_raw(g, x, 3);
}

static void emit_entry(gen_t *g, int framesize)
{
    const int imm = framesize / 8;
    emit3(g, 0x36, (uint8_t)(((imm & 0xf) << 4) | 1), (uint8_t)(imm >> 4));
}

/*
 * How much stack a function needs is only known once its body has been read,
 * and ENTRY stands at the top of it - so it is emitted with the largest frame
 * and rewritten with the real one.  Sizing every frame at the maximum instead
 * would cost each call the deepest function's stack.
 */
static void patch_entry(gen_t *g, size_t site, int framesize)
{
    if (site + 2 >= g->len || g->code[site] != 0x36) {
        gfail(g, "bad entry site");
        return;
    }
    const int imm = framesize / 8;
    g->code[site + 1] = (uint8_t)(((imm & 0xf) << 4) | 1);
    g->code[site + 2] = (uint8_t)(imm >> 4);
}

static void emit_retw_n(gen_t *g)
{
    emit2(g, 0x1d, 0xf0);
}

static void emit_mov_n(gen_t *g, int ad, int as)
{
    emit2(g, (uint8_t)((ad << 4) | 0x0d), (uint8_t)as);
}

static int add_lit(gen_t *g, uint8_t kind, uint32_t val_or_off)
{
    for (int i = 0; i < g->nlit; i++) {
        if (g->lit_kind[i] == kind && g->lit_off[i] == val_or_off) {
            return i;
        }
    }
    if (g->nlit >= LIT_CAP) {
        gfail(g, "too many constants");
        return -1;
    }
    const int li = g->nlit++;
    g->lit_kind[li] = kind;
    g->lit_off[li] = val_or_off;
    g->lit[li] = (kind == LIT_IMM) ? val_or_off : 0;
    return li;
}

static void emit_l32r_lit(gen_t *g, int at, int li)
{
    if (li < 0 || li >= g->nlit) {
        gfail(g, "bad literal index");
        return;
    }
    if (g->nlit_sites >= MAX_LIT_SITES) {
        gfail(g, "too many literal uses");
        return;
    }
    g->lit_site_off[g->nlit_sites] = g->len;
    g->lit_site_li[g->nlit_sites] = (uint16_t)li;
    g->nlit_sites++;
    /* Placeholder: byte1/byte2 patched in fix_l32r (li kept in side table). */
    emit3(g, (uint8_t)((at << 4) | 0x1), 0xff, 0xff);
}

static void emit_movi(gen_t *g, int at, int32_t imm)
{
    if (imm >= -32 && imm <= 95) {
        const uint32_t u = (uint32_t)imm & 0x7fu;
        emit2(g, (uint8_t)(((u >> 4) << 4) | 0x0c),
              (uint8_t)(((u & 0x0fu) << 4) | (uint32_t)at));
        return;
    }
    if (imm >= -2048 && imm <= 2047) {
        const uint32_t u = (uint32_t)imm & 0xfffu;
        emit3(g, (uint8_t)((at << 4) | 0x2),
              (uint8_t)(0xa0u | ((u >> 8) & 0xfu)), (uint8_t)(u & 0xffu));
        return;
    }
    const int li = add_lit(g, LIT_IMM, (uint32_t)imm);
    if (li < 0) {
        return;
    }
    emit_l32r_lit(g, at, li);
}

static void emit_li_data(gen_t *g, int at, uint32_t data_off)
{
    const int li = add_lit(g, LIT_DATA_OFF, data_off);
    if (li < 0) {
        return;
    }
    emit_l32r_lit(g, at, li);
}

static void emit_li_code(gen_t *g, int at, uint32_t code_off)
{
    const int li = add_lit(g, LIT_CODE_OFF, code_off);
    if (li < 0) {
        return;
    }
    emit_l32r_lit(g, at, li);
}

static void emit_add_n(gen_t *g, int ar, int as, int at)
{
    emit2(g, (uint8_t)((at << 4) | 0x0a), (uint8_t)((ar << 4) | as));
}

static void emit_rrr(gen_t *g, uint8_t opc, int ar, int as, int at)
{
    emit3(g, (uint8_t)((at << 4) | 0x0), (uint8_t)((ar << 4) | as), opc);
}

static void emit_sub(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0xc0, ar, as, at);
}

static void emit_mull(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0x82, ar, as, at);
}

static void emit_quos(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0xd2, ar, as, at);
}

static void emit_rems(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0xf2, ar, as, at);
}

static void emit_and(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0x10, ar, as, at);
}

static void emit_or(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0x20, ar, as, at);
}

static void emit_xor(gen_t *g, int ar, int as, int at)
{
    emit_rrr(g, 0x30, ar, as, at);
}

/*
 * Shifts by a register go through SAR: SSL/SSR set it, SLL/SRA use it.  SLL
 * takes its operand in the s field and SRA in the t field - an asymmetry of the
 * ISA, not of this compiler.
 */
static void emit_ssl(gen_t *g, int as)
{
    emit3(g, 0x00, (uint8_t)(0x10 | as), 0x40);
}

static void emit_ssr(gen_t *g, int as)
{
    emit3(g, 0x00, (uint8_t)as, 0x40);
}

static void emit_sll(gen_t *g, int ar, int as)
{
    emit3(g, 0x00, (uint8_t)((ar << 4) | as), 0xa1);
}

static void emit_sra(gen_t *g, int ar, int at)
{
    emit3(g, (uint8_t)(at << 4), (uint8_t)(ar << 4), 0xb1);
}

/* SLLI encodes 32 - sa, which is why a shift by 0 has no encoding at all. */
static void emit_slli(gen_t *g, int ar, int as, int sa)
{
    const int inv = 32 - sa;
    emit3(g, (uint8_t)((inv & 0xf) << 4), (uint8_t)((ar << 4) | as),
          (uint8_t)(((inv >> 4) << 4) | 0x1));
}

static void emit_srai(gen_t *g, int ar, int at, int sa)
{
    emit3(g, (uint8_t)(at << 4), (uint8_t)((ar << 4) | (sa & 0xf)),
          (uint8_t)(((0x2 | (sa >> 4)) << 4) | 0x1));
}

static void emit_l8ui(gen_t *g, int at, int as, int off)
{
    if (off < 0 || off > 255) {
        gfail(g, "bad load offset");
        return;
    }
    emit3(g, (uint8_t)((at << 4) | 0x2), (uint8_t)as, (uint8_t)off);
}

static void emit_s8i(gen_t *g, int at, int as, int off)
{
    if (off < 0 || off > 255) {
        gfail(g, "bad store offset");
        return;
    }
    emit3(g, (uint8_t)((at << 4) | 0x2), (uint8_t)((0x4 << 4) | as),
          (uint8_t)off);
}

static void emit_l32i(gen_t *g, int at, int as, int off)
{
    if ((off & 3) != 0 || off < 0 || off > 1020) {
        gfail(g, "bad load offset");
        return;
    }
    const int imm = off / 4;
    if (imm <= 15) {
        emit2(g, (uint8_t)((at << 4) | 0x8), (uint8_t)((imm << 4) | as));
    } else {
        emit3(g, (uint8_t)((at << 4) | 0x2), (uint8_t)((0x2 << 4) | as),
              (uint8_t)imm);
    }
}

static void emit_s32i(gen_t *g, int at, int as, int off)
{
    if ((off & 3) != 0 || off < 0 || off > 1020) {
        gfail(g, "bad store offset");
        return;
    }
    const int imm = off / 4;
    if (imm <= 15) {
        emit2(g, (uint8_t)((at << 4) | 0x9), (uint8_t)((imm << 4) | as));
    } else {
        emit3(g, (uint8_t)((at << 4) | 0x2), (uint8_t)((0x6 << 4) | as),
              (uint8_t)imm);
    }
}

static void emit_callx8(gen_t *g, int as)
{
    emit3(g, 0xe0, (uint8_t)as, 0x00);
}

static void emit_j_to(gen_t *g, size_t from_off, size_t target_off)
{
    const int32_t imm = (int32_t)target_off - (int32_t)(from_off + 4);
    if (imm < -131072 || imm > 131071) {
        gfail(g, "jump too far");
        return;
    }
    const uint32_t w = 0x06u | (((uint32_t)imm & 0x3ffffu) << 6);
    if (from_off + 3 > g->len) {
        gfail(g, "bad jump site");
        return;
    }
    g->code[from_off] = (uint8_t)w;
    g->code[from_off + 1] = (uint8_t)(w >> 8);
    g->code[from_off + 2] = (uint8_t)(w >> 16);
}

static void emit_j_placeholder(gen_t *g, size_t *site)
{
    *site = g->len;
    emit3(g, 0x46, 0x00, 0x00);
}

enum {
    B_BEQ = 0x1,
    B_BLT = 0x2,
    B_BGE = 0xa,
    B_BNE = 0x9
};

static void emit_b_rr(gen_t *g, int opn, int as, int at, size_t from_off,
                      size_t target_off)
{
    const int32_t imm = (int32_t)target_off - (int32_t)(from_off + 4);
    if (imm < -128 || imm > 127) {
        gfail(g, "branch too far");
        return;
    }
    g->code[from_off] = (uint8_t)((at << 4) | 0x7);
    g->code[from_off + 1] = (uint8_t)((opn << 4) | as);
    g->code[from_off + 2] = (uint8_t)imm;
}

static void emit_b_placeholder(gen_t *g, int opn, int as, int at, size_t *site)
{
    *site = g->len;
    emit3(g, (uint8_t)((at << 4) | 0x7), (uint8_t)((opn << 4) | as), 0x00);
}

static void patch_b(gen_t *g, size_t site, size_t target)
{
    emit_b_rr(g, (g->code[site + 1] >> 4) & 0xf, g->code[site + 1] & 0xf,
              (g->code[site] >> 4) & 0xf, site, target);
}

static void emit_setlt(gen_t *g, int ar, int as, int at)
{
    size_t blt_site, j_site;
    emit_b_placeholder(g, B_BLT, as, at, &blt_site);
    emit_movi(g, ar, 0);
    emit_j_placeholder(g, &j_site);
    patch_b(g, blt_site, g->len);
    emit_movi(g, ar, 1);
    emit_j_to(g, j_site, g->len);
}

static int find_local(gen_t *g, const char *name)
{
    for (int i = 0; i < g->nlocals; i++) {
        if (CC_STRCMP(g->locals[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_global(gen_t *g, const char *name)
{
    for (int i = 0; i < g->nglobals; i++) {
        if (CC_STRCMP(g->globals[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_func(gen_t *g, const char *name)
{
    for (int i = 0; i < g->nfuncs; i++) {
        if (CC_STRCMP(g->funcs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * How much memory a declaration takes: a pointer is a word whatever it points
 * at, an array is its elements, everything else is itself.
 */
static int sym_bytes(const gen_t *g, const ptype_t *t, int nelem)
{
    if (t->kind == PK_PTR) {
        return 4;
    }
    const int size = ty_size(g, t);
    return (t->kind == PK_ARRAY) ? nelem * size : size;
}

static int add_local_t(gen_t *g, const char *name, const ptype_t *t, int nelem)
{
    if (find_local(g, name) >= 0) {
        gfail(g, "duplicate local");
        return -1;
    }
    if (nelem <= 0 || ty_size(g, t) <= 0) {
        gfail(g, "bad array size");
        return -1;
    }
    const int nwords = (sym_bytes(g, t, nelem) + 3) / 4;
    if (g->nlocals >= MAX_LOCALS) {
        gfail(g, "too many locals");
        return -1;
    }
    if (g->local_words + nwords > MAX_LOCAL_WORDS) {
        gfail(g, "too many local slots");
        return -1;
    }
    const int i = g->nlocals++;
    cpy_err(g->locals[i].name, MAX_NAME, name);
    g->locals[i].off = SPILL_BYTES + g->local_words * 4;
    g->locals[i].nwords = nwords;
    g->locals[i].t = *t;
    g->local_words += nwords;
    return i;
}

static int add_global_t(gen_t *g, const char *name, const ptype_t *t, int nelem)
{
    if (find_global(g, name) >= 0 || find_func(g, name) >= 0) {
        gfail(g, "duplicate global");
        return -1;
    }
    if (nelem <= 0 || ty_size(g, t) <= 0) {
        gfail(g, "bad array size");
        return -1;
    }
    if (g->nglobals >= MAX_GLOBALS) {
        gfail(g, "too many globals");
        return -1;
    }
    /*
     * Every global starts on a word even when it is a char array: word loads
     * must stay aligned, and globals are all declared before the first string
     * literal is placed, so nothing packed follows them into the same word.
     */
    g->data_next = (g->data_next + 3) & ~3;
    const int bytes = sym_bytes(g, t, nelem);
    if (g->data_next + bytes > g->data_cap) {
        gfail(g, "data too large");
        return -1;
    }
    const int i = g->nglobals++;
    cpy_err(g->globals[i].name, MAX_NAME, name);
    g->globals[i].off = g->data_next;
    g->globals[i].nwords = (bytes + 3) / 4;
    g->globals[i].t = *t;
    g->data_next += bytes;
    return i;
}

static int find_tdef(gen_t *g, const char *name)
{
    for (int i = 0; i < g->ntdefs; i++) {
        if (CC_STRCMP(g->tdefs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_enum(gen_t *g, const char *name)
{
    for (int i = 0; i < g->nenums; i++) {
        if (CC_STRCMP(g->enums[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_enum(gen_t *g, const char *name, int32_t val)
{
    if (find_enum(g, name) >= 0 || find_global(g, name) >= 0 ||
        find_tdef(g, name) >= 0) {
        gfail(g, "duplicate enumerator");
        return -1;
    }
    if (g->nenums >= MAX_ENUMS) {
        gfail(g, "too many enumerators");
        return -1;
    }
    const int i = g->nenums++;
    cpy_err(g->enums[i].name, MAX_NAME, name);
    g->enums[i].val = val;
    return i;
}

static int find_struct(gen_t *g, const char *name)
{
    for (int i = 0; i < g->nstructs; i++) {
        if (CC_STRCMP(g->structs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_field(const struct_t *s, const char *name)
{
    for (int i = 0; i < s->nfields; i++) {
        if (CC_STRCMP(s->fields[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_string(gen_t *g, const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    n++;
    if (g->data_next + (int)n > g->data_cap) {
        gfail(g, "data too large");
        return -1;
    }
    const int off = g->data_next;
    memcpy(g->data + off, s, n);
    g->data_next += (int)n;
    return off;
}

static void add_reloc(gen_t *g, uint32_t entry)
{
    if (g->nrelocs >= MAX_RELOCS) {
        gfail(g, "too many relocations");
        return;
    }
    g->relocs[g->nrelocs++] = entry;
}

static void push_a8(gen_t *g)
{
    if (g->spill_top >= SPILL_SLOTS) {
        gfail(g, "expression too deep");
        return;
    }
    emit_s32i(g, 8, 7, g->spill_top * 4);
    g->spill_top++;
}

static void pop_a9(gen_t *g)
{
    if (g->spill_top <= 0) {
        gfail(g, "expression stack underflow");
        return;
    }
    g->spill_top--;
    emit_l32i(g, 9, 7, g->spill_top * 4);
}

static void emit_bool_not_a8(gen_t *g)
{
    emit_movi(g, 9, 0);
    size_t beq_site, j_site;
    emit_b_placeholder(g, B_BEQ, 8, 9, &beq_site);
    emit_movi(g, 8, 0);
    emit_j_placeholder(g, &j_site);
    patch_b(g, beq_site, g->len);
    emit_movi(g, 8, 1);
    emit_j_to(g, j_site, g->len);
}

static void emit_load_api_fn(gen_t *g, int at, int api_off, int fn_off)
{
    emit_li_data(g, at, 0);
    emit_l32i(g, at, at, 0);
    emit_l32i(g, at, at, api_off);
    emit_l32i(g, at, at, fn_off);
}

/* The size of one access to this variable; a pointer is read as a word. */
static int sym_access_size(const sym_t *s)
{
    return ty_store(&s->t);
}

/*
 * [as + off] -> at, a byte or a word.  A byte offset only reaches 255, far less
 * than a frame is deep, so past that the address is computed - in `at` itself,
 * which is why it must not be the base register.
 */
static void emit_load_sized(gen_t *g, int at, int as, int off, int esize)
{
    if (esize != 1) {
        emit_l32i(g, at, as, off);
        return;
    }
    if (off <= 255) {
        emit_l8ui(g, at, as, off);
        return;
    }
    emit_movi(g, at, off);
    emit_add_n(g, at, at, as);
    emit_l8ui(g, at, at, 0);
}

static void emit_store_sized(gen_t *g, int at, int as, int off, int esize,
                             int scratch)
{
    if (esize != 1) {
        emit_s32i(g, at, as, off);
        return;
    }
    if (off <= 255) {
        emit_s8i(g, at, as, off);
        return;
    }
    emit_movi(g, scratch, off);
    emit_add_n(g, scratch, scratch, as);
    emit_s8i(g, at, scratch, 0);
}

/*
 * An index times the element size.  Two adds beat movi + mull for a word, a
 * shift wins as soon as the element is bigger, and a struct of an awkward size
 * is the only case that has to multiply.  a11 is the scratch: an address is
 * always computed before the value that will be stored into it, so nothing of
 * the caller's is living there yet.
 */
static void emit_scale(gen_t *g, int reg, int size)
{
    if (size <= 1) {
        return;
    }
    if (size == 2) {
        emit_add_n(g, reg, reg, reg);
        return;
    }
    if (size == 4) {
        emit_add_n(g, reg, reg, reg);
        emit_add_n(g, reg, reg, reg);
        return;
    }
    int sh = 0;
    int v = size;
    while ((v & 1) == 0) {
        v >>= 1;
        sh++;
    }
    if (v == 1 && sh <= 31) {
        emit_slli(g, reg, reg, sh);
        return;
    }
    emit_movi(g, 11, size);
    emit_mull(g, reg, reg, 11);
}

/*
 * A place is where a name leads: a base - a symbol, or an address already in
 * a8 - plus a constant byte offset, plus what is found there.  Keeping that
 * offset constant for as long as possible is the point: it rides into the
 * immediate of the final load or store, so `p->x` is one instruction and
 * `v.a.b` costs nothing at all beyond reaching `v`.
 */
typedef struct {
    int     mat; /* the base address is in a8 */
    int     li;
    int     gi;
    int     off;
    ptype_t t;
} place_t;

static int off_fits(int size, int off)
{
    if (off < 0) {
        return 0;
    }
    if (size == 1) {
        return off <= 255;
    }
    return (off & 3) == 0 && off <= 1020;
}

/* The address of the place, in a8, with no offset left pending. */
static void place_addr(gen_t *g, place_t *pl)
{
    if (pl->mat) {
        if (pl->off != 0) {
            emit_movi(g, 9, pl->off);
            emit_add_n(g, 8, 8, 9);
            pl->off = 0;
        }
        return;
    }
    if (pl->li >= 0) {
        const int off = g->locals[pl->li].off + pl->off;
        if (off == 0) {
            emit_mov_n(g, 8, 7);
        } else {
            emit_movi(g, 8, off);
            emit_add_n(g, 8, 8, 7);
        }
    } else {
        emit_li_data(g, 8, (uint32_t)(g->globals[pl->gi].off + pl->off));
    }
    pl->mat = 1;
    pl->li = -1;
    pl->gi = -1;
    pl->off = 0;
}

static void place_load(gen_t *g, place_t *pl, int at)
{
    const int size = ty_store(&pl->t);
    if (ty_is_addr(&pl->t)) {
        gfail(g, "not a value");
        return;
    }
    if (!pl->mat) {
        if (pl->li >= 0) {
            emit_load_sized(g, at, 7, g->locals[pl->li].off + pl->off, size);
        } else {
            emit_li_data(g, at, (uint32_t)(g->globals[pl->gi].off + pl->off));
            emit_load_sized(g, at, at, 0, size);
        }
        return;
    }
    if (!off_fits(size, pl->off)) {
        place_addr(g, pl);
    }
    emit_load_sized(g, at, 8, pl->off, size);
}

static void place_store(gen_t *g, place_t *pl, int val, int scratch)
{
    const int size = ty_store(&pl->t);
    if (ty_is_addr(&pl->t)) {
        gfail(g, "cannot assign a whole array or struct");
        return;
    }
    if (!pl->mat) {
        if (pl->li >= 0) {
            emit_store_sized(g, val, 7, g->locals[pl->li].off + pl->off, size,
                             scratch);
        } else {
            emit_li_data(g, scratch, (uint32_t)(g->globals[pl->gi].off +
                                                pl->off));
            emit_store_sized(g, val, scratch, 0, size, scratch);
        }
        return;
    }
    if (!off_fits(size, pl->off)) {
        place_addr(g, pl);
    }
    emit_store_sized(g, val, 8, pl->off, size, scratch);
}

/* ---- builtins ---------------------------------------------------------- */

typedef struct {
    const char *name;
    int         nargs;    /* -1: 1..MAX_PARAMS arguments, counted at the call */
    int         api_off;
    int         fn_off;
    int         ret_mode; /* RET_VOID / RET_BOOL / RET_RAW */
    int         null_arg; /* call with a10=0 and no parsed args */
    int         min_abi;
    const char *fmt;      /* format the compiler supplies itself, or NULL */
} builtin_t;

static const builtin_t BUILTINS[] = {
    {"ag_delay", 1, API_OFF_TIME, TIME_OFF_DELAY_MS, RET_VOID, 0, ABI_MINOR_BASE, NULL},
    /* us() is uint64 in the ABI; windowed return puts the low 32 bits in a2. */
    {"ag_micros", 0, API_OFF_TIME, TIME_OFF_US, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_millis", 0, API_OFF_TIME, TIME_OFF_MS, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_key", 1, API_OFF_INP, INP_OFF_KEY_PRESSED, RET_BOOL, 0, ABI_MINOR_BASE, NULL},
    /* ag_btn: level state via HostFS PADPUSH (same path as SMS). ids 0..7 */
    {"ag_btn", 1, API_OFF_INP, INP_OFF_BTN, RET_BOOL, 0, ABI_MINOR_BTN, NULL},
    /* Session focus: drain with poll; sleep+heartbeat while backgrounded. */
    {"ag_poll_event", 2, API_OFF_INP, INP_OFF_POLL, RET_BOOL, 0, ABI_MINOR_BASE, NULL},
    {"ag_heartbeat", 0, API_OFF_SYS, SYS_OFF_HEARTBEAT, RET_VOID, 0, ABI_MINOR_BASE, NULL},
    {"ag_focused", 0, API_OFF_PROC, PROC_OFF_FOCUSED, RET_BOOL, 0, ABI_MINOR_FOCUS, NULL},

    /* Console text.  ag_printf is the kernel's own, reached with its arguments
     * in the outgoing registers, which is what the windowed ABI asks of any
     * caller of a variadic function. */
    {"ag_print", 1, API_OFF_CON, CON_OFF_PUTS, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_printf", -1, API_OFF_CON, CON_OFF_PRINTF, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_print_int", 1, API_OFF_CON, CON_OFF_PRINTF, RET_RAW, 0, ABI_MINOR_BASE, "%d"},
    {"ag_print_hex", 1, API_OFF_CON, CON_OFF_PRINTF, RET_RAW, 0, ABI_MINOR_BASE, "%08x"},
    {"ag_cls", 0, API_OFF_CON, CON_OFF_CLS, RET_VOID, 0, ABI_MINOR_BASE, NULL},
    {"ag_gotoxy", 2, API_OFF_CON, CON_OFF_GOTOXY, RET_VOID, 0, ABI_MINOR_BASE, NULL},

    /* Pins, so that a board can be made to do something without a host GCC. */
    {"ag_gpio_config", 2, API_OFF_IO, IO_OFF_GPIO_CONFIG, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_gpio_write", 2, API_OFF_IO, IO_OFF_GPIO_WRITE, RET_VOID, 0, ABI_MINOR_BASE, NULL},
    {"ag_gpio_read", 1, API_OFF_IO, IO_OFF_GPIO_READ, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_adc_read", 1, API_OFF_IO, IO_OFF_ADC_READ, RET_RAW, 0, ABI_MINOR_BASE, NULL},

    {"ag_gfx_acquire", 0, API_OFF_GFX, GFX_OFF_ACQUIRE, RET_VOID, 1, ABI_MINOR_GFX, NULL},
    {"ag_gfx_release", 0, API_OFF_GFX, GFX_OFF_RELEASE, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_clear", 1, API_OFF_GFX, GFX_OFF_CLEAR, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_flush", 4, API_OFF_GFX, GFX_OFF_FLUSH, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_swap", 0, API_OFF_GFX, GFX_OFF_SWAP, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_fill_rect", 5, API_OFF_GFX, GFX_OFF_FILL_RECT, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_text", 5, API_OFF_GFX, GFX_OFF_TEXT, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_pixel", 3, API_OFF_GFX, GFX_OFF_PIXEL, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_line", 5, API_OFF_GFX, GFX_OFF_LINE, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_circle", 4, API_OFF_GFX, GFX_OFF_CIRCLE, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_fill_circle", 4, API_OFF_GFX, GFX_OFF_FILL_CIRCLE, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_poly_begin", 0, API_OFF_GFX, GFX_OFF_POLY_BEGIN, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_poly_vertex", 2, API_OFF_GFX, GFX_OFF_POLY_VERTEX, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_poly_fill", 1, API_OFF_GFX, GFX_OFF_POLY_FILL, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_poly_stroke", 1, API_OFF_GFX, GFX_OFF_POLY_STROKE, RET_VOID, 0, ABI_MINOR_GFX, NULL},
    {"ag_gfx_clip", 4, API_OFF_GFX, GFX_OFF_CLIP, RET_VOID, 0, ABI_MINOR_GFX16, NULL},
    {"ag_gfx_clip_reset", 0, API_OFF_GFX, GFX_OFF_CLIP_RESET, RET_VOID, 0,
     ABI_MINOR_GFX16, NULL},
    {"ag_gfx_stroke_rect", 5, API_OFF_GFX, GFX_OFF_STROKE_RECT, RET_VOID, 0,
     ABI_MINOR_GFX16, NULL},
    {"ag_gfx_fill_round_rect", 6, API_OFF_GFX, GFX_OFF_FILL_ROUND_RECT, RET_VOID,
     0, ABI_MINOR_GFX16, NULL},
    /* ABI 0.17: stateful RGB565 blit (one-shot blit_key is 8 args — CC max 6). */
    {"ag_gfx_blit_bind", 2, API_OFF_GFX, GFX_OFF_BLIT_BIND, RET_VOID, 0,
     ABI_MINOR_GFX17, NULL},
    {"ag_gfx_blit_copy", 4, API_OFF_GFX, GFX_OFF_BLIT_COPY, RET_VOID, 0,
     ABI_MINOR_GFX17, NULL},
    {"ag_gfx_blit_keyed", 5, API_OFF_GFX, GFX_OFF_BLIT_KEYED, RET_VOID, 0,
     ABI_MINOR_GFX17, NULL},
    {"ag_gfx_text_fit", 6, API_OFF_GFX, GFX_OFF_TEXT_FIT, RET_RAW, 0,
     ABI_MINOR_GFX25, NULL},
    {"ag_gfx_blit_src_rect", 4, API_OFF_GFX, GFX_OFF_BLIT_SRC_RECT, RET_VOID, 0,
     ABI_MINOR_GFX26, NULL},
    {"ag_gfx_blit_scaled", 4, API_OFF_GFX, GFX_OFF_BLIT_SCALED, RET_VOID, 0,
     ABI_MINOR_GFX26, NULL},
    {"ag_gfx_blit_tiled", 4, API_OFF_GFX, GFX_OFF_BLIT_TILED, RET_VOID, 0,
     ABI_MINOR_GFX26, NULL},
    {"ag_gfx_poly_uv", 2, API_OFF_GFX, GFX_OFF_POLY_UV, RET_VOID, 0,
     ABI_MINOR_GFX26, NULL},
    {"ag_gfx_poly_fill_tex", 0, API_OFF_GFX, GFX_OFF_POLY_FILL_TEX, RET_VOID, 0,
     ABI_MINOR_GFX26, NULL},

    /* audio (ABI 0.14): open(NULL) default 22050 stereo s16 */
    {"ag_audio_present", 0, API_OFF_AUDIO, AUDIO_OFF_PRESENT, RET_RAW, 0, ABI_MINOR_AUDIO, NULL},
    {"ag_audio_is_hw", 0, API_OFF_AUDIO, AUDIO_OFF_IS_HW, RET_RAW, 0, ABI_MINOR_AUDIO, NULL},
    {"ag_audio_open", 0, API_OFF_AUDIO, AUDIO_OFF_OPEN, RET_RAW, 1, ABI_MINOR_AUDIO, NULL},
    {"ag_audio_close", 0, API_OFF_AUDIO, AUDIO_OFF_CLOSE, RET_VOID, 0, ABI_MINOR_AUDIO, NULL},
    {"ag_audio_write", 2, API_OFF_AUDIO, AUDIO_OFF_WRITE, RET_RAW, 0, ABI_MINOR_AUDIO, NULL},
    {"ag_audio_space", 0, API_OFF_AUDIO, AUDIO_OFF_SPACE, RET_RAW, 0, ABI_MINOR_AUDIO, NULL},

    /* mem / fs / dev - phase D; seek is omitted (int64_t, Mini-C is 32-bit). */
    {"ag_malloc", 1, API_OFF_MEM, MEM_OFF_ALLOC, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_realloc", 2, API_OFF_MEM, MEM_OFF_REALLOC, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_free", 1, API_OFF_MEM, MEM_OFF_FREE, RET_VOID, 0, ABI_MINOR_BASE, NULL},

    {"ag_open", 2, API_OFF_FS, FS_OFF_OPEN, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_close", 1, API_OFF_FS, FS_OFF_CLOSE, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_read", 3, API_OFF_FS, FS_OFF_READ, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_write", 3, API_OFF_FS, FS_OFF_WRITE, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_opendir", 1, API_OFF_FS, FS_OFF_OPENDIR, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_readdir", 2, API_OFF_FS, FS_OFF_READDIR, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_closedir", 1, API_OFF_FS, FS_OFF_CLOSEDIR, RET_RAW, 0, ABI_MINOR_BASE, NULL},

    {"ag_dev_open", 1, API_OFF_DEV, DEV_OFF_OPEN, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_dev_close", 1, API_OFF_DEV, DEV_OFF_CLOSE, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_dev_read", 3, API_OFF_DEV, DEV_OFF_READ, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_dev_write", 3, API_OFF_DEV, DEV_OFF_WRITE, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_dev_ioctl", 4, API_OFF_DEV, DEV_OFF_IOCTL, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    /* Publish-side: only legal inside ag_driver_init (kernel enforces). */
    {"ag_dev_add", 1, API_OFF_DEV, DEV_OFF_ADD, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_dev_remove", 1, API_OFF_DEV, DEV_OFF_REMOVE, RET_RAW, 0, ABI_MINOR_BASE, NULL},
    {"ag_dev_priv", 1, API_OFF_DEV, DEV_OFF_GET_PRIV, RET_RAW, 0, ABI_MINOR_BASE, NULL},
};

static const builtin_t *find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        if (CC_STRCMP(BUILTINS[i].name, name) == 0) {
            return &BUILTINS[i];
        }
    }
    return NULL;
}

/* ---- parser ------------------------------------------------------------ */

/*
 * `pesize`/`psidx` are the whole of the type system for expressions: the size
 * of what the value now in a8 points at, and which struct that is - or 0 and -1
 * when the value is a plain int.  Whatever produced the value sets them; `*`,
 * `[]`, `->` and pointer arithmetic consult them; every operator that yields a
 * number rather than an address clears them.  That is enough for `char *s` to
 * walk a string a byte at a time and for `struct v *p` to step by its size,
 * without carrying a type on every node of a tree this compiler never builds.
 */
/*
 * break/continue targets for the innermost loop or switch.  Continue is only
 * armed for real loops; a switch frame accepts break alone.
 */
typedef struct {
    int    has_cont;
    int    cont_known;
    size_t cont_target;
    size_t conts[MAX_BREAK_SITES];
    int    nconts;
    size_t breaks[MAX_BREAK_SITES];
    int    nbreaks;
} jump_frame_t;

typedef struct {
    lex_t        *L;
    gen_t        *g;
    int           pesize;
    int           psidx;
    jump_frame_t  jumps[MAX_LOOP_DEPTH];
    int           njumps;
} par_t;

/* A declared type: what one element is, and whether the name is a pointer. */
typedef struct {
    int esize;
    int sidx;
    int is_ptr;
} type_t;

static void pfail(par_t *p, const char *msg)
{
    if (p->g->err) {
        return;
    }
    p->g->err = 1;
    cpy_err_at(p->g->errmsg, sizeof(p->g->errmsg), msg, p->L);
}

static int accept(par_t *p, tok_t t)
{
    if (p->L->tok == t) {
        lex_next(p->L);
        return 1;
    }
    return 0;
}

static void expect(par_t *p, tok_t t, const char *msg)
{
    if (!accept(p, t)) {
        pfail(p, msg);
    }
}

static int32_t const_expr_from(lex_t *L, gen_t *g);

static void skip_cv(par_t *p)
{
    while (accept(p, T_CONST) || accept(p, T_VOLATILE)) {
    }
}

static void jump_push(par_t *p, int has_cont, int cont_known, size_t cont_target)
{
    if (p->njumps >= MAX_LOOP_DEPTH) {
        pfail(p, "too many nested loops");
        return;
    }
    jump_frame_t *f = &p->jumps[p->njumps++];
    f->has_cont = has_cont;
    f->cont_known = cont_known;
    f->cont_target = cont_target;
    f->nconts = 0;
    f->nbreaks = 0;
}

static void jump_set_cont(par_t *p, size_t target)
{
    if (p->njumps <= 0) {
        return;
    }
    jump_frame_t *f = &p->jumps[p->njumps - 1];
    f->cont_target = target;
    f->cont_known = 1;
    for (int i = 0; i < f->nconts; i++) {
        emit_j_to(p->g, f->conts[i], target);
    }
    f->nconts = 0;
}

static void jump_break(par_t *p)
{
    if (p->njumps <= 0) {
        pfail(p, "break outside loop or switch");
        return;
    }
    jump_frame_t *f = &p->jumps[p->njumps - 1];
    if (f->nbreaks >= MAX_BREAK_SITES) {
        pfail(p, "too many breaks");
        return;
    }
    size_t site;
    emit_j_placeholder(p->g, &site);
    f->breaks[f->nbreaks++] = site;
}

static void jump_continue(par_t *p)
{
    int i;
    for (i = p->njumps - 1; i >= 0; i--) {
        if (p->jumps[i].has_cont) {
            break;
        }
    }
    if (i < 0) {
        pfail(p, "continue outside loop");
        return;
    }
    jump_frame_t *f = &p->jumps[i];
    if (f->cont_known) {
        size_t site;
        emit_j_placeholder(p->g, &site);
        emit_j_to(p->g, site, f->cont_target);
        return;
    }
    if (f->nconts >= MAX_BREAK_SITES) {
        pfail(p, "too many continues");
        return;
    }
    size_t site;
    emit_j_placeholder(p->g, &site);
    f->conts[f->nconts++] = site;
}

static void jump_finish(par_t *p, size_t break_target)
{
    if (p->njumps <= 0) {
        return;
    }
    jump_frame_t *f = &p->jumps[p->njumps - 1];
    for (int i = 0; i < f->nbreaks; i++) {
        emit_j_to(p->g, f->breaks[i], break_target);
    }
    if (f->nconts > 0) {
        pfail(p, "continue target unresolved");
    }
    p->njumps--;
}

/*
 * How many elements `[...]` asks for.  It is a constant expression and not just
 * a number because a macro is not a number: with `#define W 320`, `int b[W * 4]`
 * is the ordinary way to write it, and refusing that would make the two
 * features useless together.
 */
static int parse_array_size(par_t *p)
{
    const int32_t n = const_expr_from(p->L, p->g);
    if (p->L->err) {
        pfail(p, "expected array size");
        return -1;
    }
    if (n <= 0 || n > MAX_ARRAY) {
        pfail(p, "expected array size");
        return -1;
    }
    return (int)n;
}

static int accept_type(par_t *p, type_t *ty)
{
    skip_cv(p);
    ty->sidx = -1;
    ty->is_ptr = 0;
    if (accept(p, T_INT)) {
        ty->esize = 4;
    } else if (accept(p, T_CHAR)) {
        ty->esize = 1;
    } else if (p->L->tok == T_STRUCT) {
        lex_next(p->L);
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected a struct name");
            return 0;
        }
        const int si = find_struct(p->g, p->L->text);
        if (si < 0) {
            pfail(p, "unknown struct");
            return 0;
        }
        lex_next(p->L);
        ty->sidx = si;
        ty->esize = p->g->structs[si].size;
    } else if (p->L->tok == T_IDENT) {
        const int ti = find_tdef(p->g, p->L->text);
        if (ti < 0) {
            return 0;
        }
        lex_next(p->L);
        ty->esize = p->g->tdefs[ti].esize;
        ty->sidx = p->g->tdefs[ti].sidx;
        ty->is_ptr = p->g->tdefs[ti].is_ptr;
    } else {
        return 0;
    }
    skip_cv(p);
    if (accept(p, T_STAR)) {
        if (ty->is_ptr) {
            pfail(p, "no pointer to pointer");
            return 0;
        }
        ty->is_ptr = 1;
        skip_cv(p);
    }
    return 1;
}

/* The declared type as a place type, once the shape of the name is known. */
static ptype_t ty_of(const type_t *ty, int is_array)
{
    ptype_t t;
    t.esize = ty->esize;
    t.sidx = ty->sidx;
    if (ty->is_ptr) {
        t.kind = PK_PTR;
    } else if (is_array) {
        t.kind = PK_ARRAY;
    } else {
        t.kind = (ty->sidx >= 0) ? PK_STRUCT : PK_SCALAR;
    }
    return t;
}

static void parse_expr(par_t *p);
static void parse_stmt(par_t *p);
static void parse_call_by_name(par_t *p, const char *name);

static void parse_call_args_to_regs(par_t *p, int nargs)
{
    expect(p, T_LPAREN, "expected '('");
    if (nargs == 0) {
        expect(p, T_RPAREN, "expected ')'");
        return;
    }
    for (int i = 0; i < nargs; i++) {
        parse_expr(p);
        if (i + 1 < nargs) {
            push_a8(p->g);
            expect(p, T_COMMA, "expected ','");
        }
    }
    expect(p, T_RPAREN, "expected ')'");
    emit_mov_n(p->g, 10 + nargs - 1, 8);
    for (int i = nargs - 2; i >= 0; i--) {
        pop_a9(p->g);
        emit_mov_n(p->g, 10 + i, 9);
    }
}

/*
 * The same, for a call whose arity the source decides - a printf.  Every
 * argument is spilled, because the count is only known at the closing paren
 * and the last one cannot be left in a8 without knowing which register it
 * belongs in.
 */
static int parse_call_args_var(par_t *p, int maxargs)
{
    int n = 0;
    expect(p, T_LPAREN, "expected '('");
    if (accept(p, T_RPAREN)) {
        return 0;
    }
    for (;;) {
        parse_expr(p);
        if (n >= maxargs) {
            pfail(p, "too many arguments");
            return n;
        }
        push_a8(p->g);
        n++;
        if (!accept(p, T_COMMA)) {
            break;
        }
    }
    expect(p, T_RPAREN, "expected ')'");
    for (int i = n - 1; i >= 0; i--) {
        pop_a9(p->g);
        emit_mov_n(p->g, 10 + i, 9);
    }
    return n;
}

static void emit_user_call(par_t *p, int fi)
{
    const func_t *fn = &p->g->funcs[fi];
    parse_call_args_to_regs(p, fn->nparams);
    emit_li_code(p->g, 8, (uint32_t)fn->code_off);
    emit_callx8(p->g, 8);
    if (!fn->is_void) {
        emit_mov_n(p->g, 8, 10);
    } else {
        emit_movi(p->g, 8, 0);
    }
}

static void emit_builtin_call(par_t *p, const builtin_t *b)
{
    if (b->api_off == API_OFF_GFX) {
        p->g->needs_gfx = 1;
    }
    if (b->api_off == API_OFF_AUDIO) {
        p->g->needs_audio = 1;
    }
    if (b->min_abi > p->g->min_abi) {
        p->g->min_abi = b->min_abi;
    }
    if (b->null_arg) {
        expect(p, T_LPAREN, "expected '('");
        expect(p, T_RPAREN, "expected ')'");
        emit_movi(p->g, 10, 0);
    } else if (b->fmt != NULL) {
        parse_call_args_to_regs(p, 1);
        emit_mov_n(p->g, 11, 10);
        const int off = add_string(p->g, b->fmt);
        if (off < 0) {
            return;
        }
        emit_li_data(p->g, 10, (uint32_t)off);
    } else if (b->nargs < 0) {
        if (parse_call_args_var(p, MAX_PARAMS) < 1) {
            pfail(p, "expected a format string");
            return;
        }
    } else {
        parse_call_args_to_regs(p, b->nargs);
    }
    emit_load_api_fn(p->g, 8, b->api_off, b->fn_off);
    emit_callx8(p->g, 8);
    if (b->ret_mode == RET_BOOL) {
        emit_mov_n(p->g, 8, 10);
        emit_movi(p->g, 9, 0);
        size_t bne_site, j_end;
        emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
        emit_j_placeholder(p->g, &j_end);
        patch_b(p->g, bne_site, p->g->len);
        emit_movi(p->g, 8, 1);
        emit_j_to(p->g, j_end, p->g->len);
    } else if (b->ret_mode == RET_RAW) {
        emit_mov_n(p->g, 8, 10);
    } else {
        emit_movi(p->g, 8, 0);
    }
}

static void parse_call_by_name(par_t *p, const char *name)
{
    const builtin_t *b = find_builtin(name);
    if (b != NULL) {
        emit_builtin_call(p, b);
        return;
    }
    const int fi = find_func(p->g, name);
    if (fi < 0) {
        pfail(p, "unknown function");
        return;
    }
    emit_user_call(p, fi);
}

/*
 * Precedence levels of the operand a binary operator expects on its right:
 * 0 unary, 1 product, 2 sum, 3 shift, 4 comparison, 5 equality, 6 bit-and,
 * 7 bit-xor.  A shortcut may only take the operand if nothing that follows it
 * binds tighter - `a + b / 2` must not hand `b` to the addition and leave the
 * division behind.
 */
static int binds_tighter(tok_t t, int level)
{
    /*
     * Postfix binds tighter than every operator, at every level: a name with
     * `[`, `.` or `->` after it is a place, and handing the bare name to the
     * operator would leave the rest of the designator for the next parser to
     * trip on.
     */
    if (t == T_LPAREN || t == T_LBRACK || t == T_DOT || t == T_ARROW ||
        t == T_PLUSPLUS || t == T_MINUSMINUS) {
        return 1;
    }
    if (level >= 1 && (t == T_STAR || t == T_SLASH || t == T_PERCENT)) {
        return 1;
    }
    if (level >= 2 && (t == T_PLUS || t == T_MINUS)) {
        return 1;
    }
    if (level >= 3 && (t == T_SHL || t == T_SHR)) {
        return 1;
    }
    if (level >= 4 && (t == T_LT || t == T_GT || t == T_LE || t == T_GE)) {
        return 1;
    }
    if (level >= 5 && (t == T_EQ || t == T_NE)) {
        return 1;
    }
    if (level >= 6 && t == T_AMP) {
        return 1;
    }
    if (level >= 7 && t == T_CARET) {
        return 1;
    }
    return 0;
}

/*
 * A binary operator has to keep its left side somewhere while the right side is
 * evaluated, and the general answer is a spill slot.  But most right sides are
 * a constant or a plain variable - nothing that can disturb a8 - and those can
 * go straight into a9, which removes a store and a load from the majority of
 * the arithmetic in a program.
 *
 * Returns 1 when it did so, leaving the left side in a8 and the right in a9;
 * the caller must then emit the operation with the operands that way round.
 */
static int try_operand_to_a9(par_t *p, int level)
{
    lex_t *L = p->L;
    if (p->g->err || L->err) {
        return 0;
    }
    if (L->tok != T_NUM && L->tok != T_IDENT) {
        return 0;
    }
    if (binds_tighter(peek_tok(L), level)) {
        return 0;
    }
    if (L->tok == T_NUM) {
        emit_movi(p->g, 9, L->num);
        lex_next(L);
        p->pesize = 0;
        p->psidx = -1;
        return 1;
    }
    const int li = find_local(p->g, L->text);
    const int gi = (li < 0) ? find_global(p->g, L->text) : -1;
    if (li < 0 && gi < 0) {
        return 0;
    }
    const sym_t *s = (li >= 0) ? &p->g->locals[li] : &p->g->globals[gi];
    if (ty_is_addr(&s->t)) {
        return 0;
    }
    if (li >= 0) {
        emit_load_sized(p->g, 9, 7, s->off, sym_access_size(s));
    } else {
        emit_li_data(p->g, 9, (uint32_t)s->off);
        emit_load_sized(p->g, 9, 9, 0, sym_access_size(s));
    }
    lex_next(L);
    p->pesize = (s->t.kind == PK_PTR) ? ty_size(p->g, &s->t) : 0;
    p->psidx = (s->t.kind == PK_PTR) ? s->t.sidx : -1;
    return 1;
}

/*
 * `[i]` on a place.  The index goes into a8 first when the base is still a
 * symbol plus a constant, which is the ordinary `a[i]` and costs no spill; an
 * address already computed - `p->row[i]` - has to be parked while the index is
 * evaluated, because evaluating it needs a8 too.
 */
static void place_index(par_t *p, place_t *pl)
{
    gen_t *g = p->g;
    if (pl->t.kind != PK_ARRAY && pl->t.kind != PK_PTR) {
        pfail(p, "not an array");
        return;
    }
    const ptype_t el = ty_elem(&pl->t);
    const int esize = ty_size(g, &el);
    if (pl->t.kind == PK_PTR) {
        place_load(g, pl, 8);
        pl->mat = 1;
        pl->li = -1;
        pl->gi = -1;
        pl->off = 0;
    }
    if (pl->mat) {
        place_addr(g, pl);
        push_a8(g);
        parse_expr(p);
        emit_scale(g, 8, esize);
        pop_a9(g);
    } else {
        parse_expr(p);
        emit_scale(g, 8, esize);
        if (pl->li >= 0) {
            emit_movi(g, 9, g->locals[pl->li].off + pl->off);
            emit_add_n(g, 9, 9, 7);
        } else {
            emit_li_data(g, 9, (uint32_t)(g->globals[pl->gi].off + pl->off));
        }
        pl->mat = 1;
        pl->li = -1;
        pl->gi = -1;
    }
    emit_add_n(g, 8, 8, 9);
    pl->off = 0;
    pl->t = el;
}

/* `.f` and `->f`: the first only moves the offset, the second reads a pointer. */
static void place_field(par_t *p, place_t *pl, int arrow)
{
    gen_t *g = p->g;
    if (arrow) {
        if (pl->t.kind != PK_PTR || pl->t.sidx < 0) {
            pfail(p, "not a pointer to a struct");
            return;
        }
        const ptype_t target = ty_elem(&pl->t);
        place_load(g, pl, 8);
        pl->mat = 1;
        pl->li = -1;
        pl->gi = -1;
        pl->off = 0;
        pl->t = target;
    }
    if (pl->t.kind != PK_STRUCT) {
        pfail(p, "not a struct");
        return;
    }
    if (p->L->tok != T_IDENT) {
        pfail(p, "expected a field name");
        return;
    }
    const struct_t *s = &g->structs[pl->t.sidx];
    const int fi = find_field(s, p->L->text);
    if (fi < 0) {
        pfail(p, "no such field");
        return;
    }
    lex_next(p->L);
    pl->off += s->fields[fi].off;
    pl->t = s->fields[fi].t;
}

/*
 * A name and everything that follows it - `x`, `a[i]`, `v.f`, `p->row[i].g`.
 * One parser for every place a value can be read from or written to, so that
 * reading and assigning cannot disagree about what a name means.
 */
static int parse_place(par_t *p, const char *name, place_t *pl)
{
    const int li = find_local(p->g, name);
    const int gi = (li < 0) ? find_global(p->g, name) : -1;
    if (li < 0 && gi < 0) {
        pfail(p, "unknown identifier");
        return 0;
    }
    pl->mat = 0;
    pl->li = li;
    pl->gi = gi;
    pl->off = 0;
    pl->t = (li >= 0) ? p->g->locals[li].t : p->g->globals[gi].t;
    for (;;) {
        if (accept(p, T_LBRACK)) {
            place_index(p, pl);
            expect(p, T_RBRACK, "expected ']'");
        } else if (accept(p, T_DOT)) {
            place_field(p, pl, 0);
        } else if (accept(p, T_ARROW)) {
            place_field(p, pl, 1);
        } else {
            break;
        }
        if (p->g->err) {
            return 0;
        }
    }
    return 1;
}

/* The value of a place: an array is its address, a scalar is its contents. */
static void place_rvalue(par_t *p, place_t *pl)
{
    if (pl->t.kind == PK_STRUCT) {
        pfail(p, "a struct is not a value; take a field or its address");
        return;
    }
    if (pl->t.kind == PK_ARRAY) {
        place_addr(p->g, pl);
        const ptype_t el = ty_elem(&pl->t);
        p->pesize = ty_size(p->g, &el);
        p->psidx = el.sidx;
        return;
    }
    place_load(p->g, pl, 8);
    if (pl->t.kind == PK_PTR) {
        p->pesize = ty_size(p->g, &pl->t);
        p->psidx = pl->t.sidx;
    } else {
        p->pesize = 0;
        p->psidx = -1;
    }
}

static int is_compound_assign(tok_t t)
{
    return t == T_PLUS_EQ || t == T_MINUS_EQ || t == T_STAR_EQ ||
           t == T_SLASH_EQ || t == T_PERCENT_EQ || t == T_AMP_EQ ||
           t == T_PIPE_EQ || t == T_CARET_EQ || t == T_SHL_EQ ||
           t == T_SHR_EQ;
}

static int place_step(gen_t *g, const place_t *pl)
{
    if (pl->t.kind == PK_PTR) {
        const ptype_t el = ty_elem(&pl->t);
        return ty_size(g, &el);
    }
    return 1;
}

static void emit_compound_op(gen_t *g, tok_t op, int dst, int lhs, int rhs)
{
    if (op == T_PLUS_EQ) {
        emit_add_n(g, dst, lhs, rhs);
    } else if (op == T_MINUS_EQ) {
        emit_sub(g, dst, lhs, rhs);
    } else if (op == T_STAR_EQ) {
        emit_mull(g, dst, lhs, rhs);
    } else if (op == T_SLASH_EQ) {
        emit_quos(g, dst, lhs, rhs);
    } else if (op == T_PERCENT_EQ) {
        emit_rems(g, dst, lhs, rhs);
    } else if (op == T_AMP_EQ) {
        emit_and(g, dst, lhs, rhs);
    } else if (op == T_PIPE_EQ) {
        emit_or(g, dst, lhs, rhs);
    } else if (op == T_CARET_EQ) {
        emit_xor(g, dst, lhs, rhs);
    } else if (op == T_SHL_EQ) {
        emit_ssl(g, rhs);
        emit_sll(g, dst, lhs);
    } else {
        emit_ssr(g, rhs);
        emit_sra(g, dst, lhs);
    }
}

/*
 * `place = expr`.  A place that is still a symbol plus a constant needs nothing
 * held anywhere: the value can be computed straight into a8 and stored.  An
 * address that had to be computed is parked in a spill slot while the value is
 * evaluated, and comes back to a8 for the store.
 */
static void assign_to_place(par_t *p, place_t *pl)
{
    expect(p, T_ASSIGN, "expected '='");
    if (!pl->mat) {
        parse_expr(p);
        place_store(p->g, pl, 8, 9);
        return;
    }
    push_a8(p->g);
    parse_expr(p);
    emit_mov_n(p->g, 11, 8);
    pop_a9(p->g);
    emit_mov_n(p->g, 8, 9);
    place_store(p->g, pl, 11, 9);
}

/* `place op= expr` and prefix/postfix `++`/`--` on a place. */
static void compound_to_place(par_t *p, place_t *pl, tok_t op)
{
    gen_t *g = p->g;
    const int step = place_step(g, pl);
    lex_next(p->L);
    if (ty_is_addr(&pl->t)) {
        pfail(p, "cannot assign a whole array or struct");
        return;
    }
    if (!pl->mat) {
        place_load(g, pl, 8);
        push_a8(g);
        parse_expr(p);
        if (pl->t.kind == PK_PTR && (op == T_PLUS_EQ || op == T_MINUS_EQ)) {
            emit_scale(g, 8, step);
        }
        pop_a9(g);
        emit_compound_op(g, op, 8, 9, 8);
        place_store(g, pl, 8, 9);
        return;
    }
    push_a8(g);
    place_load(g, pl, 8);
    push_a8(g);
    parse_expr(p);
    if (pl->t.kind == PK_PTR && (op == T_PLUS_EQ || op == T_MINUS_EQ)) {
        emit_scale(g, 8, step);
    }
    pop_a9(g);
    emit_compound_op(g, op, 11, 9, 8);
    pop_a9(g);
    emit_mov_n(g, 8, 9);
    place_store(g, pl, 11, 9);
    emit_mov_n(g, 8, 11);
}

static void incdec_place(par_t *p, place_t *pl, int is_inc, int is_prefix)
{
    gen_t *g = p->g;
    const int step = place_step(g, pl);
    if (ty_is_addr(&pl->t)) {
        pfail(p, "cannot increment array or struct");
        return;
    }
    if (!pl->mat) {
        place_load(g, pl, 8);
        if (!is_prefix) {
            emit_mov_n(g, 11, 8);
        }
        emit_movi(g, 9, is_inc ? step : -step);
        emit_add_n(g, 8, 8, 9);
        place_store(g, pl, 8, 9);
        if (!is_prefix) {
            emit_mov_n(g, 8, 11);
        }
        p->pesize = (pl->t.kind == PK_PTR) ? step : 0;
        p->psidx = (pl->t.kind == PK_PTR) ? pl->t.sidx : -1;
        return;
    }
    /* mat: loading through a8 destroys the base — keep the address in a spill. */
    push_a8(g);
    place_load(g, pl, 8);
    if (!is_prefix) {
        emit_mov_n(g, 11, 8);
    }
    emit_movi(g, 9, is_inc ? step : -step);
    emit_add_n(g, 8, 8, 9);
    emit_mov_n(g, 10, 8);
    pop_a9(g);
    emit_mov_n(g, 8, 9);
    place_store(g, pl, 10, 9);
    if (is_prefix) {
        emit_mov_n(g, 8, 10);
    } else {
        emit_mov_n(g, 8, 11);
    }
    p->pesize = (pl->t.kind == PK_PTR) ? step : 0;
    p->psidx = (pl->t.kind == PK_PTR) ? pl->t.sidx : -1;
}

/* Assign / compound / ++/-- on a parsed place (statement or for-step). */
static void assign_or_update_place(par_t *p, place_t *pl)
{
    if (p->L->tok == T_PLUSPLUS) {
        lex_next(p->L);
        incdec_place(p, pl, 1, 0);
        return;
    }
    if (p->L->tok == T_MINUSMINUS) {
        lex_next(p->L);
        incdec_place(p, pl, 0, 0);
        return;
    }
    if (is_compound_assign(p->L->tok)) {
        compound_to_place(p, pl, p->L->tok);
        return;
    }
    assign_to_place(p, pl);
}

static int sizeof_type_bytes(par_t *p, const type_t *ty)
{
    if (ty->is_ptr) {
        return 4;
    }
    if (ty->sidx >= 0) {
        return p->g->structs[ty->sidx].size;
    }
    return ty->esize;
}

/* `sizeof(type)` only — enough for array sizes and layout constants. */
static void parse_sizeof(par_t *p)
{
    expect(p, T_LPAREN, "expected '(' after sizeof");
    type_t ty;
    if (!accept_type(p, &ty)) {
        pfail(p, "expected type in sizeof");
        return;
    }
    expect(p, T_RPAREN, "expected ')'");
    emit_movi(p->g, 8, sizeof_type_bytes(p, &ty));
    p->pesize = 0;
    p->psidx = -1;
}

static void parse_primary(par_t *p)
{
    if (p->g->err) {
        return;
    }
    p->pesize = 0;
    p->psidx = -1;
    if (p->L->tok == T_NUM) {
        emit_movi(p->g, 8, p->L->num);
        lex_next(p->L);
        return;
    }
    if (p->L->tok == T_STRING) {
        const int off = add_string(p->g, p->L->strbuf);
        lex_next(p->L);
        if (off < 0) {
            return;
        }
        emit_li_data(p->g, 8, (uint32_t)off);
        p->pesize = 1;
        return;
    }
    if (p->L->tok == T_IDENT) {
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (p->L->tok == T_LPAREN) {
            parse_call_by_name(p, name);
            p->pesize = 0;
            p->psidx = -1;
            return;
        }
        const int ei = find_enum(p->g, name);
        if (ei >= 0 && p->L->tok != T_LBRACK && p->L->tok != T_DOT &&
            p->L->tok != T_ARROW) {
            emit_movi(p->g, 8, p->g->enums[ei].val);
            return;
        }
        place_t pl;
        if (parse_place(p, name, &pl)) {
            if (p->L->tok == T_PLUSPLUS) {
                lex_next(p->L);
                incdec_place(p, &pl, 1, 0);
                return;
            }
            if (p->L->tok == T_MINUSMINUS) {
                lex_next(p->L);
                incdec_place(p, &pl, 0, 0);
                return;
            }
            place_rvalue(p, &pl);
        }
        return;
    }
    if (accept(p, T_LPAREN)) {
        parse_expr(p);
        expect(p, T_RPAREN, "expected ')'");
        return;
    }
    pfail(p, "expected expression");
}

/*
 * `&place`: the address of storage rather than what is in it.  A pointer to a
 * pointer has nowhere to say so in a type this thin, so taking the address of
 * one is refused instead of quietly losing the second star.
 *
 * `&func` is the code address of a function already defined (define-before-use),
 * as an ordinary word — enough to fill an `ag_dev_ops_t` from Mini-C.
 */
static void parse_addr_of(par_t *p)
{
    if (p->L->tok != T_IDENT) {
        pfail(p, "expected a variable after '&'");
        return;
    }
    char name[MAX_NAME];
    cpy_err(name, sizeof(name), p->L->text);
    lex_next(p->L);
    if (p->L->tok != T_LBRACK && p->L->tok != T_DOT && p->L->tok != T_ARROW) {
        const int fi = find_func(p->g, name);
        if (fi >= 0) {
            emit_li_code(p->g, 8, (uint32_t)p->g->funcs[fi].code_off);
            p->pesize = 4;
            p->psidx = -1;
            return;
        }
    }
    place_t pl;
    if (!parse_place(p, name, &pl)) {
        return;
    }
    if (pl.t.kind == PK_PTR) {
        pfail(p, "no pointer to pointer");
        return;
    }
    const ptype_t target =
        (pl.t.kind == PK_ARRAY) ? ty_elem(&pl.t) : pl.t;
    place_addr(p->g, &pl);
    p->pesize = ty_size(p->g, &target);
    p->psidx = target.sidx;
}

static void parse_unary(par_t *p)
{
    if (accept(p, T_SIZEOF)) {
        parse_sizeof(p);
        return;
    }
    /*
     * Prefix ++/-- need a place.  Only `++name…` is accepted (not `++*p`);
     * `*p = *p + 1` remains the general form for indirect updates.
     */
    if (p->L->tok == T_PLUSPLUS || p->L->tok == T_MINUSMINUS) {
        const int is_inc = (p->L->tok == T_PLUSPLUS);
        lex_next(p->L);
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected a variable after ++/--");
            return;
        }
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        place_t pl;
        if (parse_place(p, name, &pl)) {
            incdec_place(p, &pl, is_inc, 1);
        }
        return;
    }
    if (accept(p, T_MINUS)) {
        parse_unary(p);
        emit_movi(p->g, 9, 0);
        emit_sub(p->g, 8, 9, 8);
        p->pesize = 0;
        return;
    }
    if (accept(p, T_NOT)) {
        parse_unary(p);
        emit_bool_not_a8(p->g);
        p->pesize = 0;
        return;
    }
    if (accept(p, T_TILDE)) {
        parse_unary(p);
        emit_movi(p->g, 9, -1);
        emit_xor(p->g, 8, 8, 9);
        p->pesize = 0;
        return;
    }
    if (accept(p, T_STAR)) {
        parse_unary(p);
        const int k = p->pesize;
        if (k == 0) {
            pfail(p, "not a pointer");
            return;
        }
        if (p->psidx >= 0) {
            pfail(p, "a struct is not a value; use '->'");
            return;
        }
        emit_load_sized(p->g, 8, 8, 0, k);
        p->pesize = 0;
        return;
    }
    if (accept(p, T_AMP)) {
        parse_addr_of(p);
        return;
    }
    if (accept(p, T_PLUS)) {
        parse_unary(p);
        return;
    }
    parse_primary(p);
}

static void parse_mul(par_t *p)
{
    parse_unary(p);
    while (p->L->tok == T_STAR || p->L->tok == T_SLASH ||
           p->L->tok == T_PERCENT) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 0)) {
            push_a8(p->g);
            parse_unary(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        if (op == T_STAR) {
            emit_mull(p->g, 8, lhs, rhs);
        } else if (op == T_SLASH) {
            emit_quos(p->g, 8, lhs, rhs);
        } else {
            emit_rems(p->g, 8, lhs, rhs);
        }
        p->pesize = 0;
    }
}

static void parse_add(par_t *p)
{
    parse_mul(p);
    while (p->L->tok == T_PLUS || p->L->tok == T_MINUS) {
        const tok_t op = p->L->tok;
        const int lp = p->pesize;
        const int lsid = p->psidx;
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 1)) {
            push_a8(p->g);
            parse_mul(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        const int rp = p->pesize;
        const int rsid = p->psidx;
        /*
         * A pointer steps by elements, as C says, so the int side is scaled
         * before the add - which for a char pointer is no instruction at all.
         * Two pointers have no meaning added and their difference is not worth
         * a divide here, so those stay plain ints.
         */
        int res = 0;
        int res_sid = -1;
        if (lp != 0 && rp == 0) {
            emit_scale(p->g, rhs, lp);
            res = lp;
            res_sid = lsid;
        } else if (lp == 0 && rp != 0 && op == T_PLUS) {
            emit_scale(p->g, lhs, rp);
            res = rp;
            res_sid = rsid;
        }
        if (op == T_PLUS) {
            emit_add_n(p->g, 8, lhs, rhs);
        } else {
            emit_sub(p->g, 8, lhs, rhs);
        }
        p->pesize = res;
        p->psidx = res_sid;
    }
}

static void parse_shift(par_t *p)
{
    parse_add(p);
    while (p->L->tok == T_SHL || p->L->tok == T_SHR) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        /*
         * Shifting by a constant is the case that matters - masks, fixed point,
         * a phase accumulator - and it needs neither SAR nor a register for the
         * count.  A shift by zero is not encodable in SLLI, and is nothing.
         */
        if (p->L->tok == T_NUM && p->L->num >= 0 && p->L->num <= 31 &&
            !binds_tighter(peek_tok(p->L), 2)) {
            const int sa = (int)p->L->num;
            lex_next(p->L);
            if (sa != 0) {
                if (op == T_SHL) {
                    emit_slli(p->g, 8, 8, sa);
                } else {
                    emit_srai(p->g, 8, 8, sa);
                }
            }
            p->pesize = 0;
            continue;
        }
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 2)) {
            push_a8(p->g);
            parse_add(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        if (op == T_SHL) {
            emit_ssl(p->g, rhs);
            emit_sll(p->g, 8, lhs);
        } else {
            emit_ssr(p->g, rhs);
            emit_sra(p->g, 8, lhs);
        }
        p->pesize = 0;
    }
}

static void parse_rel(par_t *p)
{
    parse_shift(p);
    while (p->L->tok == T_LT || p->L->tok == T_GT || p->L->tok == T_LE ||
           p->L->tok == T_GE) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 3)) {
            push_a8(p->g);
            parse_shift(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        if (op == T_LT) {
            emit_setlt(p->g, 8, lhs, rhs);
        } else if (op == T_GT) {
            emit_setlt(p->g, 8, rhs, lhs);
        } else if (op == T_LE) {
            emit_setlt(p->g, 8, rhs, lhs);
            emit_bool_not_a8(p->g);
        } else {
            emit_setlt(p->g, 8, lhs, rhs);
            emit_bool_not_a8(p->g);
        }
        p->pesize = 0;
    }
}

static void parse_eq(par_t *p)
{
    parse_rel(p);
    while (p->L->tok == T_EQ || p->L->tok == T_NE) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 4)) {
            push_a8(p->g);
            parse_rel(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        const int opn = (op == T_EQ) ? B_BEQ : B_BNE;
        size_t b_site, j_site;
        emit_b_placeholder(p->g, opn, lhs, rhs, &b_site);
        emit_movi(p->g, 8, 0);
        emit_j_placeholder(p->g, &j_site);
        patch_b(p->g, b_site, p->g->len);
        emit_movi(p->g, 8, 1);
        emit_j_to(p->g, j_site, p->g->len);
        p->pesize = 0;
    }
}

/*
 * The three bitwise levels sit between equality and `&&`, exactly where C puts
 * them - which is the reason `(x & 3) == 0` needs its parentheses there too.
 */
static void parse_bitand(par_t *p)
{
    parse_eq(p);
    while (p->L->tok == T_AMP) {
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 5)) {
            push_a8(p->g);
            parse_eq(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        emit_and(p->g, 8, lhs, rhs);
        p->pesize = 0;
    }
}

static void parse_bitxor(par_t *p)
{
    parse_bitand(p);
    while (p->L->tok == T_CARET) {
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 6)) {
            push_a8(p->g);
            parse_bitand(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        emit_xor(p->g, 8, lhs, rhs);
        p->pesize = 0;
    }
}

static void parse_bitor(par_t *p)
{
    parse_bitxor(p);
    while (p->L->tok == T_PIPE) {
        lex_next(p->L);
        int lhs = 8, rhs = 9;
        if (!try_operand_to_a9(p, 7)) {
            push_a8(p->g);
            parse_bitxor(p);
            pop_a9(p->g);
            lhs = 9;
            rhs = 8;
        }
        emit_or(p->g, 8, lhs, rhs);
        p->pesize = 0;
    }
}

static void parse_and(par_t *p)
{
    parse_bitor(p);
    while (accept(p, T_ANDAND)) {
        emit_movi(p->g, 9, 0);
        size_t false_b, false_b2, end_j;
        emit_b_placeholder(p->g, B_BEQ, 8, 9, &false_b);
        parse_bitor(p);
        /*
         * The right side freely uses a9 (the fast operand path leaves a
         * constant there), so zero it again before asking whether the result
         * is zero - otherwise `a && b != 5` compares the bool against 5.
         */
        emit_movi(p->g, 9, 0);
        emit_b_placeholder(p->g, B_BEQ, 8, 9, &false_b2);
        emit_movi(p->g, 8, 1);
        emit_j_placeholder(p->g, &end_j);
        patch_b(p->g, false_b, p->g->len);
        patch_b(p->g, false_b2, p->g->len);
        emit_movi(p->g, 8, 0);
        emit_j_to(p->g, end_j, p->g->len);
        p->pesize = 0;
    }
}

static void parse_or(par_t *p)
{
    parse_and(p);
    while (accept(p, T_OROR)) {
        emit_movi(p->g, 9, 0);
        size_t true_b, true_b2, end_j;
        emit_b_placeholder(p->g, B_BNE, 8, 9, &true_b);
        parse_and(p);
        emit_movi(p->g, 9, 0);
        emit_b_placeholder(p->g, B_BNE, 8, 9, &true_b2);
        emit_movi(p->g, 8, 0);
        emit_j_placeholder(p->g, &end_j);
        patch_b(p->g, true_b, p->g->len);
        patch_b(p->g, true_b2, p->g->len);
        emit_movi(p->g, 8, 1);
        emit_j_to(p->g, end_j, p->g->len);
        p->pesize = 0;
    }
}

/* `cond ? a : b` — both arms leave a value in a8. */
static void parse_cond(par_t *p)
{
    parse_or(p);
    if (!accept(p, T_QUESTION)) {
        return;
    }
    emit_movi(p->g, 9, 0);
    size_t bne_site, j_else, j_end;
    emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
    emit_j_placeholder(p->g, &j_else);
    patch_b(p->g, bne_site, p->g->len);
    parse_expr(p);
    emit_j_placeholder(p->g, &j_end);
    emit_j_to(p->g, j_else, p->g->len);
    expect(p, T_COLON, "expected ':' in ?:");
    parse_cond(p);
    emit_j_to(p->g, j_end, p->g->len);
    p->pesize = 0;
    p->psidx = -1;
}

static void parse_expr(par_t *p)
{
    parse_cond(p);
}

static void parse_block(par_t *p)
{
    expect(p, T_LBRACE, "expected '{'");
    while (p->L->tok != T_RBRACE && p->L->tok != T_EOF && !p->g->err &&
           !p->L->err) {
        parse_stmt(p);
    }
    expect(p, T_RBRACE, "expected '}'");
}

/* `*expr = value` / `*expr op= value` — target is an address, not a name. */
static void assign_deref(par_t *p)
{
    parse_unary(p);
    const int k = p->pesize;
    if (k == 0) {
        pfail(p, "not a pointer");
        return;
    }
    if (p->psidx >= 0) {
        pfail(p, "cannot assign a whole struct");
        return;
    }
    if (is_compound_assign(p->L->tok)) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        push_a8(p->g);
        emit_load_sized(p->g, 8, 8, 0, k);
        push_a8(p->g);
        parse_expr(p);
        pop_a9(p->g);
        emit_compound_op(p->g, op, 11, 9, 8);
        pop_a9(p->g);
        emit_store_sized(p->g, 11, 9, 0, k, 8);
        emit_mov_n(p->g, 8, 11);
        return;
    }
    expect(p, T_ASSIGN, "expected '='");
    push_a8(p->g);
    parse_expr(p);
    emit_mov_n(p->g, 11, 8);
    pop_a9(p->g);
    emit_store_sized(p->g, 11, 9, 0, k, 8);
}

/*
 * The name and shape of a local declaration, the type having been taken
 * already: `x`, `c`, `*p`, `a[N]` and `struct v s` all arrive here.
 */
static void parse_local_decl(par_t *p, const type_t *ty)
{
    if (p->L->tok != T_IDENT) {
        pfail(p, "expected identifier after type");
        return;
    }
    char name[MAX_NAME];
    cpy_err(name, sizeof(name), p->L->text);
    lex_next(p->L);
    if (accept(p, T_LBRACK)) {
        if (ty->is_ptr) {
            pfail(p, "no arrays of pointers");
            return;
        }
        const int n = parse_array_size(p);
        if (n < 0) {
            return;
        }
        expect(p, T_RBRACK, "expected ']'");
        const ptype_t t = ty_of(ty, 1);
        (void)add_local_t(p->g, name, &t, n);
        return;
    }
    const ptype_t t = ty_of(ty, 0);
    const int idx = add_local_t(p->g, name, &t, 1);
    if (idx < 0) {
        return;
    }
    if (t.kind == PK_STRUCT) {
        if (p->L->tok == T_ASSIGN) {
            pfail(p, "cannot assign a whole struct");
        }
        return;
    }
    if (accept(p, T_ASSIGN)) {
        parse_expr(p);
    } else {
        emit_movi(p->g, 8, 0);
    }
    emit_store_sized(p->g, 8, 7, p->g->locals[idx].off,
                     sym_access_size(&p->g->locals[idx]), 9);
}

static void parse_for_assign_or_empty(par_t *p, int allow_decl)
{
    if (p->L->tok == T_SEMI || p->L->tok == T_RPAREN) {
        return;
    }
    if (allow_decl) {
        type_t ty;
        if (accept_type(p, &ty)) {
            parse_local_decl(p, &ty);
            return;
        }
    }
    if (p->L->tok == T_STAR) {
        lex_next(p->L);
        assign_deref(p);
        return;
    }
    if (p->L->tok == T_PLUSPLUS || p->L->tok == T_MINUSMINUS) {
        parse_unary(p);
        return;
    }
    if (p->L->tok != T_IDENT) {
        pfail(p, "expected assignment in for");
        return;
    }
    char name[MAX_NAME];
    cpy_err(name, sizeof(name), p->L->text);
    lex_next(p->L);
    place_t pl;
    if (parse_place(p, name, &pl)) {
        assign_or_update_place(p, &pl);
    }
}

static void parse_switch(par_t *p)
{
    expect(p, T_LPAREN, "expected '(' after switch");
    parse_expr(p);
    expect(p, T_RPAREN, "expected ')'");
    if (p->g->spill_top >= SPILL_SLOTS) {
        pfail(p, "expression too deep");
        return;
    }
    const int slot = p->g->spill_top++;
    emit_s32i(p->g, 8, 7, slot * 4);

    size_t j_dispatch;
    emit_j_placeholder(p->g, &j_dispatch);
    jump_push(p, 0, 0, 0);

    size_t case_off[MAX_CASES];
    int32_t case_val[MAX_CASES];
    int ncases = 0;
    size_t default_off = (size_t)-1;

    expect(p, T_LBRACE, "expected '{' after switch");
    while (p->L->tok != T_RBRACE && p->L->tok != T_EOF && !p->g->err &&
           !p->L->err) {
        if (accept(p, T_CASE)) {
            const int32_t v = const_expr_from(p->L, p->g);
            if (p->L->err) {
                pfail(p, "expected case value");
                return;
            }
            expect(p, T_COLON, "expected ':' after case");
            if (ncases >= MAX_CASES) {
                pfail(p, "too many cases");
                return;
            }
            case_val[ncases] = v;
            case_off[ncases] = p->g->len;
            ncases++;
            continue;
        }
        if (accept(p, T_DEFAULT)) {
            expect(p, T_COLON, "expected ':' after default");
            if (default_off != (size_t)-1) {
                pfail(p, "duplicate default");
                return;
            }
            default_off = p->g->len;
            continue;
        }
        parse_stmt(p);
    }
    expect(p, T_RBRACE, "expected '}'");

    size_t j_end;
    emit_j_placeholder(p->g, &j_end);
    emit_j_to(p->g, j_dispatch, p->g->len);
    emit_l32i(p->g, 8, 7, slot * 4);
    for (int i = 0; i < ncases; i++) {
        /* beq is only ±128 bytes; case bodies sit above the dispatch. */
        emit_movi(p->g, 9, case_val[i]);
        size_t bne_site, j_case;
        emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
        emit_j_placeholder(p->g, &j_case);
        emit_j_to(p->g, j_case, case_off[i]);
        patch_b(p->g, bne_site, p->g->len);
    }
    size_t j_miss = 0;
    if (default_off != (size_t)-1) {
        size_t j_def;
        emit_j_placeholder(p->g, &j_def);
        emit_j_to(p->g, j_def, default_off);
    } else {
        emit_j_placeholder(p->g, &j_miss);
    }
    emit_j_to(p->g, j_end, p->g->len);
    if (default_off == (size_t)-1) {
        emit_j_to(p->g, j_miss, p->g->len);
    }
    jump_finish(p, p->g->len);
    p->g->spill_top--;
}

static void parse_stmt(par_t *p)
{
    if (p->g->err || p->L->err) {
        return;
    }
    if (p->L->tok == T_LBRACE) {
        parse_block(p);
        return;
    }
    if (accept(p, T_BREAK)) {
        expect(p, T_SEMI, "expected ';' after break");
        jump_break(p);
        return;
    }
    if (accept(p, T_CONTINUE)) {
        expect(p, T_SEMI, "expected ';' after continue");
        jump_continue(p);
        return;
    }
    if (accept(p, T_RETURN)) {
        if (p->L->tok != T_SEMI) {
            parse_expr(p);
            emit_mov_n(p->g, 2, 8);
        } else {
            emit_movi(p->g, 2, 0);
        }
        expect(p, T_SEMI, "expected ';' after return");
        emit_retw_n(p->g);
        return;
    }
    if (accept(p, T_IF)) {
        expect(p, T_LPAREN, "expected '(' after if");
        parse_expr(p);
        expect(p, T_RPAREN, "expected ')'");
        emit_movi(p->g, 9, 0);
        size_t bne_site, j_else;
        emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
        emit_j_placeholder(p->g, &j_else);
        patch_b(p->g, bne_site, p->g->len);
        parse_stmt(p);
        if (accept(p, T_ELSE)) {
            size_t j_end;
            emit_j_placeholder(p->g, &j_end);
            emit_j_to(p->g, j_else, p->g->len);
            parse_stmt(p);
            emit_j_to(p->g, j_end, p->g->len);
        } else {
            emit_j_to(p->g, j_else, p->g->len);
        }
        return;
    }
    if (accept(p, T_WHILE)) {
        expect(p, T_LPAREN, "expected '(' after while");
        const size_t check = p->g->len;
        jump_push(p, 1, 1, check);
        parse_expr(p);
        expect(p, T_RPAREN, "expected ')'");
        emit_movi(p->g, 9, 0);
        size_t bne_site, j_done;
        emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
        emit_j_placeholder(p->g, &j_done);
        patch_b(p->g, bne_site, p->g->len);
        parse_stmt(p);
        size_t j_back;
        emit_j_placeholder(p->g, &j_back);
        emit_j_to(p->g, j_back, check);
        emit_j_to(p->g, j_done, p->g->len);
        jump_finish(p, p->g->len);
        return;
    }
    if (accept(p, T_DO)) {
        const size_t body = p->g->len;
        jump_push(p, 1, 0, 0);
        parse_stmt(p);
        expect(p, T_WHILE, "expected 'while' after do");
        expect(p, T_LPAREN, "expected '(' after while");
        jump_set_cont(p, p->g->len);
        parse_expr(p);
        expect(p, T_RPAREN, "expected ')'");
        expect(p, T_SEMI, "expected ';' after do-while");
        emit_movi(p->g, 9, 0);
        size_t bne_site;
        emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
        patch_b(p->g, bne_site, body);
        jump_finish(p, p->g->len);
        return;
    }
    if (accept(p, T_FOR)) {
        expect(p, T_LPAREN, "expected '(' after for");
        parse_for_assign_or_empty(p, 1);
        expect(p, T_SEMI, "expected ';' in for");
        const size_t check = p->g->len;
        size_t j_done = 0;
        int has_cond = 0;
        if (p->L->tok != T_SEMI) {
            has_cond = 1;
            parse_expr(p);
        }
        expect(p, T_SEMI, "expected ';' in for");
        if (has_cond) {
            emit_movi(p->g, 9, 0);
            size_t bne_site;
            emit_b_placeholder(p->g, B_BNE, 8, 9, &bne_site);
            emit_j_placeholder(p->g, &j_done);
            patch_b(p->g, bne_site, p->g->len);
        }
        size_t j_body, step_start, j_after_step;
        emit_j_placeholder(p->g, &j_body);
        step_start = p->g->len;
        parse_for_assign_or_empty(p, 0);
        expect(p, T_RPAREN, "expected ')'");
        emit_j_placeholder(p->g, &j_after_step);
        emit_j_to(p->g, j_after_step, check);
        emit_j_to(p->g, j_body, p->g->len);
        jump_push(p, 1, 1, step_start);
        parse_stmt(p);
        size_t j_to_step;
        emit_j_placeholder(p->g, &j_to_step);
        emit_j_to(p->g, j_to_step, step_start);
        if (has_cond) {
            emit_j_to(p->g, j_done, p->g->len);
        }
        jump_finish(p, p->g->len);
        return;
    }
    if (accept(p, T_SWITCH)) {
        parse_switch(p);
        return;
    }
    {
        type_t ty;
        if (accept_type(p, &ty)) {
            parse_local_decl(p, &ty);
            expect(p, T_SEMI, "expected ';' after declaration");
            return;
        }
    }
    if (p->L->tok == T_STAR) {
        lex_next(p->L);
        assign_deref(p);
        expect(p, T_SEMI, "expected ';'");
        return;
    }
    if (p->L->tok == T_IDENT) {
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (p->L->tok == T_LPAREN) {
            parse_call_by_name(p, name);
            expect(p, T_SEMI, "expected ';'");
            return;
        }
        place_t pl;
        if (parse_place(p, name, &pl)) {
            assign_or_update_place(p, &pl);
        }
        expect(p, T_SEMI, "expected ';'");
        return;
    }
    parse_expr(p);
    expect(p, T_SEMI, "expected ';'");
}

static int parse_params(par_t *p)
{
    int nparams = 0;
    expect(p, T_LPAREN, "expected '('");
    if (accept(p, T_VOID) || p->L->tok == T_RPAREN) {
        expect(p, T_RPAREN, "expected ')'");
        return 0;
    }
    for (;;) {
        type_t ty;
        if (!accept_type(p, &ty)) {
            pfail(p, "expected parameter type");
            return 0;
        }
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected parameter name");
            return 0;
        }
        if (nparams >= MAX_PARAMS) {
            pfail(p, "too many parameters");
            return 0;
        }
        if (ty.sidx >= 0 && !ty.is_ptr) {
            pfail(p, "no struct parameters; pass a pointer");
            return 0;
        }
        const ptype_t t = ty_of(&ty, 0);
        (void)add_local_t(p->g, p->L->text, &t, 1);
        lex_next(p->L);
        nparams++;
        if (!accept(p, T_COMMA)) {
            break;
        }
    }
    expect(p, T_RPAREN, "expected ')'");
    return nparams;
}

static void parse_function(par_t *p, int is_void, const char *name)
{
    if (find_func(p->g, name) >= 0 || find_global(p->g, name) >= 0) {
        pfail(p, "duplicate function");
        return;
    }
    if (find_builtin(name) != NULL) {
        pfail(p, "reserved builtin name");
        return;
    }
    if (p->g->nfuncs >= MAX_FUNCS) {
        pfail(p, "too many functions");
        return;
    }

    p->g->nlocals = 0;
    p->g->local_words = 0;
    p->g->spill_top = 0;

    const size_t code_off = p->g->len;
    emit_entry(p->g, SPILL_BYTES + MAX_LOCAL_WORDS * 4 + FRAME_TOP_SAVE);

    /*
     * Parameters arrive in a2..a7 and are stored through a1, because a7 is
     * about to become the frame pointer - with six parameters the last one
     * would otherwise be overwritten by its own frame pointer.  a7 and a1 hold
     * the same address; only the order matters.
     */
    const int nparams = parse_params(p);
    if (p->g->err) {
        return;
    }
    for (int i = 0; i < nparams && i < MAX_PARAMS; i++) {
        emit_store_sized(p->g, 2 + i, 1, p->g->locals[i].off,
                         sym_access_size(&p->g->locals[i]), 8);
    }
    emit_mov_n(p->g, 7, 1);

    const int fi = p->g->nfuncs++;
    cpy_err(p->g->funcs[fi].name, MAX_NAME, name);
    p->g->funcs[fi].code_off = code_off;
    p->g->funcs[fi].nparams = nparams;
    p->g->funcs[fi].is_void = is_void;
    if (CC_STRCMP(name, "ag_main") == 0 ||
        CC_STRCMP(name, "ag_driver_init") == 0) {
        if (p->g->entry_idx >= 0) {
            pfail(p, "only one of ag_main / ag_driver_init");
            return;
        }
        p->g->entry_idx = fi;
        p->g->is_driver = (CC_STRCMP(name, "ag_driver_init") == 0) ? 1 : 0;
    }

    parse_block(p);

    emit_movi(p->g, 2, 0);
    emit_retw_n(p->g);

    /* The stack pointer stays 16-byte aligned, so the frame rounds to 16. */
    const int used = SPILL_BYTES + p->g->local_words * 4 + FRAME_TOP_SAVE;
    patch_entry(p->g, code_off, (used + 15) & ~15);
}

/*
 * `struct name { ... };`.  The name is registered before the body is read, so a
 * field can point at the struct being defined; its size stays 0 until the body
 * closes, which is why nothing but a pointer to it can be declared inside.
 *
 * Fields are laid out in order, each on its natural alignment - a word for
 * everything but a char - and the whole rounds up to a word.  That is what the
 * C on the host does with the same fields, so a struct described here and one
 * described there are the same bytes, which matters the moment a program passes
 * a pointer to one across the ABI.
 */
static void parse_struct_body(par_t *p, const char *name)
{
    gen_t *g = p->g;
    if (find_struct(g, name) >= 0) {
        pfail(p, "duplicate struct");
        return;
    }
    if (g->nstructs >= MAX_STRUCTS) {
        pfail(p, "too many structs");
        return;
    }
    const int si = g->nstructs++;
    struct_t *s = &g->structs[si];
    cpy_err(s->name, MAX_NAME, name);
    s->size = 0;
    s->nfields = 0;
    expect(p, T_LBRACE, "expected '{'");
    int off = 0;
    while (p->L->tok != T_RBRACE && !p->g->err && !p->L->err) {
        type_t ty;
        if (!accept_type(p, &ty)) {
            pfail(p, "expected a field type");
            return;
        }
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected a field name");
            return;
        }
        char fname[MAX_NAME];
        cpy_err(fname, sizeof(fname), p->L->text);
        lex_next(p->L);
        int nelem = 1;
        int is_array = 0;
        if (accept(p, T_LBRACK)) {
            if (ty.is_ptr) {
                pfail(p, "no arrays of pointers");
                return;
            }
            nelem = parse_array_size(p);
            if (nelem < 0) {
                return;
            }
            is_array = 1;
            expect(p, T_RBRACK, "expected ']'");
        }
        expect(p, T_SEMI, "expected ';' after a field");
        if (p->g->err || p->L->err) {
            return;
        }
        if (s->nfields >= MAX_FIELDS) {
            pfail(p, "too many fields");
            return;
        }
        if (find_field(s, fname) >= 0) {
            pfail(p, "duplicate field");
            return;
        }
        const ptype_t t = ty_of(&ty, is_array);
        if (t.kind != PK_PTR && ty.sidx == si) {
            pfail(p, "a struct cannot contain itself");
            return;
        }
        const int bytes = sym_bytes(g, &t, nelem);
        if (bytes <= 0) {
            pfail(p, "bad field size");
            return;
        }
        const int align = (t.kind != PK_PTR && ty_size(g, &t) == 1) ? 1 : 4;
        off = (off + align - 1) & ~(align - 1);
        field_t *f = &s->fields[s->nfields++];
        cpy_err(f->name, MAX_NAME, fname);
        f->off = off;
        f->t = t;
        off += bytes;
    }
    expect(p, T_RBRACE, "expected '}'");
    expect(p, T_SEMI, "expected ';'");
    if (p->g->err || p->L->err) {
        return;
    }
    if (s->nfields == 0) {
        pfail(p, "a struct needs a field");
        return;
    }
    s->size = (off + 3) & ~3;
}

/* `enum [tag] { A, B = 3, C };` — enumerators become int constants. */
static void parse_enum_body(par_t *p)
{
    expect(p, T_LBRACE, "expected '{' after enum");
    int32_t next = 0;
    int got = 0;
    while (p->L->tok != T_RBRACE && !p->g->err && !p->L->err) {
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected enumerator");
            return;
        }
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (accept(p, T_ASSIGN)) {
            next = const_expr_from(p->L, p->g);
            if (p->L->err) {
                pfail(p, "expected enumerator value");
                return;
            }
        }
        if (add_enum(p->g, name, next) < 0) {
            pfail(p, p->g->errmsg);
            return;
        }
        next++;
        got = 1;
        if (!accept(p, T_COMMA)) {
            break;
        }
    }
    expect(p, T_RBRACE, "expected '}'");
    if (!got) {
        pfail(p, "empty enum");
    }
}

static void parse_typedef(par_t *p)
{
    type_t ty;
    if (accept(p, T_ENUM)) {
        if (p->L->tok == T_IDENT) {
            lex_next(p->L); /* optional tag */
        }
        parse_enum_body(p);
        ty.esize = 4;
        ty.sidx = -1;
        ty.is_ptr = 0;
    } else if (!accept_type(p, &ty)) {
        pfail(p, "expected type after typedef");
        return;
    }
    if (p->L->tok != T_IDENT) {
        pfail(p, "expected typedef name");
        return;
    }
    if (find_tdef(p->g, p->L->text) >= 0 || find_enum(p->g, p->L->text) >= 0) {
        pfail(p, "duplicate typedef");
        return;
    }
    if (p->g->ntdefs >= MAX_TYPEDEFS) {
        pfail(p, "too many typedefs");
        return;
    }
    tdef_t *td = &p->g->tdefs[p->g->ntdefs++];
    cpy_err(td->name, MAX_NAME, p->L->text);
    td->esize = ty.esize;
    td->sidx = ty.sidx;
    td->is_ptr = ty.is_ptr;
    lex_next(p->L);
    expect(p, T_SEMI, "expected ';' after typedef");
}

static void parse_program(par_t *p)
{
    p->g->data_next = 4;
    p->g->entry_idx = -1;
    p->g->is_driver = 0;

    while (p->L->tok != T_EOF && !p->g->err && !p->L->err) {
        if (accept(p, T_TYPEDEF)) {
            if (p->g->nfuncs > 0) {
                pfail(p, "typedefs must precede functions");
                return;
            }
            parse_typedef(p);
            continue;
        }
        if (accept(p, T_ENUM)) {
            if (p->g->nfuncs > 0) {
                pfail(p, "enums must precede functions");
                return;
            }
            if (p->L->tok == T_IDENT) {
                lex_next(p->L);
            }
            parse_enum_body(p);
            expect(p, T_SEMI, "expected ';' after enum");
            continue;
        }
        if (accept(p, T_VOID)) {
            if (p->L->tok != T_IDENT) {
                pfail(p, "expected function name");
                return;
            }
            char name[MAX_NAME];
            cpy_err(name, sizeof(name), p->L->text);
            lex_next(p->L);
            parse_function(p, 1, name);
            continue;
        }
        type_t ty;
        if (p->L->tok == T_STRUCT) {
            /*
             * `struct v {` defines, `struct v x` declares: which one it is only
             * shows up a token after the name, so the type is taken apart here
             * rather than in accept_type.
             */
            lex_next(p->L);
            if (p->L->tok != T_IDENT) {
                pfail(p, "expected a struct name");
                return;
            }
            char sname[MAX_NAME];
            cpy_err(sname, sizeof(sname), p->L->text);
            lex_next(p->L);
            if (p->L->tok == T_LBRACE) {
                parse_struct_body(p, sname);
                continue;
            }
            const int si = find_struct(p->g, sname);
            if (si < 0) {
                pfail(p, "unknown struct");
                return;
            }
            ty.sidx = si;
            ty.esize = p->g->structs[si].size;
            ty.is_ptr = accept(p, T_STAR) ? 1 : 0;
        } else if (!accept_type(p, &ty)) {
            pfail(p, "expected declaration or function");
            return;
        }
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected identifier");
            return;
        }
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (p->L->tok == T_LPAREN) {
            /* Values are ints; an address returned as an int is one too. */
            if (ty.is_ptr || ty.sidx >= 0 || ty.esize != 4) {
                pfail(p, "a function returns int or void");
                return;
            }
            parse_function(p, 0, name);
            continue;
        }
        if (p->g->nfuncs > 0) {
            pfail(p, "globals must precede functions");
            return;
        }
        if (accept(p, T_LBRACK)) {
            if (ty.is_ptr) {
                pfail(p, "no arrays of pointers");
                return;
            }
            const int n = parse_array_size(p);
            if (n < 0) {
                return;
            }
            expect(p, T_RBRACK, "expected ']'");
            expect(p, T_SEMI, "expected ';'");
            const ptype_t t = ty_of(&ty, 1);
            (void)add_global_t(p->g, name, &t, n);
            continue;
        }
        expect(p, T_SEMI, "expected ';'");
        const ptype_t t = ty_of(&ty, 0);
        (void)add_global_t(p->g, name, &t, 1);
    }

    if (!p->g->err && p->g->entry_idx < 0) {
        pfail(p, "entry must be ag_main or ag_driver_init");
    }
    if (!p->g->err && p->g->is_driver && !p->g->pp.has_drv) {
        pfail(p, "driver needs #pragma drv \"NAME\" \"VER\" \"AUTHOR\"");
    }
}

static void finalize_lits(gen_t *g, size_t lit_bytes)
{
    for (int i = 0; i < g->nlit; i++) {
        if (g->lit_kind[i] == LIT_IMM) {
            continue;
        }
        if (g->lit_kind[i] == LIT_CODE_OFF) {
            g->lit[i] = CODE_BASE + (uint32_t)lit_bytes + g->lit_off[i];
            add_reloc(g, (uint32_t)i * 4u);
        } else if (g->lit_kind[i] == LIT_DATA_OFF) {
            g->lit[i] = DATA_BASE + g->lit_off[i];
            add_reloc(g, ((uint32_t)i * 4u) | AG_AXE_R_TO_DATA);
        }
    }
}

static void fix_l32r(gen_t *g, size_t lit_bytes)
{
    for (int s = 0; s < g->nlit_sites; s++) {
        const size_t i = g->lit_site_off[s];
        const int li = (int)g->lit_site_li[s];
        if (i + 2 >= g->len || (g->code[i] & 0x0f) != 0x1 ||
            g->code[i + 1] != 0xff || g->code[i + 2] != 0xff) {
            gfail(g, "bad literal site");
            return;
        }
        if (li < 0 || li >= g->nlit) {
            gfail(g, "bad literal index");
            return;
        }
        const uint32_t pc = CODE_BASE + (uint32_t)lit_bytes + (uint32_t)i;
        const uint32_t next = (pc + 3u) & ~3u;
        const uint32_t target = CODE_BASE + (uint32_t)li * 4u;
        const int32_t imm = (int32_t)((int32_t)target - (int32_t)next) >> 2;
        if (imm < -32768 || imm > 32767) {
            gfail(g, "literal out of range");
            return;
        }
        g->code[i + 1] = (uint8_t)(imm & 0xff);
        g->code[i + 2] = (uint8_t)((imm >> 8) & 0xff);
    }
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_str(uint8_t *p, size_t n, const char *s)
{
    size_t i = 0;
    while (s[i] != '\0' && i + 1 < n) {
        p[i] = (uint8_t)s[i];
        i++;
    }
    while (i < n) {
        p[i++] = 0;
    }
}

/* The compiler owns what the include reader handed it; this is where it goes. */
static void gen_free(gen_t *g)
{
    for (int i = 0; i < g->pp.nincls; i++) {
        CC_FREE(g->pp.incls[i].text);
    }
    CC_FREE(g->data);
    CC_FREE(g);
}

int cc_compile_to_axe(const char *src, size_t src_len, cc_result_t *out)
{
    return cc_compile_to_axe_inc(src, src_len, NULL, NULL, out);
}

int cc_compile_to_axe_inc(const char *src, size_t src_len,
                          cc_read_file_fn reader, void *ctx, cc_result_t *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (src == NULL) {
        cpy_err(out->err, sizeof(out->err), "null source");
        return -1;
    }

    uint8_t *text = (uint8_t *)CC_ALLOC(CODE_CAP);
    if (text == NULL) {
        cpy_err(out->err, sizeof(out->err), "out of memory");
        return -1;
    }

    uint8_t *data_buf = (uint8_t *)CC_ALLOC(DATA_CAP);
    if (data_buf == NULL) {
        cpy_err(out->err, sizeof(out->err), "out of memory");
        CC_FREE(text);
        return -1;
    }

    gen_t *g = (gen_t *)CC_ALLOC(sizeof(gen_t));
    if (g == NULL) {
        cpy_err(out->err, sizeof(out->err), "out of memory");
        CC_FREE(text);
        CC_FREE(data_buf);
        return -1;
    }
    memset(g, 0, sizeof(*g));
    g->code = text;
    g->cap = CODE_CAP;
    g->data = data_buf;
    g->data_cap = (int)DATA_CAP;
    memset(data_buf, 0, DATA_CAP);
    g->entry_idx = -1;
    g->is_driver = 0;
    g->data_next = 4;
    g->min_abi = ABI_MINOR_BASE;
    g->pp.reader = reader;
    g->pp.reader_ctx = ctx;

    lex_t L;
    memset(&L, 0, sizeof(L));
    L.src = src;
    L.len = src_len;
    L.pp = &g->pp;
    L.cur_incl = -1;
    lex_next(&L);

    par_t p = {.L = &L, .g = g};
    parse_program(&p);

    if (L.err) {
        cpy_err(out->err, sizeof(out->err), L.errmsg);
        CC_FREE(text);
        gen_free(g);
        return -1;
    }
    if (g->err) {
        cpy_err(out->err, sizeof(out->err), g->errmsg);
        CC_FREE(text);
        gen_free(g);
        return -1;
    }

    const size_t lit_bytes = (size_t)g->nlit * 4u;
    finalize_lits(g, lit_bytes);
    if (g->err) {
        cpy_err(out->err, sizeof(out->err), g->errmsg);
        CC_FREE(text);
        gen_free(g);
        return -1;
    }
    fix_l32r(g, lit_bytes);
    if (g->err) {
        cpy_err(out->err, sizeof(out->err), g->errmsg);
        CC_FREE(text);
        gen_free(g);
        return -1;
    }

    const size_t code_raw = lit_bytes + g->len;
    const size_t code_size = (code_raw + 3u) & ~3u;
    const size_t data_size = ((size_t)g->data_next + 3u) & ~3u;
    const size_t data_file = data_size;
    const size_t reloc_bytes = (size_t)g->nrelocs * 4u;
    const size_t file_size = AXE_HDR + code_size + data_file + reloc_bytes;

    uint8_t *axe = (uint8_t *)CC_ALLOC(file_size);
    if (axe == NULL) {
        cpy_err(out->err, sizeof(out->err), "out of memory");
        CC_FREE(text);
        gen_free(g);
        return -1;
    }
    memset(axe, 0, file_size);

    memcpy(axe + 0, "AXE1", 4);
    wr_u16(axe + 4, 0);
    wr_u16(axe + 6, (uint16_t)g->min_abi);
    wr_u16(axe + 8, 1);
    wr_u16(axe + 10, (uint16_t)AXE_HDR);
    {
        uint32_t flags = 0;
        if (g->needs_gfx) {
            flags |= AG_AXE_NEEDS_GFX;
        }
        if (g->needs_audio) {
            flags |= AG_AXE_NEEDS_AUDIO;
        }
        if (g->is_driver) {
            flags |= AG_AXE_DRIVER;
        }
        wr_u32(axe + 12, flags);
    }

    wr_u32(axe + 16, CODE_BASE);
    wr_u32(axe + 20, (uint32_t)code_size);
    wr_u32(axe + 24, (uint32_t)code_size);
    wr_u32(axe + 28, AXE_HDR);

    wr_u32(axe + 32, DATA_BASE);
    wr_u32(axe + 36, (uint32_t)data_size);
    wr_u32(axe + 40, (uint32_t)data_file);
    wr_u32(axe + 44, AXE_HDR + (uint32_t)code_size);

    const uint32_t entry = CODE_BASE + (uint32_t)lit_bytes +
                           (uint32_t)g->funcs[g->entry_idx].code_off;
    wr_u32(axe + 48, entry);
    wr_u32(axe + 52, DATA_BASE);

    wr_u32(axe + 56, AXE_HDR + (uint32_t)code_size + (uint32_t)data_file);
    wr_u32(axe + 60, (uint32_t)g->nrelocs);

    wr_u32(axe + 64, g->pp.app_stack);
    wr_u32(axe + 68, g->pp.app_heap);
    if (g->is_driver) {
        wr_str(axe + 72, 32, g->pp.drv_name);
        wr_str(axe + 104, 16, g->pp.drv_ver);
        wr_str(axe + 120, 32, g->pp.drv_author);
    } else {
        wr_str(axe + 72, 32, "TCC");
        wr_str(axe + 104, 16, "0.2");
        wr_str(axe + 120, 32, "cc");
    }

    uint8_t *code = axe + AXE_HDR;
    for (int i = 0; i < g->nlit; i++) {
        wr_u32(code + (size_t)i * 4u, g->lit[i]);
    }
    memcpy(code + lit_bytes, text, g->len);

    uint8_t *data = axe + AXE_HDR + code_size;
    memcpy(data, g->data, (size_t)g->data_next);

    uint8_t *rel = data + data_file;
    for (int i = 0; i < g->nrelocs; i++) {
        wr_u32(rel + (size_t)i * 4u, g->relocs[i]);
    }

    CC_FREE(text);
    gen_free(g);
    out->axe = axe;
    out->axe_len = file_size;
    return 0;
}

void cc_result_free(cc_result_t *out)
{
    if (out == NULL) {
        return;
    }
    CC_FREE(out->axe);
    out->axe = NULL;
    out->axe_len = 0;
}

/*
 * The constant evaluator borrows the lexer rather than owning one, so that the
 * same code can read `cc -e "1+2"` and the size in `int buf[W * H];` - where the
 * tokens come from the middle of a program, and from macros.
 */
typedef struct {
    lex_t *L;
    gen_t *g; /* NULL for cc_eval_expr; set when evaluating inside a program */
} ev_t;

static int32_t ev_expr(ev_t *e);

/* The value of a constant expression read from a program being compiled. */
static int32_t const_expr_from(lex_t *L, gen_t *g)
{
    ev_t e = {.L = L, .g = g};
    return ev_expr(&e);
}

static int32_t ev_sizeof(ev_t *e)
{
    if (e->L->tok != T_LPAREN) {
        lex_fail(e->L, "expected '(' after sizeof");
        return 0;
    }
    lex_next(e->L);
    /* Mirror accept_type without a parser — const/volatile are ignored. */
    while (e->L->tok == T_CONST || e->L->tok == T_VOLATILE) {
        lex_next(e->L);
    }
    int esize = 0;
    int sidx = -1;
    int is_ptr = 0;
    if (e->L->tok == T_INT) {
        esize = 4;
        lex_next(e->L);
    } else if (e->L->tok == T_CHAR) {
        esize = 1;
        lex_next(e->L);
    } else if (e->L->tok == T_STRUCT) {
        lex_next(e->L);
        if (e->L->tok != T_IDENT || e->g == NULL) {
            lex_fail(e->L, "expected a struct name");
            return 0;
        }
        sidx = find_struct(e->g, e->L->text);
        if (sidx < 0) {
            lex_fail(e->L, "unknown struct");
            return 0;
        }
        esize = e->g->structs[sidx].size;
        lex_next(e->L);
    } else if (e->L->tok == T_IDENT && e->g != NULL) {
        const int ti = find_tdef(e->g, e->L->text);
        if (ti < 0) {
            lex_fail(e->L, "expected type in sizeof");
            return 0;
        }
        esize = e->g->tdefs[ti].esize;
        sidx = e->g->tdefs[ti].sidx;
        is_ptr = e->g->tdefs[ti].is_ptr;
        lex_next(e->L);
    } else {
        lex_fail(e->L, "expected type in sizeof");
        return 0;
    }
    while (e->L->tok == T_CONST || e->L->tok == T_VOLATILE) {
        lex_next(e->L);
    }
    if (e->L->tok == T_STAR) {
        if (is_ptr) {
            lex_fail(e->L, "no pointer to pointer");
            return 0;
        }
        is_ptr = 1;
        lex_next(e->L);
    }
    if (e->L->tok != T_RPAREN) {
        lex_fail(e->L, "expected ')'");
        return 0;
    }
    lex_next(e->L);
    if (is_ptr) {
        return 4;
    }
    if (sidx >= 0 && e->g != NULL) {
        return e->g->structs[sidx].size;
    }
    return esize;
}

static int32_t ev_primary(ev_t *e)
{
    if (e->L->tok == T_NUM) {
        const int32_t v = e->L->num;
        lex_next(e->L);
        return v;
    }
    if (e->L->tok == T_IDENT && e->g != NULL) {
        const int ei = find_enum(e->g, e->L->text);
        if (ei >= 0) {
            const int32_t v = e->g->enums[ei].val;
            lex_next(e->L);
            return v;
        }
    }
    if (e->L->tok == T_LPAREN) {
        lex_next(e->L);
        const int32_t v = ev_expr(e);
        if (e->L->tok != T_RPAREN) {
            lex_fail(e->L, "expected ')'");
            return 0;
        }
        lex_next(e->L);
        return v;
    }
    lex_fail(e->L, "expected expression");
    return 0;
}

static int32_t ev_unary(ev_t *e)
{
    if (e->L->tok == T_SIZEOF) {
        lex_next(e->L);
        return ev_sizeof(e);
    }
    if (e->L->tok == T_MINUS) {
        lex_next(e->L);
        return -ev_unary(e);
    }
    if (e->L->tok == T_NOT) {
        lex_next(e->L);
        return ev_unary(e) == 0 ? 1 : 0;
    }
    if (e->L->tok == T_TILDE) {
        lex_next(e->L);
        return (int32_t)~(uint32_t)ev_unary(e);
    }
    if (e->L->tok == T_PLUS) {
        lex_next(e->L);
        return ev_unary(e);
    }
    return ev_primary(e);
}

static int32_t ev_mul(ev_t *e)
{
    int32_t v = ev_unary(e);
    while (e->L->tok == T_STAR || e->L->tok == T_SLASH || e->L->tok == T_PERCENT) {
        const tok_t op = e->L->tok;
        lex_next(e->L);
        const int32_t r = ev_unary(e);
        if (op == T_STAR) {
            v *= r;
        } else if (op == T_SLASH) {
            if (r == 0) {
                lex_fail(e->L, "division by zero");
                return 0;
            }
            v /= r;
        } else {
            if (r == 0) {
                lex_fail(e->L, "division by zero");
                return 0;
            }
            v %= r;
        }
    }
    return v;
}

static int32_t ev_add(ev_t *e)
{
    int32_t v = ev_mul(e);
    while (e->L->tok == T_PLUS || e->L->tok == T_MINUS) {
        const tok_t op = e->L->tok;
        lex_next(e->L);
        const int32_t r = ev_mul(e);
        v = (op == T_PLUS) ? (v + r) : (v - r);
    }
    return v;
}

/*
 * The evaluator answers `cc -e` and the tests, and it exists to agree with the
 * code generator: the two share a lexer, and the levels below are the same
 * levels, so an expression cannot mean one thing here and another in a program.
 */
static int32_t ev_shift(ev_t *e)
{
    int32_t v = ev_add(e);
    while (e->L->tok == T_SHL || e->L->tok == T_SHR) {
        const tok_t op = e->L->tok;
        lex_next(e->L);
        const int32_t r = ev_add(e);
        if (r < 0 || r > 31) {
            lex_fail(e->L, "shift count out of range");
            return 0;
        }
        if (op == T_SHL) {
            v = (int32_t)((uint32_t)v << r);
        } else {
            v = v >> r;
        }
    }
    return v;
}

static int32_t ev_rel(ev_t *e)
{
    int32_t v = ev_shift(e);
    while (e->L->tok == T_LT || e->L->tok == T_GT || e->L->tok == T_LE ||
           e->L->tok == T_GE) {
        const tok_t op = e->L->tok;
        lex_next(e->L);
        const int32_t r = ev_shift(e);
        if (op == T_LT) {
            v = v < r;
        } else if (op == T_GT) {
            v = v > r;
        } else if (op == T_LE) {
            v = v <= r;
        } else {
            v = v >= r;
        }
    }
    return v;
}

static int32_t ev_eq(ev_t *e)
{
    int32_t v = ev_rel(e);
    while (e->L->tok == T_EQ || e->L->tok == T_NE) {
        const tok_t op = e->L->tok;
        lex_next(e->L);
        const int32_t r = ev_rel(e);
        v = (op == T_EQ) ? (v == r) : (v != r);
    }
    return v;
}

static int32_t ev_bitand(ev_t *e)
{
    int32_t v = ev_eq(e);
    while (e->L->tok == T_AMP) {
        lex_next(e->L);
        v = (int32_t)((uint32_t)v & (uint32_t)ev_eq(e));
    }
    return v;
}

static int32_t ev_bitxor(ev_t *e)
{
    int32_t v = ev_bitand(e);
    while (e->L->tok == T_CARET) {
        lex_next(e->L);
        v = (int32_t)((uint32_t)v ^ (uint32_t)ev_bitand(e));
    }
    return v;
}

static int32_t ev_bitor(ev_t *e)
{
    int32_t v = ev_bitxor(e);
    while (e->L->tok == T_PIPE) {
        lex_next(e->L);
        v = (int32_t)((uint32_t)v | (uint32_t)ev_bitxor(e));
    }
    return v;
}

static int32_t ev_and(ev_t *e)
{
    int32_t v = ev_bitor(e);
    while (e->L->tok == T_ANDAND) {
        lex_next(e->L);
        const int32_t r = ev_bitor(e);
        v = (v != 0 && r != 0);
    }
    return v;
}

static int32_t ev_or(ev_t *e)
{
    int32_t v = ev_and(e);
    while (e->L->tok == T_OROR) {
        lex_next(e->L);
        const int32_t r = ev_and(e);
        v = (v != 0 || r != 0);
    }
    return v;
}

static int32_t ev_expr(ev_t *e)
{
    return ev_or(e);
}

int cc_eval_expr(const char *expr, int32_t *out_value, char *err, size_t errlen)
{
    if (expr == NULL || out_value == NULL) {
        if (err != NULL && errlen > 0) {
            cpy_err(err, errlen, "null argument");
        }
        return -1;
    }
    lex_t L;
    memset(&L, 0, sizeof(L));
    L.src = expr;
    L.len = strlen(expr);
    L.cur_incl = -1;
    lex_next(&L);
    ev_t e = {.L = &L, .g = NULL};
    const int32_t v = ev_expr(&e);
    if (L.err) {
        if (err != NULL) {
            cpy_err(err, errlen, L.errmsg);
        }
        return -1;
    }
    if (L.tok != T_EOF) {
        if (err != NULL) {
            cpy_err(err, errlen, "trailing junk");
        }
        return -1;
    }
    *out_value = v;
    if (err != NULL && errlen > 0) {
        err[0] = '\0';
    }
    return 0;
}
