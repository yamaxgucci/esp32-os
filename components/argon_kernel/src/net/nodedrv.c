/*
 * ArgonOS - the other node's card, as a drive letter.
 *
 * Channel 5, separate from `link get` on channel 4, because this is no longer
 * one conversation but many at once: every open file is its own.
 *
 * Three things here are easy to get wrong and are done deliberately:
 *
 *   1. A read is synchronous by nature and the link is not, so every request
 *      carries a TAG and a reply is only taken if the tag matches.  Without it
 *      a reply to a request that has already timed out would be handed to
 *      whoever asked next, and two readers would get each other's bytes.
 *
 *   2. Handles are numbers the FAR node made up.  We never index a table of
 *      our own with them.
 *
 *   3. A request may not be made from the link's reader task: the reply would
 *      arrive on the very task that is blocked waiting for it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodedrv.h>

#include <string.h>

#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/vfs.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

#define ND_CHAN 5u
#define ND_HDR 12u
#define ND_MAX_PAYLOAD 232u
#define ND_REPLY 0x80u

enum {
    ND_OPEN = 1,
    ND_CLOSE,
    ND_READ,
    ND_WRITE,
    ND_SEEK,
    ND_STAT,
    ND_UNLINK,
    ND_MKDIR,
    ND_RMDIR,
    ND_OPENDIR,
    ND_READDIR,
    ND_CLOSEDIR,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static bool s_serving;

/*
 * One request outstanding at a time.  Deliberate: it is the shape the rest of
 * this codebase already uses for file work, and it keeps the tag the only
 * thing standing between a late reply and the wrong caller - a rule small
 * enough to be sure of.
 */
static volatile bool     s_busy;
static volatile uint16_t s_tag;
static volatile bool     s_have;
static volatile int32_t  s_status;
static uint8_t           s_rbuf[ND_MAX_PAYLOAD];
static volatile uint16_t s_rlen;

static ag_err_t send_raw(uint8_t op, uint16_t tag, uint32_t h, uint32_t arg,
                         uint8_t cls, const void *pay, uint16_t plen)
{
    uint8_t buf[ND_HDR + ND_MAX_PAYLOAD];
    if (plen > ND_MAX_PAYLOAD) {
        plen = ND_MAX_PAYLOAD;
    }
    buf[0] = op;
    buf[1] = 0;
    put16(buf + 2, tag);
    put32(buf + 4, h);
    put32(buf + 8, arg);
    if (plen > 0 && pay != NULL) {
        memcpy(buf + ND_HDR, pay, plen);
    }
    /* A full window is back-pressure, not a failure; bounded so a departed
     * node cannot block us for ever. */
    for (int t = 0; t < 2000; t++) {
        const int32_t r =
            ag_linkprov_send(ND_CHAN, cls, buf, (uint16_t)(ND_HDR + plen));
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        ag_linkprov_poll(2);
    }
    return -AG_ETIMEDOUT;
}

static int32_t ask(uint8_t op, uint32_t h, uint32_t arg, uint8_t cls,
                   const void *pay, uint16_t plen, void *out, uint16_t *outlen)
{
    static uint16_t next_tag = 1;

    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (ag_linkprov_in_reader()) {
        return -AG_EBUSY;
    }

    const int64_t give_up = ag_port_us() + 4000000;
    while (s_busy) {
        if (ag_port_us() >= give_up) {
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    }
    s_busy = true;
    if (++next_tag == 0) {
        next_tag = 1;
    }
    s_tag = next_tag;
    s_have = false;
    s_rlen = 0;

    int32_t rc = (int32_t)send_raw(op, s_tag, h, arg, cls, pay, plen);
    if (rc == AG_OK) {
        const int64_t deadline = ag_port_us() + 15000000;
        rc = -AG_ETIMEDOUT;
        while (ag_port_us() < deadline) {
            if (s_have) {
                rc = s_status;
                break;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(2));
        }
    }
    if (rc >= 0 && out != NULL && outlen != NULL) {
        uint16_t n = s_rlen;
        if (n > *outlen) {
            n = *outlen;
        }
        memcpy(out, s_rbuf, n);
        *outlen = n;
    }
    /* Clear the tag before the flag: a reply arriving now belongs to nobody. */
    s_tag = 0;
    s_busy = false;
    return rc;
}

/* ---- answering the other node ----------------------------------------- */

static void reply(uint8_t op, uint16_t tag, int32_t status, uint8_t cls,
                  const void *pay, uint16_t plen)
{
    (void)send_raw((uint8_t)(op | ND_REPLY), tag, (uint32_t)status, 0, cls, pay,
                   plen);
}

static uint16_t enc_stat(uint8_t *p, const ag_stat_t *st)
{
    put64(p, st->size);
    put64(p + 8, st->mtime);
    put32(p + 16, st->attr);
    return 20;
}
static void dec_stat(const uint8_t *p, ag_stat_t *st)
{
    st->size = get64(p);
    st->mtime = get64(p + 8);
    st->attr = get32(p + 16);
}

static void serve(const uint8_t *b, uint16_t n)
{
    const uint8_t  op = b[0];
    const uint16_t tag = get16(b + 2);
    const uint32_t h = get32(b + 4);
    const uint32_t arg = get32(b + 8);
    const uint8_t *pay = b + ND_HDR;
    const uint16_t plen = (uint16_t)(n - ND_HDR);
    char           path[AG_PATH_MAX];
    uint8_t        obuf[ND_MAX_PAYLOAD];

    uint16_t pc = plen;
    if (pc >= sizeof(path)) {
        pc = (uint16_t)(sizeof(path) - 1);
    }
    memcpy(path, pay, pc);
    path[pc] = '\0';

    switch (op) {
    case ND_OPEN:
        reply(op, tag, (int32_t)ag_vfs_open(path, "/", arg), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_OPENDIR:
        reply(op, tag, (int32_t)ag_vfs_opendir(path, "/"), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_CLOSE:
        reply(op, tag, (int32_t)ag_vfs_close((ag_handle_t)h), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_CLOSEDIR:
        reply(op, tag, (int32_t)ag_vfs_closedir((ag_handle_t)h),
              AG_LINK_CONTROL, NULL, 0);
        break;
    case ND_UNLINK:
        reply(op, tag, (int32_t)ag_vfs_unlink(path, "/"), AG_LINK_CONTROL, NULL,
              0);
        break;
    case ND_MKDIR:
        reply(op, tag, (int32_t)ag_vfs_mkdir(path, "/"), AG_LINK_CONTROL, NULL,
              0);
        break;
    case ND_RMDIR:
        reply(op, tag, (int32_t)ag_vfs_rmdir(path, "/"), AG_LINK_CONTROL, NULL,
              0);
        break;
    case ND_READ: {
        uint32_t want = arg;
        if (want > ND_MAX_PAYLOAD) {
            want = ND_MAX_PAYLOAD;
        }
        const int32_t r = ag_vfs_read((ag_handle_t)h, obuf, want);
        /* BULK: file bytes may wait behind anything with a deadline. */
        reply(op, tag, r, AG_LINK_BULK, obuf, (uint16_t)((r > 0) ? r : 0));
        break;
    }
    case ND_WRITE:
        reply(op, tag, ag_vfs_write((ag_handle_t)h, pay, plen), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_SEEK: {
        const int64_t off = (plen >= 8) ? (int64_t)get64(pay) : 0;
        const int64_t r = ag_vfs_seek((ag_handle_t)h, off, (int)arg);
        if (r < 0) {
            reply(op, tag, (int32_t)r, AG_LINK_CONTROL, NULL, 0);
        } else {
            put64(obuf, (uint64_t)r);
            reply(op, tag, 0, AG_LINK_CONTROL, obuf, 8);
        }
        break;
    }
    case ND_STAT: {
        ag_stat_t      st;
        const ag_err_t e = ag_vfs_stat(path, "/", &st);
        if (e != AG_OK) {
            reply(op, tag, (int32_t)e, AG_LINK_CONTROL, NULL, 0);
        } else {
            reply(op, tag, 0, AG_LINK_CONTROL, obuf, enc_stat(obuf, &st));
        }
        break;
    }
    case ND_READDIR: {
        ag_dirent_t    de;
        const ag_err_t e = ag_vfs_readdir((ag_handle_t)h, &de);
        if (e != AG_OK) {
            reply(op, tag, (int32_t)e, AG_LINK_CONTROL, NULL, 0);
        } else {
            const uint16_t k = enc_stat(obuf, &de.st);
            size_t         nl = strlen(de.name);
            if (nl > (size_t)(ND_MAX_PAYLOAD - k - 1u)) {
                nl = (size_t)(ND_MAX_PAYLOAD - k - 1u);
            }
            memcpy(obuf + k, de.name, nl);
            obuf[k + nl] = '\0';
            reply(op, tag, 0, AG_LINK_CONTROL, obuf, (uint16_t)(k + nl + 1));
        }
        break;
    }
    default:
        reply(op, tag, -AG_ENOTSUP, AG_LINK_CONTROL, NULL, 0);
        break;
    }
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < ND_HDR) {
        return;
    }
    const uint8_t *b = (const uint8_t *)data;

    if ((b[0] & ND_REPLY) == 0) {
        serve(b, n);
        return;
    }

    /* A reply.  Only the one we are waiting for, matched by tag. */
    const uint16_t tag = get16(b + 2);
    if (!s_busy || s_have || tag == 0 || tag != s_tag) {
        return;
    }
    s_status = (int32_t)get32(b + 4);
    uint16_t plen = (uint16_t)(n - ND_HDR);
    if (plen > sizeof(s_rbuf)) {
        plen = (uint16_t)sizeof(s_rbuf);
    }
    memcpy(s_rbuf, b + ND_HDR, plen);
    s_rlen = plen;
    s_have = true;
}

ag_err_t ag_nodedrv_serve(void)
{
    if (s_serving) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_linkprov_listen(ND_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }
    s_serving = true;
    return AG_OK;
}

/* ---- the backend ------------------------------------------------------- */

static uint32_t fh(void *file)
{
    return (uint32_t)(uintptr_t)file - 1u;
}

static ag_err_t nd_open(void *c, const char *rel, uint32_t flags, void **file)
{
    (void)c;
    const int32_t r = ask(ND_OPEN, 0, flags, AG_LINK_CONTROL, rel,
                          (uint16_t)strlen(rel), NULL, NULL);
    if (r < 0) {
        return (ag_err_t)r;
    }
    /* +1 so handle 0 is not NULL, which the core reads as "no file". */
    *file = (void *)(uintptr_t)(uint32_t)(r + 1);
    return AG_OK;
}
static ag_err_t nd_close(void *c, void *file)
{
    (void)c;
    return (ag_err_t)ask(ND_CLOSE, fh(file), 0, AG_LINK_CONTROL, NULL, 0, NULL,
                         NULL);
}
static int32_t nd_read(void *c, void *file, void *buf, size_t len)
{
    (void)c;
    if (len > ND_MAX_PAYLOAD) {
        len = ND_MAX_PAYLOAD;
    }
    uint16_t got = (uint16_t)len;
    return ask(ND_READ, fh(file), (uint32_t)len, AG_LINK_CONTROL, NULL, 0, buf,
               &got);
}
static int32_t nd_write(void *c, void *file, const void *buf, size_t len)
{
    (void)c;
    if (len > ND_MAX_PAYLOAD) {
        len = ND_MAX_PAYLOAD;
    }
    return ask(ND_WRITE, fh(file), 0, AG_LINK_BULK, buf, (uint16_t)len, NULL,
               NULL);
}
static int64_t nd_seek(void *c, void *file, int64_t off, int whence)
{
    (void)c;
    uint8_t  p[8];
    uint8_t  o[8];
    uint16_t on = (uint16_t)sizeof(o);
    put64(p, (uint64_t)off);
    const int32_t r =
        ask(ND_SEEK, fh(file), (uint32_t)whence, AG_LINK_CONTROL, p, 8, o, &on);
    if (r < 0) {
        return r;
    }
    return (on >= 8) ? (int64_t)get64(o) : 0;
}
static ag_err_t nd_stat(void *c, const char *rel, ag_stat_t *out)
{
    (void)c;
    /*
     * The mount root is a directory by definition, and answering that here
     * rather than over the wire is not an optimisation: `dir N:\` stats the
     * path first to decide whether it is a directory or a filename pattern, so
     * a root that cannot be stat'ed is a root that gets listed as a PATTERN -
     * which is what it did, and it looked like an empty far card rather than
     * like a failure.
     */
    if (rel == NULL || rel[0] == '\0' ||
        (rel[0] == '/' && rel[1] == '\0')) {
        memset(out, 0, sizeof(*out));
        out->attr = AG_A_DIR;
        return AG_OK;
    }
    uint8_t       o[20];
    uint16_t      on = (uint16_t)sizeof(o);
    const int32_t r = ask(ND_STAT, 0, 0, AG_LINK_CONTROL, rel,
                          (uint16_t)strlen(rel), o, &on);
    if (r < 0) {
        return (ag_err_t)r;
    }
    if (on < 20) {
        return -AG_EIO;
    }
    dec_stat(o, out);
    return AG_OK;
}
static ag_err_t nd_path_op(uint8_t op, const char *rel)
{
    return (ag_err_t)ask(op, 0, 0, AG_LINK_CONTROL, rel, (uint16_t)strlen(rel),
                         NULL, NULL);
}
static ag_err_t nd_unlink(void *c, const char *rel)
{
    (void)c;
    return nd_path_op(ND_UNLINK, rel);
}
static ag_err_t nd_mkdir(void *c, const char *rel)
{
    (void)c;
    return nd_path_op(ND_MKDIR, rel);
}
static ag_err_t nd_rmdir(void *c, const char *rel)
{
    (void)c;
    return nd_path_op(ND_RMDIR, rel);
}
static ag_err_t nd_opendir(void *c, const char *rel, void **dir)
{
    (void)c;
    const int32_t r = ask(ND_OPENDIR, 0, 0, AG_LINK_CONTROL, rel,
                          (uint16_t)strlen(rel), NULL, NULL);
    if (r < 0) {
        return (ag_err_t)r;
    }
    *dir = (void *)(uintptr_t)(uint32_t)(r + 1);
    return AG_OK;
}
static ag_err_t nd_readdir(void *c, void *dir, ag_dirent_t *out)
{
    (void)c;
    uint8_t       o[ND_MAX_PAYLOAD];
    uint16_t      on = (uint16_t)sizeof(o);
    const int32_t r =
        ask(ND_READDIR, fh(dir), 0, AG_LINK_CONTROL, NULL, 0, o, &on);
    if (r < 0) {
        return (ag_err_t)r;
    }
    if (on < 21) {
        return -AG_EIO;
    }
    memset(out, 0, sizeof(*out));
    dec_stat(o, &out->st);
    o[on - 1] = '\0';
    ag_strlcpy(out->name, (const char *)(o + 20), sizeof(out->name));
    return AG_OK;
}
static ag_err_t nd_closedir(void *c, void *dir)
{
    (void)c;
    return (ag_err_t)ask(ND_CLOSEDIR, fh(dir), 0, AG_LINK_CONTROL, NULL, 0,
                         NULL, NULL);
}
static ag_err_t nd_info(void *c, ag_fsinfo_t *out)
{
    (void)c;
    memset(out, 0, sizeof(*out));
    ag_strlcpy(out->fs, "node", sizeof(out->fs));
    out->removable = true;
    return AG_OK;
}

static const ag_fs_ops_t nd_ops = {
    .name = "node",
    .open = nd_open,
    .close = nd_close,
    .read = nd_read,
    .write = nd_write,
    .seek = nd_seek,
    .stat = nd_stat,
    .unlink = nd_unlink,
    .mkdir = nd_mkdir,
    .rmdir = nd_rmdir,
    .opendir = nd_opendir,
    .readdir = nd_readdir,
    .closedir = nd_closedir,
    .info = nd_info,
};

ag_err_t ag_nodedrv_mount(const char *mountpoint)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_nodedrv_serve();
    if (e != AG_OK) {
        return e;
    }
    return ag_vfs_mount(mountpoint, &nd_ops, NULL, AG_MOUNT_REMOVABLE);
}

ag_err_t ag_nodedrv_unmount(const char *mountpoint)
{
    return ag_vfs_unmount(mountpoint);
}
