/*
 * ArgonOS - a file's bytes at an address, read only.
 *
 * For data too big to keep in memory and too unchanging to need to be: a
 * cartridge, a font, a table of impulse responses.  The bytes go into the
 * `appfs` flash partition and are mapped into the data address space, where the
 * same cache that fetches the processor's own instructions fetches them.  The
 * cost in memory is nothing at all.
 *
 * That is the whole point, and it is worth being plain about the size of it: a
 * Game Boy cartridge of half a megabyte on a board with sixty kilobytes of
 * memory free is not a matter of being careful with allocations.  Either the
 * bytes stay in flash or the machine cannot run the game.
 *
 * What this is not
 * ----------------
 *
 * Not demand paging: the file is copied into flash first, in full, and the copy
 * takes as long as writing that much flash takes.  Not a cache: a second run
 * copies it again.  Not writable, and not a way to make a filesystem mappable -
 * littlefs and FAT scatter a file across blocks and neither can be mapped in
 * place, which is precisely why the copy exists.
 *
 * The staging is worth its cost only for something read many times: a cartridge
 * read once per emulated instruction, yes; a configuration file, no.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "argon/filemap.h"

#include <string.h>

#include <argon/log.h>
#include <argon/vfs.h>

#include "loader/appfs.h"

#define TAG "filemap"

/*
 * How much is read from the file at a time on its way to flash.
 *
 * On the stack of whoever called, so it cannot be large; flash writes are
 * buffered by the driver anyway, so a bigger piece buys little.
 */
#define STAGE_CHUNK 512

/*
 * One entry per live mapping.  Small and fixed: a process maps a cartridge, not
 * a thousand files, and a table means unmap can find its slot from the pointer
 * alone - which is all an application has to give back.
 */
#define AG_FILEMAP_MAX 4

typedef struct {
    bool             used;
    ag_appfs_slot_t *slot;
    const void      *ptr;
    uint64_t         len;
} entry_t;

static entry_t s_map[AG_FILEMAP_MAX];

static entry_t *free_entry(void)
{
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (!s_map[i].used) {
            return &s_map[i];
        }
    }
    return NULL;
}

static entry_t *entry_for(const void *ptr)
{
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (s_map[i].used && s_map[i].ptr == ptr) {
            return &s_map[i];
        }
    }
    return NULL;
}

ag_err_t ag_filemap_open(const char *path, const char *cwd, const void **out,
                         uint64_t *out_len)
{
    if (path == NULL || out == NULL) {
        return -AG_EINVAL;
    }

    entry_t *e = free_entry();
    if (e == NULL) {
        return -AG_ENFILE;
    }

    ag_stat_t st;
    ag_err_t  err = ag_vfs_stat(path, cwd, &st);
    if (err != AG_OK) {
        return err;
    }
    if (st.size == 0) {
        return -AG_EINVAL;
    }

    ag_appfs_slot_t *slot = NULL;
    err = ag_appfs_reserve_data((size_t)st.size, &slot);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, TAG, "%s: no room in appfs for %u KB (%d)", path,
               (unsigned)(st.size / 1024u), (int)err);
        return err;
    }

    const ag_handle_t h = ag_vfs_open(path, cwd, AG_O_RDONLY);
    if (h < 0) {
        ag_appfs_release(slot);
        return h;
    }

    uint8_t  buf[STAGE_CHUNK];
    uint64_t done = 0;
    while (done < st.size) {
        const int32_t n = ag_vfs_read(h, buf, sizeof(buf));
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            err = -AG_EIO; /* shorter than it said it was */
            break;
        }
        err = ag_appfs_program_at(slot, (size_t)done, buf, (size_t)n);
        if (err != AG_OK) {
            break;
        }
        done += (uint64_t)n;
    }
    ag_vfs_close(h);

    if (err != AG_OK) {
        ag_appfs_release(slot);
        return err;
    }

    const void *ptr = NULL;
    err = ag_appfs_mmap_data(slot, &ptr);
    if (err != AG_OK) {
        ag_appfs_release(slot);
        return err;
    }

    e->used = true;
    e->slot = slot;
    e->ptr = ptr;
    e->len = st.size;

    *out = ptr;
    if (out_len != NULL) {
        *out_len = st.size;
    }
    ag_log(AG_LOG_INFO, TAG, "%s: %u KB staged in flash, mapped at %p", path,
           (unsigned)(st.size / 1024u), ptr);
    return AG_OK;
}

ag_err_t ag_filemap_close(const void *ptr)
{
    entry_t *e = entry_for(ptr);
    if (e == NULL) {
        return -AG_ENOENT;
    }
    ag_appfs_release(e->slot);
    memset(e, 0, sizeof(*e));
    return AG_OK;
}

uint32_t ag_filemap_release_all(void)
{
    uint32_t freed = 0;
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (s_map[i].used) {
            ag_appfs_release(s_map[i].slot);
            memset(&s_map[i], 0, sizeof(s_map[i]));
            freed++;
        }
    }
    return freed;
}
