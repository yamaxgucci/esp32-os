/*
 * ArgonOS - resource list tests.
 *
 * What matters here is not that entries go in and come out, it is the order they
 * come out in and the fact that a full list refuses instead of losing an entry.
 * Both decide whether killing a process actually gives its resources back.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/reslist.h>

#include "test.h"

/* Fake references: the list never dereferences them, it only records them. */
static char s_objects[16];

#define REF(i) ((void *)&s_objects[(i)])

/* ---------------------------------------------------------------------- */
/* Recording what was released, in the order it was released.             */

#define LOG_MAX 32

typedef struct {
    ag_res_type_t type[LOG_MAX];
    void         *ref[LOG_MAX];
    uint32_t      extra[LOG_MAX];
    uint32_t      n;
} log_t;

static void log_release(ag_res_type_t type, void *ref, uint32_t extra, void *ctx)
{
    log_t *log = (log_t *)ctx;
    if (log->n < LOG_MAX) {
        log->type[log->n] = type;
        log->ref[log->n] = ref;
        log->extra[log->n] = extra;
        log->n++;
    }
}

/* ---------------------------------------------------------------------- */

static void test_add_and_remove(void)
{
    ag_res_t     storage[4];
    ag_reslist_t list;
    ag_reslist_init(&list, storage, 4);

    AG_CHECK_INT(ag_reslist_count(&list), 0);

    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(0), 100), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_FILE, REF(1), 0), AG_OK);
    AG_CHECK_INT(ag_reslist_count(&list), 2);
    AG_CHECK_INT(ag_reslist_count_of(&list, AG_RES_MEM), 1);
    AG_CHECK_INT(ag_reslist_count_of(&list, AG_RES_THREAD), 0);
    AG_CHECK(ag_reslist_holds(&list, AG_RES_MEM, REF(0)));

    /* The type is part of the identity: the same address as another kind of
     * resource is a different resource. */
    AG_CHECK(!ag_reslist_holds(&list, AG_RES_FILE, REF(0)));

    uint32_t extra = 0;
    AG_CHECK(ag_reslist_remove(&list, AG_RES_MEM, REF(0), &extra));
    AG_CHECK_INT(extra, 100);
    AG_CHECK_INT(ag_reslist_count(&list), 1);

    /* Removing it twice is how a double free arrives, and it has to be caught
     * here rather than passed to the allocator. */
    AG_CHECK(!ag_reslist_remove(&list, AG_RES_MEM, REF(0), NULL));

    /* A pointer that was never recorded is somebody else's memory. */
    AG_CHECK(!ag_reslist_remove(&list, AG_RES_MEM, REF(9), NULL));
}

static void test_rejects_nonsense(void)
{
    ag_res_t     storage[2];
    ag_reslist_t list;
    ag_reslist_init(&list, storage, 2);

    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, NULL, 0), -AG_EINVAL);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_NONE, REF(0), 0), -AG_EINVAL);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_TYPE_COUNT, REF(0), 0),
                 -AG_EINVAL);
    AG_CHECK_INT(ag_reslist_add(NULL, AG_RES_MEM, REF(0), 0), -AG_EINVAL);
    AG_CHECK_INT(ag_reslist_count(&list), 0);

    /* A list with no storage records nothing and says so, rather than
     * pretending and losing the resource. */
    ag_reslist_t empty;
    ag_reslist_init(&empty, NULL, 0);
    AG_CHECK_INT(ag_reslist_add(&empty, AG_RES_MEM, REF(0), 0), -AG_EINVAL);
}

static void test_full_list_refuses(void)
{
    ag_res_t     storage[2];
    ag_reslist_t list;
    ag_reslist_init(&list, storage, 2);

    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(0), 0), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(1), 0), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(2), 0), -AG_ENOMEM);
    AG_CHECK_INT(ag_reslist_count(&list), 2);

    /* And a slot that comes free is used again: an allocate-and-free loop must
     * not run the list out. */
    AG_CHECK(ag_reslist_remove(&list, AG_RES_MEM, REF(0), NULL));
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(2), 0), AG_OK);
    AG_CHECK_INT(ag_reslist_peak(&list), 2);
}

/*
 * The order release happens in, which is the whole reason this module exists:
 * threads stop before handles close, handles close before memory goes away.
 */
static void test_reclaim_order_is_by_type(void)
{
    ag_res_t     storage[8];
    ag_reslist_t list;
    ag_reslist_init(&list, storage, 8);

    /* Registered in an order that has nothing to do with the release order. */
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(0), 10), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_FILE, REF(1), 0), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_THREAD, REF(2), 0), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(3), 20), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_FILE, REF(4), 0), AG_OK);

    log_t log = {0};
    AG_CHECK_INT(ag_reslist_reclaim(&list, log_release, &log), 5);
    AG_CHECK_INT(log.n, 5);
    AG_CHECK_INT(ag_reslist_count(&list), 0);

    /* Threads first, then files, then memory. */
    AG_CHECK_INT(log.type[0], AG_RES_THREAD);
    AG_CHECK_INT(log.type[1], AG_RES_FILE);
    AG_CHECK_INT(log.type[2], AG_RES_FILE);
    AG_CHECK_INT(log.type[3], AG_RES_MEM);
    AG_CHECK_INT(log.type[4], AG_RES_MEM);

    /* Newest first within a type. */
    AG_CHECK(log.ref[1] == REF(4));
    AG_CHECK(log.ref[2] == REF(1));
    AG_CHECK(log.ref[3] == REF(3));
    AG_CHECK(log.ref[4] == REF(0));
    AG_CHECK_INT(log.extra[3], 20);
    AG_CHECK_INT(log.extra[4], 10);

    /* Reclaiming an empty list is not an error, it is a no-op. */
    log_t again = {0};
    AG_CHECK_INT(ag_reslist_reclaim(&list, log_release, &again), 0);
    AG_CHECK_INT(again.n, 0);
}

/*
 * Slots are reused, so a slot's position says nothing about its age.  This is
 * the case that breaks an implementation that walks the array backwards.
 */
static void test_reclaim_order_survives_slot_reuse(void)
{
    ag_res_t     storage[3];
    ag_reslist_t list;
    ag_reslist_init(&list, storage, 3);

    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(0), 1), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(1), 2), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(2), 3), AG_OK);

    /* Free the middle one and register a newer one, which lands in that slot. */
    AG_CHECK(ag_reslist_remove(&list, AG_RES_MEM, REF(1), NULL));
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(5), 5), AG_OK);

    log_t log = {0};
    AG_CHECK_INT(ag_reslist_reclaim(&list, log_release, &log), 3);

    /* Newest first: the one in the middle slot is the youngest of the three. */
    AG_CHECK(log.ref[0] == REF(5));
    AG_CHECK(log.ref[1] == REF(2));
    AG_CHECK(log.ref[2] == REF(0));
}

static void test_totals_and_names(void)
{
    ag_res_t     storage[4];
    ag_reslist_t list;
    ag_reslist_init(&list, storage, 4);

    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(0), 1024), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_MEM, REF(1), 512), AG_OK);
    AG_CHECK_INT(ag_reslist_add(&list, AG_RES_FILE, REF(2), 0), AG_OK);

    AG_CHECK_INT(ag_reslist_total_of(&list, AG_RES_MEM), 1536);
    AG_CHECK_INT(ag_reslist_total_of(&list, AG_RES_FILE), 0);

    AG_CHECK_STR(ag_res_type_name(AG_RES_THREAD), "thread");
    AG_CHECK_STR(ag_res_type_name(AG_RES_FILE), "file");
    AG_CHECK_STR(ag_res_type_name(AG_RES_MEM), "memory");
    AG_CHECK_STR(ag_res_type_name(AG_RES_NONE), "none");
}

void run_reslist_tests(void)
{
    test_add_and_remove();
    test_rejects_nonsense();
    test_full_list_refuses();
    test_reclaim_order_is_by_type();
    test_reclaim_order_survives_slot_reuse();
    test_totals_and_names();
}
