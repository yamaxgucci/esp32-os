/*
 * ArgonOS - an application that gives nothing back.
 *
 * Allocates until its arena is empty and exits without freeing a byte.  Run it
 * twice with mem in between: the second run has to find the same arena as the
 * first, or the reclaim on exit is not doing its job.
 *
 * It also allocates one block outside the arena - internal, fast memory - since
 * that is the case the resource list has to catch: it does not come from the
 * arena and so is not reclaimed by dropping it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("LEAK", "1.0", "argon", 0);

#define CHUNK (32u * 1024u)

int ag_main(int argc, char **argv)
{
    ag_meminfo_t before;
    ag_meminfo(&before);
    ag_printf("arena: %u KB, %u KB free\n",
              (unsigned)(before.arena_total / 1024u),
              (unsigned)(before.arena_free / 1024u));

    unsigned taken = 0;
    for (;;) {
        void *p = ag_malloc(CHUNK);
        if (p == NULL) {
            break;
        }
        /* Touch it: memory that is never written is memory that may not be there. */
        *(volatile unsigned char *)p = 0xa5;
        taken++;
    }

    void *fast = ag_malloc_caps(4096, AG_MEM_FAST | AG_MEM_ZERO);

    ag_meminfo_t after;
    ag_meminfo(&after);

    ag_printf("took %u KB in %u blocks, %u KB left, fast block %s\n",
              (unsigned)((taken * CHUNK) / 1024u), taken,
              (unsigned)(after.arena_free / 1024u),
              (fast != NULL) ? "yes" : "no");

    /* Freeing something twice is caught by the resource list, not by the heap. */
    if (fast != NULL) {
        ag_free(fast);
        ag_free(fast);
        ag_print("freed the fast block twice on purpose; see log\n");
    }

    ag_print("exiting without freeing the rest\n");
    return 0;
}
