/*
 * ArgonOS - the file and disk example.
 *
 * Build it with the SDK's image tool and run it from the shell:
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o DISK.AXE apps/disk/disk.c
 *   run t:\disk.axe            (or: run t:\disk.axe c:\sys)
 *
 * Everything an application does with storage, in the order it would do it:
 * find out which drives exist, list a directory, write a file, read it back,
 * check what came back is what went in, and delete it.  It reports the outcome
 * and returns non-zero if any step failed, so it is a test as well as an
 * example - `run` leaves the code in `errorlevel`.
 *
 * The one habit worth copying from here: every call is checked and every failure
 * is printed with ag_strerror.  A file system is where an application meets the
 * outside world - a missing card, a full disk, a read-only medium - and "it did
 * not work" without the reason is the one message nobody can act on.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("DISK", "1.0", "argon", 0);

/* Text with a length that is known, so the read-back can be compared exactly. */
static const char k_content[] =
    "ArgonOS disk example\r\n"
    "line two\r\n";

static int s_failures;

/* Prints the reason rather than the number, and counts the failure. */
static void failed(const char *what, ag_err_t err)
{
    ag_printf("  %s: %s (%d)\n", what, ag_strerror(err), (int)err);
    s_failures++;
}

/* ---- the drives ---------------------------------------------------------- */

static void show_drives(void)
{
    static const char *const k_mounts[] = {"/sd", "/sys", "/tmp"};

    ag_print("drives:\n");
    for (unsigned i = 0; i < sizeof(k_mounts) / sizeof(k_mounts[0]); i++) {
        ag_fsinfo_t fs;
        const ag_err_t err = ag_mountinfo(k_mounts[i], &fs);
        if (err != AG_OK) {
            ag_printf("  %-6s not mounted\n", k_mounts[i]);
            continue;
        }
        ag_printf("  %-6s %-4s %u KB total, %u KB free%s%s\n", k_mounts[i],
                  fs.fs, (unsigned)(fs.total / 1024u),
                  (unsigned)(fs.free / 1024u),
                  fs.read_only ? ", read-only" : "",
                  fs.removable ? ", removable" : "");
    }
}

/* ---- a directory -------------------------------------------------------- */

static void list_directory(const char *path)
{
    const ag_handle_t d = ag_opendir(path);
    if (d < 0) {
        failed(path, d);
        return;
    }

    ag_printf("directory of %s:\n", path);

    ag_dirent_t ent;
    unsigned    files = 0, dirs = 0;
    while (ag_readdir(d, &ent) == AG_OK) {
        if (ent.st.attr & AG_A_DIR) {
            ag_printf("  %-24s <DIR>\n", ent.name);
            dirs++;
        } else {
            ag_printf("  %-24s %u\n", ent.name, (unsigned)ent.st.size);
            files++;
        }
    }
    ag_closedir(d);
    ag_printf("  %u file(s), %u directory(ies)\n", files, dirs);
}

/* ---- write, read back, compare, delete ---------------------------------- */

static void round_trip(const char *dir)
{
    char path[AG_PATH_MAX];
    ag_strlcpy(path, dir, sizeof(path));
    ag_strlcat(path, "/disktest.txt", sizeof(path));

    /* AG_O_TRUNC because a leftover from a previous run would be read back as
     * a pass if the new content happened to be shorter. */
    ag_handle_t h = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        failed(path, h);
        return;
    }

    /* strlen, not sizeof: the terminator belongs to C, not to the file. */
    const size_t  want = strlen(k_content);
    const int32_t written = ag_write(h, k_content, want);
    if (written < 0) {
        failed("write", written);
    } else if ((size_t)written != want) {
        ag_printf("  write: %d of %u bytes\n", (int)written, (unsigned)want);
        s_failures++;
    }

    /* Before close, so a power cut between the two is the medium's problem and
     * not this program's; close alone does not promise the bytes are down. */
    const ag_err_t synced = ag_sync(h);
    if (synced != AG_OK) {
        failed("sync", synced);
    }
    ag_close(h);

    /* What the system says about the file, before reading a byte of it. */
    ag_stat_t st;
    const ag_err_t stated = ag_stat(path, &st);
    if (stated != AG_OK) {
        failed("stat", stated);
    } else if (st.size != (uint64_t)want) {
        ag_printf("  stat: %u bytes on disk, %u written\n",
                  (unsigned)st.size, (unsigned)want);
        s_failures++;
    }

    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        failed(path, h);
        return;
    }

    /*
     * A chunk at a time, and never assuming one read returns everything asked
     * for: a short read is normal at the end of a file and legal anywhere.
     */
    char     got[64];
    size_t   at = 0;
    bool     same = true;
    int32_t  n;
    while ((n = ag_read(h, got, sizeof(got))) > 0) {
        for (int32_t i = 0; i < n; i++) {
            if (at + (size_t)i >= want || got[i] != k_content[at + i]) {
                same = false;
            }
        }
        at += (size_t)n;
    }
    if (n < 0) {
        failed("read", n);
    }
    ag_close(h);

    if (at != want || !same) {
        ag_printf("  read back %u bytes of %u, content %s\n", (unsigned)at,
                  (unsigned)want, same ? "matches" : "DIFFERS");
        s_failures++;
    } else {
        ag_printf("wrote and read back %u bytes at %s\n", (unsigned)want, path);
    }

    const ag_err_t removed = ag_unlink(path);
    if (removed != AG_OK) {
        failed("delete", removed);
    }

    /* And it really is gone: stat is how you ask, and -AG_ENOENT is the answer
     * that means the delete happened. */
    if (ag_stat(path, &st) != -AG_ENOENT) {
        ag_print("  the file is still there after being deleted\n");
        s_failures++;
    }
}

int ag_main(int argc, char **argv)
{
    /*
     * argv[1] is where to work, and the RAM disk is the default because it is
     * always there and nothing on it matters.  A relative path would be resolved
     * against this process's own working directory, which the shell gave it.
     */
    const char *dir = (argc > 1) ? argv[1] : "T:\\";

    char cwd[AG_PATH_MAX];
    if (ag_getcwd(cwd, sizeof(cwd)) == AG_OK) {
        ag_printf("started in %s\n\n", cwd);
    }

    show_drives();
    ag_print("\n");

    /* chdir is per process: this does not move the shell, which gets its own
     * directory back the moment this program ends. */
    const ag_err_t moved = ag_chdir(dir);
    if (moved != AG_OK) {
        failed(dir, moved);
        return 1;
    }

    list_directory(".");
    ag_print("\n");
    round_trip(".");

    ag_printf("\n%s\n", (s_failures == 0) ? "all checks passed"
                                          : "some checks failed");
    return (s_failures == 0) ? 0 : 1;
}
