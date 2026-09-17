/*
 * Counting upwards only - the four operators where it changes the answer.
 *
 * Every line here is a value whose signed and unsigned readings differ, which
 * is the only kind worth testing: a compiler that recorded `unsigned` and
 * ignored it passes every test written with small numbers and fails the day
 * something crosses 0x80000000.
 *
 * Run it:
 *   run t:\cc.axe t:\unsigned.c t:\unsigned.axe
 *   run t:\unsigned.axe
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */

#pragma app "UNSIGNED" "1.0" "argon"

typedef unsigned int u32;

/*
 * `static` here says what this compiler already does - one image, nothing
 * outside it can see a name - and is accepted for exactly that reason.
 *
 * No value on the declaration: a global here is zero until something assigns
 * it, which is a limit of this compiler and not of the word `static`.
 */
static u32 top_bit;

int ag_main(void)
{
    top_bit = 0x80000000;
    u32  top = top_bit;
    u32  all = 0xffffffff;
    bool big = top > 1;
    int stop = 0x80000000; /* the same bits, read the other way */

    /*
     * A shift that fills with zeros rather than with the sign.  Signed, this
     * is 0xf8000000 - the classic way to lose the top half of a word.
     */
    ag_printf("shr   %08x\n", top >> 4);
    ag_printf("shr_s %08x\n", stop >> 4);

    /* Division and remainder.  Signed, 0xffffffff is -1: -1/3 is 0 and
     * -1 %% 10 is -1, neither of which is what a counter means. */
    ag_printf("div   %08x\n", all / 3);
    ag_printf("rem   %d\n", all % 10);

    /*
     * And the comparison, which is the one that bites hardest: a counter past
     * 0x80000000 is a large number, and a signed `<` calls it negative.
     */
    if (big) {
        ag_print("gt    yes\n");
    } else {
        ag_print("gt    no\n");
    }
    if (stop > 1) {
        ag_print("gt_s  yes\n");
    } else {
        ag_print("gt_s  no\n");
    }

    /* A shift by more than fifteen, which SRLI cannot encode - the reason the
     * constant case goes through SAR like the register one. */
    ag_printf("shr20 %08x\n", top >> 20);
    return 0;
}
