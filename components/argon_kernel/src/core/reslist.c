/*
 * ArgonOS - a process's resource list.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/reslist.h>

#include <string.h>

void ag_reslist_init(ag_reslist_t *list, ag_res_t *storage, uint32_t capacity)
{
    if (list == NULL) {
        return;
    }
    memset(list, 0, sizeof(*list));
    if (storage != NULL && capacity > 0) {
        memset(storage, 0, capacity * sizeof(*storage));
        list->slots = storage;
        list->capacity = capacity;
    }
}

ag_err_t ag_reslist_add(ag_reslist_t *list, ag_res_type_t type, void *ref,
                        uint32_t extra)
{
    if (list == NULL || list->slots == NULL || ref == NULL ||
        type <= AG_RES_NONE || type >= AG_RES_TYPE_COUNT) {
        return -AG_EINVAL;
    }

    for (uint32_t i = 0; i < list->capacity; i++) {
        ag_res_t *slot = &list->slots[i];
        if (slot->type != AG_RES_NONE) {
            continue;
        }
        slot->type = type;
        slot->ref = ref;
        slot->extra = extra;
        slot->seq = list->next_seq++;
        list->count++;
        if (list->count > list->peak) {
            list->peak = list->count;
        }
        return AG_OK;
    }

    return -AG_ENOMEM;
}

static ag_res_t *find(const ag_reslist_t *list, ag_res_type_t type, void *ref)
{
    if (list == NULL || list->slots == NULL || ref == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < list->capacity; i++) {
        ag_res_t *slot = &list->slots[i];
        if (slot->type == type && slot->ref == ref) {
            return slot;
        }
    }
    return NULL;
}

bool ag_reslist_remove(ag_reslist_t *list, ag_res_type_t type, void *ref,
                       uint32_t *extra_out)
{
    ag_res_t *slot = find(list, type, ref);
    if (slot == NULL) {
        return false;
    }
    if (extra_out != NULL) {
        *extra_out = slot->extra;
    }
    memset(slot, 0, sizeof(*slot));
    list->count--;
    return true;
}

bool ag_reslist_holds(const ag_reslist_t *list, ag_res_type_t type, void *ref)
{
    return find(list, type, ref) != NULL;
}

uint32_t ag_reslist_count(const ag_reslist_t *list)
{
    return (list != NULL) ? list->count : 0;
}

uint32_t ag_reslist_count_of(const ag_reslist_t *list, ag_res_type_t type)
{
    if (list == NULL || list->slots == NULL) {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < list->capacity; i++) {
        if (list->slots[i].type == type) {
            n++;
        }
    }
    return n;
}

uint32_t ag_reslist_peak(const ag_reslist_t *list)
{
    return (list != NULL) ? list->peak : 0;
}

uint32_t ag_reslist_total_of(const ag_reslist_t *list, ag_res_type_t type)
{
    if (list == NULL || list->slots == NULL) {
        return 0;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < list->capacity; i++) {
        if (list->slots[i].type == type) {
            total += list->slots[i].extra;
        }
    }
    return total;
}

uint32_t ag_reslist_reclaim(ag_reslist_t *list, ag_res_release_fn release,
                            void *ctx)
{
    if (list == NULL || list->slots == NULL) {
        return 0;
    }

    uint32_t released = 0;

    for (int type = AG_RES_NONE + 1; type < AG_RES_TYPE_COUNT; type++) {
        /*
         * Newest first within the type.  Repeatedly picking the highest sequence
         * number rather than walking the array backwards, because slots are
         * reused and their index says nothing about their age.
         */
        for (;;) {
            ag_res_t *newest = NULL;
            for (uint32_t i = 0; i < list->capacity; i++) {
                ag_res_t *slot = &list->slots[i];
                if (slot->type != (ag_res_type_t)type) {
                    continue;
                }
                if (newest == NULL || slot->seq > newest->seq) {
                    newest = slot;
                }
            }
            if (newest == NULL) {
                break;
            }

            const ag_res_t taken = *newest;
            /* Cleared before the callback: a release that comes back round to
             * the list must not find the entry it is already releasing. */
            memset(newest, 0, sizeof(*newest));
            list->count--;

            if (release != NULL) {
                release(taken.type, taken.ref, taken.extra, ctx);
            }
            released++;
        }
    }

    return released;
}

const char *ag_res_type_name(ag_res_type_t type)
{
    switch (type) {
    case AG_RES_THREAD: return "thread";
    case AG_RES_TIMER:  return "timer";
    case AG_RES_FILE:   return "file";
    case AG_RES_MUTEX:  return "mutex";
    case AG_RES_SEM:    return "semaphore";
    case AG_RES_QUEUE:  return "queue";
    case AG_RES_MEM:    return "memory";
    default:            return "none";
    }
}
