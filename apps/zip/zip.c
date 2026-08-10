/*
 * ArgonOS - ZIP.AXE: create / update / list / extract archives.
 *
 *   zip archive.zip files...          create (store)
 *   zip -u archive.zip files...       update (rewrite with added files)
 *   zip -l archive.zip                list
 *   zip -x archive.zip dest           extract
 *
 * Create/update write store-method (0) ZIPs.  List/extract support store;
 * deflate extract is left to the shell builtin `unzip` (same tinfl stack).
 *
 * Build: see README.md
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("ZIP", "1.0", "argon", 0);

typedef struct {
    char     name[AG_NAME_MAX + 1];
    uint8_t *buf;
    size_t   size;
} staged_file_t;

typedef struct {
    uint32_t local_ofs;
    uint16_t method;
    uint32_t comp_size;
    uint32_t uncomp_size;
    char     name[AG_NAME_MAX + 1];
} zip_cd_ent_t;

static const char *basename_of(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
    }
    return (base[0] != '\0') ? base : path;
}

static int write_fully(ag_handle_t h, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const int32_t w = ag_write(h, (const uint8_t *)buf + done, n - done);
        if (w <= 0) {
            return 1;
        }
        done += (size_t)w;
    }
    return 0;
}

static int read_fully(ag_handle_t h, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const int32_t r = ag_read(h, (uint8_t *)buf + done, n - done);
        if (r <= 0) {
            return 1;
        }
        done += (size_t)r;
    }
    return 0;
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t crc32_zip(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

#define AG_ZIP_MAX_FILES 16

static int write_store_archive_n(const char *archive, staged_file_t *files, int n)
{
    if (n < 0 || n > AG_ZIP_MAX_FILES) {
        ag_print("zip: too many files\n");
        return 1;
    }

    const ag_handle_t h =
        ag_open(archive, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        ag_printf("zip: cannot create %s\n", archive);
        return 1;
    }

    uint32_t local_ofs[AG_ZIP_MAX_FILES];
    uint32_t crc[AG_ZIP_MAX_FILES];
    uint32_t pos = 0;

    for (int i = 0; i < n; i++) {
        const uint16_t namelen = (uint16_t)strlen(files[i].name);
        crc[i] = crc32_zip(files[i].buf, files[i].size);
        local_ofs[i] = pos;

        uint8_t ldh[30];
        put_le32(ldh + 0, 0x04034b50u);
        put_le16(ldh + 4, 20);
        put_le16(ldh + 6, 0);
        put_le16(ldh + 8, 0);
        put_le16(ldh + 10, 0);
        put_le16(ldh + 12, 0);
        put_le32(ldh + 14, crc[i]);
        put_le32(ldh + 18, (uint32_t)files[i].size);
        put_le32(ldh + 22, (uint32_t)files[i].size);
        put_le16(ldh + 26, namelen);
        put_le16(ldh + 28, 0);

        if (write_fully(h, ldh, sizeof(ldh)) != 0 ||
            write_fully(h, files[i].name, namelen) != 0 ||
            (files[i].size > 0 &&
             write_fully(h, files[i].buf, files[i].size) != 0)) {
            ag_close(h);
            ag_print("zip: write failed\n");
            return 1;
        }
        pos += 30u + namelen + (uint32_t)files[i].size;
    }

    const uint32_t cd_ofs = pos;
    for (int i = 0; i < n; i++) {
        const uint16_t namelen = (uint16_t)strlen(files[i].name);
        uint8_t cdh[46];
        put_le32(cdh + 0, 0x02014b50u);
        put_le16(cdh + 4, 20);
        put_le16(cdh + 6, 20);
        put_le16(cdh + 8, 0);
        put_le16(cdh + 10, 0);
        put_le16(cdh + 12, 0);
        put_le16(cdh + 14, 0);
        put_le32(cdh + 16, crc[i]);
        put_le32(cdh + 20, (uint32_t)files[i].size);
        put_le32(cdh + 24, (uint32_t)files[i].size);
        put_le16(cdh + 28, namelen);
        put_le16(cdh + 30, 0);
        put_le16(cdh + 32, 0);
        put_le16(cdh + 34, 0);
        put_le16(cdh + 36, 0);
        put_le32(cdh + 38, 0);
        put_le32(cdh + 42, local_ofs[i]);
        if (write_fully(h, cdh, sizeof(cdh)) != 0 ||
            write_fully(h, files[i].name, namelen) != 0) {
            ag_close(h);
            ag_print("zip: write failed\n");
            return 1;
        }
        pos += 46u + namelen;
    }

    const uint32_t cd_size = pos - cd_ofs;
    uint8_t eocd[22];
    put_le32(eocd + 0, 0x06054b50u);
    put_le16(eocd + 4, 0);
    put_le16(eocd + 6, 0);
    put_le16(eocd + 8, (uint16_t)n);
    put_le16(eocd + 10, (uint16_t)n);
    put_le32(eocd + 12, cd_size);
    put_le32(eocd + 16, cd_ofs);
    put_le16(eocd + 20, 0);
    if (write_fully(h, eocd, sizeof(eocd)) != 0) {
        ag_close(h);
        ag_print("zip: write failed\n");
        return 1;
    }

    ag_close(h);
    return 0;
}

static int stage_file(const char *path, staged_file_t *out)
{
    ag_stat_t st;
    if (ag_stat(path, &st) != AG_OK) {
        ag_printf("zip: missing %s\n", path);
        return 1;
    }
    if (st.attr & AG_A_DIR) {
        ag_printf("zip: skip directory %s\n", path);
        return 1;
    }
    if (st.size > (1024u * 1024u)) {
        ag_printf("zip: too large: %s\n", path);
        return 1;
    }

    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("zip: cannot open %s\n", path);
        return 1;
    }

    out->buf = NULL;
    out->size = (size_t)st.size;
    if (out->size > 0) {
        out->buf = (uint8_t *)ag_malloc(out->size);
        if (out->buf == NULL) {
            ag_close(h);
            ag_print("zip: out of memory\n");
            return 1;
        }
        if (read_fully(h, out->buf, out->size) != 0) {
            ag_free(out->buf);
            out->buf = NULL;
            ag_close(h);
            ag_printf("zip: read failed: %s\n", path);
            return 1;
        }
    }
    ag_close(h);

    const char *base = basename_of(path);
    size_t i = 0;
    for (; base[i] != '\0' && i < AG_NAME_MAX; i++) {
        out->name[i] = base[i];
    }
    out->name[i] = '\0';
    return 0;
}

static int cmd_create(const char *archive, int argc, char **argv)
{
    if (argc <= 0 || argc > AG_ZIP_MAX_FILES) {
        ag_print("zip: need 1..16 files\n");
        return 1;
    }

    staged_file_t files[AG_ZIP_MAX_FILES];
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (stage_file(argv[i], &files[n]) != 0) {
            for (int j = 0; j < n; j++) {
                ag_free(files[j].buf);
            }
            return 1;
        }
        n++;
    }

    const int rc = write_store_archive_n(archive, files, n);
    for (int i = 0; i < n; i++) {
        ag_free(files[i].buf);
    }
    if (rc != 0) {
        return 1;
    }
    ag_printf("created %s (%d file(s))\n", archive, n);
    return 0;
}

static int find_eocd(ag_handle_t h, uint64_t file_size, uint32_t *cd_ofs,
                     uint32_t *cd_size, uint16_t *entries)
{
    if (file_size < 22) {
        return 1;
    }
    const uint32_t scan = (file_size > 65557u) ? 65557u : (uint32_t)file_size;
    uint8_t *buf = (uint8_t *)ag_malloc(scan);
    if (buf == NULL) {
        return 1;
    }
    if (ag_seek(h, (int64_t)(file_size - scan), AG_SEEK_SET) < 0 ||
        read_fully(h, buf, scan) != 0) {
        ag_free(buf);
        return 1;
    }
    int found = 1;
    for (int i = (int)scan - 22; i >= 0; i--) {
        if (rd_le32(buf + i) != 0x06054b50u) {
            continue;
        }
        *entries = rd_le16(buf + i + 10);
        *cd_size = rd_le32(buf + i + 12);
        *cd_ofs = rd_le32(buf + i + 16);
        found = 0;
        break;
    }
    ag_free(buf);
    return found;
}

static int load_central_dir(const char *archive, zip_cd_ent_t **out_ents,
                            int *out_n)
{
    ag_stat_t st;
    if (ag_stat(archive, &st) != AG_OK) {
        ag_printf("zip: cannot open %s\n", archive);
        return 1;
    }
    const ag_handle_t h = ag_open(archive, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("zip: cannot open %s\n", archive);
        return 1;
    }

    uint32_t cd_ofs = 0, cd_size = 0;
    uint16_t entries = 0;
    if (find_eocd(h, st.size, &cd_ofs, &cd_size, &entries) != 0) {
        ag_close(h);
        ag_print("zip: not a zip archive\n");
        return 1;
    }

    uint8_t *cd = (uint8_t *)ag_malloc(cd_size ? cd_size : 1);
    zip_cd_ent_t *ents = (zip_cd_ent_t *)ag_malloc(
        (entries ? entries : 1u) * sizeof(zip_cd_ent_t));
    if (cd == NULL || ents == NULL) {
        ag_free(cd);
        ag_free(ents);
        ag_close(h);
        ag_print("zip: out of memory\n");
        return 1;
    }
    if (cd_size > 0 &&
        (ag_seek(h, (int64_t)cd_ofs, AG_SEEK_SET) < 0 ||
         read_fully(h, cd, cd_size) != 0)) {
        ag_free(cd);
        ag_free(ents);
        ag_close(h);
        return 1;
    }
    ag_close(h);

    size_t at = 0;
    int n = 0;
    while (at + 46 <= cd_size && n < (int)entries) {
        if (rd_le32(cd + at) != 0x02014b50u) {
            break;
        }
        const uint16_t method = rd_le16(cd + at + 10);
        const uint32_t csz = rd_le32(cd + at + 20);
        const uint32_t usz = rd_le32(cd + at + 24);
        const uint16_t namelen = rd_le16(cd + at + 28);
        const uint16_t extra = rd_le16(cd + at + 30);
        const uint16_t comment = rd_le16(cd + at + 32);
        const uint32_t local_ofs = rd_le32(cd + at + 42);
        if (at + 46u + namelen + extra + comment > cd_size) {
            break;
        }
        zip_cd_ent_t *e = &ents[n++];
        e->local_ofs = local_ofs;
        e->method = method;
        e->comp_size = csz;
        e->uncomp_size = usz;
        size_t copy = namelen;
        if (copy > AG_NAME_MAX) {
            copy = AG_NAME_MAX;
        }
        memcpy(e->name, cd + at + 46, copy);
        e->name[copy] = '\0';
        at += 46u + namelen + extra + comment;
    }
    ag_free(cd);
    *out_ents = ents;
    *out_n = n;
    return 0;
}

static int cmd_list(const char *archive)
{
    zip_cd_ent_t *ents = NULL;
    int n = 0;
    if (load_central_dir(archive, &ents, &n) != 0) {
        return 1;
    }
    for (int i = 0; i < n; i++) {
        const char *meth =
            (ents[i].method == 0) ? "store" : (ents[i].method == 8) ? "deflate"
                                                                    : "?";
        ag_printf("  %10u  %-8s %s\n", (unsigned)ents[i].uncomp_size, meth,
                  ents[i].name);
    }
    ag_printf("%d file(s)\n", n);
    ag_free(ents);
    return 0;
}

static bool entry_path_safe(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '/' || name[0] == '\\') {
        return false;
    }
    if (((name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= 'a' && name[0] <= 'z')) &&
        name[1] == ':') {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
            return false;
        }
    }
    return true;
}

static ag_err_t mkdir_parents(const char *path)
{
    char tmp[AG_PATH_MAX];
    size_t n = 0;
    while (path[n] && n + 1 < sizeof(tmp)) {
        tmp[n] = path[n];
        n++;
    }
    tmp[n] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') {
            continue;
        }
        char save = *p;
        *p = '\0';
        ag_stat_t st;
        if (ag_stat(tmp, &st) != AG_OK) {
            ag_err_t err = ag_mkdir(tmp);
            if (err != AG_OK && err != -AG_EEXIST) {
                *p = save;
                return err;
            }
        }
        *p = save;
    }
    ag_stat_t st;
    if (ag_stat(tmp, &st) == AG_OK && (st.attr & AG_A_DIR)) {
        return AG_OK;
    }
    ag_err_t err = ag_mkdir(tmp);
    return (err == -AG_EEXIST) ? AG_OK : err;
}

static int cmd_extract(const char *archive, const char *dest)
{
    zip_cd_ent_t *ents = NULL;
    int n = 0;
    if (load_central_dir(archive, &ents, &n) != 0) {
        return 1;
    }
    if (mkdir_parents(dest) != AG_OK) {
        ag_printf("zip: cannot create %s\n", dest);
        ag_free(ents);
        return 1;
    }

    const ag_handle_t h = ag_open(archive, AG_O_RDONLY);
    if (h < 0) {
        ag_free(ents);
        return 1;
    }

    int failures = 0;
    for (int i = 0; i < n; i++) {
        if (!entry_path_safe(ents[i].name)) {
            failures++;
            continue;
        }
        if (ents[i].method != 0) {
            ag_printf("zip: deflate needs builtin unzip: %s\n", ents[i].name);
            failures++;
            continue;
        }

        char out_path[AG_PATH_MAX];
        size_t k = 0;
        for (; dest[k] && k + 1 < sizeof(out_path); k++) {
            out_path[k] = dest[k];
        }
        if (k > 0 && out_path[k - 1] != '/' && out_path[k - 1] != '\\' &&
            k + 1 < sizeof(out_path)) {
            out_path[k++] = '/';
        }
        for (size_t j = 0; ents[i].name[j] && k + 1 < sizeof(out_path); j++) {
            out_path[k++] =
                (ents[i].name[j] == '\\') ? '/' : ents[i].name[j];
        }
        out_path[k] = '\0';

        uint8_t ldh[30];
        if (ag_seek(h, (int64_t)ents[i].local_ofs, AG_SEEK_SET) < 0 ||
            read_fully(h, ldh, 30) != 0 || rd_le32(ldh) != 0x04034b50u) {
            failures++;
            continue;
        }
        const uint16_t namelen = rd_le16(ldh + 26);
        const uint16_t extra = rd_le16(ldh + 28);
        if (ag_seek(h, (int64_t)ents[i].local_ofs + 30 + namelen + extra,
                    AG_SEEK_SET) < 0) {
            failures++;
            continue;
        }

        uint8_t *data = NULL;
        if (ents[i].comp_size > 0) {
            data = (uint8_t *)ag_malloc(ents[i].comp_size);
            if (data == NULL ||
                read_fully(h, data, ents[i].comp_size) != 0) {
                ag_free(data);
                failures++;
                continue;
            }
        }

        /* parents */
        char *slash = NULL;
        for (char *p = out_path; *p; p++) {
            if (*p == '/') {
                slash = p;
            }
        }
        if (slash) {
            *slash = '\0';
            mkdir_parents(out_path);
            *slash = '/';
        }

        const ag_handle_t out =
            ag_open(out_path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (out < 0) {
            ag_free(data);
            failures++;
            continue;
        }
        if (ents[i].comp_size > 0 &&
            write_fully(out, data, ents[i].comp_size) != 0) {
            failures++;
        } else {
            ag_printf("  %s\n", ents[i].name);
        }
        ag_close(out);
        ag_free(data);
    }

    ag_close(h);
    ag_free(ents);
    return failures ? 1 : 0;
}

static int names_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

static int cmd_update(const char *archive, int argc, char **argv)
{
    /* Update = create with prior store entries kept (deflate entries dropped). */
    staged_file_t files[AG_ZIP_MAX_FILES];
    int n = 0;

    zip_cd_ent_t *ents = NULL;
    int ent_n = 0;
    if (load_central_dir(archive, &ents, &ent_n) == 0) {
        const ag_handle_t h = ag_open(archive, AG_O_RDONLY);
        if (h >= 0) {
            for (int i = 0; i < ent_n && n < AG_ZIP_MAX_FILES; i++) {
                int replaced = 0;
                for (int j = 0; j < argc; j++) {
                    if (names_equal(ents[i].name, basename_of(argv[j]))) {
                        replaced = 1;
                        break;
                    }
                }
                if (replaced || ents[i].method != 0) {
                    continue;
                }
                uint8_t ldh[30];
                if (ag_seek(h, (int64_t)ents[i].local_ofs, AG_SEEK_SET) < 0 ||
                    read_fully(h, ldh, 30) != 0) {
                    continue;
                }
                const uint16_t namelen = rd_le16(ldh + 26);
                const uint16_t extra = rd_le16(ldh + 28);
                if (ag_seek(h,
                            (int64_t)ents[i].local_ofs + 30 + namelen + extra,
                            AG_SEEK_SET) < 0) {
                    continue;
                }
                staged_file_t *dst = &files[n];
                size_t k = 0;
                for (; ents[i].name[k] && k < AG_NAME_MAX; k++) {
                    dst->name[k] = ents[i].name[k];
                }
                dst->name[k] = '\0';
                dst->size = ents[i].uncomp_size;
                dst->buf = NULL;
                if (dst->size > 0) {
                    dst->buf = (uint8_t *)ag_malloc(dst->size);
                    if (dst->buf == NULL ||
                        read_fully(h, dst->buf, dst->size) != 0) {
                        ag_free(dst->buf);
                        continue;
                    }
                }
                n++;
            }
            ag_close(h);
        }
        ag_free(ents);
    }

    for (int i = 0; i < argc && n < AG_ZIP_MAX_FILES; i++) {
        if (stage_file(argv[i], &files[n]) != 0) {
            for (int j = 0; j < n; j++) {
                ag_free(files[j].buf);
            }
            return 1;
        }
        n++;
    }

    char tmp[AG_PATH_MAX];
    size_t nlen = 0;
    while (archive[nlen] && nlen + 5 < sizeof(tmp)) {
        tmp[nlen] = archive[nlen];
        nlen++;
    }
    tmp[nlen++] = '.';
    tmp[nlen++] = 't';
    tmp[nlen++] = 'm';
    tmp[nlen++] = 'p';
    tmp[nlen] = '\0';

    const int rc = write_store_archive_n(tmp, files, n);
    for (int i = 0; i < n; i++) {
        ag_free(files[i].buf);
    }
    if (rc != 0) {
        ag_unlink(tmp);
        return 1;
    }
    ag_unlink(archive);
    if (ag_rename(tmp, archive) != AG_OK) {
        ag_printf("zip: cannot replace %s\n", archive);
        return 1;
    }
    return 0;
}

static void usage(void)
{
    ag_print(
        "usage:\n"
        "  zip archive.zip files...       create (store)\n"
        "  zip -u archive.zip files...    update / add\n"
        "  zip -l archive.zip             list\n"
        "  zip -x archive.zip dest        extract (store)\n");
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (argv[1][0] == '-' && argv[1][1] != '\0' && argv[1][2] == '\0') {
        const char opt = argv[1][1];
        if (opt == 'l') {
            if (argc != 3) {
                usage();
                return 1;
            }
            return cmd_list(argv[2]);
        }
        if (opt == 'x') {
            if (argc != 4) {
                usage();
                return 1;
            }
            return cmd_extract(argv[2], argv[3]);
        }
        if (opt == 'u') {
            if (argc < 4) {
                usage();
                return 1;
            }
            return cmd_update(argv[2], argc - 3, argv + 3);
        }
        usage();
        return 1;
    }

    if (argc < 3) {
        usage();
        return 1;
    }
    return cmd_create(argv[1], argc - 2, argv + 2);
}
