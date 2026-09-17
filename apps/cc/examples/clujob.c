/*
 * CLUJOB - one program on the cores of every board, written in the compiler
 * that lives on the board.
 *
 * This is GRIND's little brother (apps/grind/grind.c) in the Mini-C dialect,
 * and it exists to prove one claim: a job for the whole cluster can be WRITTEN
 * WHERE IT IS TO RUN.  Until the cluster calls were in this compiler's builtin
 * table, anything that used more than one board had to be cross-compiled on a
 * PC and carried over - which makes a cluster of boards a thing you operate
 * from a desk rather than a thing that can be programmed from inside.
 *
 * It does the same work twice: once on this core alone, then cut into blocks
 * and handed to a thread on every node.  The two sums must be the same number.
 * A speed-up with a different answer is not a speed-up, and at these sizes
 * there is no speed-up to claim anyway - the blocks are small on purpose so
 * that the emulator finishes in seconds.  What is being shown is that the work
 * left, was done somewhere else, and came back right.
 *
 * Build and run it on the board:
 *   run t:\cc.axe t:\clujob.c t:\clujob.axe
 *   run t:\clujob.axe
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */

#pragma app "CLUJOB" "1.0" "argon"

typedef unsigned int u32;

#define BLOCKS 6
#define ITERS  1000000
#define MAXT   4

/*
 * ag_clu_entry_t, word by word: a name and a code address.  This compiler has
 * no function-pointer type and does not need one here - `&worker` is an
 * ordinary word, the same way a .SYS built here fills ag_dev_ops_t.
 */
struct entry {
    int name;
    int fn;
};

/* One block of work, master to worker.  iters == 0 means "come home". */
struct work {
    u32 block;
    u32 iters;
};

/* And the answer back. */
struct done {
    u32 block;
    u32 sum;
};

/*
 * Globals, because this compiler wants them before the first function and
 * because the entry table must outlive the call that registers it - the
 * kernel keeps the pointer for as long as the job lasts.
 */
struct entry g_tab[1];
int          g_tid[MAXT];

/*
 * The work: an integer mixer, deterministic, that touches no memory.  Not
 * floating point on purpose - the boards in this cluster do not have the same
 * floating-point hardware, and "both nodes agree" would then mean nothing.
 */
u32 grind_block(u32 seed, u32 iters)
{
    u32 x;
    u32 i;

    x = seed | 1;
    i = 0;
    while (i < iters) {
        x = x ^ (x << 13);
        x = x ^ (x >> 17);
        x = x ^ (x << 5);
        x = x * 0x9e3779b1 + i;
        i = i + 1;
    }
    return x;
}

/*
 * A thread of this job, wherever it woke up.  It prints nothing: on the far
 * node its console is somebody else's screen.
 */
void worker(int self, char *arg, int len)
{
    struct work w;
    struct done d;
    int from;
    int n;

    for (;;) {
        from = 0;
        n = ag_clu_recv(&from, &w, sizeof(w), 30000);
        /*
         * A signed comparison, and it is signed because sizeof here yields an
         * int rather than C's size_t.  The same line in apps/grind/grind.c has
         * to say `(int32_t)sizeof(w)` or a negative answer - a timeout - would
         * be promoted to an enormous unsigned number and read as a message
         * that arrived.
         */
        if (n < sizeof(w)) {
            /*
             * Nothing came.  Going home is the right answer: the master may
             * have been stopped, and a worker waiting for ever for a job that
             * has ended is a process alive on somebody else's board with
             * nothing left to end it.
             */
            return;
        }
        if (w.iters == 0) {
            return;
        }
        d.block = w.block;
        d.sum = grind_block(w.block * 0x9e3779b1 + 1, w.iters);
        if (ag_clu_send(0, &d, sizeof(d), 10000) != 0) {
            return;
        }
    }
}

int ag_main(void)
{
    struct work w;
    struct done d;
    u32 sum_here;
    u32 sum_out;
    int nn;
    int nt;
    int i;
    int t;
    int next;
    int got;
    int busy;
    int from;
    int n;

    g_tab[0].name = "clujob";
    g_tab[0].fn = &worker;

    /*
     * Before anything is printed: this copy may BE a worker, started by the
     * node next door.  ag_clu_start runs that share to its end and answers
     * true, and the only right thing then is to leave quietly - anything
     * printed here appears on the other board's console out of the blue.
     */
    if (ag_clu_start(&g_tab[0], 1)) {
        return 0;
    }

    /* The same work, here, so that there is something to compare against. */
    sum_here = 0;
    i = 0;
    while (i < BLOCKS) {
        sum_here = sum_here + grind_block(i * 0x9e3779b1 + 1, ITERS);
        i = i + 1;
    }
    ag_printf("clujob: here   %d blocks, sum %08x\n", BLOCKS, sum_here);

    /* NULL asks how many there are; one node is the answer on a board that is
     * alone, and that is not an error - the job simply runs here. */
    nn = ag_clu_nodes(0, 0);
    if (nn < 1) {
        ag_printf("clujob: no cluster: %d\n", nn);
        ag_print("clujob: failed\n");
        return 1;
    }
    ag_printf("clujob: %d node(s)\n", nn);

    /*
     * One thread per node rather than per core: this compiler cannot read
     * ag_clu_node_t (a struct with a char array in it) yet, so it asks for the
     * count and not the table.  A node that refuses is said out loud - a
     * refusal swallowed here is the hardest thing to find later.
     */
    nt = 0;
    i = 0;
    while (i < nn && nt < MAXT) {
        t = ag_clu_spawn(i, "clujob", 0, 0);
        if (t < 0) {
            ag_printf("clujob: node %d took no thread: %d\n", i, t);
        } else {
            g_tid[nt] = t;
            nt = nt + 1;
        }
        i = i + 1;
    }
    if (nt == 0) {
        ag_print("clujob: nowhere to run anything\n");
        ag_print("clujob: failed\n");
        return 1;
    }
    ag_printf("clujob: %d thread(s)\n", nt);

    /*
     * A queue and not a fixed share: the boards are not the same speed, and a
     * fixed share finishes at the pace of the slowest one.
     */
    next = 0;
    got = 0;
    busy = 0;
    sum_out = 0;

    i = 0;
    while (i < nt && next < BLOCKS) {
        w.block = next;
        w.iters = ITERS;
        if (ag_clu_send(g_tid[i], &w, sizeof(w), 10000) == 0) {
            next = next + 1;
            busy = busy + 1;
        }
        i = i + 1;
    }

    while (got < BLOCKS && busy > 0) {
        from = 0;
        n = ag_clu_recv(&from, &d, sizeof(d), 60000);
        if (n < sizeof(d)) {
            ag_printf("clujob: lost a block: %d\n", n);
            busy = 0;
        } else {
            sum_out = sum_out + d.sum;
            got = got + 1;
            if (next < BLOCKS) {
                w.block = next;
                w.iters = ITERS;
                if (ag_clu_send(from, &w, sizeof(w), 10000) == 0) {
                    next = next + 1;
                } else {
                    busy = busy - 1;
                }
            } else {
                w.block = 0;
                w.iters = 0;
                n = ag_clu_send(from, &w, sizeof(w), 2000);
                busy = busy - 1;
            }
        }
    }

    i = 0;
    while (i < nt) {
        n = ag_clu_join(g_tid[i], 3000);
        i = i + 1;
    }
    ag_clu_done();

    ag_printf("clujob: spread %d blocks, sum %08x\n", got, sum_out);

    /*
     * The whole correctness claim: the work was cut up, sent over a wire, done
     * somewhere else and brought back, and it is the same number.
     */
    if (got != BLOCKS || sum_out != sum_here) {
        ag_print("clujob: MISMATCH\n");
        ag_print("clujob: failed\n");
        return 1;
    }
    ag_print("clujob: answers agree\n");

    /*
     * Printed on every path out of here, good and bad alike: a harness that
     * waits for a line only the good path prints does not fail when something
     * goes wrong - it hangs, and reports a timeout somewhere unrelated.  This
     * tree has done exactly that twice.
     */
    ag_print("clujob: done\n");
    return 0;
}
