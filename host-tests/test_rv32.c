/*
 * ArgonOS - what the RISC-V back end actually computes.
 *
 * The disassembler says the instructions are legal; it does not say they are
 * the right ones.  SRLI where SRAI belonged decodes perfectly and is wrong from
 * 0x80000000 up, and the Xtensa side learned that the hard way - which is why
 * the note in apps/cc/README.md says a host test cannot see which instruction
 * was chosen, only an ANSWER can.
 *
 * On Xtensa the answers come from QEMU.  There is no QEMU for the C3 in this
 * tree, and a board is a slow way to find out which of nine hundred
 * instructions was wrong - so this file runs the code here: an RV32IM
 * interpreter, a flat memory, and the KERNEL'S OWN relocation code applied to
 * the image first.  That last part matters as much as the arithmetic: it is
 * the only thing that checks the compiler and the loader agree about what a
 * lui/addi pair means.
 *
 * What it does not cover is the board: caches, alignment, the real ABI table,
 * and whether the C3's loader accepts the file at all.  `argon c3job` is that.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include <argon/axe.h>
#include <argon/axeload.h>

#include "cc_compile.h"

/*
 * A megabyte of flat memory, addressed from zero, with the parts put where
 * this file chooses rather than where the image says - which is the whole
 * point: an image that only worked at the address it was linked at would pass
 * nothing here, and is exactly what relocation exists to prevent.
 */
#define MEM_SIZE   0x100000u
#define CODE_AT    0x00001000u
#define DATA_AT    0x00040000u
#define API_AT     0x00080000u
#define CON_AT     0x00080100u
#define STACK_TOP  0x000F0000u

/* Addresses that are not code: calling one is a call into the kernel. */
#define FN_PUTS   0xFFFF0010u
#define FN_PRINTF 0xFFFF0020u

#define OUT_CAP 4096

typedef struct {
    uint8_t *mem;
    uint32_t x[32];
    uint32_t pc;
    int      halted;
    int      fault;
    char     out[OUT_CAP];
    size_t   outlen;
    uint32_t steps;
} rv_t;

static uint32_t ld32(const rv_t *m, uint32_t a)
{
    if (a + 4u > MEM_SIZE) {
        return 0;
    }
    uint32_t v;
    memcpy(&v, m->mem + a, 4);
    return v;
}

static void st32(rv_t *m, uint32_t a, uint32_t v)
{
    if (a + 4u > MEM_SIZE) {
        return;
    }
    memcpy(m->mem + a, &v, 4);
}

static void emit_out(rv_t *m, char c)
{
    if (m->outlen + 1u < sizeof(m->out)) {
        m->out[m->outlen++] = c;
        m->out[m->outlen] = '\0';
    }
}

static void emit_str(rv_t *m, const char *s)
{
    for (; *s != '\0'; s++) {
        emit_out(m, *s);
    }
}

static void emit_guest_str(rv_t *m, uint32_t addr)
{
    for (uint32_t i = 0; i < 1024u && addr + i < MEM_SIZE; i++) {
        const char c = (char)m->mem[addr + i];
        if (c == '\0') {
            return;
        }
        emit_out(m, c);
    }
}

/*
 * Enough printf for the programs below, and no more: %d %u %x %08x %s %c.  A
 * test that needed a format this does not have would be a test about printf
 * rather than about the compiler.
 */
static void do_printf(rv_t *m)
{
    uint32_t fmt = m->x[10];
    int      argi = 11;
    char     num[32];

    for (uint32_t i = 0; i < 1024u && fmt + i < MEM_SIZE; i++) {
        const char c = (char)m->mem[fmt + i];
        if (c == '\0') {
            return;
        }
        if (c != '%') {
            emit_out(m, c);
            continue;
        }
        i++;
        char spec = (char)m->mem[fmt + i];
        int  zero8 = 0;
        if (spec == '0') {
            /* only %08x is used */
            i += 2;
            spec = (char)m->mem[fmt + i];
            zero8 = 1;
        }
        const uint32_t v = (argi <= 15) ? m->x[argi] : 0u;
        switch (spec) {
        case '%':
            emit_out(m, '%');
            continue;
        case 'd':
            snprintf(num, sizeof(num), "%d", (int)v);
            emit_str(m, num);
            break;
        case 'u':
            snprintf(num, sizeof(num), "%u", (unsigned)v);
            emit_str(m, num);
            break;
        case 'x':
            snprintf(num, sizeof(num), zero8 ? "%08x" : "%x", (unsigned)v);
            emit_str(m, num);
            break;
        case 'c':
            emit_out(m, (char)v);
            break;
        case 's':
            emit_guest_str(m, v);
            break;
        default:
            emit_out(m, spec);
            continue;
        }
        argi++;
    }
}

static int32_t sx(uint32_t v, int bits)
{
    const uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

/* One instruction.  Only the shapes this compiler emits; anything else is a
 * fault, which is the answer that finds an encoding bug fastest. */
static void step(rv_t *m)
{
    const uint32_t w = ld32(m, m->pc);
    const uint32_t op = w & 0x7fu;
    const uint32_t rd = (w >> 7) & 0x1fu;
    const uint32_t f3 = (w >> 12) & 0x7u;
    const uint32_t rs1 = (w >> 15) & 0x1fu;
    const uint32_t rs2 = (w >> 20) & 0x1fu;
    const uint32_t f7 = w >> 25;
    uint32_t       next = m->pc + 4u;

    switch (op) {
    case 0x37u: /* lui */
        m->x[rd] = w & 0xfffff000u;
        break;
    case 0x13u: { /* addi, slli, srli, srai */
        const uint32_t a = m->x[rs1];
        if (f3 == 0x0u) {
            m->x[rd] = a + (uint32_t)sx(w >> 20, 12);
        } else if (f3 == 0x1u && f7 == 0x00u) {
            m->x[rd] = a << rs2;
        } else if (f3 == 0x5u && f7 == 0x00u) {
            m->x[rd] = a >> rs2;
        } else if (f3 == 0x5u && f7 == 0x20u) {
            m->x[rd] = (uint32_t)((int32_t)a >> rs2);
        } else {
            m->fault = 1;
        }
        break;
    }
    case 0x33u: { /* the register-register set, plus M */
        const uint32_t a = m->x[rs1];
        const uint32_t b = m->x[rs2];
        if (f7 == 0x01u) {
            switch (f3) {
            case 0x0u:
                m->x[rd] = (uint32_t)((int32_t)a * (int32_t)b);
                break;
            case 0x4u:
                m->x[rd] = (b == 0) ? 0xffffffffu
                                    : (uint32_t)((int32_t)a / (int32_t)b);
                break;
            case 0x5u:
                m->x[rd] = (b == 0) ? 0xffffffffu : (a / b);
                break;
            case 0x6u:
                m->x[rd] = (b == 0) ? a : (uint32_t)((int32_t)a % (int32_t)b);
                break;
            case 0x7u:
                m->x[rd] = (b == 0) ? a : (a % b);
                break;
            default:
                m->fault = 1;
                break;
            }
            break;
        }
        switch (f3) {
        case 0x0u:
            m->x[rd] = (f7 == 0x20u) ? (a - b) : (a + b);
            break;
        case 0x1u:
            m->x[rd] = a << (b & 31u);
            break;
        case 0x2u:
            m->x[rd] = ((int32_t)a < (int32_t)b) ? 1u : 0u;
            break;
        case 0x3u:
            m->x[rd] = (a < b) ? 1u : 0u;
            break;
        case 0x4u:
            m->x[rd] = a ^ b;
            break;
        case 0x5u:
            m->x[rd] = (f7 == 0x20u) ? (uint32_t)((int32_t)a >> (b & 31u))
                                     : (a >> (b & 31u));
            break;
        case 0x6u:
            m->x[rd] = a | b;
            break;
        default:
            m->x[rd] = a & b;
            break;
        }
        break;
    }
    case 0x03u: { /* lw, lbu */
        const uint32_t a = m->x[rs1] + (uint32_t)sx(w >> 20, 12);
        if (f3 == 0x2u) {
            m->x[rd] = ld32(m, a);
        } else if (f3 == 0x4u) {
            m->x[rd] = (a < MEM_SIZE) ? m->mem[a] : 0u;
        } else {
            m->fault = 1;
        }
        break;
    }
    case 0x23u: { /* sw, sb */
        const uint32_t imm = ((w >> 25) << 5) | ((w >> 7) & 0x1fu);
        const uint32_t a = m->x[rs1] + (uint32_t)sx(imm, 12);
        if (f3 == 0x2u) {
            st32(m, a, m->x[rs2]);
        } else if (f3 == 0x0u) {
            if (a < MEM_SIZE) {
                m->mem[a] = (uint8_t)m->x[rs2];
            }
        } else {
            m->fault = 1;
        }
        break;
    }
    case 0x63u: { /* the branches */
        const uint32_t imm = (((w >> 31) & 1u) << 12) |
                             (((w >> 7) & 1u) << 11) |
                             (((w >> 25) & 0x3fu) << 5) |
                             (((w >> 8) & 0xfu) << 1);
        const uint32_t a = m->x[rs1];
        const uint32_t b = m->x[rs2];
        int            take = 0;
        switch (f3) {
        case 0x0u:
            take = (a == b);
            break;
        case 0x1u:
            take = (a != b);
            break;
        case 0x4u:
            take = ((int32_t)a < (int32_t)b);
            break;
        case 0x5u:
            take = ((int32_t)a >= (int32_t)b);
            break;
        case 0x6u:
            take = (a < b);
            break;
        case 0x7u:
            take = (a >= b);
            break;
        default:
            m->fault = 1;
            break;
        }
        if (take) {
            next = m->pc + (uint32_t)sx(imm, 13);
        }
        break;
    }
    case 0x6fu: { /* jal */
        const uint32_t imm = (((w >> 31) & 1u) << 20) |
                             (((w >> 12) & 0xffu) << 12) |
                             (((w >> 20) & 1u) << 11) |
                             (((w >> 21) & 0x3ffu) << 1);
        if (rd != 0) {
            m->x[rd] = m->pc + 4u;
        }
        next = m->pc + (uint32_t)sx(imm, 21);
        break;
    }
    case 0x67u: { /* jalr */
        const uint32_t target =
            (m->x[rs1] + (uint32_t)sx(w >> 20, 12)) & ~1u;
        const uint32_t link = m->pc + 4u;
        if (target == FN_PUTS) {
            emit_guest_str(m, m->x[10]);
            next = m->x[1];
        } else if (target == FN_PRINTF) {
            do_printf(m);
            next = m->x[1];
        } else {
            if (rd != 0) {
                m->x[rd] = link;
            }
            next = target;
        }
        break;
    }
    default:
        m->fault = 1;
        break;
    }
    m->x[0] = 0;
    m->pc = next;
}

/*
 * Compile for RISC-V, place the two parts where this file wants them, let the
 * KERNEL relocate the image, and run ag_main.  Returns what it returned, or
 * fills `err` and returns a sentinel.
 */
static int run_rv32(const char *src, int32_t *ret, char *out, size_t outcap,
                    char *err, size_t errcap)
{
    cc_result_t res;
    if (cc_compile_to_axe_for(src, strlen(src), NULL, NULL, CC_ARCH_RV32,
                              &res) != 0) {
        snprintf(err, errcap, "compile: %s", res.err);
        return -1;
    }

    const ag_axe_header_t *h = (const ag_axe_header_t *)res.axe;
    if (h->arch != AG_ARCH_RISCV32) {
        snprintf(err, errcap, "header says arch %u", (unsigned)h->arch);
        cc_result_free(&res);
        return -1;
    }

    rv_t *m = (rv_t *)calloc(1, sizeof(rv_t));
    m->mem = (uint8_t *)calloc(1, MEM_SIZE);

    memcpy(m->mem + CODE_AT, res.axe + h->code.offset, h->code.file_size);
    memcpy(m->mem + DATA_AT, res.axe + h->data.offset, h->data.file_size);

    /*
     * The loader's own code, not a copy of it.  If the compiler and the kernel
     * ever disagree about which bits of a lui an address goes into, this is
     * where it shows - and it shows as a wrong answer rather than as a board
     * that reboots.
     */
    const ag_axe_ireloc_t *ir =
        (const ag_axe_ireloc_t *)(res.axe + h->ireloc_offset);
    for (uint32_t i = 0; i < h->ireloc_count; i++) {
        if (!ag_axe_ireloc_apply(h, m->mem + CODE_AT, h->code.file_size,
                                 CODE_AT, DATA_AT, 0, ir[i])) {
            snprintf(err, errcap, "ireloc %u refused", (unsigned)i);
            free(m->mem);
            free(m);
            cc_result_free(&res);
            return -1;
        }
    }
    /* An RV32 image should have no word relocations at all: every address it
     * uses is built in an instruction. */
    if (h->reloc_count != 0) {
        snprintf(err, errcap, "%u word relocations on riscv",
                 (unsigned)h->reloc_count);
        free(m->mem);
        free(m);
        cc_result_free(&res);
        return -1;
    }

    /* The api table, and the pointer to it the loader would have written. */
    st32(m, API_AT + 20u, CON_AT);          /* api->con   (API_OFF_CON)  */
    st32(m, CON_AT + 8u, FN_PUTS);          /* con->puts                 */
    st32(m, CON_AT + 12u, FN_PRINTF);       /* con->printf               */
    st32(m, DATA_AT + (h->api_slot - h->data.base), API_AT);

    m->x[2] = STACK_TOP;                    /* sp                        */
    m->x[1] = 0xDEADBEEFu;                  /* ra: returning here stops  */
    m->pc = CODE_AT + (h->entry - h->code.base);

    for (m->steps = 0; m->steps < 80000000u; m->steps++) {
        if (m->pc == 0xDEADBEEEu || m->pc == 0xDEADBEEFu) {
            break;
        }
        step(m);
        if (m->fault) {
            snprintf(err, errcap, "bad instruction %08x at %08x",
                     (unsigned)ld32(m, m->pc), (unsigned)m->pc);
            free(m->mem);
            free(m);
            cc_result_free(&res);
            return -1;
        }
    }
    if (m->steps >= 80000000u) {
        snprintf(err, errcap, "did not stop");
        free(m->mem);
        free(m);
        cc_result_free(&res);
        return -1;
    }

    *ret = (int32_t)m->x[10];
    if (out != NULL) {
        snprintf(out, outcap, "%s", m->out);
    }
    free(m->mem);
    free(m);
    cc_result_free(&res);
    return 0;
}

/* The two shapes every test below takes. */
static void expect_ret(const char *src, int32_t want)
{
    int32_t got = 0;
    char    err[256] = {0};
    if (run_rv32(src, &got, NULL, 0, err, sizeof(err)) != 0) {
        ag_test_checks++;
        ag_test_failures++;
        printf("FAIL rv32: %s\n     %.120s\n", err, src);
        return;
    }
    AG_CHECK_INT(got, want);
    if (got != want) {
        printf("     %.160s\n", src);
    }
}

static void expect_out(const char *src, const char *want)
{
    int32_t got = 0;
    char    out[OUT_CAP] = {0};
    char    err[256] = {0};
    if (run_rv32(src, &got, out, sizeof(out), err, sizeof(err)) != 0) {
        ag_test_checks++;
        ag_test_failures++;
        printf("FAIL rv32: %s\n     %.120s\n", err, src);
        return;
    }
    AG_CHECK_STR(out, want);
}

void run_rv32_tests(void)
{
    printf("rv32\n");

    /* ---- the machine at all ---- */
    expect_ret("int ag_main(void) { return 7; }\n", 7);
    expect_ret("int ag_main(void) { return 1 + 2 * 3; }\n", 7);
    expect_ret("int ag_main(void) { int a = 5; int b = 3; return a - b; }\n", 2);

    /*
     * Constants across the ADDI boundary.  A lui/addi pair that forgets that
     * ADDI sign-extends is off by exactly 4096 for half of all values, and
     * every one of these is a value where that shows.
     */
    expect_ret("int ag_main(void) { return 2047; }\n", 2047);
    expect_ret("int ag_main(void) { return 2048; }\n", 2048);
    expect_ret("int ag_main(void) { return 4095; }\n", 4095);
    expect_ret("int ag_main(void) { return 4096; }\n", 4096);
    expect_ret("int ag_main(void) { return -2048; }\n", -2048);
    expect_ret("int ag_main(void) { return -2049; }\n", -2049);
    expect_ret("int ag_main(void) { return 0x7ffff800; }\n", 0x7ffff800);
    expect_ret("int ag_main(void) { return 0x7fffffff; }\n", 0x7fffffff);
    expect_ret("int ag_main(void) { int x = 0xfffff800; return x; }\n",
               (int32_t)0xfffff800u);
    expect_ret("int ag_main(void) { int x = 0x80000000; return x + 1; }\n",
               (int32_t)0x80000001u);

    /* ---- counting upwards only: the six operators that differ ---- */
    expect_ret("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 a = 0xffffffff; return a / 3; }\n",
               (int32_t)0x55555555u);
    expect_ret("int ag_main(void) { int a = 0xffffffff; return a / 3; }\n", 0);
    expect_ret("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 a = 0xffffffff; return a % 10; }\n", 5);
    expect_ret("int ag_main(void) { int a = 0xffffffff; return a % 10; }\n",
               -1);
    expect_ret("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 a = 0x80000000; return a >> 4; }\n",
               0x08000000);
    expect_ret("int ag_main(void) { int a = 0x80000000; return a >> 4; }\n",
               (int32_t)0xf8000000u);
    /* A shift past fifteen, which is where the Xtensa side needs a register. */
    expect_ret("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 a = 0x80000000; return a >> 20; }\n",
               0x800);
    expect_ret("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 a = 0x80000000; u32 s = 20;\n"
               "  return a >> s; }\n", 0x800);
    expect_ret("int ag_main(void) { int a = 0x80000000; int s = 20;\n"
               "  return a >> s; }\n", (int32_t)0xfffff800u);
    expect_ret("int ag_main(void) { int a = 3; int s = 5; return a << s; }\n",
               96);
    /* And the comparison, which is the one that bites hardest. */
    expect_ret("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 a = 0x80000000;\n"
               "  if (a > 1) { return 1; } return 0; }\n", 1);
    expect_ret("int ag_main(void) { int a = 0x80000000;\n"
               "  if (a > 1) { return 1; } return 0; }\n", 0);

    /* ---- control flow ---- */
    expect_ret("int ag_main(void) { int s = 0; int i = 0;\n"
               "  while (i < 10) { s = s + i; i = i + 1; } return s; }\n", 45);
    expect_ret("int ag_main(void) { int s = 0;\n"
               "  for (int i = 0; i < 100; i++) { if (i % 3 == 0) { continue; }\n"
               "    if (i > 50) { break; } s = s + i; } return s; }\n", 867);
    expect_ret("int ag_main(void) { int n = 3; int r = 0;\n"
               "  switch (n) { case 1: r = 10; break; case 3: r = 30; break;\n"
               "    default: r = 99; } return r; }\n", 30);
    expect_ret("int ag_main(void) { int a = 1; int b = 0;\n"
               "  if (a && !b) { return 11; } return 0; }\n", 11);

    /* ---- calls, which is where the missing window shows ---- */
    expect_ret("int add6(int a, int b, int c, int d, int e, int f)\n"
               "{ return a + b*2 + c*3 + d*4 + e*5 + f*6; }\n"
               "int ag_main(void) { return add6(1, 2, 3, 4, 5, 6); }\n", 91);
    expect_ret("int fact(int n) { if (n < 2) { return 1; }\n"
               "  return n * fact(n - 1); }\n"
               "int ag_main(void) { return fact(10); }\n", 3628800);
    expect_ret("int f(int n) { return n + 1; }\n"
               "int g(int n) { return f(n) * 2; }\n"
               "int ag_main(void) { return g(f(3)) + f(g(1)); }\n", 15);
    /* A call inside an argument to a call: the value in hand has to survive,
     * and on this machine t0 does not - the spill slots do. */
    expect_ret("int f(int n) { return n * 3; }\n"
               "int ag_main(void) { return f(f(2) + f(3)) + f(1); }\n", 48);

    /* ---- memory ---- */
    expect_ret("int g_a[8];\n"
               "int ag_main(void) { for (int i = 0; i < 8; i++)\n"
               "  { g_a[i] = i * i; } return g_a[7] - g_a[3]; }\n", 40);
    expect_ret("char b[8];\n"
               "int ag_main(void) { b[0] = 200; b[1] = 55;\n"
               "  return b[0] + b[1]; }\n", 255);
    expect_ret("struct v { int a; char c; int b; };\n"
               "struct v s;\n"
               "int ag_main(void) { s.a = 5; s.c = 3; s.b = 7;\n"
               "  struct v *p = &s; return p->a * p->b + p->c + sizeof(s); }\n",
               50);
    expect_ret("int ag_main(void) { char *s = \"hello\"; int n = 0;\n"
               "  while (s[n] != 0) { n = n + 1; } return n; }\n", 5);
    /* A local array and a pointer walking it.  One `i`, because a for-init
     * declaration is a local of the whole function here. */
    expect_ret("int ag_main(void) { int a[5]; int *p = &a[0]; int s = 0;\n"
               "  for (int i = 0; i < 5; i++) { a[i] = i + 1; }\n"
               "  for (i = 0; i < 5; i++) { s = s + *p; p = p + 1; }\n"
               "  return s; }\n", 15);

    /* ---- and the ABI table, which is the whole reason for the irelocs ---- */
    expect_out("int ag_main(void) { ag_print(\"hi\\n\"); return 0; }\n", "hi\n");
    expect_out("int ag_main(void) { ag_printf(\"%d %d\\n\", 7, -7);\n"
               "  return 0; }\n", "7 -7\n");
    expect_out("typedef unsigned int u32;\n"
               "int ag_main(void) { u32 t = 0x80000000;\n"
               "  ag_printf(\"%08x %08x\\n\", t >> 4, t >> 20); return 0; }\n",
               "08000000 00000800\n");
    expect_out("int ag_main(void) { char *s = \"world\";\n"
               "  ag_printf(\"%s %c\\n\", s, 33); return 0; }\n", "world !\n");
    /* Six arguments through a variadic call - the most the ABI carries. */
    expect_out("int ag_main(void) { ag_printf(\"%d%d%d%d%d\\n\", 1, 2, 3, 4, 5);\n"
               "  return 0; }\n", "12345\n");

    /*
     * A branch far enough to need the full range, which the Xtensa side
     * refuses at 128 bytes and this machine allows to four kilobytes.  Written
     * as a loop body big enough to push the `if` past that.
     */
    expect_ret("int ag_main(void) { int s = 0; int i = 0;\n"
               "  while (i < 4) { if (i == 2) { s = s + 1000; }\n"
               "    s = s + 1; s = s + 2; s = s + 3; s = s + 4; s = s + 5;\n"
               "    s = s + 6; s = s + 7; s = s + 8; s = s + 9; s = s + 10;\n"
               "    i = i + 1; } return s; }\n", 1220);
}
