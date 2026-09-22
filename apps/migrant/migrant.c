/*
 * ArgonOS - a program that can be moved to the other board while it runs.
 *
 * The smallest honest demonstration of migration level 1, and the reference
 * for the shape every migrating program has to take.  It counts, slowly, out
 * loud.  Move it with `migrate <pid>` and it goes on counting on the other
 * board FROM WHERE IT WAS - which is the only claim worth making, and the one
 * a number in the output can prove.
 *
 *   run /b t:\migrant.axe        start it counting
 *   ps                           find its pid
 *   migrate 1                    ask it to move
 *
 * Three things here are the whole contract, and all three are the program's
 * own doing rather than the system's:
 *
 *   1. it says it can be moved (AG_AXE_CAN_MIGRATE), which is a promise;
 *   2. it notices the request WHERE IT IS SAFE TO - between counts, not in the
 *      middle of one - and hands over exactly what it wants carried;
 *   3. it REOPENS what it needs on the far side.  This one needs only the
 *      console, which it gets by being a process at all; a program holding a
 *      file or a device would open it again here, and that is precisely the
 *      part nothing else could do for it: those are different objects on the
 *      other board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

/*
 * EIGHT KILOBYTES, NAMED RATHER THAN TAKEN FROM THE DEFAULT, and the number is
 * the loader's floor rather than this program's need.
 *
 * MIGRANT keeps three words of state and prints a number; its own frames are
 * shallow.  What is not shallow is the load: the image is read, relocated and
 * started on this very task, and AG_PROC_LOAD_STACK_MIN (8 KB) is the kernel's
 * statement of what that needs.
 *
 * The default it used to take is 16 KB (CONFIG_ARGON_APP_STACK_KB, which no
 * board overrides), and on the C3 that is the whole standby shape: measured on
 * COM8, one default-sized guest leaves 18 KB free and a 9 KB largest block, so
 * the MIGRANT that `link standby` sends over - always the NEIGHBOUR'S build of
 * this program, which the far board has to substitute for its own - cannot
 * start.  `link standby` then answers -2, and the handover silently does not
 * happen.  Two 8 KB stacks fit where a 16 KB one leaves no room for a second.
 */
AG_APP_SIZED("MIGRANT", "1.0", "argon", AG_AXE_CAN_MIGRATE, 8 * 1024, 0);

/*
 * What travels.  A version stamp in front of it because the two ends are two
 * copies of the same image today and need not be tomorrow - and a struct that
 * quietly changed shape between them would be read as nonsense rather than
 * refused.
 */
typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t hops; /* how many times this program has changed boards */
} state_t;

#define MIGRANT_VERSION 1u

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    state_t s = {MIGRANT_VERSION, 0, 0};

    const int32_t got = ag_migrate_restore(&s, sizeof(s));
    if (got < 0) {
        ag_printf("migrant: the state that arrived is not mine (%d)\n",
                  (int)got);
        return 1;
    }
    if (got == 0) {
        ag_print("migrant: starting fresh at 0\n");
    } else if (s.version != MIGRANT_VERSION) {
        ag_printf("migrant: state version %u, I speak %u\n",
                  (unsigned)s.version, (unsigned)MIGRANT_VERSION);
        return 1;
    } else {
        s.hops++;
        ag_printf("migrant: carried on from %u after %u hop%s\n",
                  (unsigned)s.count, (unsigned)s.hops,
                  (s.hops == 1) ? "" : "s");
    }

    /*
     * Here is where a program that held a file or a device would open it
     * again.  This one holds nothing but the console, and says so rather than
     * leaving the reader to wonder whether it forgot.
     */

    for (;;) {
        s.count++;
        ag_printf("migrant: %u\n", (unsigned)s.count);

        /*
         * The safe point, and it is between counts on purpose: the state is
         * consistent here and would not be halfway through the line above.
         * Noticing at a place of its own choosing is the whole reason this is
         * consent rather than seizure.
         */
        if (ag_interrupted()) {
            if (ag_migrate_wanted()) {
                const ag_err_t e = ag_migrate_save(&s, sizeof(s));
                if (e != AG_OK) {
                    ag_printf("migrant: cannot hand over my state: %d\n",
                              (int)e);
                    return 1;
                }
                ag_printf("migrant: handing over at %u\n", (unsigned)s.count);
            } else {
                ag_print("migrant: asked to stop\n");
            }
            return 0;
        }

        ag_delay(500);
    }
}
