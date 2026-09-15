/*
 * ArgonOS - one program on every core of every board.
 *
 * This is the demonstration of ag_clu_*, and it is also the measurement: it
 * does the same work twice, once on this core alone and once spread over every
 * core of every node, and prints both times and both answers.  Two numbers
 * rather than one, because "it ran on two boards" is not the claim anybody
 * cares about - the claim is that it finished sooner, and that it got the same
 * result.
 *
 * WHY THE WORK LOOKS LIKE THIS.  Each block is twelve bytes in and twelve bytes
 * back and costs millions of cycles in between, which is the shape that crosses
 * a wire profitably: a byte on the measured link is worth about 210 cycles of a
 * 240 MHz core, so anything that carries more than it computes would have been
 * cheaper at home.  A program whose blocks are small and cheap will measure
 * SLOWER here than alone, and that is not a defect of the mechanism - it is the
 * mechanism telling the truth about the wire.
 *
 *   grind                 the default: 16 blocks of 400 000 iterations
 *   grind 32 1000000      blocks, and iterations in each
 *   grind 32 1000000 -1   the same, on this board only (what it costs alone)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("GRIND", "1.0", "argon", 0);

/* The SDK has no atoi and does not want one - see the note in apps/link. */
static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(*s - '0');
        if (v > (0xffffffffu - digit) / 10u) {
            return false;
        }
        v = v * 10u + digit;
    }
    *out = v;
    return true;
}

/* What a worker is told when it starts: nothing but who to answer. */
typedef struct {
    int32_t master;
} start_t;

/* One block of work, master to worker.  count == 0 means "no more, come home". */
typedef struct {
    uint32_t block;
    uint32_t iters;
    uint32_t seed;
} work_t;

/* And back.  `ms` is what the block took where it ran, which is what makes a
 * slow node visible as a slow node rather than as a slow program. */
typedef struct {
    uint32_t block;
    uint32_t sum;
    uint32_t ms;
} done_t;

/*
 * The work itself: an integer mixer, deterministic, with no memory traffic to
 * speak of.  Deliberately not a floating-point benchmark - the two boards in
 * this cluster do not have the same floating-point hardware, and an answer that
 * depended on that would make "both nodes agree" meaningless.
 */
static uint32_t grind_block(uint32_t seed, uint32_t iters)
{
    uint32_t x = seed | 1u;
    for (uint32_t i = 0; i < iters; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x = x * 2654435761u + i;
    }
    return x;
}

static uint32_t now_ms(void) { return (uint32_t)(ag_micros() / 1000u); }

/* ---------------------------------------------------------------------- */
/* The worker half                                                         */
/* ---------------------------------------------------------------------- */

static void worker(ag_clu_tid_t self, const void *arg, uint32_t len)
{
    start_t s = { .master = AG_CLU_MASTER };
    if (arg != NULL && len >= sizeof(s)) {
        memcpy(&s, arg, sizeof(s));
    }

    for (;;) {
        work_t       w;
        ag_clu_tid_t from = 0;
        const int32_t n = ag_clu_recv(&from, &w, sizeof(w), 30000);
        if (n < (int32_t)sizeof(w)) {
            /*
             * Nothing came.  Going home is the right answer and not a
             * surrender: the master may have been stopped, and a worker that
             * waits for ever for a job that has ended is a process alive on
             * somebody else's board with nothing to end it.
             */
            return;
        }
        if (w.iters == 0) {
            return;
        }

        const uint32_t t0 = now_ms();
        done_t d = { .block = w.block,
                     .sum = grind_block(w.seed, w.iters),
                     .ms = 0 };
        d.ms = now_ms() - t0;
        (void)self;
        if (ag_clu_send(s.master, &d, sizeof(d), 10000) != AG_OK) {
            return;
        }
    }
}

static const ag_clu_entry_t k_entries[] = {
    { "grind", worker },
};

/* ---------------------------------------------------------------------- */
/* The master half                                                         */
/* ---------------------------------------------------------------------- */

#define GRIND_MAX_THREADS 8

int ag_main(int argc, char **argv)
{
    /*
     * First, and before anything is printed: this copy may BE a worker, started
     * by the node next door.  start() runs its share and returns true, and the
     * only right thing to do then is leave quietly - anything printed here goes
     * to the other board's console, out of the blue.
     */
    if (ag_clu_start(k_entries,
                     (uint32_t)(sizeof(k_entries) / sizeof(k_entries[0])))) {
        return 0;
    }

    uint32_t blocks = 16;
    uint32_t iters = 400000;
    bool     alone = false;

    if (argc > 1 && argv[1][0] != '-' && !parse_u32(argv[1], &blocks)) {
        blocks = 0;
    }
    if (argc > 2 && argv[2][0] != '-' && !parse_u32(argv[2], &iters)) {
        iters = 0;
    }
    for (int i = 1; i < argc; i++) {
        if (ag_strcmp(argv[i], "-1") == 0 ||
            ag_strcmp(argv[i], "-local") == 0) {
            alone = true;
        }
    }
    if (blocks == 0 || blocks > 4096 || iters == 0) {
        ag_print("usage: grind [blocks] [iterations] [-1]\n");
        ag_print("grind: failed\n");
        return 1;
    }

    ag_clu_node_t nodes[4];
    const int32_t nn = ag_clu_nodes(nodes, 4);
    if (nn < 1) {
        ag_printf("no cluster: %d\n", (int)nn);
        ag_print("grind: failed\n");
        return 1;
    }

    uint32_t cores = 0;
    ag_printf("nodes:\n");
    for (int32_t i = 0; i < nn && i < 4; i++) {
        ag_printf("  %-8s %u core%s %s\n", nodes[i].name,
                  (unsigned)nodes[i].cores, (nodes[i].cores == 1) ? "" : "s",
                  nodes[i].here ? "(here)" : "");
        cores += nodes[i].cores;
    }

    /* ---- the same work, alone, for something to compare against ---- */
    const uint32_t t_alone0 = now_ms();
    uint32_t       sum_alone = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        sum_alone += grind_block(b * 2654435761u + 1u, iters);
    }
    const uint32_t ms_alone = now_ms() - t_alone0;
    ag_printf("\none core here: %u blocks in %u ms, sum %08x\n",
              (unsigned)blocks, (unsigned)ms_alone, (unsigned)sum_alone);

    if (alone) {
        ag_print("grind: done\n");
        return 0;
    }

    /* ---- and now on everything there is ---- */
    ag_clu_tid_t tid[GRIND_MAX_THREADS];
    uint32_t     on_node[4] = { 0, 0, 0, 0 };
    uint32_t     threads = 0;
    start_t      s = { .master = AG_CLU_MASTER };

    const uint32_t t_all0 = now_ms();
    for (int32_t i = 0; i < nn && threads < GRIND_MAX_THREADS; i++) {
        for (uint8_t c = 0; c < nodes[i].cores && threads < GRIND_MAX_THREADS;
             c++) {
            const ag_clu_tid_t t = ag_clu_spawn(i, "grind", &s, sizeof(s));
            if (t < 0) {
                /*
                 * Said, not swallowed.  A node that would not take a thread is
                 * the single most useful thing to know when the answer comes
                 * back slower than expected.
                 */
                ag_printf("  %s took no thread: %d\n", nodes[i].name, (int)t);
                break;
            }
            tid[threads++] = t;
            if (i < 4) {
                on_node[i]++;
            }
        }
    }
    if (threads == 0) {
        ag_print("nowhere to run anything\n");
        ag_print("grind: failed\n");
        return 1;
    }
    /*
     * What starting them cost, on its own.
     *
     * Reported because it is most of the answer and is otherwise invisible: the
     * first thread on the far board means loading this image over the wire a
     * few hundred bytes at a time, and every spawn after it waits for the far
     * node to say yes.  A reader who sees only a total cannot tell work that
     * was too slow from work that never had a chance.
     */
    const uint32_t ms_start = now_ms() - t_all0;
    ag_printf("%u thread%s on %d node%s (starting them took %u ms)\n",
              (unsigned)threads, (threads == 1) ? "" : "s", (int)nn,
              (nn == 1) ? "" : "s", (unsigned)ms_start);

    /*
     * Hand out one block to each thread, then one more each time an answer
     * comes back.  A queue rather than a fixed share, because the nodes are not
     * the same speed and a fixed share finishes at the pace of the slowest.
     */
    uint32_t next = 0;
    uint32_t got = 0;
    uint32_t sum_all = 0;
    uint32_t busy = 0;

    for (uint32_t i = 0; i < threads && next < blocks; i++) {
        work_t w = { .block = next,
                     .iters = iters,
                     .seed = next * 2654435761u + 1u };
        if (ag_clu_send(tid[i], &w, sizeof(w), 10000) == AG_OK) {
            next++;
            busy++;
        }
    }

    while (got < blocks && busy > 0) {
        done_t       d;
        ag_clu_tid_t from = 0;
        const int32_t n = ag_clu_recv(&from, &d, sizeof(d), 60000);
        if (n < (int32_t)sizeof(d)) {
            ag_printf("lost a block: %d\n", (int)n);
            break;
        }
        sum_all += d.sum;
        got++;

        if (next < blocks) {
            work_t w = { .block = next,
                         .iters = iters,
                         .seed = next * 2654435761u + 1u };
            if (ag_clu_send(from, &w, sizeof(w), 10000) == AG_OK) {
                next++;
            } else {
                busy--;
            }
        } else {
            work_t stop = { .block = 0, .iters = 0, .seed = 0 };
            (void)ag_clu_send(from, &stop, sizeof(stop), 2000);
            busy--;
        }
    }
    const uint32_t ms_all = now_ms() - t_all0;

    for (uint32_t i = 0; i < threads; i++) {
        (void)ag_clu_join(tid[i], 3000);
    }
    ag_clu_done();

    /* ---- what happened ---- */
    ag_printf("all cores:     %u blocks in %u ms, sum %08x\n", (unsigned)got,
              (unsigned)ms_all, (unsigned)sum_all);
    for (int32_t i = 0; i < nn && i < 4; i++) {
        ag_printf("  %-8s %u thread%s\n", nodes[i].name, (unsigned)on_node[i],
                  (on_node[i] == 1) ? "" : "s");
    }

    /*
     * The two answers must be the same number.  This is the whole correctness
     * claim of the mechanism - the work was cut up, sent over a wire, done
     * somewhere else and brought back - and a speed-up with a different answer
     * is not a speed-up.
     */
    if (got != blocks || sum_all != sum_alone) {
        ag_printf("MISMATCH: %08x here, %08x spread out\n",
                  (unsigned)sum_alone, (unsigned)sum_all);
        ag_print("grind: failed\n");
        return 1;
    }
    ag_print("answers agree\n");

    /*
     * The verdict, and it refuses to be a number when a number would lie.
     *
     * A ratio is only about the cluster once the work is large against what it
     * cost to start: with eight milliseconds of work and half a second of
     * start-up, "0.0x" is arithmetic about the start-up and says nothing
     * whatever about how the boards divide work.  Printing it anyway would be
     * the kind of measurement this project keeps having to un-believe, so the
     * small case gets a sentence instead - and that sentence is the whole
     * lesson: the piece was too small to pay for the journey.
     */
    if (ms_all > 0 && ms_alone > ms_start * 2u) {
        /* One decimal, in integers: this system has no printf for floats and
         * "1.8x" says more than "180 percent". */
        const uint32_t x10 = (ms_alone * 10u) / ms_all;
        ag_printf("speedup %u.%ux on %u core%s\n", (unsigned)(x10 / 10u),
                  (unsigned)(x10 % 10u), (unsigned)cores,
                  (cores == 1) ? "" : "s");
    } else {
        ag_printf("no speedup to report: %u ms of work against %u ms of "
                  "starting up.  The pieces were too small to pay for the "
                  "journey - ask for more iterations.\n",
                  (unsigned)ms_alone, (unsigned)ms_start);
    }

    /*
     * Printed on EVERY path out of here, success and failure alike, and that is
     * the point: a harness that waits for a line only the good path prints does
     * not fail when something goes wrong - it hangs, and then reports a timeout
     * somewhere unrelated.  This tree has done exactly that twice.
     */
    ag_print("grind: done\n");
    return 0;
}
