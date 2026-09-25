/*
 * AGVM - the machine with no processor.  The third rung of the cascade.
 *
 *   run t:\agvm.axe t:\prog.bc
 *
 * A bytecode image runs here unchanged on every board this system knows, which
 * is the whole of what this rung buys: rung 1 needs the far node to have its
 * own build of the program, rung 2 needs it to have the memory to compile one,
 * and when neither holds there has to be something that simply works.  It costs
 * five to fifty times the speed, and that is the right trade for coordination -
 * policy, supervision, glue - because such a program sleeps between events.
 * For audio it is not the right trade and nothing here pretends otherwise.
 *
 * WHY THE INTERPRETER IS THIS SMALL.  It knows no builtins.  A bytecode program
 * reaches the ABI exactly as a native one does - load the api pointer out of
 * its own data, walk to the sub-table, walk to the function, call it - so all
 * the interpreter needs is a CALL that can call an ordinary function address
 * with six arguments.  Adding an ABI call to the system adds nothing here.
 *
 * AND IT IS NOT A SANDBOX.  Registers hold real addresses: `&local` is an
 * address in the frame store below, a string is an address in the image's data
 * part, and a handle from ag_open is whatever the kernel said.  There is no
 * MMU under any of this to build a sandbox with - see [T-12] in
 * docs/06-ideas.md.  What this rung buys is portability, not safety.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/axe.h>
#include <argon/libc.h>

#include "ag_bc.h"

/*
 * WHAT THIS ASKS FOR, and why it is not a round number with a margin.
 *
 * The first draft wanted 96 KB of heap because that was cheap on an S3.  On a
 * C3 with a wire driver already resident the largest free block was 73 KB, and
 * the cascade got all the way to the end - the far node refused the foreign
 * image, found the portable build, and the kernel sent it here - only for this
 * to be refused for memory.  A program that asks for room it does not need
 * fails in the one place where the answer was otherwise correct.
 *
 * What it actually needs: the bytecode file, the program's data part, the
 * frame store below, and about two kilobytes for its own state.  The file is
 * read into a buffer that GROWS rather than one sized for the largest
 * imaginable image, so a two-kilobyte program costs two kilobytes.
 *
 * 32 KB since 25 Sep 2026: the merge with main cost the C3 twelve kilobytes of
 * heap (the VFS miss cache, the thread budgets, the timer task behind the lock
 * watch), its largest block after the wire driver came down to 40 KB, and 48
 * was refused again in exactly the place described above.  8 KB of frames,
 * the file and the data part of the programs in this tree, and the state, fit
 * in 32 with room.
 */
AG_APP_SIZED("AGVM", "1.0", "argon", 0, 8 * 1024, 32 * 1024);

/*
 * The frame store, and why it is one block taken up front.
 *
 * Every frame a bytecode program opens lives in here, and `&local` is an
 * address into it that the program hands to the ABI - so it has to be ordinary
 * memory rather than an index into something.  One allocation rather than one
 * per call because a call has to cost an increment, not a trip to the heap.
 */
#define VM_FRAME_BYTES (8u * 1024u)

/* The largest bytecode image this will read.  A ceiling rather than an
 * allocation: the buffer below grows to what the file needs and no further. */
#define VM_FILE_MAX (32u * 1024u)

/* Nested calls.  Deeper than any program this rung is for, and bounded because
 * a runaway recursion must say so rather than walk off the end of an array. */
#define VM_DEPTH 128

typedef struct {
    uint32_t pc;  /* where the caller carries on            */
    uint32_t fp;  /* the caller's frame                     */
    uint32_t fsp; /* and how much of the store it was using */
} vm_frame_t;

typedef struct {
    const uint8_t *code;
    uint32_t       code_len;
    uint8_t       *data;
    uint32_t       data_len;
    uint8_t       *store;
    uint32_t       store_len;
    uint32_t       fsp;
    uint32_t       reg[AG_BC_REGS];
    vm_frame_t     frame[VM_DEPTH];
    int            depth;
    const char    *fault;
} vm_t;

/* Six arguments and an answer, which is what the seam's calling convention is
 * on every machine: the caller leaves them in 10..15 and reads 10 afterwards.
 * A callee that takes fewer ignores the rest; a variadic one spills them the
 * way it spills any caller's, which is how the native back ends call
 * ag_printf too. */
typedef int32_t (*vm_native_t)(int32_t, int32_t, int32_t, int32_t, int32_t,
                               int32_t);

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/*
 * One instruction.  Returns 0 while the program runs, 1 when it has returned
 * from its outermost frame, -1 on a fault - and a fault is always a sentence
 * in vm->fault, because "returned -22" from an interpreter tells nobody which
 * of a thousand instructions was wrong.
 */
static int vm_step(vm_t *vm, uint32_t *pc)
{
    if (*pc >= vm->code_len) {
        vm->fault = "ran past the end of the code";
        return -1;
    }
    const uint8_t *c = vm->code + *pc;
    const uint32_t left = vm->code_len - *pc;
    const uint8_t  op = c[0];

    /* The longest instruction is six bytes (op, rr, target32). */
    if (left < 6u && op != AG_BC_RET && op != AG_BC_NOP) {
        /* Not fatal by itself - a short tail is only a fault if the operands
         * are actually read past the end, which the cases below do check by
         * their own lengths.  Checked here once so each case need not. */
        if (left < 2u) {
            vm->fault = "an instruction runs off the end of the code";
            return -1;
        }
    }

    uint32_t *r = vm->reg;
    const int rd = (c[1] >> 4) & 0xf;
    const int rs = c[1] & 0xf;

    switch (op) {
    case AG_BC_NOP:
        *pc += 1u;
        return 0;

    case AG_BC_MOV:
        r[rd] = r[rs];
        *pc += 2u;
        return 0;

    case AG_BC_LI:
        r[c[1] & 0xf] = rd32(c + 2);
        *pc += 6u;
        return 0;

    case AG_BC_LIDATA: {
        const uint32_t off = rd32(c + 2);
        if (off > vm->data_len) {
            vm->fault = "an address outside this program's data";
            return -1;
        }
        r[c[1] & 0xf] = (uint32_t)(uintptr_t)(vm->data + off);
        *pc += 6u;
        return 0;
    }

    case AG_BC_LICODE:
        r[c[1] & 0xf] = AG_BC_CODE_TAG | rd32(c + 2);
        *pc += 6u;
        return 0;

    case AG_BC_ADD:
    case AG_BC_SUB:
    case AG_BC_MUL:
    case AG_BC_DIVS:
    case AG_BC_REMS:
    case AG_BC_DIVU:
    case AG_BC_REMU:
    case AG_BC_AND:
    case AG_BC_OR:
    case AG_BC_XOR:
    case AG_BC_SHL:
    case AG_BC_SHRS:
    case AG_BC_SHRU:
    case AG_BC_SLT:
    case AG_BC_SLTU: {
        const uint32_t a = r[rs];
        const uint32_t b = r[c[2] & 0xf];
        uint32_t       v = 0;
        switch (op) {
        case AG_BC_ADD:
            v = a + b;
            break;
        case AG_BC_SUB:
            v = a - b;
            break;
        case AG_BC_MUL:
            v = (uint32_t)((int32_t)a * (int32_t)b);
            break;
        /* Division by zero answers the way RISC-V's hardware does - all ones,
         * and the dividend for a remainder - rather than trapping.  A machine
         * that behaves differently from the ones beside it would make the same
         * program give two answers, which is the one thing this rung exists to
         * prevent. */
        case AG_BC_DIVS:
            v = (b == 0u) ? 0xffffffffu : (uint32_t)((int32_t)a / (int32_t)b);
            break;
        case AG_BC_REMS:
            v = (b == 0u) ? a : (uint32_t)((int32_t)a % (int32_t)b);
            break;
        case AG_BC_DIVU:
            v = (b == 0u) ? 0xffffffffu : (a / b);
            break;
        case AG_BC_REMU:
            v = (b == 0u) ? a : (a % b);
            break;
        case AG_BC_AND:
            v = a & b;
            break;
        case AG_BC_OR:
            v = a | b;
            break;
        case AG_BC_XOR:
            v = a ^ b;
            break;
        case AG_BC_SHL:
            v = a << (b & 31u);
            break;
        case AG_BC_SHRS:
            v = (uint32_t)((int32_t)a >> (b & 31u));
            break;
        case AG_BC_SHRU:
            v = a >> (b & 31u);
            break;
        case AG_BC_SLT:
            v = ((int32_t)a < (int32_t)b) ? 1u : 0u;
            break;
        default:
            v = (a < b) ? 1u : 0u;
            break;
        }
        r[rd] = v;
        *pc += 3u;
        return 0;
    }

    case AG_BC_SHLI:
        r[rd] = r[rs] << (c[2] & 31u);
        *pc += 3u;
        return 0;
    case AG_BC_SHRSI:
        r[rd] = (uint32_t)((int32_t)r[rs] >> (c[2] & 31u));
        *pc += 3u;
        return 0;
    case AG_BC_SHRUI:
        r[rd] = r[rs] >> (c[2] & 31u);
        *pc += 3u;
        return 0;

    case AG_BC_LW:
    case AG_BC_LBU:
    case AG_BC_SW:
    case AG_BC_SB: {
        const uint32_t off = rd16(c + 2);
        /* memcpy for the word cases: this is a real address the program chose,
         * and Xtensa refuses an unaligned word where the interpreter's own
         * compiler would have assumed one. */
        if (op == AG_BC_LW) {
            uint32_t v;
            memcpy(&v, (const void *)(uintptr_t)(r[rs] + off), 4);
            r[rd] = v;
        } else if (op == AG_BC_LBU) {
            r[rd] = *(const uint8_t *)(uintptr_t)(r[rs] + off);
        } else if (op == AG_BC_SW) {
            const uint32_t v = r[rd];
            memcpy((void *)(uintptr_t)(r[rs] + off), &v, 4);
        } else {
            *(uint8_t *)(uintptr_t)(r[rs] + off) = (uint8_t)r[rd];
        }
        *pc += 4u;
        return 0;
    }

    case AG_BC_CALL: {
        const uint32_t target = r[c[1] & 0xf];
        if ((target & AG_BC_CODE_TAG) != 0u) {
            const uint32_t to = target & ~AG_BC_CODE_TAG;
            if (to >= vm->code_len) {
                vm->fault = "a call to an address outside the code";
                return -1;
            }
            if (vm->depth >= VM_DEPTH) {
                vm->fault = "too many nested calls";
                return -1;
            }
            vm->frame[vm->depth].pc = *pc + 2u;
            vm->frame[vm->depth].fp = r[7];
            vm->frame[vm->depth].fsp = vm->fsp;
            vm->depth++;
            *pc = to;
            return 0;
        }
        if (target == 0u) {
            vm->fault = "a call through a null address";
            return -1;
        }
        {
            const vm_native_t fn = (vm_native_t)(uintptr_t)target;
            r[10] = (uint32_t)fn((int32_t)r[10], (int32_t)r[11],
                                 (int32_t)r[12], (int32_t)r[13],
                                 (int32_t)r[14], (int32_t)r[15]);
        }
        *pc += 2u;
        return 0;
    }

    case AG_BC_ENTER: {
        const uint32_t want = (rd32(c + 1) + 15u) & ~15u;
        if (want > vm->fsp) {
            vm->fault = "out of frame store - the program nests too deep";
            return -1;
        }
        vm->fsp -= want;
        r[7] = (uint32_t)(uintptr_t)(vm->store + vm->fsp);
        *pc += 5u;
        return 0;
    }

    case AG_BC_RET:
        if (vm->depth == 0) {
            return 1;
        }
        vm->depth--;
        r[7] = vm->frame[vm->depth].fp;
        vm->fsp = vm->frame[vm->depth].fsp;
        *pc = vm->frame[vm->depth].pc;
        return 0;

    case AG_BC_JMP:
        *pc = rd32(c + 1);
        if (*pc > vm->code_len) {
            vm->fault = "a jump outside the code";
            return -1;
        }
        return 0;

    case AG_BC_BEQ:
    case AG_BC_BNE:
    case AG_BC_BLT:
    case AG_BC_BGE:
    case AG_BC_BLTU:
    case AG_BC_BGEU: {
        const uint32_t a = r[rd];
        const uint32_t b = r[rs];
        int            take = 0;
        switch (op) {
        case AG_BC_BEQ:
            take = (a == b);
            break;
        case AG_BC_BNE:
            take = (a != b);
            break;
        case AG_BC_BLT:
            take = ((int32_t)a < (int32_t)b);
            break;
        case AG_BC_BGE:
            take = ((int32_t)a >= (int32_t)b);
            break;
        case AG_BC_BLTU:
            take = (a < b);
            break;
        default:
            take = (a >= b);
            break;
        }
        if (take) {
            *pc = rd32(c + 2);
            if (*pc > vm->code_len) {
                vm->fault = "a branch outside the code";
                return -1;
            }
        } else {
            *pc += 6u;
        }
        return 0;
    }

    default:
        vm->fault = "an instruction this interpreter does not know";
        return -1;
    }
}

static uint8_t *read_all(const char *path, uint32_t *out_len)
{
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("agvm: open %s: %s\n", path, ag_strerror(h));
        return NULL;
    }
    /*
     * Grown rather than guessed.  There is no seek in the application ABI - it
     * is int64 and every value here is a word - so the size is not knowable
     * before reading; doubling costs one copy per doubling and leaves a small
     * program paying for a small program.
     */
    uint32_t cap = 4u * 1024u;
    uint8_t *buf = (uint8_t *)ag_malloc(cap);
    if (buf == NULL) {
        ag_close(h);
        ag_print("agvm: out of memory\n");
        return NULL;
    }
    uint32_t n = 0;
    for (;;) {
        if (n == cap) {
            if (cap >= VM_FILE_MAX) {
                ag_close(h);
                ag_free(buf);
                ag_printf("agvm: %s is larger than %u bytes\n", path,
                          (unsigned)VM_FILE_MAX);
                return NULL;
            }
            uint8_t *bigger = (uint8_t *)ag_realloc(buf, cap * 2u);
            if (bigger == NULL) {
                ag_close(h);
                ag_free(buf);
                ag_printf("agvm: no room for %s past %u bytes\n", path,
                          (unsigned)cap);
                return NULL;
            }
            buf = bigger;
            cap *= 2u;
        }
        const int32_t got = ag_read(h, buf + n, cap - n);
        if (got < 0) {
            ag_close(h);
            ag_free(buf);
            ag_printf("agvm: read %s: %s\n", path, ag_strerror(got));
            return NULL;
        }
        if (got == 0) {
            break;
        }
        n += (uint32_t)got;
    }
    ag_close(h);
    *out_len = n;
    return buf;
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        ag_print("usage: agvm <prog.bc>\n");
        return 2;
    }

    uint32_t file_len = 0;
    uint8_t *file = read_all(argv[1], &file_len);
    if (file == NULL) {
        return 1;
    }
    if (file_len < sizeof(ag_axe_header_t)) {
        ag_print("agvm: that file is too short to be an image\n");
        ag_free(file);
        return 1;
    }

    const ag_axe_header_t *h = (const ag_axe_header_t *)file;
    if (memcmp(h->magic, AG_AXE_MAGIC_STR, 4) != 0) {
        ag_print("agvm: that is not an ArgonOS image\n");
        ag_free(file);
        return 1;
    }
    /*
     * Said by name rather than refused with a number.  An image for a
     * processor is the commonest thing to be handed here by mistake, and the
     * whole point of this rung is that the difference between machines is
     * something the system talks about out loud.
     */
    if (h->arch != AG_ARCH_BC) {
        ag_printf("agvm: %s is built for a processor (arch %u), not for this "
                  "machine; run it directly\n",
                  argv[1], (unsigned)h->arch);
        ag_free(file);
        return 1;
    }
    if (h->code.offset + h->code.file_size > file_len ||
        h->data.offset + h->data.file_size > file_len) {
        ag_print("agvm: the image disagrees with its own size\n");
        ag_free(file);
        return 1;
    }

    vm_t *vm = (vm_t *)ag_malloc(sizeof(*vm));
    if (vm == NULL) {
        ag_print("agvm: out of memory\n");
        ag_free(file);
        return 1;
    }
    memset(vm, 0, sizeof(*vm));

    vm->code = file + h->code.offset;
    vm->code_len = h->code.file_size;

    /* The data part, with whatever the file did not carry zeroed - the same
     * rule the loader follows for a native image. */
    vm->data_len = h->data.size;
    vm->data = (uint8_t *)ag_malloc(vm->data_len ? vm->data_len : 4u);
    if (vm->data == NULL) {
        ag_print("agvm: out of memory for the program's data\n");
        ag_free(vm);
        ag_free(file);
        return 1;
    }
    memset(vm->data, 0, vm->data_len ? vm->data_len : 4u);
    memcpy(vm->data, file + h->data.offset, h->data.file_size);

    vm->store = (uint8_t *)ag_malloc(VM_FRAME_BYTES);
    if (vm->store == NULL) {
        ag_print("agvm: out of memory for the frame store\n");
        ag_free(vm->data);
        ag_free(vm);
        ag_free(file);
        return 1;
    }
    vm->store_len = VM_FRAME_BYTES;
    vm->fsp = VM_FRAME_BYTES;

    /*
     * The api pointer, where the program's own code expects to find it: the
     * compiler emits "load data+0, then walk the tables", which is exactly
     * what a native image does with the word the loader writes there.
     */
    if (h->api_slot >= h->data.base &&
        h->api_slot + 4u <= h->data.base + vm->data_len) {
        const uint32_t at = h->api_slot - h->data.base;
        const uint32_t api = (uint32_t)(uintptr_t)g_ag_api;
        memcpy(vm->data + at, &api, 4);
    } else {
        ag_print("agvm: the image has nowhere to put the system table\n");
        ag_free(vm->store);
        ag_free(vm->data);
        ag_free(vm);
        ag_free(file);
        return 1;
    }

    uint32_t pc = h->entry - h->code.base;
    int      rc = 0;
    for (;;) {
        const int st = vm_step(vm, &pc);
        if (st == 1) {
            break;
        }
        if (st < 0) {
            ag_printf("agvm: %s (at %u)\n",
                      vm->fault ? vm->fault : "stopped", (unsigned)pc);
            rc = 1;
            break;
        }
    }
    if (rc == 0) {
        rc = (int)(int32_t)vm->reg[10];
    }

    ag_free(vm->store);
    ag_free(vm->data);
    ag_free(vm);
    ag_free(file);
    return rc;
}
