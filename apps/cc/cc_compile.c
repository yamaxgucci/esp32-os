/*
 * ArgonOS - Tiny C → Xtensa .AXE (stack-machine codegen).
 *
 * Subset: int, locals, return, if/else, while, expressions.
 * Entry must be named ag_main.  No preprocessor, floats, structs, or calls.
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
#define CODE_CAP  (24u * 1024u)
#define LIT_CAP   64
#define MAX_LOCALS 32
#define MAX_NAME   32
#define SPILL_SLOTS 16

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
    T_INT,
    T_VOID,
    T_RETURN,
    T_IF,
    T_ELSE,
    T_WHILE,
    T_LPAREN,
    T_RPAREN,
    T_LBRACE,
    T_RBRACE,
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
    uint8_t *code;
    size_t   len;
    size_t   cap;
    uint32_t lit[LIT_CAP];
    int      nlit;
    int      nlocals;
    char     local_name[MAX_LOCALS][MAX_NAME];
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

static void emit_movi(gen_t *g, int at, int32_t imm)
{
    if (imm >= -32 && imm <= 95) {
        /* MOVI.N: byte0 = ((imm>>4)<<4)|0xC, byte1 = ((imm&0xf)<<4)|at */
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
    if (g->nlit >= LIT_CAP) {
        gfail(g, "too many constants");
        return;
    }
    const int li = g->nlit++;
    g->lit[li] = (uint32_t)imm;
    /* Placeholder l32r: byte1=0xFF, byte2=lit index — fixed in finish. */
    emit3(g, (uint8_t)((at << 4) | 0x1), 0xff, (uint8_t)li);
}

static void emit_add_n(gen_t *g, int ar, int as, int at)
{
    emit2(g, (uint8_t)((at << 4) | 0x0a), (uint8_t)((ar << 4) | as));
}

/* RRR packed LE: byte0=(at<<4), byte1=(ar<<4)|as, byte2=opc */
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

/* ar = (as < at) ? 1 : 0 — branch form (avoid optional SALT). */
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

static int local_off(gen_t *g, int idx)
{
    (void)g;
    return idx * 4;
}

static int find_local(gen_t *g, const char *name)
{
    for (int i = 0; i < g->nlocals; i++) {
        if (CC_STRCMP(g->local_name[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_local(gen_t *g, const char *name)
{
    if (find_local(g, name) >= 0) {
        gfail(g, "duplicate local");
        return -1;
    }
    if (g->nlocals >= MAX_LOCALS) {
        gfail(g, "too many locals");
        return -1;
    }
    const int i = g->nlocals++;
    cpy_err(g->local_name[i], MAX_NAME, name);
    return i;
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

/* a8 = (a8 == 0) ? 1 : 0  — used for ! and inverted compares */
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
    if (p->L->tok == T_IDENT) {
        const int idx = find_local(p->g, p->L->text);
        if (idx < 0) {
            pfail(p, "unknown identifier");
            return;
        }
        emit_l32i(p->g, 8, 7, local_off(p->g, idx));
        lex_next(p->L);
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
    if (accept(p, T_INT)) {
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected identifier after int");
            return;
        }
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        const int idx = add_local(p->g, name);
        if (idx < 0) {
            return;
        }
        if (accept(p, T_ASSIGN)) {
            parse_expr(p);
        } else {
            emit_movi(p->g, 8, 0);
        }
        emit_s32i(p->g, 8, 7, local_off(p->g, idx));
        expect(p, T_SEMI, "expected ';' after declaration");
        return;
    }
    if (p->L->tok == T_IDENT) {
        char name[MAX_NAME];
        cpy_err(name, sizeof(name), p->L->text);
        lex_next(p->L);
        if (!accept(p, T_ASSIGN)) {
            pfail(p, "expected '=' after identifier");
            return;
        }
        const int idx = find_local(p->g, name);
        if (idx < 0) {
            pfail(p, "unknown identifier");
            return;
        }
        parse_expr(p);
        emit_s32i(p->g, 8, 7, local_off(p->g, idx));
        expect(p, T_SEMI, "expected ';'");
        return;
    }
    parse_expr(p);
    expect(p, T_SEMI, "expected ';'");
}

static void parse_params(par_t *p)
{
    expect(p, T_LPAREN, "expected '('");
    if (accept(p, T_VOID) || p->L->tok == T_RPAREN) {
        expect(p, T_RPAREN, "expected ')'");
        return;
    }
    /* int argc, int argv-style — accept and ignore names as locals. */
    for (;;) {
        expect(p, T_INT, "expected parameter type");
        if (p->L->tok != T_IDENT) {
            pfail(p, "expected parameter name");
            return;
        }
        (void)add_local(p->g, p->L->text);
        lex_next(p->L);
        if (!accept(p, T_COMMA)) {
            break;
        }
    }
    expect(p, T_RPAREN, "expected ')'");
}

static void parse_program(par_t *p)
{
    /* int|void ag_main(...) { ... } */
    if (p->L->tok != T_INT && p->L->tok != T_VOID) {
        pfail(p, "expected function");
        return;
    }
    lex_next(p->L);
    if (p->L->tok != T_IDENT || CC_STRCMP(p->L->text, "ag_main") != 0) {
        pfail(p, "entry must be ag_main");
        return;
    }
    lex_next(p->L);

    /* Frame: reserve max locals + spills up front (fixed layout). */
    const int framesize = 32 + MAX_LOCALS * 4 + SPILL_SLOTS * 4;
    p->g->spill_base = MAX_LOCALS * 4;
    p->g->spill_top = 0;
    emit_entry(p->g, framesize);
    emit_mov_n(p->g, 7, 1); /* a7 = frame */

    parse_params(p);
    /* Copy incoming a2/a3 into first locals if present. */
    if (p->g->nlocals >= 1) {
        emit_s32i(p->g, 2, 7, 0);
    }
    if (p->g->nlocals >= 2) {
        emit_s32i(p->g, 3, 7, 4);
    }

    parse_block(p);

    /* Fallthrough return 0. */
    emit_movi(p->g, 2, 0);
    emit_retw_n(p->g);

    if (p->L->tok != T_EOF && !p->g->err) {
        pfail(p, "trailing junk after ag_main");
    }
}

/* ---- AXE packing ------------------------------------------------------- */

static void fix_l32r(gen_t *g, size_t lit_bytes)
{
    for (size_t i = 0; i + 2 < g->len; i++) {
        if ((g->code[i] & 0x0f) == 0x1 && g->code[i + 1] == 0xff) {
            const int li = g->code[i + 2];
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

    gen_t g;
    memset(&g, 0, sizeof(g));
    g.code = text;
    g.cap = CODE_CAP;

    lex_t L;
    memset(&L, 0, sizeof(L));
    L.src = src;
    L.len = src_len;
    lex_next(&L);

    par_t p = {.L = &L, .g = &g};
    parse_program(&p);

    if (L.err) {
        cpy_err(out->err, sizeof(out->err), L.errmsg);
        CC_FREE(text);
        return -1;
    }
    if (g.err) {
        cpy_err(out->err, sizeof(out->err), g.errmsg);
        CC_FREE(text);
        return -1;
    }

    const size_t lit_bytes = (size_t)g.nlit * 4u;
    fix_l32r(&g, lit_bytes);
    if (g.err) {
        cpy_err(out->err, sizeof(out->err), g.errmsg);
        CC_FREE(text);
        return -1;
    }

    /* Loader requires each part's size to be word-aligned. */
    const size_t code_raw = lit_bytes + g.len;
    const size_t code_size = (code_raw + 3u) & ~3u;
    const size_t data_size = 4; /* g_ag_api bss */
    const size_t file_size = AXE_HDR + code_size;
    uint8_t *axe = (uint8_t *)CC_ALLOC(file_size);
    if (axe == NULL) {
        cpy_err(out->err, sizeof(out->err), "out of memory");
        CC_FREE(text);
        return -1;
    }
    memset(axe, 0, file_size);

    /* ag_axe_header_t — must match tools/mkaxe.py HEADER_FORMAT */
    memcpy(axe + 0, "AXE1", 4);
    wr_u16(axe + 4, 0); /* abi_major */
    wr_u16(axe + 6, 8); /* abi_minor */
    wr_u16(axe + 8, 1); /* arch = xtensa */
    wr_u16(axe + 10, (uint16_t)AXE_HDR);
    wr_u32(axe + 12, 0); /* flags */

    wr_u32(axe + 16, CODE_BASE);
    wr_u32(axe + 20, (uint32_t)code_size);
    wr_u32(axe + 24, (uint32_t)code_size);
    wr_u32(axe + 28, AXE_HDR);

    wr_u32(axe + 32, DATA_BASE);
    wr_u32(axe + 36, (uint32_t)data_size);
    wr_u32(axe + 40, 0);
    wr_u32(axe + 44, AXE_HDR + (uint32_t)code_size);

    wr_u32(axe + 48, CODE_BASE + (uint32_t)lit_bytes); /* entry */
    wr_u32(axe + 52, DATA_BASE);                       /* api_slot */

    wr_u32(axe + 56, AXE_HDR + (uint32_t)code_size); /* reloc_offset */
    wr_u32(axe + 60, 0);                             /* reloc_count */

    wr_u32(axe + 64, 0); /* stack */
    wr_u32(axe + 68, 0); /* heap */
    wr_str(axe + 72, 32, "TCC");
    wr_str(axe + 104, 16, "0.1");
    wr_str(axe + 120, 32, "cc");

    uint8_t *code = axe + AXE_HDR;
    for (int i = 0; i < g.nlit; i++) {
        wr_u32(code + (size_t)i * 4u, g.lit[i]);
    }
    memcpy(code + lit_bytes, text, g.len);
    /* Padding bytes stay zero from memset. */

    CC_FREE(text);
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

/* ---- host expression evaluator ----------------------------------------- */

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
