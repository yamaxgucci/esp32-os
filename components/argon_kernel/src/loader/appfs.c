/*
 * ArgonOS - flash slots for application code (R-1 XIP).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "loader/appfs.h"

#include <string.h>

#include <argon/log.h>
#include <argon/module.h>
#include <argon/proc.h>

#include <argon/port/flash.h>

/* Instruction MMU pages on Espressif chips are 64 KB. */
#define AG_APPFS_PAGE (64u * 1024u)
#define AG_APPFS_SLOTS (AG_PROC_MAX + AG_MODULE_MAX)

struct ag_appfs_slot {
    bool                       used;
    bool                       mapped;
    size_t                     offset;
    size_t                     reserved; /* erase/mmap size, page-aligned */
    size_t                     programmed;
    ag_port_map_t              mmap;
    const void                *mapped_ptr;
};

static ag_port_part_t         s_part;
static ag_appfs_slot_t        s_slots[AG_APPFS_SLOTS];
static bool                   s_ready;

static size_t align_up(size_t n, size_t a)
{
    return (n + a - 1u) & ~(a - 1u);
}

ag_err_t ag_appfs_init(void)
{
    if (s_ready) {
        return AG_OK;
    }

    s_part = ag_port_part_find(AG_APPFS_NAME);
    if (s_part == NULL) {
        ag_log(AG_LOG_WARN, "appfs", "partition '%s' not found; XIP disabled",
               AG_APPFS_NAME);
        return -AG_ENODEV;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_ready = true;
    ag_log(AG_LOG_INFO, "appfs", "%s: %u KB for code XIP", AG_APPFS_NAME,
           (unsigned)(ag_port_part_size(s_part) / 1024u));
    return AG_OK;
}

bool ag_appfs_ready(void) { return s_ready && s_part != NULL; }

static bool range_free(size_t offset, size_t size)
{
    const size_t end = offset + size;
    for (int i = 0; i < AG_APPFS_SLOTS; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        const size_t s = s_slots[i].offset;
        const size_t e = s + s_slots[i].reserved;
        if (offset < e && s < end) {
            return false;
        }
    }
    return true;
}

static ag_err_t find_space(size_t need, size_t *out_offset)
{
    const size_t size = (size_t)ag_port_part_size(s_part);
    size_t       cursor = 0;
    while (cursor + need <= size) {
        if (range_free(cursor, need)) {
            *out_offset = cursor;
            return AG_OK;
        }
        /* Skip to the end of the overlapping used slot. */
        size_t next = cursor + AG_APPFS_PAGE;
        for (int i = 0; i < AG_APPFS_SLOTS; i++) {
            if (!s_slots[i].used) {
                continue;
            }
            const size_t s = s_slots[i].offset;
            const size_t e = s + s_slots[i].reserved;
            if (cursor < e && s < cursor + need && e > next) {
                next = e;
            }
        }
        cursor = align_up(next, AG_APPFS_PAGE);
    }
    return -AG_ENOSPC;
}

ag_err_t ag_appfs_reserve(size_t bytes, ag_appfs_slot_t **out_slot,
                          void **out_exec_addr)
{
    if (out_slot == NULL || out_exec_addr == NULL || bytes == 0) {
        return -AG_EINVAL;
    }
    if (!ag_appfs_ready()) {
        const ag_err_t err = ag_appfs_init();
        if (err != AG_OK) {
            return err;
        }
    }

    ag_appfs_slot_t *slot = NULL;
    for (int i = 0; i < AG_APPFS_SLOTS; i++) {
        if (!s_slots[i].used) {
            slot = &s_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        return -AG_ENFILE;
    }

    const size_t need = align_up(bytes, AG_APPFS_PAGE);
    size_t       offset = 0;
    ag_err_t     err = find_space(need, &offset);
    if (err != AG_OK) {
        return err;
    }

    err = ag_port_part_erase(s_part, offset, need);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "appfs", "erase @%u+%u failed: %d",
               (unsigned)offset, (unsigned)need, (int)err);
        return -AG_EIO;
    }

    /*
     * Probe the instruction window address for this flash range.  The second
     * mapping after programming is expected to return the same pointer.
     */
    const void   *probe = NULL;
    ag_port_map_t probe_h = 0;
    err = ag_port_part_map_exec(s_part, offset, need, &probe, &probe_h);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "appfs", "probe mapping failed: %d", (int)err);
        return -AG_ENOMEM;
    }
    void *exec = (void *)probe;
    ag_port_part_unmap(probe_h);

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->offset = offset;
    slot->reserved = need;
    slot->programmed = 0;

    *out_slot = slot;
    *out_exec_addr = exec;
    return AG_OK;
}

ag_err_t ag_appfs_program(ag_appfs_slot_t *slot, const void *data, size_t bytes)
{
    if (slot == NULL || !slot->used || data == NULL || bytes == 0) {
        return -AG_EINVAL;
    }
    if (bytes > slot->reserved) {
        return -AG_EINVAL;
    }
    if (slot->mapped) {
        return -AG_EBUSY;
    }

    const ag_err_t err = ag_port_part_write(s_part, slot->offset, data, bytes);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "appfs", "write @%u failed: %d",
               (unsigned)slot->offset, (int)err);
        return -AG_EIO;
    }
    slot->programmed = bytes;
    return AG_OK;
}

ag_err_t ag_appfs_mmap(ag_appfs_slot_t *slot, const void **out_ptr)
{
    if (slot == NULL || !slot->used || out_ptr == NULL) {
        return -AG_EINVAL;
    }
    if (slot->mapped) {
        *out_ptr = slot->mapped_ptr;
        return AG_OK;
    }

    const void    *ptr = NULL;
    ag_port_map_t  h = 0;
    const ag_err_t err =
        ag_port_part_map_exec(s_part, slot->offset, slot->reserved, &ptr, &h);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "appfs", "mapping failed: %d", (int)err);
        return -AG_ENOMEM;
    }

    slot->mapped = true;
    slot->mmap = h;
    slot->mapped_ptr = ptr;
    *out_ptr = ptr;
    return AG_OK;
}

void ag_appfs_release(ag_appfs_slot_t *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }
    if (slot->mapped) {
        ag_port_part_unmap(slot->mmap);
    }
    memset(slot, 0, sizeof(*slot));
}
