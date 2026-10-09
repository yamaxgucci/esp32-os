/*
 * ArgonOS - a parent that is slow to wait, the test for who collects a zombie.
 *
 *   run t:\reapwait.axe late t:\hello.axe    spawn, sleep 1 s, then wait
 *   run t:\reapwait.axe kill t:\spin.axe     spawn, kill it, sleep 1 s, wait
 *   run t:\reapwait.axe leave t:\hello.axe   spawn, sleep 1 s, never wait
 *   run t:\reapwait.axe storm t:\spin.axe log
 *                                            spawn, kill, wait - forty times
 *
 * An application's spawn and its wait are two calls.  The supervisor collects
 * a finished process nobody is waiting for every 250 ms, so a child that ends
 * before its parent gets to the wait used to be gone by then, and the wait
 * answered -AG_ENOENT (-2) instead of the child's code.  A kill between two
 * slices of a wait (the desktop waits in slices, for Ctrl+C) collected the
 * child at once, the same way.  A second of sleep holds each gap open for
 * four supervisor ticks, so this fails every time or never.
 *
 * It prints one line the scenario reads:
 *
 *   reapwait late: wait 0, code 0          the child's own code
 *   reapwait kill: wait -92, code ...      -AG_EKILLED: it was killed
 *
 * and a wait of -2 is the fault.  A second wait must then say -2: the first one
 * collected it, and nobody else may have.
 *
 * leave ends without waiting.  Its finished child was kept for it, so now it
 * is an orphan and the supervisor must collect it: ps a moment later has to
 * show neither of them.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("REAPWAIT", "1.0", "argon", 0);

/*
 * The storm: spawn, kill a moment later, collect - STORM_ROUNDS times, with
 * the moment different every round so the kill lands at a different point of
 * whatever the child is doing.  For SPIN log, which is inside the kernel's
 * output path nearly all the time, that is a kill inside the journal's lock
 * or the console's, over and over.
 *
 * A child deleted holding one of those holds it for ever (FreeRTOS lets only
 * the owner give a mutex back), and from then on nothing that logs gets
 * anywhere - this program included, so the line below never comes and that
 * absence is the failure.  It hung proctest once in four runs on 1 Oct 2026
 * with one kill.  Before the fix this storm hung on its very first kill.
 */
#define STORM_ROUNDS 40

static int storm(const char *program, const char *arg)
{
    const char *child_argv[] = {program, arg};
    unsigned    killed = 0, refused = 0;

    for (unsigned round = 0; round < STORM_ROUNDS; round++) {
        const ag_pid_t child =
            ag_spawn(program, (arg != NULL) ? 2 : 1, child_argv,
                     AG_SPAWN_BACKGROUND | AG_SPAWN_NO_SESSION);
        if (child < 0) {
            ag_printf("reapwait: spawn %s failed %d\n", program, (int)child);
            return 1;
        }
        ag_delay(1u + (round * 7u) % 23u);

        const ag_err_t k = ag_kill(child);
        int32_t        code = 12345;
        const ag_err_t w = ag_wait(child, &code, 5000u);
        if (k == AG_OK && (w == -AG_EKILLED || w == AG_OK)) {
            killed++;
        } else if (k == -AG_EBUSY) {
            refused++; /* the kernel left it running and said so */
            ag_printf("reapwait: round %u kill refused, wait %d\n", round,
                      (int)w);
        } else {
            ag_printf("reapwait: round %u kill %d wait %d code %d\n", round,
                      (int)k, (int)w, (int)code);
        }
    }
    /* Short: in the journal it is behind "I (16998) app/REAPWAIT:7: ", and a
     * line past 80 columns reaches the transcript in two pieces. */
    ag_printf("reapwait storm: %u of %u killed, %u refused\n", killed,
              (unsigned)STORM_ROUNDS, refused);
    return 0;
}

int ag_main(int argc, char **argv)
{
    if (argc < 3) {
        ag_print("usage: reapwait late|kill|leave|storm <program> [arg]\n");
        return 1;
    }
    if (argv[1][0] == 's') {
        return storm(argv[2], (argc > 3) ? argv[3] : NULL);
    }
    const bool kill = argv[1][0] == 'k';

    const char    *child_argv[] = {argv[2]};
    const ag_pid_t child = ag_spawn(argv[2], 1, child_argv,
                                    AG_SPAWN_BACKGROUND | AG_SPAWN_NO_SESSION);
    if (child < 0) {
        ag_printf("reapwait: spawn %s failed %d\n", argv[2], (int)child);
        return 1;
    }

    if (kill) {
        ag_delay(300);
        ag_printf("reapwait: kill says %d\n", (int)ag_kill(child));
    }
    ag_delay(1000);
    if (argv[1][0] == 'l' && argv[1][1] == 'e') { /* "late" is an l too */
        ag_print("reapwait leave: not waiting\n");
        return 0;
    }

    int32_t        code = 12345;
    const ag_err_t first = ag_wait(child, &code, 5000u);
    int32_t        again = 0;
    const ag_err_t second = ag_wait(child, &again, 100u);

    ag_printf("reapwait %s: wait %d, code %d, again %d\n", argv[1], (int)first,
              (int)code, (int)second);
    return 0;
}
