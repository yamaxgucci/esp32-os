/*
 * ArgonOS - what a lookup table costs when it lives in flash.
 *
 *   xipbench <table.bin> [thousands of lookups]
 *
 * One number, and a decision rests on it.
 *
 * An emulator like SMS Plus GX buys speed with memory: 736 KB of tables that are
 * computed once at start and then only read.  On a board with no PSRAM that is
 * not a trade, it is a wall - but the tables are read-only, and this system can
 * put read-only bytes in flash and map them (fs->map stages a file into appfs and
 * mmaps it, so the same cache that fetches instructions fetches the data).  In
 * memory the tables then cost nothing at all.
 *
 * The catch is what kind of reads they are.  `bp_lut` is indexed by the tile word
 * being decoded and the Z80 flag tables by the operand pair, so the access is
 * data-dependent and spread over the whole table - the worst thing to hand a
 * cache.  Moving the tables to flash might turn "does not fit" into "does not
 * keep up", and nobody should edit a third-party emulator to find that out.
 *
 * Four walks, and every one of them is here to rule something out:
 *
 *   SRAM random      the baseline: what a lookup costs when the table is where
 *                    the emulator wants it.
 *   flash random     the question.
 *   flash sequential the cache control.  A working cache makes this far cheaper
 *                    than random; if the two match, the region is simply
 *                    uncached and the random figure is not about caches at all.
 *   flash, small     random again, over a region small enough to stay resident.
 *                    Cheap here and expensive above means the cache works and
 *                    the table is what does not fit in it - which is exactly the
 *                    claim being tested.
 *
 * Time is measured in microseconds and not in cycles, and that is not a
 * preference.  ag_cycles is the core's own 32-bit counter and wraps every 2^32 -
 * twenty-seven seconds at 160 MHz - so a walk that straddles the wrap reports a
 * difference that underflows.  The first cut of this program used it and
 * returned, across three runs of the same code, 639 then 17980 then 197416
 * cycles for the same lookup.  ag_micros is 64-bit and does not do that.
 *
 * Build:
 *   argon apps --only XIPBENCH.AXE
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("XIPBENCH", "1.1", "argon", 0);

/*
 * The SRAM side, in entries.  32 KB of uint32, and the size does not matter to
 * the answer: internal SRAM has no cache in front of it, so a lookup costs the
 * same whether the table is four kilobytes or four hundred.  Only the flash side
 * cares about size, and it gets the whole file.
 *
 * What does set this number is the arena.  A RISC-V .AXE is one contiguous block
 * - the code reaches its data PC-relatively, so the two cannot be separated - and
 * the whole image has to fit the executable arena, 64 KB here with a driver
 * already in it.  64 KB of table made a 68 KB image and the loader said so:
 * `will not fit the arena (54336 free)`.
 *
 * Static rather than allocated, and that is not laziness either.  An .AXE gets
 * the heap its header asks for and no more, and the default is none at all -
 * right for images that never call malloc, and something this program found out
 * the direct way (`no 64 KB for the SRAM side`, with 90 KB free).  Bss in the
 * data part is the same memory, placed by the loader, without a number in the
 * header to keep in step with this constant.
 */
#define RAM_WORDS (8u * 1024u)

static uint32_t s_ram[RAM_WORDS];

/* The resident-region control: 4 KB, which any cache worth the name holds. */
#define SMALL_WORDS 1024u

/* Digits, by hand: the SDK has no atoi and one argument is not a reason to want
 * one. */
static uint32_t number(const char *s)
{
    uint32_t v = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/*
 * One multiplier, one addend, and the same sequence on every side.
 *
 * It has to be the *same* walk or the numbers are not comparable, and it has to
 * be cheap or the generator is what is being timed rather than the memory.  A
 * 32-bit LCG is three instructions and its high bits are the usable ones - hence
 * the shift when it is turned into an index.
 */
static uint32_t lcg(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

/*
 * The timed loops return the checksum for a reason: with nothing consuming the
 * loads the compiler is free to delete the walk, and an empty loop measures
 * beautifully.
 */
static uint32_t walk_random(const uint32_t *tab, uint32_t mask, uint32_t iters,
                            uint64_t *us)
{
    uint32_t        state = 0x12345678u;
    uint32_t        sum = 0;
    const ag_time_t t0 = ag_micros();

    for (uint32_t i = 0; i < iters; i++) {
        sum += tab[(lcg(&state) >> 8) & mask];
    }

    *us = (uint64_t)(ag_micros() - t0);
    return sum;
}

static uint32_t walk_seq(const uint32_t *tab, uint32_t mask, uint32_t iters,
                         uint64_t *us)
{
    uint32_t        sum = 0;
    const ag_time_t t0 = ag_micros();

    for (uint32_t i = 0; i < iters; i++) {
        sum += tab[i & mask];
    }

    *us = (uint64_t)(ag_micros() - t0);
    return sum;
}

/* The generator and the loop, with no table at all: what to subtract. */
static uint32_t walk_nothing(uint32_t iters, uint64_t *us)
{
    uint32_t        state = 0x12345678u;
    uint32_t        sum = 0;
    const ag_time_t t0 = ag_micros();

    for (uint32_t i = 0; i < iters; i++) {
        sum += lcg(&state) >> 8;
    }

    *us = (uint64_t)(ag_micros() - t0);
    return sum;
}

/*
 * Tenths of a nanosecond per lookup, and everything downstream is 32-bit.
 *
 * Picoseconds were the first attempt and they printed `4294967295.4294967295`:
 * a lookup that costs two microseconds is two million picoseconds, the numbers
 * stopped fitting the casts on the way into ag_printf, and the derived columns
 * became noise while the raw microseconds beside them were right.  Tenths of a
 * nanosecond hold a two-microsecond lookup in 19792, which fits everything.
 */
static uint32_t ns10_each(uint64_t us, uint32_t iters)
{
    return (uint32_t)((us * 10000u) / iters);
}

/*
 * Gross, not net, and that is a correction rather than a simplification.
 *
 * The first version timed the generator alone and subtracted it, which is the
 * textbook thing to do and was wrong here: the loop-only run came back *slower*
 * than the same loop with an SRAM load in it.  Two hundred thousand iterations
 * is twenty-odd milliseconds and the system around them - the tick, the panel's
 * text path pushing pixels over SPI - is worth more than that difference, so the
 * subtraction was subtracting noise and turning small numbers into zero.  Every
 * row runs the identical loop, so the gross figures compare cleanly and the
 * loop's own cost is visible as the `loop only` row rather than hidden in a
 * correction.
 *
 * One short line per row, because the console here is forty columns by
 * twenty-one rows and a report that scrolls its own first half away is a report
 * that has to be run twice.
 */
static void report(const char *what, uint64_t us, uint32_t iters)
{
    const uint32_t ns10 = ns10_each(us, iters);

    ag_printf("%-10s %7u %7u %5u.%01u\n", what, (unsigned)iters, (unsigned)us,
              (unsigned)(ns10 / 10u), (unsigned)(ns10 % 10u));
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        ag_printf("usage: xipbench <table.bin> [thousands of lookups]\n");
        return 1;
    }

    uint32_t thousands = 200;
    if (argc >= 3) {
        thousands = number(argv[2]);
        if (thousands == 0u) {
            thousands = 1u;
        }
    }
    const uint32_t fast = thousands * 1000u;

    const void *mapped = NULL;
    uint64_t    len = 0;
    ag_err_t    err = ag_map(argv[1], &mapped, &len);
    if (err != AG_OK) {
        ag_printf("xipbench: %s: map failed (%d)\n", argv[1], (int)err);
        return 1;
    }

    /* The mask needs a power of two, and rounding down is the honest way: a
     * table half the size is a table half the size, and it is said out loud. */
    const uint32_t words = (uint32_t)(len / 4u);
    uint32_t       fmask = 1u;
    while ((fmask * 2u) <= words) {
        fmask *= 2u;
    }
    fmask -= 1u;

    for (uint32_t i = 0; i < RAM_WORDS; i++) {
        s_ram[i] = i * 2654435761u;
    }

    ag_printf("xipbench: %s mapped at %p, %u KB, walking %u KB of it\n",
              argv[1], mapped, (unsigned)(len / 1024u),
              (unsigned)(((fmask + 1u) * 4u) / 1024u));

    uint64_t u_none = 0, u_ram = 0, u_ramseq = 0;
    uint64_t u_fl = 0, u_flseq = 0, u_flsmall = 0;
    uint32_t s = 0;

    s += walk_nothing(fast, &u_none);
    s += walk_random(s_ram, RAM_WORDS - 1u, fast, &u_ram);
    s += walk_seq(s_ram, RAM_WORDS - 1u, fast, &u_ramseq);

    /* The slow side gets a tenth of the iterations.  Not for the wrap any more -
     * microseconds do not wrap - but because a run nobody waits for is a run
     * nobody repeats. */
    const uint32_t slow = (fast / 10u < 20000u) ? 20000u : fast / 10u;
    s += walk_random((const uint32_t *)mapped, fmask, slow, &u_fl);
    s += walk_seq((const uint32_t *)mapped, fmask, slow, &u_flseq);
    s += walk_random((const uint32_t *)mapped, SMALL_WORDS - 1u, fast,
                     &u_flsmall);

    ag_printf("what        lookups      us   ns each\n");
    report("loop only", u_none, fast);
    report("sram rnd", u_ram, fast);
    report("sram seq", u_ramseq, fast);
    report("flash rnd", u_fl, slow);
    report("flash seq", u_flseq, slow);
    report("flash 4K", u_flsmall, fast);

    /*
     * Three ratios, each ruling something out.  Whole numbers and hundredths:
     * the raw columns above are there for whoever wants to redo the division.
     */
    const uint32_t p_ram = ns10_each(u_ram, fast);
    const uint32_t p_fl = ns10_each(u_fl, slow);
    const uint32_t p_sm = ns10_each(u_flsmall, fast);
    const uint32_t p_sq = ns10_each(u_flseq, slow);

    ag_printf("\nflash rnd / sram rnd  %ux <- the question\n",
              (unsigned)(p_fl / (p_ram ? p_ram : 1u)));
    ag_printf("flash rnd / flash seq %ux 1x=uncached\n",
              (unsigned)(p_fl / (p_sq ? p_sq : 1u)));
    ag_printf("flash rnd / flash 4K  %ux 1x=not the cache\n",
              (unsigned)(p_fl / (p_sm ? p_sm : 1u)));
    ag_printf("sum %u\n", (unsigned)s);

    (void)ag_unmap(mapped);
    return 0;
}
