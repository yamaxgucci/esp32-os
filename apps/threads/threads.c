/*
 * ArgonOS - threads, and what happens to the ones an application forgets.
 *
 * Two threads count into a shared total under a mutex, and pass what they
 * counted back through a queue.  One is joined properly.  The other is left
 * running on purpose, along with a mutex and a queue that are never deleted:
 * the process ends anyway, and everything it forgot has to go with it.
 *
 *   run t:\threads.axe
 *   ps          nothing left loaded
 *   mem         internal memory back where it started
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("THREADS", "1.0", "argon", 0);

#define ROUNDS 2000

static ag_mutex_t s_lock;
static ag_queue_t s_results;
static volatile unsigned s_total;

static void counter(void *arg)
{
    const unsigned id = (unsigned)(uintptr_t)arg;
    unsigned       mine = 0;

    for (int i = 0; i < ROUNDS; i++) {
        /*
         * The point of the mutex: two threads on two cores adding to the same
         * word without one would lose increments, and lose them rarely enough to
         * be a bad kind of bug.
         */
        if (ag_mutex_lock(s_lock, 1000)) {
            s_total++;
            ag_mutex_unlock(s_lock);
            mine++;
        }
        if ((i % 200) == 0) {
            ag_yield();
        }
    }

    (void)ag_queue_send(s_results, &mine, 1000);

    /* And then it does not stop, which is the interesting part. */
    for (;;) {
        ag_delay(200);
    }
}

int ag_main(int argc, char **argv)
{
    s_lock = ag_mutex_create();
    s_results = ag_queue_create(4, sizeof(unsigned));
    if (s_lock == NULL || s_results == NULL) {
        ag_print("no mutex or no queue\n");
        return 1;
    }

    ag_thread_t a = ag_thread_create(counter, (void *)1u, "count", 4096, 1, 0);
    ag_thread_t b = ag_thread_create(counter, (void *)2u, "count", 4096, 1, 0);
    if (a == NULL || b == NULL) {
        ag_print("could not start two threads\n");
        return 1;
    }

    unsigned first = 0, second = 0;
    (void)ag_queue_recv(s_results, &first, 5000);
    (void)ag_queue_recv(s_results, &second, 5000);

    ag_printf("threads counted %u and %u, total %u (expected %u)\n", first,
              second, s_total, (unsigned)(2 * ROUNDS));

    /* One is waited for; the other is left running, and so are the mutex and
     * the queue.  Ending the process has to deal with all three. */
    if (ag_thread_join(a, 100) == AG_OK) {
        ag_print("joined the first thread\n");
    } else {
        ag_print("the first thread is still running, as expected\n");
    }
    (void)b;

    ag_print("exiting with a thread, a mutex and a queue still held\n");
    return (s_total == 2 * ROUNDS) ? 0 : 1;
}
