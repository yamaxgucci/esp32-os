/*
 * ArgonOS - unzip: list or extract a ZIP archive (store / deflate).
 *
 *   unzip archive.zip           list
 *   unzip archive.zip dest\     extract into dest
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "shell/cmd_unzip.h"

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/path.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include "esp_heap_caps.h"

#include "miniz.h"

#define AG_UNZIP_CHUNK 4096

typedef struct {
    ag_handle_t h;
} zip_io_t;

static void *ag_mz_alloc(void *opaque, size_t items, size_t size)
{
    (void)opaque;
    const size_t n = items * size;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void ag_mz_free(void *opaque, void *addr)
{
    (void)opaque;
    heap_caps_free(addr);
}

static void *ag_mz_realloc(void *opaque, void *addr, size_t items, size_t size)
{
    (void)opaque;
    const size_t n = items * size;
    void *p = heap_caps_realloc(addr, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL && n > 0) {
        p = heap_caps_realloc(addr, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static size_t zip_read(void *opaque, mz_uint64 file_ofs, void *buf, size_t n)
{
    zip_io_t *io = (zip_io_t *)opaque;
    if (ag_vfs_seek(io->h, (int64_t)file_ofs, AG_SEEK_SET) < 0) {
        return 0;
    }
    size_t got = 0;
    while (got < n) {
        const int32_t r = ag_vfs_read(io->h, (uint8_t *)buf + got, n - got);
        if (r < 0) {
            return got;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    return got;
}

static void print_err(const char *what, const char *detail)
{
    if (detail != NULL && detail[0] != '\0') {
        ag_console_printf("%s: %s\n", what, detail);
    } else {
        ag_console_printf("%s\n", what);
    }
}

/* Reject absolute paths, drive letters, and ".." components (zip-slip). */
static bool entry_path_safe(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (name[0] == '/' || name[0] == '\\') {
        return false;
    }
    if (((name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= 'a' && name[0] <= 'z')) &&
        name[1] == ':') {
        return false;
    }

    const char *p = name;
    while (*p != '\0') {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
            return false;
        }
        while (*p != '\0' && *p != '/' && *p != '\\') {
            p++;
        }
        while (*p == '/' || *p == '\\') {
            p++;
        }
    }
    return true;
}

/* Copy name with backslashes → slashes into out. */
static void normalize_entry_name(const char *name, char *out, size_t outlen)
{
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < outlen; i++) {
        out[i] = (name[i] == '\\') ? '/' : name[i];
    }
    out[i] = '\0';
    /* Drop trailing slash for file join (directories handled separately). */
    while (i > 0 && out[i - 1] == '/') {
        out[--i] = '\0';
    }
}

static ag_err_t mkdir_parents(const char *path)
{
    char tmp[AG_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        return -AG_ERANGE;
    }

    /* Skip leading slash of absolute path; create each intermediate. */
    char *p = tmp;
    if (*p == '/') {
        p++;
    }
    for (; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        ag_stat_t st;
        if (ag_vfs_stat(tmp, NULL, &st) != AG_OK) {
            const ag_err_t err = ag_vfs_mkdir(tmp, NULL);
            if (err != AG_OK && err != -AG_EEXIST) {
                *p = '/';
                return err;
            }
        }
        *p = '/';
    }

    ag_stat_t st;
    if (ag_vfs_stat(tmp, NULL, &st) == AG_OK && (st.attr & AG_A_DIR)) {
        return AG_OK;
    }
    const ag_err_t err = ag_vfs_mkdir(tmp, NULL);
    return (err == -AG_EEXIST) ? AG_OK : err;
}

static const char *method_name(mz_uint16 method)
{
    if (method == 0) {
        return "store";
    }
    if (method == 8) {
        return "deflate";
    }
    return "other";
}

static int list_archive(mz_zip_archive *zip)
{
    const mz_uint n = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < n; i++) {
        if (ag_shell_interrupted()) {
            return 1;
        }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st)) {
            print_err("unzip", "cannot read entry");
            return 1;
        }
        if (st.m_is_directory) {
            ag_console_printf("  %10s  %-8s  %s\n", "<DIR>", "", st.m_filename);
        } else {
            ag_console_printf("  %10llu  %-8s  %s\n",
                              (unsigned long long)st.m_uncomp_size,
                              method_name(st.m_method), st.m_filename);
        }
    }
    ag_console_printf("%u file(s)\n", (unsigned)n);
    return 0;
}

static int extract_one(mz_zip_archive *zip, mz_uint index,
                       const char *dest_root)
{
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, index, &st)) {
        print_err("unzip", "cannot read entry");
        return 1;
    }
    if (!entry_path_safe(st.m_filename)) {
        ag_console_printf("unzip: unsafe path: %s\n", st.m_filename);
        return 1;
    }

    char rel[AG_PATH_MAX];
    normalize_entry_name(st.m_filename, rel, sizeof(rel));
    if (rel[0] == '\0') {
        return 0;
    }

    char out_path[AG_PATH_MAX];
    if (ag_path_join(dest_root, rel, out_path, sizeof(out_path)) != AG_OK) {
        print_err("unzip", "path too long");
        return 1;
    }

    if (st.m_is_directory) {
        const ag_err_t err = mkdir_parents(out_path);
        if (err != AG_OK) {
            ag_console_printf("unzip: mkdir %s failed\n", out_path);
            return 1;
        }
        return 0;
    }

    if (!st.m_is_supported) {
        ag_console_printf("unzip: unsupported method for %s (%s)\n",
                          st.m_filename, method_name(st.m_method));
        return 1;
    }

    char parent[AG_PATH_MAX];
    if (ag_path_dirname(out_path, parent, sizeof(parent)) == AG_OK &&
        strcmp(parent, "/") != 0) {
        const ag_err_t err = mkdir_parents(parent);
        if (err != AG_OK) {
            ag_console_printf("unzip: mkdir %s failed\n", parent);
            return 1;
        }
    }

    const ag_handle_t out =
        ag_vfs_open(out_path, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (out < 0) {
        ag_console_printf("unzip: cannot create %s\n", out_path);
        return 1;
    }

    mz_zip_reader_extract_iter_state *iter =
        mz_zip_reader_extract_iter_new(zip, index, 0);
    if (iter == NULL) {
        ag_vfs_close(out);
        print_err("unzip", mz_zip_get_error_string(mz_zip_get_last_error(zip)));
        return 1;
    }

    static uint8_t chunk[AG_UNZIP_CHUNK];
    int rc = 0;
    for (;;) {
        if (ag_shell_interrupted()) {
            rc = 1;
            break;
        }
        const size_t n =
            mz_zip_reader_extract_iter_read(iter, chunk, sizeof(chunk));
        if (n == 0) {
            break;
        }
        size_t done = 0;
        while (done < n) {
            const int32_t w =
                ag_vfs_write(out, chunk + done, (uint32_t)(n - done));
            if (w <= 0) {
                ag_console_printf("unzip: write failed: %s\n", out_path);
                rc = 1;
                break;
            }
            done += (size_t)w;
        }
        if (rc != 0) {
            break;
        }
    }

    if (!mz_zip_reader_extract_iter_free(iter) && rc == 0) {
        print_err("unzip", "extract failed");
        rc = 1;
    }
    ag_vfs_close(out);
    if (rc == 0) {
        ag_console_printf("  %s\n", rel);
    }
    return rc;
}

static int extract_archive(mz_zip_archive *zip, const char *dest)
{
    const mz_uint n = mz_zip_reader_get_num_files(zip);
    int failures = 0;
    for (mz_uint i = 0; i < n; i++) {
        if (ag_shell_interrupted()) {
            return 1;
        }
        failures += extract_one(zip, i, dest);
    }
    if (failures == 0) {
        ag_console_printf("%u file(s) extracted\n", (unsigned)n);
    }
    return (failures != 0) ? 1 : 0;
}

int ag_cmd_unzip(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        ag_console_printf("usage: unzip <archive.zip> [dest]\n");
        return 1;
    }

    char zip_path[AG_PATH_MAX];
    if (ag_path_resolve(argv[1], ag_shell_cwd(), zip_path, sizeof(zip_path)) !=
        AG_OK) {
        print_err(argv[1], "invalid path");
        return 1;
    }

    ag_stat_t st;
    if (ag_vfs_stat(zip_path, NULL, &st) != AG_OK) {
        print_err(argv[1], "file not found");
        return 1;
    }
    if (st.attr & AG_A_DIR) {
        print_err(argv[1], "is a directory");
        return 1;
    }

    const ag_handle_t h = ag_vfs_open(zip_path, NULL, AG_O_RDONLY);
    if (h < 0) {
        print_err(argv[1], "cannot open");
        return 1;
    }

    zip_io_t io = { .h = h };
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pAlloc = ag_mz_alloc;
    zip.m_pFree = ag_mz_free;
    zip.m_pRealloc = ag_mz_realloc;
    zip.m_pRead = zip_read;
    zip.m_pIO_opaque = &io;

    if (!mz_zip_reader_init(&zip, st.size, 0)) {
        ag_vfs_close(h);
        print_err("unzip", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return 1;
    }

    int rc;
    if (argc == 2) {
        rc = list_archive(&zip);
    } else {
        char dest[AG_PATH_MAX];
        if (ag_path_resolve(argv[2], ag_shell_cwd(), dest, sizeof(dest)) !=
            AG_OK) {
            print_err(argv[2], "invalid path");
            rc = 1;
        } else {
            ag_stat_t dst;
            if (ag_vfs_stat(dest, NULL, &dst) != AG_OK) {
                const ag_err_t err = mkdir_parents(dest);
                if (err != AG_OK) {
                    print_err(argv[2], "cannot create destination");
                    rc = 1;
                    goto done;
                }
            } else if (!(dst.attr & AG_A_DIR)) {
                print_err(argv[2], "not a directory");
                rc = 1;
                goto done;
            }
            rc = extract_archive(&zip, dest);
        }
    }

done:
    mz_zip_reader_end(&zip);
    ag_vfs_close(h);
    return rc;
}
