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
 *   run t:\crash.axe locked   fault inside a kernel call that holds a lock
 *
 * The last one is not recovered, and must not be: it is there to prove that the
 * panic path leaves a record too.
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

/*
 * A fault the recovery path has to REFUSE, and the reason it has to.
 *
 * The kernel writes into a buffer this application handed it, and it does that
 * holding the console lock.  A buffer that cannot be written to therefore faults
 * inside the kernel with that lock held - and unwinding the task out of the
 * fault would leave the lock held for ever, which is a hung board rather than
 * one dead application.  So this fault is left to take its normal course: the
 * panic handler prints, the board reboots, and the only thing that can survive
 * that is the record the port keeps in RTC memory.
 *
 * It is also the shape of the real thing: a load or store access fault in code
 * that was called with the kernel's lock in hand, mid-transfer.
 *
 * ag_coninfo AND NOT ag_con_peek_row, which is what this was first written
 * with, and the difference is measured rather than preferred.  peek_row asks for
 * the console focus before it takes the lock and answers -AG_EPERM without it -
 * and a program that has only just started does not have the focus yet: a
 * process is "adopted into its slot a little after it starts running"
 * (src/proc/session.c), and this image is small enough to reach the call inside
 * that window.  Measured on the C3:
 *
 *   I (11407) app/CRASH:1: peek row answered -1 without faulting (no console focus?)
 *
 * which is a deliberate panic that does not panic - the worst kind of test,
 * because it looks like a fault the port recovered.  ag_coninfo has no focus to
 * ask for: it takes the lock and writes into the caller's structure, every time,
 * so the fault is the kernel storing through a bad pointer with the lock held.
 */
static void fault_under_a_kernel_lock(void)
{
    ag_coninfo((ag_coninfo_t *)0x10);

    ag_print("coninfo answered without faulting\n");
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
    } else if (what[0] == 'l') {
        fault_under_a_kernel_lock();
    } else {
        write_through_null();
    }

    ag_print("still here, which means nothing faulted\n");
    return 0;
}
