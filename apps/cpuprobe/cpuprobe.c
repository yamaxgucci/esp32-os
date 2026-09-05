/*
 * ArgonOS - a stopwatch on the core itself.
 *
 * It does the same arithmetic over and over and says how long each round
 * took.  Same work every second, so any change in the number is the machine
 * underneath changing, not the program.
 *
 * It exists because "the core is free" and "the core is fast" turned out to be
 * different claims, and a day was spent confusing them: while REMDISP puts a
 * whole frame on the wire, Fallout's frame grows from 20 ms to 700, its own
 * core is provably free - a thread of priority twelve on it wakes within
 * milliseconds - and the driver's own doors account for only 39 ms of the 700.
 * Either the instructions are running slower, or the game is waiting on
 * something nobody has timed yet.  This tells the two apart with no game in
 * the room.
 *
 *   run a:@cpuprobe.axe        (foreground, so it lands on the application core)
 */
#include <argon/argon.h>

AG_APP("CPUPROBE", "1.0", "argon", 0);

static uint32_t cell[256];

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_print("cpuprobe: same work every second; watch the microseconds\n");

    uint32_t worst_us = 0, worst_sleep = 0, worst_busy = 0;

    for (uint32_t round = 0;; round++) {
        const uint64_t t0 = (uint64_t)ag_api()->time->us();
        uint32_t       acc = 1u;
        for (uint32_t pass = 0; pass < 200u; pass++) {
            for (uint32_t i = 0; i < 256u; i++) {
                acc = acc * 1664525u + 1013904223u;
                cell[i] += acc;
            }
        }
        const uint32_t us = (uint32_t)((uint64_t)ag_api()->time->us() - t0);

        /*
         * And a sleep of a known length, timed the same way.  Slow arithmetic
         * and a late wake-up are different illnesses: the first is the bus or
         * the cache, the second is the scheduler, and printing both on one
         * line means never having to guess which of them a number is.
         */
        /*
         * Two hundred milliseconds of waiting, done two different ways.
         *
         * The first spends them awake, watching the hardware counter and
         * yielding to nobody: if that stretches, this core is being taken
         * away.  The second asks the system to wake us and goes to sleep: if
         * only that one stretches, the core was ours all along and it is the
         * waking that is late - a tick that did not arrive rather than a
         * thread that did not run.  One number cannot tell those apart, which
         * is why there are two.
         */
        const uint64_t b0 = (uint64_t)ag_api()->time->us();
        while ((uint64_t)ag_api()->time->us() - b0 < 100000ull) {
        }
        const uint32_t busy = (uint32_t)((uint64_t)ag_api()->time->us() - b0);

        const uint64_t s0 = (uint64_t)ag_api()->time->us();
        ag_delay(100);
        const uint32_t slept = (uint32_t)((uint64_t)ag_api()->time->us() - s0);

        /*
         * Printed once in twenty, not every round.  This console is mirrored
         * down the same wire whose traffic is under suspicion, so a program
         * that prints every round is disturbing the system it measures.  If
         * the stalls survive two seconds of quiet, they have nothing to do
         * with anything this program says out loud.
         */
        if (us > worst_us) {
            worst_us = us;
        }
        if (slept > worst_sleep) {
            worst_sleep = slept;
        }
        if (busy > worst_busy) {
            worst_busy = busy;
        }
        if ((round % 20u) == 19u) {
            ag_printf("[cpu %u] worst of 20: work %u us (floor 2168), "
                      "100 ms awake %u us, 100 ms asleep %u us\n",
                      (unsigned)round, (unsigned)worst_us,
                      (unsigned)worst_busy, (unsigned)worst_sleep);
            worst_us = 0;
            worst_sleep = 0;
            worst_busy = 0;
        }
    }
    return 0;
}
