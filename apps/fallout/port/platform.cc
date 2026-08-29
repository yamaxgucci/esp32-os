/*
 * ArgonOS - the POSIX corners the engine reaches for outside SDL.
 *
 * Directory enumeration, and nothing else so far.  Everything the engine needs
 * from <stdio.h> and <string.h> already comes from apps/common/libc; this file
 * is for the calls that need the ArgonOS filesystem rather than a shim over
 * the console.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dirent.h"

#include <argon/argon.h>
#include <string.h>

/*
 * One entry is returned at a time and the caller must not hold it - that is
 * POSIX, and it is why the buffer lives in the DIR rather than being handed
 * out.  ArgonOS hands back a whole ag_dirent_t, so the copy is short.
 */
struct DIR {
    ag_handle_t handle;
    struct dirent entry;
};

extern "C" {

DIR *opendir(const char *path)
{
    if (path == NULL) {
        return NULL;
    }

    ag_handle_t h = ag_opendir(path);
    if (h < 0) {
        return NULL;
    }

    DIR *dir = (DIR *)ag_malloc(sizeof(DIR));
    if (dir == NULL) {
        ag_closedir(h);
        return NULL;
    }
    dir->handle = h;
    return dir;
}

struct dirent *readdir(DIR *dir)
{
    if (dir == NULL) {
        return NULL;
    }

    ag_dirent_t e;
    if (ag_readdir(dir->handle, &e) != AG_OK) {
        return NULL;
    }

    dir->entry.d_type = (e.st.attr & AG_A_DIR) ? DT_DIR : DT_REG;
    /* d_name is the same 256 bytes on both sides, and the kernel's is always
     * terminated; the explicit terminator is for the day one of them changes. */
    strncpy(dir->entry.d_name, e.name, sizeof(dir->entry.d_name) - 1);
    dir->entry.d_name[sizeof(dir->entry.d_name) - 1] = '\0';
    return &dir->entry;
}

int closedir(DIR *dir)
{
    if (dir == NULL) {
        return -1;
    }
    ag_closedir(dir->handle);
    ag_free(dir);
    return 0;
}

}  // extern "C"
