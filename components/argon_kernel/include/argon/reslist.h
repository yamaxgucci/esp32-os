/*
 * ArgonOS - a process's resource list.
 *
 * Everything a process obtains through a syscall is written down here, so that
 * ending the process - whether it exits or is killed - gives all of it back
 * without the supervisor keeping bookkeeping of its own.  This is the part DOS
 * did not have, and the reason a hung application can be removed instead of
 * rebooting the board (see docs/00-architecture.md §5.3).
 *
 * Storage is supplied by the caller, so a process's list is part of the process
 * and there is no allocation on the path that has to work when memory is short.
 * A full list fails the syscall rather than growing: a bound that is reached is
 * a diagnosable event, and an unbounded list is a leak with extra steps.
 *
 * No dependency on FreeRTOS or the chip, so this is built and tested on the
 * host - which matters, because the order things are released in is the kind of
 * detail that is wrong once and then wrong forever.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_RESLIST_H
#define ARGON_RESLIST_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The release order is by type, in the order below, and within a type in
 * reverse of registration.  By type rather than strictly last-in-first-out
 * because that is what the dependencies actually are: a thread may be using a
 * file, a file may be using a buffer, so threads have to stop before handles
 * close and handles have to close before memory goes away.  Strict LIFO would
 * also mean slots could never be reused while a process ran, which would turn
 * an ordinary allocate-and-free loop into a full list.
 */
typedef enum {
    AG_RES_NONE = 0, /* free slot                                          */
    AG_RES_THREAD,   /* a thread of the process                            */
    AG_RES_TIMER,
    AG_RES_FILE,   /* an open handle, when it is not tracked by the VFS     */
    AG_RES_MUTEX,
    AG_RES_SEM,
    AG_RES_QUEUE,
    AG_RES_MEM, /* memory outside the process arena: fast, DMA, executable  */
    AG_RES_TYPE_COUNT
} ag_res_type_t;

typedef struct {
    ag_res_type_t type;
    void         *ref;   /* pointer or handle, whatever the type means      */
    uint32_t      extra; /* size for memory, informational otherwise        */
    uint32_t      seq;   /* registration order, for releasing in reverse    */
} ag_res_t;

typedef struct {
    ag_res_t *slots;
    uint32_t  capacity;
    uint32_t  count;
    uint32_t  next_seq;
    uint32_t  peak; /* most entries ever held, for the record              */
} ag_reslist_t;

void ag_reslist_init(ag_reslist_t *list, ag_res_t *storage, uint32_t capacity);

/*
 * Records one resource.  -AG_ENOMEM when the list is full, -AG_EINVAL for a
 * NULL reference or a type outside the enum: a resource that cannot be recorded
 * must not be handed out, or it would leak past the end of the process.
 */
ag_err_t ag_reslist_add(ag_reslist_t *list, ag_res_type_t type, void *ref,
                        uint32_t extra);

/*
 * Forgets one resource, because the process released it itself.  Returns false
 * when it was never recorded, which is how a double free or a foreign pointer is
 * caught before it reaches the allocator.
 */
bool ag_reslist_remove(ag_reslist_t *list, ag_res_type_t type, void *ref,
                       uint32_t *extra_out);

bool     ag_reslist_holds(const ag_reslist_t *list, ag_res_type_t type,
                          void *ref);
uint32_t ag_reslist_count(const ag_reslist_t *list);
uint32_t ag_reslist_count_of(const ag_reslist_t *list, ag_res_type_t type);
uint32_t ag_reslist_peak(const ag_reslist_t *list);
/* Total of `extra` over one type - the memory a process holds outside its arena. */
uint32_t ag_reslist_total_of(const ag_reslist_t *list, ag_res_type_t type);

/*
 * Releases everything, in the order described above, and empties the list.  The
 * callback does the actual freeing; this decides what and in which order.
 * Returns how many entries were released.
 */
typedef void (*ag_res_release_fn)(ag_res_type_t type, void *ref, uint32_t extra,
                                  void *ctx);

uint32_t ag_reslist_reclaim(ag_reslist_t *list, ag_res_release_fn release,
                            void *ctx);

/* "thread", "file", "memory" - for the journal and for ps. */
const char *ag_res_type_name(ag_res_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_RESLIST_H */
