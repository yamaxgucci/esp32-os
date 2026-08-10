/*
 * ArgonOS - Tiny C → Xtensa .AXE (stack-machine codegen).
 *
 * Subset: int/void, locals, globals, int arrays, return, if/else, while/for,
 * multiple functions (callx8), string literals, and ABI builtins for time/input/gfx/audio.
 * Entry must be named ag_main.
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
#define CODE_CAP  (48u * 1024u)
#define DATA_CAP  4096u
#define LIT_CAP   256
#define MAX_LIT_SITES 2048
#define MAX_LOCALS 64
#define MAX_GLOBALS 32
#define MAX_FUNCS 32
#define MAX_RELOCS 512
#define MAX_NAME   32
#define MAX_STR    192
#define SPILL_SLOTS 16

#define AG_AXE_R_IN_DATA 0x1u
#define AG_AXE_R_TO_DATA 0x2u
#define AG_AXE_NEEDS_GFX (1u << 1)
#define AG_AXE_NEEDS_AUDIO (1u << 6)

#define API_OFF_INP   24
#define API_OFF_GFX   28
#define API_OFF_TIME  40
#define API_OFF_AUDIO 60

#define INP_OFF_KEY_PRESSED 12
#define INP_OFF_PAD         20
#define INP_OFF_BTN         24
#define TIME_OFF_DELAY_MS   16

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

#define AUDIO_OFF_PRESENT 4
#define AUDIO_OFF_IS_HW   8
#define AUDIO_OFF_OPEN    12
#define AUDIO_OFF_CLOSE   16
#define AUDIO_OFF_WRITE   20
#define AUDIO_OFF_SPACE   24

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

/* ---- lexer ------------------------------------------------------------- */

typedef enum {
    T_EOF = 0,
    T_IDENT,
    T_NUM,
    T_STRING,
    T_INT,
    T_VOID,
    T_RETURN,
    T_IF,
    T_ELSE,
    T_WHILE,
    T_FOR,
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
    T_BAD
} tok_t;

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

static void lex_fail(lex_t *L, const char *msg)
{
    if (L->err) {
        return;
    }
    L->err = 1;
    cpy_err(L->errmsg, sizeof(L->errmsg), msg);
    L->tok = T_BAD;
}

static void lex_skip(lex_t *L)
{
    while (L->pos < L->len) {
        const char c = L->src[L->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            L->pos++;
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
    return T_IDENT;
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
    if (L->pos >= L->len) {
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
        L->tok = kw(L->text);
        return;
    }
    if (is_digit(c)) {
        int32_t v = 0;
        while (L->pos < L->len && is_digit(L->src[L->pos])) {
            const int d = L->src[L->pos] - '0';
            if (v > (2147483647 - d) / 10) {
                lex_fail(L, "integer constant too large");
                return;
            }
            v = v * 10 + d;
            L->pos++;
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
                const char e = L->src[L->pos++];
                if (e == 'n') {
                    ch = '\n';
                } else if (e == 't') {
                    ch = '\t';
                } else if (e == '\\' || e == '"') {
                    ch = e;
                } else {
                    ch = e;
                }
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
        L->tok = T_PLUS;
        break;
    case '-':
        L->tok = T_MINUS;
        break;
    case '*':
        L->tok = T_STAR;
        break;
    case '%':
        L->tok = T_PERCENT;
        break;
    case '/':
        L->tok = T_SLASH;
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
        } else {
            L->tok = T_LT;
        }
        break;
    case '>':
        if (L->pos < L->len && L->src[L->pos] == '=') {
            L->pos++;
            L->tok = T_GE;
        } else {
            L->tok = T_GT;
        }
        break;
    case '&':
        if (L->pos < L->len && L->src[L->pos] == '&') {
            L->pos++;
            L->tok = T_ANDAND;
        } else {
            lex_fail(L, "bitwise & not supported");
        }
        break;
    case '|':
        if (L->pos < L->len && L->src[L->pos] == '|') {
            L->pos++;
            L->tok = T_OROR;
        } else {
            lex_fail(L, "bitwise | not supported");
        }
        break;
    default:
        lex_fail(L, "unexpected character");
        break;
    }
}

/* ---- codegen ----------------------------------------------------------- */

typedef struct {
    char name[MAX_NAME];
    int  off;
    int  nwords;
    int  is_array;
} sym_t;

typedef struct {
    char   name[MAX_NAME];
    size_t code_off;
    int    nparams;
    int    is_void;
} func_t;

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
    int      data_next;
    uint8_t  data[DATA_CAP];
    int      nfuncs;
    func_t   funcs[MAX_FUNCS];
    int      ag_main_idx;
    uint32_t relocs[MAX_RELOCS];
    int      nrelocs;
    int      needs_gfx;
    int      needs_audio;
    int      spill_base;
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

static int add_local_n(gen_t *g, const char *name, int nwords, int is_array)
{
    if (find_local(g, name) >= 0) {
        gfail(g, "duplicate local");
        return -1;
    }
    if (nwords <= 0) {
        gfail(g, "bad array size");
        return -1;
    }
    if (g->nlocals >= MAX_LOCALS) {
        gfail(g, "too many locals");
        return -1;
    }
    if (g->local_words + nwords > MAX_LOCALS) {
        gfail(g, "too many local slots");
        return -1;
    }
    const int i = g->nlocals++;
    cpy_err(g->locals[i].name, MAX_NAME, name);
    g->locals[i].off = g->local_words * 4;
    g->locals[i].nwords = nwords;
    g->locals[i].is_array = is_array;
    g->local_words += nwords;
    return i;
}

static int add_local(gen_t *g, const char *name)
{
    return add_local_n(g, name, 1, 0);
}

static int add_global_n(gen_t *g, const char *name, int nwords, int is_array)
{
    if (find_global(g, name) >= 0 || find_func(g, name) >= 0) {
        gfail(g, "duplicate global");
        return -1;
    }
    if (nwords <= 0) {
        gfail(g, "bad array size");
        return -1;
    }
    if (g->nglobals >= MAX_GLOBALS) {
        gfail(g, "too many globals");
        return -1;
    }
    const int bytes = nwords * 4;
    if (g->data_next + bytes > (int)DATA_CAP) {
        gfail(g, "data too large");
        return -1;
    }
    const int i = g->nglobals++;
    cpy_err(g->globals[i].name, MAX_NAME, name);
    g->globals[i].off = g->data_next;
    g->globals[i].nwords = nwords;
    g->globals[i].is_array = is_array;
    g->data_next += bytes;
    return i;
}

static int add_string(gen_t *g, const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    n++;
    if (g->data_next + (int)n > (int)DATA_CAP) {
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
    emit_s32i(g, 8, 7, g->spill_base + g->spill_top * 4);
    g->spill_top++;
}

static void pop_a9(gen_t *g)
{
    if (g->spill_top <= 0) {
        gfail(g, "expression stack underflow");
        return;
    }
    g->spill_top--;
    emit_l32i(g, 9, 7, g->spill_base + g->spill_top * 4);
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

static void emit_base_local(gen_t *g, int li)
{
    emit_mov_n(g, 8, 7);
    if (g->locals[li].off != 0) {
        emit_movi(g, 9, g->locals[li].off);
        emit_add_n(g, 8, 8, 9);
    }
}

static void emit_base_global(gen_t *g, int gi)
{
    emit_li_data(g, 8, (uint32_t)g->globals[gi].off);
}

static void emit_index_addr(gen_t *g, int is_global, int idx)
{
    push_a8(g);
    if (is_global) {
        emit_base_global(g, idx);
    } else {
        emit_base_local(g, idx);
    }
    pop_a9(g);
    emit_movi(g, 10, 4);
    emit_mull(g, 9, 9, 10);
    emit_add_n(g, 8, 8, 9);
}

/* ---- builtins ---------------------------------------------------------- */

typedef struct {
    const char *name;
    int         nargs;
    int         api_off;
    int         fn_off;
    int         is_gfx;
    int         is_audio;
    int         ret_mode; /* RET_VOID / RET_BOOL / RET_RAW */
    int         null_arg; /* call with a10=0 and no parsed args */
} builtin_t;

static const builtin_t BUILTINS[] = {
    {"ag_delay", 1, API_OFF_TIME, TIME_OFF_DELAY_MS, 0, 0, RET_VOID, 0},
    {"ag_key", 1, API_OFF_INP, INP_OFF_KEY_PRESSED, 0, 0, RET_BOOL, 0},
    /* ag_btn: level state via HostFS PADPUSH (same path as SMS). ids 0..7 */
    {"ag_btn", 1, API_OFF_INP, INP_OFF_BTN, 0, 0, RET_BOOL, 0},
    {"ag_gfx_acquire", 0, API_OFF_GFX, GFX_OFF_ACQUIRE, 1, 0, RET_VOID, 1},
    {"ag_gfx_release", 0, API_OFF_GFX, GFX_OFF_RELEASE, 1, 0, RET_VOID, 0},
    {"ag_gfx_clear", 1, API_OFF_GFX, GFX_OFF_CLEAR, 1, 0, RET_VOID, 0},
    {"ag_gfx_flush", 4, API_OFF_GFX, GFX_OFF_FLUSH, 1, 0, RET_VOID, 0},
    {"ag_gfx_swap", 0, API_OFF_GFX, GFX_OFF_SWAP, 1, 0, RET_VOID, 0},
    {"ag_gfx_fill_rect", 5, API_OFF_GFX, GFX_OFF_FILL_RECT, 1, 0, RET_VOID, 0},
    {"ag_gfx_text", 5, API_OFF_GFX, GFX_OFF_TEXT, 1, 0, RET_VOID, 0},
    {"ag_gfx_pixel", 3, API_OFF_GFX, GFX_OFF_PIXEL, 1, 0, RET_VOID, 0},
    {"ag_gfx_line", 5, API_OFF_GFX, GFX_OFF_LINE, 1, 0, RET_VOID, 0},
    {"ag_gfx_circle", 4, API_OFF_GFX, GFX_OFF_CIRCLE, 1, 0, RET_VOID, 0},
    {"ag_gfx_fill_circle", 4, API_OFF_GFX, GFX_OFF_FILL_CIRCLE, 1, 0, RET_VOID, 0},
    {"ag_gfx_poly_begin", 0, API_OFF_GFX, GFX_OFF_POLY_BEGIN, 1, 0, RET_VOID, 0},
    {"ag_gfx_poly_vertex", 2, API_OFF_GFX, GFX_OFF_POLY_VERTEX, 1, 0, RET_VOID, 0},
    {"ag_gfx_poly_fill", 1, API_OFF_GFX, GFX_OFF_POLY_FILL, 1, 0, RET_VOID, 0},
    {"ag_gfx_poly_stroke", 1, API_OFF_GFX, GFX_OFF_POLY_STROKE, 1, 0, RET_VOID, 0},
    /* audio (ABI 0.14): open(NULL) default 22050 stereo s16 */
    {"ag_audio_present", 0, API_OFF_AUDIO, AUDIO_OFF_PRESENT, 0, 1, RET_RAW, 0},
    {"ag_audio_is_hw", 0, API_OFF_AUDIO, AUDIO_OFF_IS_HW, 0, 1, RET_RAW, 0},
    {"ag_audio_open", 0, API_OFF_AUDIO, AUDIO_OFF_OPEN, 0, 1, RET_RAW, 1},
    {"ag_audio_close", 0, API_OFF_AUDIO, AUDIO_OFF_CLOSE, 0, 1, RET_VOID, 0},
    {"ag_audio_write", 2, API_OFF_AUDIO, AUDIO_OFF_WRITE, 0, 1, RET_RAW, 0},
    {"ag_audio_space", 0, API_OFF_AUDIO, AUDIO_OFF_SPACE, 0, 1, RET_RAW, 0},
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

typedef struct {
    lex_t *L;
    gen_t *g;
} par_t;

static void pfail(par_t *p, const char *msg)
{
    gfail(p->g, msg);
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
    if (b->is_gfx) {
        p->g->needs_gfx = 1;
    }
    if (b->is_audio) {
        p->g->needs_audio = 1;
    }
    if (b->null_arg) {
        expect(p, T_LPAREN, "expected '('");
        expect(p, T_RPAREN, "expected ')'");
        emit_movi(p->g, 10, 0);
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

static void parse_primary(par_t *p)
{
    if (p->g->err) {
        return;
    }
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
        return;
    }
    if (p->L->tok == T_IDENT) {
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (p->L->tok == T_LPAREN) {
            parse_call_by_name(p, name);
            return;
        }
        if (accept(p, T_LBRACK)) {
            parse_expr(p);
            expect(p, T_RBRACK, "expected ']'");
            const int li = find_local(p->g, name);
            const int gi = (li < 0) ? find_global(p->g, name) : -1;
            if (li >= 0) {
                if (!p->g->locals[li].is_array) {
                    pfail(p, "not an array");
                    return;
                }
                emit_index_addr(p->g, 0, li);
            } else if (gi >= 0) {
                if (!p->g->globals[gi].is_array) {
                    pfail(p, "not an array");
                    return;
                }
                emit_index_addr(p->g, 1, gi);
            } else {
                pfail(p, "unknown identifier");
                return;
            }
            emit_l32i(p->g, 8, 8, 0);
            return;
        }
        const int li = find_local(p->g, name);
        if (li >= 0) {
            if (p->g->locals[li].is_array) {
                emit_base_local(p->g, li);
            } else {
                emit_l32i(p->g, 8, 7, p->g->locals[li].off);
            }
            return;
        }
        const int gi = find_global(p->g, name);
        if (gi >= 0) {
            if (p->g->globals[gi].is_array) {
                emit_base_global(p->g, gi);
            } else {
                emit_li_data(p->g, 8, (uint32_t)p->g->globals[gi].off);
                emit_l32i(p->g, 8, 8, 0);
            }
            return;
        }
        pfail(p, "unknown identifier");
        return;
    }
    if (accept(p, T_LPAREN)) {
        parse_expr(p);
        expect(p, T_RPAREN, "expected ')'");
        return;
    }
    pfail(p, "expected expression");
}

static void parse_unary(par_t *p)
{
    if (accept(p, T_MINUS)) {
        parse_unary(p);
        emit_movi(p->g, 9, 0);
        emit_sub(p->g, 8, 9, 8);
        return;
    }
    if (accept(p, T_NOT)) {
        parse_unary(p);
        emit_bool_not_a8(p->g);
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
        push_a8(p->g);
        parse_unary(p);
        pop_a9(p->g);
        if (op == T_STAR) {
            emit_mull(p->g, 8, 9, 8);
        } else if (op == T_SLASH) {
            emit_quos(p->g, 8, 9, 8);
        } else {
            emit_rems(p->g, 8, 9, 8);
        }
    }
}

static void parse_add(par_t *p)
{
    parse_mul(p);
    while (p->L->tok == T_PLUS || p->L->tok == T_MINUS) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        push_a8(p->g);
        parse_mul(p);
        pop_a9(p->g);
        if (op == T_PLUS) {
            emit_add_n(p->g, 8, 9, 8);
        } else {
            emit_sub(p->g, 8, 9, 8);
        }
    }
}

static void parse_rel(par_t *p)
{
    parse_add(p);
    while (p->L->tok == T_LT || p->L->tok == T_GT || p->L->tok == T_LE ||
           p->L->tok == T_GE) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        push_a8(p->g);
        parse_add(p);
        pop_a9(p->g);
        if (op == T_LT) {
            emit_setlt(p->g, 8, 9, 8);
        } else if (op == T_GT) {
            emit_setlt(p->g, 8, 8, 9);
        } else if (op == T_LE) {
            emit_setlt(p->g, 8, 8, 9);
            emit_bool_not_a8(p->g);
        } else {
            emit_setlt(p->g, 8, 9, 8);
            emit_bool_not_a8(p->g);
        }
    }
}

static void parse_eq(par_t *p)
{
    parse_rel(p);
    while (p->L->tok == T_EQ || p->L->tok == T_NE) {
        const tok_t op = p->L->tok;
        lex_next(p->L);
        push_a8(p->g);
        parse_rel(p);
        pop_a9(p->g);
        const int opn = (op == T_EQ) ? B_BEQ : B_BNE;
        size_t b_site, j_site;
        emit_b_placeholder(p->g, opn, 9, 8, &b_site);
        emit_movi(p->g, 8, 0);
        emit_j_placeholder(p->g, &j_site);
        patch_b(p->g, b_site, p->g->len);
        emit_movi(p->g, 8, 1);
        emit_j_to(p->g, j_site, p->g->len);
    }
}

static void parse_and(par_t *p)
{
    parse_eq(p);
    while (accept(p, T_ANDAND)) {
        emit_movi(p->g, 9, 0);
        size_t false_b, false_b2, end_j;
        emit_b_placeholder(p->g, B_BEQ, 8, 9, &false_b);
        parse_eq(p);
        emit_b_placeholder(p->g, B_BEQ, 8, 9, &false_b2);
        emit_movi(p->g, 8, 1);
        emit_j_placeholder(p->g, &end_j);
        patch_b(p->g, false_b, p->g->len);
        patch_b(p->g, false_b2, p->g->len);
        emit_movi(p->g, 8, 0);
        emit_j_to(p->g, end_j, p->g->len);
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
        emit_b_placeholder(p->g, B_BNE, 8, 9, &true_b2);
        emit_movi(p->g, 8, 0);
        emit_j_placeholder(p->g, &end_j);
        patch_b(p->g, true_b, p->g->len);
        patch_b(p->g, true_b2, p->g->len);
        emit_movi(p->g, 8, 1);
        emit_j_to(p->g, end_j, p->g->len);
    }
}

static void parse_expr(par_t *p)
{
    parse_or(p);
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

static void store_scalar_from_a8(par_t *p, const char *name)
{
    const int li = find_local(p->g, name);
    if (li >= 0) {
        if (p->g->locals[li].is_array) {
            pfail(p, "array needs index");
            return;
        }
        emit_s32i(p->g, 8, 7, p->g->locals[li].off);
        return;
    }
    const int gi = find_global(p->g, name);
    if (gi >= 0) {
        if (p->g->globals[gi].is_array) {
            pfail(p, "array needs index");
            return;
        }
        push_a8(p->g);
        emit_li_data(p->g, 8, (uint32_t)p->g->globals[gi].off);
        pop_a9(p->g);
        emit_s32i(p->g, 9, 8, 0);
        return;
    }
    pfail(p, "unknown identifier");
}

static void assign_array(par_t *p, const char *name)
{
    parse_expr(p);
    expect(p, T_RBRACK, "expected ']'");
    expect(p, T_ASSIGN, "expected '='");
    push_a8(p->g);
    parse_expr(p);
    push_a8(p->g);

    const int li = find_local(p->g, name);
    const int gi = (li < 0) ? find_global(p->g, name) : -1;
    if (li < 0 && gi < 0) {
        pfail(p, "unknown identifier");
        return;
    }
    if (li >= 0 && !p->g->locals[li].is_array) {
        pfail(p, "not an array");
        return;
    }
    if (gi >= 0 && !p->g->globals[gi].is_array) {
        pfail(p, "not an array");
        return;
    }

    pop_a9(p->g);
    if (p->g->spill_top >= SPILL_SLOTS) {
        gfail(p->g, "expression too deep");
        return;
    }
    const int val_slot = p->g->spill_base + p->g->spill_top * 4;
    emit_s32i(p->g, 9, 7, val_slot);

    pop_a9(p->g);
    emit_mov_n(p->g, 8, 9);
    if (li >= 0) {
        emit_index_addr(p->g, 0, li);
    } else {
        emit_index_addr(p->g, 1, gi);
    }
    emit_l32i(p->g, 9, 7, val_slot);
    emit_s32i(p->g, 9, 8, 0);
}

static void parse_for_assign_or_empty(par_t *p, int allow_decl)
{
    if (p->L->tok == T_SEMI || p->L->tok == T_RPAREN) {
        return;
    }
    if (allow_decl && accept(p, T_INT)) {
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected identifier after int");
            return;
        }
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (accept(p, T_LBRACK)) {
            if (p->L->tok != T_NUM || p->L->num <= 0) {
                pfail(p, "expected array size");
                return;
            }
            const int n = (int)p->L->num;
            lex_next(p->L);
            expect(p, T_RBRACK, "expected ']'");
            (void)add_local_n(p->g, name, n, 1);
            return;
        }
        const int idx = add_local(p->g, name);
        if (idx < 0) {
            return;
        }
        if (accept(p, T_ASSIGN)) {
            parse_expr(p);
        } else {
            emit_movi(p->g, 8, 0);
        }
        emit_s32i(p->g, 8, 7, p->g->locals[idx].off);
        return;
    }
    if (p->L->tok != T_IDENT) {
        pfail(p, "expected assignment in for");
        return;
    }
    char name[MAX_NAME];
    cpy_err(name, sizeof(name), p->L->text);
    lex_next(p->L);
    if (accept(p, T_LBRACK)) {
        assign_array(p, name);
        return;
    }
    expect(p, T_ASSIGN, "expected '='");
    parse_expr(p);
    store_scalar_from_a8(p, name);
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
        parse_stmt(p);
        size_t j_to_step;
        emit_j_placeholder(p->g, &j_to_step);
        emit_j_to(p->g, j_to_step, step_start);
        if (has_cond) {
            emit_j_to(p->g, j_done, p->g->len);
        }
        return;
    }
    if (accept(p, T_INT)) {
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected identifier after int");
            return;
        }
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (accept(p, T_LBRACK)) {
            if (p->L->tok != T_NUM || p->L->num <= 0) {
                pfail(p, "expected array size");
                return;
            }
            const int n = (int)p->L->num;
            lex_next(p->L);
            expect(p, T_RBRACK, "expected ']'");
            (void)add_local_n(p->g, name, n, 1);
            expect(p, T_SEMI, "expected ';' after declaration");
            return;
        }
        const int idx = add_local(p->g, name);
        if (idx < 0) {
            return;
        }
        if (accept(p, T_ASSIGN)) {
            parse_expr(p);
        } else {
            emit_movi(p->g, 8, 0);
        }
        emit_s32i(p->g, 8, 7, p->g->locals[idx].off);
        expect(p, T_SEMI, "expected ';' after declaration");
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
        if (accept(p, T_LBRACK)) {
            assign_array(p, name);
            expect(p, T_SEMI, "expected ';'");
            return;
        }
        expect(p, T_ASSIGN, "expected '=' after identifier");
        parse_expr(p);
        store_scalar_from_a8(p, name);
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
        expect(p, T_INT, "expected parameter type");
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected parameter name");
            return 0;
        }
        if (nparams >= 4) {
            pfail(p, "too many parameters");
            return 0;
        }
        (void)add_local(p->g, p->L->text);
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
    p->g->spill_base = MAX_LOCALS * 4;

    const int framesize = 32 + MAX_LOCALS * 4 + SPILL_SLOTS * 4;
    const size_t code_off = p->g->len;
    emit_entry(p->g, framesize);
    emit_mov_n(p->g, 7, 1);

    const int nparams = parse_params(p);
    if (p->g->err) {
        return;
    }
    for (int i = 0; i < nparams && i < 4; i++) {
        emit_s32i(p->g, 2 + i, 7, p->g->locals[i].off);
    }

    const int fi = p->g->nfuncs++;
    cpy_err(p->g->funcs[fi].name, MAX_NAME, name);
    p->g->funcs[fi].code_off = code_off;
    p->g->funcs[fi].nparams = nparams;
    p->g->funcs[fi].is_void = is_void;
    if (CC_STRCMP(name, "ag_main") == 0) {
        p->g->ag_main_idx = fi;
    }

    parse_block(p);

    emit_movi(p->g, 2, 0);
    emit_retw_n(p->g);
}

static void parse_program(par_t *p)
{
    p->g->data_next = 4;
    p->g->ag_main_idx = -1;

    while (p->L->tok != T_EOF && !p->g->err && !p->L->err) {
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
        if (!accept(p, T_INT)) {
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
            parse_function(p, 0, name);
            continue;
        }
        if (p->g->nfuncs > 0) {
            pfail(p, "globals must precede functions");
            return;
        }
        if (accept(p, T_LBRACK)) {
            if (p->L->tok != T_NUM || p->L->num <= 0) {
                pfail(p, "expected array size");
                return;
            }
            const int n = (int)p->L->num;
            lex_next(p->L);
            expect(p, T_RBRACK, "expected ']'");
            expect(p, T_SEMI, "expected ';'");
            (void)add_global_n(p->g, name, n, 1);
            continue;
        }
        expect(p, T_SEMI, "expected ';'");
        (void)add_global_n(p->g, name, 1, 0);
    }

    if (!p->g->err && p->g->ag_main_idx < 0) {
        pfail(p, "entry must be ag_main");
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

int cc_compile_to_axe(const char *src, size_t src_len, cc_result_t *out)
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

    gen_t *g = (gen_t *)CC_ALLOC(sizeof(gen_t));
    if (g == NULL) {
        cpy_err(out->err, sizeof(out->err), "out of memory");
        CC_FREE(text);
        return -1;
    }
    memset(g, 0, sizeof(*g));
    g->code = text;
    g->cap = CODE_CAP;
    g->ag_main_idx = -1;
    g->data_next = 4;

    lex_t L;
    memset(&L, 0, sizeof(L));
    L.src = src;
    L.len = src_len;
    lex_next(&L);

    par_t p = {.L = &L, .g = g};
    parse_program(&p);

    if (L.err) {
        cpy_err(out->err, sizeof(out->err), L.errmsg);
        CC_FREE(text);
        CC_FREE(g);
        return -1;
    }
    if (g->err) {
        cpy_err(out->err, sizeof(out->err), g->errmsg);
        CC_FREE(text);
        CC_FREE(g);
        return -1;
    }

    const size_t lit_bytes = (size_t)g->nlit * 4u;
    finalize_lits(g, lit_bytes);
    if (g->err) {
        cpy_err(out->err, sizeof(out->err), g->errmsg);
        CC_FREE(text);
        CC_FREE(g);
        return -1;
    }
    fix_l32r(g, lit_bytes);
    if (g->err) {
        cpy_err(out->err, sizeof(out->err), g->errmsg);
        CC_FREE(text);
        CC_FREE(g);
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
        CC_FREE(g);
        return -1;
    }
    memset(axe, 0, file_size);

    memcpy(axe + 0, "AXE1", 4);
    wr_u16(axe + 4, 0);
    wr_u16(axe + 6, 14);
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
                           (uint32_t)g->funcs[g->ag_main_idx].code_off;
    wr_u32(axe + 48, entry);
    wr_u32(axe + 52, DATA_BASE);

    wr_u32(axe + 56, AXE_HDR + (uint32_t)code_size + (uint32_t)data_file);
    wr_u32(axe + 60, (uint32_t)g->nrelocs);

    wr_u32(axe + 64, 0);
    wr_u32(axe + 68, 0);
    wr_str(axe + 72, 32, "TCC");
    wr_str(axe + 104, 16, "0.2");
    wr_str(axe + 120, 32, "cc");

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
    CC_FREE(g);
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

typedef struct {
    lex_t L;
} ev_t;

static int32_t ev_expr(ev_t *e);

static int32_t ev_primary(ev_t *e)
{
    if (e->L.tok == T_NUM) {
        const int32_t v = e->L.num;
        lex_next(&e->L);
        return v;
    }
    if (e->L.tok == T_LPAREN) {
        lex_next(&e->L);
        const int32_t v = ev_expr(e);
        if (e->L.tok != T_RPAREN) {
            lex_fail(&e->L, "expected ')'");
            return 0;
        }
        lex_next(&e->L);
        return v;
    }
    lex_fail(&e->L, "expected expression");
    return 0;
}

static int32_t ev_unary(ev_t *e)
{
    if (e->L.tok == T_MINUS) {
        lex_next(&e->L);
        return -ev_unary(e);
    }
    if (e->L.tok == T_NOT) {
        lex_next(&e->L);
        return ev_unary(e) == 0 ? 1 : 0;
    }
    if (e->L.tok == T_PLUS) {
        lex_next(&e->L);
        return ev_unary(e);
    }
    return ev_primary(e);
}

static int32_t ev_mul(ev_t *e)
{
    int32_t v = ev_unary(e);
    while (e->L.tok == T_STAR || e->L.tok == T_SLASH || e->L.tok == T_PERCENT) {
        const tok_t op = e->L.tok;
        lex_next(&e->L);
        const int32_t r = ev_unary(e);
        if (op == T_STAR) {
            v *= r;
        } else if (op == T_SLASH) {
            if (r == 0) {
                lex_fail(&e->L, "division by zero");
                return 0;
            }
            v /= r;
        } else {
            if (r == 0) {
                lex_fail(&e->L, "division by zero");
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
    while (e->L.tok == T_PLUS || e->L.tok == T_MINUS) {
        const tok_t op = e->L.tok;
        lex_next(&e->L);
        const int32_t r = ev_mul(e);
        v = (op == T_PLUS) ? (v + r) : (v - r);
    }
    return v;
}

static int32_t ev_rel(ev_t *e)
{
    int32_t v = ev_add(e);
    while (e->L.tok == T_LT || e->L.tok == T_GT || e->L.tok == T_LE ||
           e->L.tok == T_GE) {
        const tok_t op = e->L.tok;
        lex_next(&e->L);
        const int32_t r = ev_add(e);
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
    while (e->L.tok == T_EQ || e->L.tok == T_NE) {
        const tok_t op = e->L.tok;
        lex_next(&e->L);
        const int32_t r = ev_rel(e);
        v = (op == T_EQ) ? (v == r) : (v != r);
    }
    return v;
}

static int32_t ev_and(ev_t *e)
{
    int32_t v = ev_eq(e);
    while (e->L.tok == T_ANDAND) {
        lex_next(&e->L);
        const int32_t r = ev_eq(e);
        v = (v != 0 && r != 0);
    }
    return v;
}

static int32_t ev_or(ev_t *e)
{
    int32_t v = ev_and(e);
    while (e->L.tok == T_OROR) {
        lex_next(&e->L);
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
    ev_t e;
    memset(&e, 0, sizeof(e));
    e.L.src = expr;
    e.L.len = strlen(expr);
    lex_next(&e.L);
    const int32_t v = ev_expr(&e);
    if (e.L.err) {
        if (err != NULL) {
            cpy_err(err, errlen, e.L.errmsg);
        }
        return -1;
    }
    if (e.L.tok != T_EOF) {
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
