/*
 * ArgonOS - a clock with no hands, for one question only.
 *
 * It prints a line every hundred milliseconds and says how late that line is.
 * Nothing else.  Run it in a second slot beside something heavy and the gaps
 * in its output are the gaps in which the system was not serving applications.
 *
 * Written because REMDISP sending a whole frame takes the game from 40 frames
 * a second to 1.6, and every explanation from the driver's side has been
 * measured and killed: not PSRAM, not the buffer's memory, not the size of
 * each write, not the core the interrupt lands on, not the flash cache, not
 * the interrupt rate.  A sleep of the same length in place of the write is
 * free, which says the cost is the transmitter running rather than the task
 * being busy.  Maxim's reading of that is that the system itself stops
 * serving applications while the wire is going, and this is the instrument
 * that agrees or disagrees with him: the game is complicated and might be
 * waiting on its own account, but this thing waits on nothing.
 *
 *   python tools/mkaxe.py --include sdk/include -o TICKER.AXE \
 *       apps/ticker/ticker.c
 *   run a:@ticker.axe          (in a second slot, while the first one works)
 */
#include <argon/argon.h>

AG_APP("TICKER", "1.0", "argon", 0);

int ag_main(int argc, char **argv)
{
    uint32_t ms = 100u;
    if (argc > 1) {
        ms = 0;
        for (const char *d = argv[1]; *d >= '0' && *d <= '9'; d++) {
            ms = ms * 10u + (uint32_t)(*d - '0');
        }
        if (ms == 0u) {
            ms = 100u;
        }
    }

    const uint64_t t0 = (uint64_t)ag_api()->time->us();
    uint64_t       due = t0;
    uint32_t       n = 0;
    uint32_t       worst = 0;
    uint32_t       pr = 0;   /* how long the last print itself took */

    for (;;) {
        due += (uint64_t)ms * 1000ull;
        const uint64_t now = (uint64_t)ag_api()->time->us();
        if (due > now) {
            ag_delay((uint32_t)((due - now) / 1000ull));
        }
        /*
         * Late by how much, measured against when this tick was due rather
         * than against the last one: a stall shows up once at its true size
         * instead of being spread over the ticks that follow it.
         */
        const uint64_t woke = (uint64_t)ag_api()->time->us();
        const uint32_t late = (woke > due) ? (uint32_t)((woke - due) / 1000ull) : 0u;
        if (late > worst) {
            worst = late;
        }
        n++;
        if ((n % 10u) == 0u) {
            /*
             * Time the printing as well as the sleeping.  They are the only
             * two things this program does, and they fail differently: a late
             * wake is the scheduler not running us, while a slow print is this
             * console being carried somewhere that is busy.  Reported apart,
             * because the answer is one or the other.
             */
            const uint64_t w0 = (uint64_t)ag_api()->time->us();
            ag_printf("[tick %u] late %u ms, worst %u, print %u ms\n",
                      (unsigned)n, (unsigned)late, (unsigned)worst,
                      (unsigned)(pr / 1000u));
            pr = (uint32_t)((uint64_t)ag_api()->time->us() - w0);
            worst = 0;
        }
    }
    return 0;
}
