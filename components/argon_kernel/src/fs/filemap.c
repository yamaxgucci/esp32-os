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
 * takes as long as writing that much flash takes - four seconds for half a
 * megabyte.  It is paid once per boot, not once per run: a staged copy outlives
 * the process and a later request for the same file is handed it.  Not writable,
 * and not a way to make a filesystem mappable -
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

#include <argon/port/mem.h>

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
    uint32_t         refs;  /* holders; an entry in use is not evicted */
    uint64_t         mtime; /* with size and name, what identifies the source */
    char             path[AG_PATH_MAX];
} entry_t;

/*
 * Asked for when something first maps a file, not reserved for the possibility.
 *
 * Four entries with a path in each is a kilobyte of static memory, held on every
 * machine whether anything is ever mapped or not - and the S3 build has less
 * than that to spare: adding it as a static array put `.dram0.bss` over the end
 * of its segment by sixteen hundred bytes and the firmware would not link.
 *
 * Which is the right lesson rather than an annoyance.  A table for a feature
 * most programs never use belongs on the heap, where a machine that does not use
 * it pays a pointer.
 */
static entry_t *s_map;

static ag_err_t table_ready(void)
{
    if (s_map != NULL) {
        return AG_OK;
    }
    s_map = (entry_t *)ag_port_calloc(AG_FILEMAP_MAX, sizeof(entry_t),
                                      AG_MEM_FAST | AG_MEM_BYTE);
    return (s_map != NULL) ? AG_OK : -AG_ENOMEM;
}

/*
 * Staged copies outlive the process that asked for them, on purpose.
 *
 * Copying half a megabyte into flash takes four seconds and wears the part a
 * little.  Doing it again every time the same cartridge is started would be
 * both, for nothing: the bytes are already there and they have not changed.
 *
 * So an entry stays after the last holder lets go, and a later request for the
 * same file - same name, same size, same modification time - is handed the
 * mapping that already exists.  That is the same identity a build system trusts,
 * and it is wrong in the same rare way: a file rewritten within the resolution
 * of its timestamp and to exactly the same length looks unchanged.
 *
 * The cache lasts a boot.  Nothing is remembered in flash about what is staged
 * there, so the first run after power-up pays the copy once.
 */
static bool same_source(const entry_t *e, const char *path, const ag_stat_t *st)
{
    return e->used && e->len == st->size && e->mtime == st->mtime &&
           strncmp(e->path, path, sizeof(e->path)) == 0;
}

static entry_t *cached(const char *path, const ag_stat_t *st)
{
    if (s_map == NULL) {
        return NULL;
    }
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (same_source(&s_map[i], path, st)) {
            return &s_map[i];
        }
    }
    return NULL;
}

/*
 * A slot to stage into: an unused one, or the flash of an entry nobody holds.
 *
 * Evicting one that is still mapped would leave whoever holds it reading an
 * address that has become something else, which is worse than refusing.
 */
static entry_t *free_entry(void)
{
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (!s_map[i].used) {
            return &s_map[i];
        }
    }
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (s_map[i].refs == 0u) {
            ag_log(AG_LOG_INFO, TAG, "dropping staged %s to make room",
                   s_map[i].path);
            ag_appfs_release(s_map[i].slot);
            memset(&s_map[i], 0, sizeof(s_map[i]));
            return &s_map[i];
        }
    }
    return NULL;
}

static entry_t *entry_for(const void *ptr)
{
    if (s_map == NULL) {
        return NULL;
    }
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

    ag_stat_t st;
    ag_err_t  err = ag_vfs_stat(path, cwd, &st);
    if (err != AG_OK) {
        return err;
    }
    if (st.size == 0) {
        return -AG_EINVAL;
    }

    err = table_ready();
    if (err != AG_OK) {
        return err;
    }

    entry_t *hit = cached(path, &st);
    if (hit != NULL) {
        hit->refs++;
        *out = hit->ptr;
        if (out_len != NULL) {
            *out_len = hit->len;
        }
        ag_log(AG_LOG_INFO, TAG, "%s: already staged, mapped at %p", path,
               hit->ptr);
        return AG_OK;
    }

    entry_t *e = free_entry();
    if (e == NULL) {
        return -AG_ENFILE;
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
    e->refs = 1u;
    e->mtime = st.mtime;
    {
        size_t i = 0;
        while (path[i] != '\0' && i + 1u < sizeof(e->path)) {
            e->path[i] = path[i];
            i++;
        }
        e->path[i] = '\0';
    }

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
    if (e->refs > 0u) {
        e->refs--;
    }
    /* The staged copy stays: see the note above same_source.  It is given back
     * only when the room is wanted for something else. */
    return AG_OK;
}

uint32_t ag_filemap_release_all(void)
{
    uint32_t freed = 0;
    if (s_map == NULL) {
        return 0;
    }
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (s_map[i].used) {
            ag_appfs_release(s_map[i].slot);
            memset(&s_map[i], 0, sizeof(s_map[i]));
            freed++;
        }
    }
    return freed;
}

uint32_t ag_filemap_forget(void)
{
    uint32_t dropped = 0;
    if (s_map == NULL) {
        return 0;
    }
    for (int i = 0; i < AG_FILEMAP_MAX; i++) {
        if (s_map[i].used && s_map[i].refs == 0u) {
            ag_appfs_release(s_map[i].slot);
            memset(&s_map[i], 0, sizeof(s_map[i]));
            dropped++;
        }
    }
    return dropped;
}
