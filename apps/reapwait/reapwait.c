/*
 * ArgonOS - a parent that is slow to wait, the test for who collects a zombie.
 *
 *   run t:\reapwait.axe late t:\hello.axe    spawn, sleep 1 s, then wait
 *   run t:\reapwait.axe kill t:\spin.axe     spawn, kill it, sleep 1 s, wait
 *   run t:\reapwait.axe leave t:\hello.axe   spawn, sleep 1 s, never wait
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

int ag_main(int argc, char **argv)
{
    if (argc < 3) {
        ag_print("usage: reapwait late|kill <program>\n");
        return 1;
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
