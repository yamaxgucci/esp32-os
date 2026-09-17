/*
 * MIGRANT, written in the compiler that lives on the board.
 *
 * The same program as apps/migrant/migrant.c - it counts, and when a board
 * asks it to move it hands over its count and carries on wherever it wakes up
 * next.  The difference is where it is built: this one can be compiled BY THE
 * BOARD, which is the second rung of the cascade in docs/plans/cluster.md 3b.
 *
 * Why it had to be possible before that rung means anything: a far node that
 * cannot execute the image it was sent looks for its own build of the same
 * PROGRAM, by the name in the header.  Every image this compiler produced was
 * called "TCC" until now, so two boards that had each compiled their own copy
 * would both answer to the same name and neither could be told from anything
 * else compiled here.  `#pragma app` is what fixes that, and this file is what
 * proves it.
 *
 * Build it on the guest:
 *   run t:\cc.axe t:\migrant.c t:\migrant.axe
 *   run /b t:\migrant.axe
 *   migrate 1
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */

#pragma app "MIGRANT" "1.0" "argon"
#pragma migrate

/*
 * What travels, and it is a struct rather than three loose globals because
 * ag_migrate_save takes an address and a length: one object, one size, and no
 * way for the two ends to disagree about the order of the fields.
 *
 * A version in front of it for the same reason the C version has one - the two
 * ends are two copies of the same program today and need not be tomorrow, and
 * a struct that quietly changed shape would be read as nonsense rather than
 * refused.
 */
struct state {
    int version;
    int count;
    int hops; /* how many times this program has changed boards */
};

int ag_main(void)
{
    struct state s;
    s.version = 1;
    s.count = 0;
    s.hops = 0;

    /*
     * 0 means nothing arrived: a fresh start, which is what happens the first
     * time and after a board that would not take the handover.  Anything else
     * is our own state, saved by the copy of us that used to be somewhere
     * else.
     */
    int got = ag_migrate_restore(&s, sizeof(s));
    if (got < 0) {
        ag_print("migrant: the state that arrived is not mine\n");
        return 1;
    }
    if (got == 0) {
        s.version = 1;
        s.count = 0;
        s.hops = 0;
        ag_print("migrant: starting fresh at 0\n");
    } else {
        if (s.version != 1) {
            ag_print("migrant: that state is a different version of me\n");
            return 1;
        }
        s.hops = s.hops + 1;
        ag_printf("migrant: carried on from %d after %d hop(s)\n", s.count,
                  s.hops);
    }

    for (;;) {
        s.count = s.count + 1;
        ag_printf("migrant: %d\n", s.count);
        ag_delay(500);

        /*
         * Asked at a point of our own choosing, which is the whole of the
         * promise: the count is consistent here and the program is between
         * two pieces of work rather than inside one.
         */
        if (ag_migrate_wanted()) {
            ag_printf("migrant: handing over at %d\n", s.count);
            ag_migrate_save(&s, sizeof(s));
            return 0;
        }
    }
    return 0;
}
