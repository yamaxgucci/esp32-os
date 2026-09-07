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

    /*
     * ARGON: tidy the path before asking, because the engine's callers do not.
     *
     * fileFindFirst splits a pattern into a directory and a mask and opens the
     * directory part, which for "MAPS\\*.SAV" is "MAPS/" - with the separator
     * still on the end.  compat_resolve_path opens "." for a relative path.
     * Neither spelling is what the filesystem wants, and what it costs is not
     * a message: xlistEnumerate simply finds nothing, fileNameListInit returns
     * zero, and loadsave.cc treats zero files as a failed save - which is what
     * "** Error writing save function #3 data! **" was, with ARTEMPLE.SAV
     * sitting on the card the whole time.
     *
     * So: backslashes become slashes, a trailing separator is dropped, and an
     * empty path or "." means where we are.  A path of "/" keeps its slash.
     */
    char fixed[256];
    size_t n = 0;
    while (path[n] != '\0' && n + 1u < sizeof(fixed)) {
        fixed[n] = (path[n] == '\\') ? '/' : path[n];
        n++;
    }
    fixed[n] = '\0';
    while (n > 1u && fixed[n - 1u] == '/') {
        fixed[--n] = '\0';
    }
    if (n == 0u || (n == 1u && fixed[0] == '.')) {
        fixed[0] = '.';
        fixed[1] = '\0';
    }

    ag_handle_t h = ag_opendir(fixed);
    if (h < 0 && fixed[0] == '.' && fixed[1] == '\0') {
        /* "." is not a name the filesystem knows; the process's own directory
         * is what an empty path means to it. */
        h = ag_opendir("");
    }
    if (h < 0) {
        /*
         * Said out loud, a few times and then never again.  A directory that
         * will not open is invisible everywhere it matters - the save that
         * cannot list its maps, the .DAT search that finds no loose files -
         * and none of those callers report anything at all.  Eight lines is
         * enough to see which path and which error, and few enough that a
         * game which asks constantly does not drown the console.
         */
        static int moaned;
        if (moaned < 8) {
            moaned++;
            ag_printf("opendir(%s) refused: %s\n", fixed, ag_strerror(h));
        }
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

/*
 * Two DOS extensions the map editor still uses.  In place, returning the same
 * pointer, which is what every implementation that ever shipped did.
 *
 * ASCII only: the engine's own text is ASCII, and a case table for anything
 * else would have to know a code page that the game does not tell anyone.
 */
char *strupr(char *s)
{
    if (s != NULL) {
        for (char *p = s; *p != '\0'; ++p) {
            if (*p >= 'a' && *p <= 'z') {
                *p = (char)(*p - 'a' + 'A');
            }
        }
    }
    return s;
}

char *strlwr(char *s)
{
    if (s != NULL) {
        for (char *p = s; *p != '\0'; ++p) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
    }
    return s;
}

}  // extern "C"
