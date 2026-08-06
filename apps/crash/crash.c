/*
 * ArgonOS - an application that does something it should not.
 *
 * The test case for catching a fault and blaming the right process.  Without an
 * MMU a wild pointer can reach anything, so this cannot be made impossible; what
 * it can be made is somebody's problem other than the whole system's.
 *
 *   run t:\crash.axe          write through a null pointer
 *   run t:\crash.axe read     read through a null pointer
 *   run t:\crash.axe jump     call a null function pointer
 *   run t:\crash.axe thread   fault on a thread instead of the main task
 *
 * Either way the shell should come back with a prompt, ps should be empty, and
 * the journal should say where in this application the fault was.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("CRASH", "1.0", "argon", 0);

static void write_through_null(void)
{
    volatile unsigned *nowhere = (volatile unsigned *)0;
    *nowhere = 0xdeadbeef;
}

/*
 * Also through a volatile pointer, and for a subtler reason: dereferencing a
 * literal null is undefined behaviour, so GCC is entitled to delete the whole
 * statement - and does, at -Os, which made this case quietly test nothing at all.
 * A pointer it cannot prove is null has to be loaded from.
 */
static unsigned *volatile s_nowhere_data;

static void read_through_null(void)
{
    const unsigned value = *s_nowhere_data;

    ag_printf("read %08x from address zero without faulting\n", value);
}

/*
 * Through a volatile pointer, not a constant: a call to a literal zero is a
 * direct call as far as the compiler is concerned, and the linker refuses to
 * encode a windowed call across that distance.  This is the shape a real bug has
 * anyway - a function pointer that was never set.
 */
static void (*volatile s_nowhere)(void);

static void jump_through_null(void) { s_nowhere(); }

static void crashing_thread(void *arg)
{
    (void)arg;
    ag_delay(50);
    ag_print("thread is about to fault\n");
    write_through_null();
}

int ag_main(int argc, char **argv)
{
    const char *what = (argc > 1) ? argv[1] : "write";

    ag_printf("pid %d is about to fault: %s\n", (int)ag_getpid(), what);

    if (what[0] == 'r') {
        read_through_null();
    } else if (what[0] == 'j') {
        jump_through_null();
    } else if (what[0] == 't') {
        (void)ag_thread_create(crashing_thread, NULL, "boom", 4096, 1, 0);
        for (int i = 0; i < 100; i++) {
            ag_delay(50);
        }
    } else {
        write_through_null();
    }

    ag_print("still here, which means nothing faulted\n");
    return 0;
}
