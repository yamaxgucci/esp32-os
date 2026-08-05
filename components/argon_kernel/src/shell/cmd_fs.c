/*
 * ArgonOS - filesystem shell commands.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "shell/cmd_fs.h"

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/path.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#define AG_COPY_CHUNK 512

/* ---------------------------------------------------------------------- */
/* Shared plumbing                                                        */
/* ---------------------------------------------------------------------- */

static void print_error(const char *what, ag_err_t err)
{
    const char *text;
    switch (-err) {
    case AG_ENOENT:  text = "file not found"; break;
    case AG_EISDIR:  text = "is a directory"; break;
    case AG_ENOTDIR: text = "not a directory"; break;
    case AG_EEXIST:  text = "already exists"; break;
    case AG_EBUSY:   text = "directory not empty"; break;
    case AG_EROFS:   text = "write protected"; break;
    case AG_ENOSPC:  text = "insufficient disk space"; break;
    case AG_EIO:     text = "drive not ready"; break;
    case AG_EACCES:  text = "access denied"; break;
    case AG_EPERM:   text = "not permitted across drives"; break;
    case AG_ENFILE:  text = "too many open files"; break;
    case AG_EINVAL:  text = "invalid path"; break;
    default:         text = "error"; break;
    }
    ag_console_printf("%s: %s\n", what, text);
}

/*
 * Splits a user-supplied argument into a directory to walk and a wildcard to
 * match.  "T:\apps\*.axe" becomes /tmp/apps plus "*.axe"; a plain directory
 * becomes itself plus "*".
 */
static ag_err_t split_pattern(const char *arg, char *dir, size_t dirlen,
                              char *pattern, size_t patlen)
{
    char resolved[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(arg, ag_shell_cwd(), resolved,
                                   sizeof(resolved));
    if (err != AG_OK) {
        return err;
    }

    ag_stat_t st;
    if (ag_vfs_stat(resolved, NULL, &st) == AG_OK && (st.attr & AG_A_DIR)) {
        snprintf(dir, dirlen, "%s", resolved);
        snprintf(pattern, patlen, "*");
        return AG_OK;
    }

    /* Not an existing directory, so the tail is a name or a wildcard. */
    const char *leaf = ag_path_basename(resolved);
    snprintf(pattern, patlen, "%s", (leaf[0] != '\0') ? leaf : "*");
    return ag_path_dirname(resolved, dir, dirlen);
}

/* Free space on the mount that holds `path`, or 0 when it cannot be told. */
static uint64_t free_space_of(const char *path)
{
    uint64_t best_free = 0;
    size_t   best_len = 0;

    for (uint32_t i = 0;; i++) {
        ag_mountinfo_t mi;
        if (ag_vfs_mount_info(i, &mi) != AG_OK) {
            break;
        }
        const size_t len = strlen(mi.mount);
        if (strncmp(path, mi.mount, len) != 0) {
            continue;
        }
        if (path[len] != '\0' && path[len] != '/') {
            continue;
        }
        if (len >= best_len) {
            best_len = len;
            best_free = mi.info.free;
        }
    }
    return best_free;
}

/* ---------------------------------------------------------------------- */

int ag_cmd_dir(int argc, char **argv)
{
    char dir[AG_PATH_MAX];
    char pattern[AG_NAME_MAX];

    if (argc > 1) {
        const ag_err_t err = split_pattern(argv[1], dir, sizeof(dir), pattern,
                                          sizeof(pattern));
        if (err != AG_OK) {
            print_error(argv[1], err);
            return 1;
        }
    } else {
        snprintf(dir, sizeof(dir), "%s", ag_shell_cwd());
        snprintf(pattern, sizeof(pattern), "*");
    }

    const ag_handle_t d = ag_vfs_opendir(dir, NULL);
    if (d < 0) {
        print_error(dir, d);
        return 1;
    }

    char dos[AG_PATH_MAX];
    ag_shell_dos_path(dir, dos, sizeof(dos));
    ag_console_printf("\n Directory of %s\n\n", dos);

    uint32_t files = 0;
    uint32_t dirs = 0;
    uint64_t bytes = 0;
    ag_dirent_t ent;

    while (ag_vfs_readdir(d, &ent) == AG_OK) {
        if (!ag_path_match(pattern, ent.name)) {
            continue;
        }
        if (ent.st.attr & AG_A_DIR) {
            ag_console_printf("%-32s   <DIR>\n", ent.name);
            dirs++;
        } else {
            ag_console_printf("%-32s %8u\n", ent.name,
                              (unsigned)ent.st.size);
            files++;
            bytes += ent.st.size;
        }
    }
    ag_vfs_closedir(d);

    ag_console_printf("\n%8u file(s) %12u bytes\n", (unsigned)files,
                      (unsigned)bytes);
    ag_console_printf("%8u dir(s)  %12u bytes free\n", (unsigned)dirs,
                      (unsigned)free_space_of(dir));
    return 0;
}

int ag_cmd_cd(int argc, char **argv)
{
    if (argc < 2) {
        /* With no argument DOS printed where you are, rather than going home. */
        char dos[AG_PATH_MAX];
        ag_shell_dos_path(ag_shell_cwd(), dos, sizeof(dos));
        ag_console_printf("%s\n", dos);
        return 0;
    }

    char resolved[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(argv[1], ag_shell_cwd(), resolved,
                                   sizeof(resolved));
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }

    ag_stat_t st;
    err = ag_vfs_stat(resolved, NULL, &st);
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }
    if (!(st.attr & AG_A_DIR)) {
        print_error(argv[1], -AG_ENOTDIR);
        return 1;
    }

    err = ag_shell_set_cwd(resolved);
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }
    return 0;
}

int ag_cmd_type(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: type <file>\n");
        return 1;
    }

    const ag_handle_t h = ag_vfs_open(argv[1], ag_shell_cwd(), AG_O_RDONLY);
    if (h < 0) {
        print_error(argv[1], h);
        return 1;
    }

    char    chunk[AG_COPY_CHUNK];
    int32_t n;
    while ((n = ag_vfs_read(h, chunk, sizeof(chunk))) > 0) {
        ag_console_write(chunk, (size_t)n);
    }
    ag_vfs_close(h);

    if (n < 0) {
        print_error(argv[1], n);
        return 1;
    }
    ag_console_puts("\n");
    return 0;
}

int ag_cmd_copy(int argc, char **argv)
{
    if (argc < 3) {
        ag_console_puts("usage: copy <source> <destination>\n");
        return 1;
    }

    /*
     * A destination that is a directory means "into it under the same name",
     * which is what everyone expects from copy.
     */
    char dest[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(argv[2], ag_shell_cwd(), dest, sizeof(dest));
    if (err != AG_OK) {
        print_error(argv[2], err);
        return 1;
    }

    ag_stat_t st;
    if (ag_vfs_stat(dest, NULL, &st) == AG_OK && (st.attr & AG_A_DIR)) {
        char joined[AG_PATH_MAX];
        if (ag_path_join(dest, ag_path_basename(argv[1]), joined,
                         sizeof(joined)) != AG_OK) {
            print_error(argv[2], -AG_ERANGE);
            return 1;
        }
        snprintf(dest, sizeof(dest), "%s", joined);
    }

    const ag_handle_t in = ag_vfs_open(argv[1], ag_shell_cwd(), AG_O_RDONLY);
    if (in < 0) {
        print_error(argv[1], in);
        return 1;
    }

    const ag_handle_t out = ag_vfs_open(dest, NULL,
                                        AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (out < 0) {
        ag_vfs_close(in);
        print_error(dest, out);
        return 1;
    }

    char     chunk[AG_COPY_CHUNK];
    uint64_t total = 0;
    int32_t  n;
    int      status = 0;

    while ((n = ag_vfs_read(in, chunk, sizeof(chunk))) > 0) {
        const int32_t written = ag_vfs_write(out, chunk, (size_t)n);
        if (written < 0) {
            print_error(dest, written);
            status = 1;
            break;
        }
        if (written != n) {
            print_error(dest, -AG_ENOSPC);
            status = 1;
            break;
        }
        total += (uint64_t)written;
    }
    if (n < 0) {
        print_error(argv[1], n);
        status = 1;
    }

    ag_vfs_close(in);
    ag_vfs_close(out);

    if (status == 0) {
        ag_console_printf("        1 file(s) copied, %u bytes\n",
                          (unsigned)total);
    }
    return status;
}

int ag_cmd_del(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: del <file or pattern>\n");
        return 1;
    }

    char dir[AG_PATH_MAX];
    char pattern[AG_NAME_MAX];
    ag_err_t err = split_pattern(argv[1], dir, sizeof(dir), pattern,
                                 sizeof(pattern));
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }

    /*
     * One match is found and deleted per pass rather than collecting a list
     * first.  Walking a directory while deleting from it is reading a list
     * that is being edited, and a fixed-size list would put an arbitrary cap
     * on how many files "del *" can remove.
     */
    uint32_t deleted = 0;
    uint32_t failures = 0;

    for (;;) {
        char victim[AG_PATH_MAX];
        bool found = false;

        const ag_handle_t d = ag_vfs_opendir(dir, NULL);
        if (d < 0) {
            print_error(dir, d);
            return 1;
        }
        ag_dirent_t ent;
        while (ag_vfs_readdir(d, &ent) == AG_OK) {
            if ((ent.st.attr & AG_A_DIR) || !ag_path_match(pattern, ent.name)) {
                continue;
            }
            if (ag_path_join(dir, ent.name, victim, sizeof(victim)) != AG_OK) {
                continue;
            }
            found = true;
            break;
        }
        ag_vfs_closedir(d);

        if (!found) {
            break;
        }

        const ag_err_t e = ag_vfs_unlink(victim, NULL);
        if (e == AG_OK) {
            deleted++;
        } else {
            /* Stop rather than spin: the same entry would match again. */
            print_error(ag_path_basename(victim), e);
            failures++;
            break;
        }
    }

    if (deleted == 0 && failures == 0) {
        print_error(argv[1], -AG_ENOENT);
        return 1;
    }

    ag_console_printf("%8u file(s) deleted\n", (unsigned)deleted);
    return (failures == 0) ? 0 : 1;
}

int ag_cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: md <path>\n");
        return 1;
    }
    const ag_err_t err = ag_vfs_mkdir(argv[1], ag_shell_cwd());
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }
    return 0;
}

int ag_cmd_rmdir(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: rd <path>\n");
        return 1;
    }
    const ag_err_t err = ag_vfs_rmdir(argv[1], ag_shell_cwd());
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }
    return 0;
}

int ag_cmd_rename(int argc, char **argv)
{
    if (argc < 3) {
        ag_console_puts("usage: ren <old> <new>\n");
        return 1;
    }

    /*
     * DOS took a bare name as the second argument and kept the directory.
     * Accepting a full path too costs nothing and surprises nobody.
     */
    char to[AG_PATH_MAX];
    if (strchr(argv[2], '/') == NULL && strchr(argv[2], '\\') == NULL &&
        strchr(argv[2], ':') == NULL) {
        char from[AG_PATH_MAX];
        char dir[AG_PATH_MAX];
        if (ag_path_resolve(argv[1], ag_shell_cwd(), from, sizeof(from)) !=
                AG_OK ||
            ag_path_dirname(from, dir, sizeof(dir)) != AG_OK ||
            ag_path_join(dir, argv[2], to, sizeof(to)) != AG_OK) {
            print_error(argv[2], -AG_EINVAL);
            return 1;
        }
    } else {
        snprintf(to, sizeof(to), "%s", argv[2]);
    }

    const ag_err_t err = ag_vfs_rename(argv[1], to, ag_shell_cwd());
    if (err != AG_OK) {
        print_error(argv[1], err);
        return 1;
    }
    return 0;
}

int ag_cmd_mount(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_console_puts("drive  mounted on   type        total        free  open\n");

    for (uint32_t i = 0;; i++) {
        ag_mountinfo_t mi;
        if (ag_vfs_mount_info(i, &mi) != AG_OK) {
            break;
        }

        char dos[AG_PATH_MAX];
        ag_shell_dos_path(mi.mount, dos, sizeof(dos));

        ag_console_printf("%-6s %-12s %-5s %8u KB %8u KB %5u%s\n", dos,
                          mi.mount, mi.info.fs,
                          (unsigned)(mi.info.total / 1024),
                          (unsigned)(mi.info.free / 1024),
                          (unsigned)mi.open_handles,
                          (mi.flags & AG_MOUNT_READONLY) ? "  read-only" : "");
    }
    return 0;
}

int ag_cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: hexdump <file>\n");
        return 1;
    }

    const ag_handle_t h = ag_vfs_open(argv[1], ag_shell_cwd(), AG_O_RDONLY);
    if (h < 0) {
        print_error(argv[1], h);
        return 1;
    }

    uint8_t  row[16];
    uint32_t offset = 0;
    int32_t  n;

    while ((n = ag_vfs_read(h, row, sizeof(row))) > 0) {
        ag_console_printf("%08x  ", (unsigned)offset);
        for (int32_t i = 0; i < 16; i++) {
            if (i < n) {
                ag_console_printf("%02x ", row[i]);
            } else {
                ag_console_puts("   ");
            }
            if (i == 7) {
                ag_console_puts(" ");
            }
        }
        ag_console_puts(" |");
        for (int32_t i = 0; i < n; i++) {
            const char c = (row[i] >= 0x20 && row[i] < 0x7f) ? (char)row[i]
                                                             : '.';
            ag_console_write(&c, 1);
        }
        ag_console_puts("|\n");
        offset += (uint32_t)n;
    }
    ag_vfs_close(h);

    if (n < 0) {
        print_error(argv[1], n);
        return 1;
    }
    return 0;
}
