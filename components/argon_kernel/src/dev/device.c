/*
 * ArgonOS - the device registry.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/device.h>

#include <string.h>

#include <argon/path.h>
#include <argon/port/place.h>

#include "core/place_bss.h"

/*
 * [memory] dev.table, a build-time piece (argon/place.h).  Internal unless the
 * board pack asks otherwise, though it is the largest table the kernel has
 * (5 KB at the Zero's 56 devices): every device call reads it - each frame on
 * a wire, each paint of the console - and in PSRAM those reads came from the
 * other core while a program on this one worked through tables of its own the
 * size of the data cache.  TUBEBENCH lost 2% of a core to it, measured on the
 * S3-Zero (9 Oct 2026); every other table moved to PSRAM together cost 0.
 */
static ag_device_t  s_devices[AG_DEV_MAX] AG_DEV_TABLE_BSS;
static ag_dev_lock_t s_lock;
static bool          s_initialised;

/* ---------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock.lock != NULL) {
        s_lock.lock(s_lock.ctx);
    }
}

static void unlock(void)
{
    if (s_lock.unlock != NULL) {
        s_lock.unlock(s_lock.ctx);
    }
}

/* The direct calls' count (ag_dev_call_begin): its own small lock, taken
 * inside the registry's where both are needed, never around it. */
static void count_lock(void)
{
    if (s_lock.count_lock != NULL) {
        s_lock.count_lock(s_lock.ctx);
    }
}

static void count_unlock(void)
{
    if (s_lock.count_unlock != NULL) {
        s_lock.count_unlock(s_lock.ctx);
    }
}

/* The registry's own lock, for the caller that reaches past the registry into
 * a driver's class vtable.  See the header for why that needs one. */
void ag_dev_lock_hold(void) { lock(); }
void ag_dev_lock_release(void) { unlock(); }

/*
 * Step into a driver, and out of it again.
 *
 * The caller holds the registry lock and has already decided what it is going
 * to call.  enter marks the device busy so the slot cannot be reused, drops the
 * registry lock and takes the device's own one instead; leave undoes all three
 * and returns having dropped the registry lock, which is what every caller
 * below wants next.  See ag_dev_lock_t in the header for why.
 *
 * A device with no lock of its own keeps the old behaviour - the registry lock
 * stays held for the length of the call - so a build that supplies no lock
 * interface, which is every host test, behaves exactly as it did.
 */
static bool call_enter(ag_device_t *dev)
{
    if (dev->iolock == NULL || s_lock.call_hold == NULL) {
        return false;
    }
    dev->calls++;
    unlock();
    s_lock.call_hold(s_lock.ctx, dev->iolock);
    return true;
}

/* Caller holds the lock.  Frees a device that was pulled while it was in use,
 * once the last handle and the last call have gone. */
static void reap_locked(ag_device_t *dev)
{
    if (dev->used && dev->revoked && dev->open_count == 0 && dev->calls == 0 &&
        dev->direct == 0) {
        if (dev->iolock != NULL && s_lock.call_close != NULL) {
            s_lock.call_close(s_lock.ctx, dev->iolock);
        }
        memset(dev, 0, sizeof(*dev));
    }
}

static void call_leave(ag_device_t *dev, bool entered)
{
    if (!entered) {
        unlock();
        return;
    }
    s_lock.call_release(s_lock.ctx, dev->iolock);
    lock();
    if (dev->calls > 0) {
        dev->calls--;
    }
    reap_locked(dev);
    unlock();
}

/*
 * The same, for a caller with bookkeeping still to do: it returns with the
 * registry lock HELD and does not reap, because the caller is about to change
 * open_count and a slot must not be freed between the two.
 */
static void call_leave_held(ag_device_t *dev, bool entered)
{
    if (!entered) {
        return; /* the registry lock was never let go */
    }
    s_lock.call_release(s_lock.ctx, dev->iolock);
    lock();
    if (dev->calls > 0) {
        dev->calls--;
    }
}

ag_err_t ag_dev_init(const ag_dev_lock_t *lock_iface)
{
    memset(s_devices, 0, sizeof(s_devices));
    memset(&s_lock, 0, sizeof(s_lock));
    if (lock_iface != NULL) {
        s_lock = *lock_iface;
    }
    s_initialised = true;
    return AG_OK;
}

/*
 * A device name becomes a path component in /dev, so anything that would make
 * it a different path is refused at registration rather than producing a device
 * nobody can open.  Case is not significant, because the filesystems this
 * system speaks to are not case significant either and "D:\SD0" has to find it.
 */
static bool name_is_sane(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (const char *p = name; *p != '\0'; p++, n++) {
        const unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == 0x7f) {
            return false;
        }
    }
    return n < AG_DEV_NAME_MAX;
}

/*
 * The registry's fields and the ABI's are the same size on purpose, so filling
 * one from the other is a copy of the whole field rather than a string copy
 * that has to be told where to stop - and the compiler is right to complain
 * about the latter, because a name exactly as long as the field would lose its
 * last character.
 */
_Static_assert(sizeof(((ag_devinfo_t *)0)->name) == AG_DEV_NAME_MAX,
               "ag_devinfo_t.name and AG_DEV_NAME_MAX must agree");
_Static_assert(sizeof(((ag_devinfo_t *)0)->driver) == AG_DEV_NAME_MAX,
               "ag_devinfo_t.driver and AG_DEV_NAME_MAX must agree");

static void fill_info(ag_devinfo_t *out, const ag_device_t *dev)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->name, dev->name, sizeof(out->name));
    memcpy(out->driver, dev->driver, sizeof(out->driver));
    out->cls = dev->cls;
    out->flags = dev->flags;
    if (dev->open_count > 0) {
        out->flags |= AG_DEVF_BUSY;
    }
}

static void copy_field(char *dst, const char *src, const char *fallback)
{
    const char *s = (src != NULL && src[0] != '\0') ? src : fallback;
    strncpy(dst, s, AG_DEV_NAME_MAX - 1);
    dst[AG_DEV_NAME_MAX - 1] = '\0';
}

/* Caller holds the lock. */
static ag_device_t *find_locked(const char *name)
{
    for (int i = 0; i < AG_DEV_MAX; i++) {
        ag_device_t *d = &s_devices[i];
        if (d->used && !d->revoked && ag_path_icmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

/*
 * A revoked device keeps its slot until the last handle closes, so a name that
 * has just been pulled cannot be taken by something else in the meantime - a
 * handle to the old sd0 must never start reading a new one.
 */
static bool name_taken_locked(const char *name)
{
    for (int i = 0; i < AG_DEV_MAX; i++) {
        const ag_device_t *d = &s_devices[i];
        if (d->used && ag_path_icmp(d->name, name) == 0) {
            return true;
        }
    }
    return false;
}

ag_err_t ag_dev_register(const ag_dev_desc_t *desc, ag_device_t **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (!s_initialised || desc == NULL || desc->ops == NULL) {
        return -AG_EINVAL;
    }
    if (!name_is_sane(desc->name)) {
        return -AG_EINVAL;
    }
    if (desc->cls <= AG_DEV_ANY || desc->cls > AG_DEV_LINK) {
        return -AG_EINVAL;
    }

    lock();

    if (name_taken_locked(desc->name)) {
        unlock();
        return -AG_EEXIST;
    }

    for (int i = 0; i < AG_DEV_MAX; i++) {
        ag_device_t *d = &s_devices[i];
        if (d->used) {
            continue;
        }
        memset(d, 0, sizeof(*d));
        copy_field(d->name, desc->name, "");
        copy_field(d->driver, desc->driver, "builtin");
        d->cls = desc->cls;
        d->flags = desc->flags;
        d->ops = desc->ops;
        d->class_ops = desc->class_ops;
        d->priv = desc->priv;
        d->owner = desc->owner;
        d->used = true;
        d->iolock =
            (s_lock.call_open != NULL) ? s_lock.call_open(s_lock.ctx) : NULL;

        if (out != NULL) {
            *out = d;
        }
        unlock();
        return AG_OK;
    }

    unlock();
    return -AG_ENFILE;
}

ag_err_t ag_dev_unregister(ag_device_t *dev)
{
    if (dev == NULL) {
        return -AG_EINVAL;
    }

    lock();
    if (!dev->used) {
        unlock();
        return -AG_ENODEV;
    }
    /* A call still inside the driver is as good a reason to refuse as an open
     * handle, and for the stronger one: the memory the vtable points into is
     * about to be handed back. */
    if (dev->open_count > 0 || dev->calls > 0 || dev->direct > 0) {
        unlock();
        return -AG_EBUSY;
    }
    if (dev->iolock != NULL && s_lock.call_close != NULL) {
        s_lock.call_close(s_lock.ctx, dev->iolock);
    }
    memset(dev, 0, sizeof(*dev));
    unlock();
    return AG_OK;
}

/* Caller holds the lock.  Returns how many handles are still open on it. */
#define AG_DEV_REVOKE_WATCHERS 4
static ag_dev_revoke_fn s_revoke_fn[AG_DEV_REVOKE_WATCHERS];

ag_err_t ag_dev_on_revoke(ag_dev_revoke_fn fn)
{
    if (fn == NULL) {
        return -AG_EINVAL;
    }
    for (int i = 0; i < AG_DEV_REVOKE_WATCHERS; i++) {
        if (s_revoke_fn[i] == fn) {
            return AG_OK; /* twice is once */
        }
        if (s_revoke_fn[i] == NULL) {
            s_revoke_fn[i] = fn;
            return AG_OK;
        }
    }
    return -AG_ENFILE;
}

static uint32_t revoke_locked(ag_device_t *dev)
{
    if (!dev->used) {
        return 0;
    }

    /*
     * Before anything else touches the slot: whoever bound this device has to
     * let go of it while the pointer still means something.
     */
    for (int i = 0; i < AG_DEV_REVOKE_WATCHERS; i++) {
        if (s_revoke_fn[i] != NULL) {
            s_revoke_fn[i](dev);
        }
    }

    const uint32_t still_open = dev->open_count;
    /* decided with the direct calls counted and new ones shut out at once:
     * ag_dev_call_begin looks at `revoked` under the same small lock */
    count_lock();
    const bool idle = still_open == 0 && dev->calls == 0 && dev->direct == 0;
    if (!idle) {
        dev->revoked = true;
    }
    count_unlock();
    if (idle) {
        if (dev->iolock != NULL && s_lock.call_close != NULL) {
            s_lock.call_close(s_lock.ctx, dev->iolock);
        }
        memset(dev, 0, sizeof(*dev));
    } else {
        /*
         * The driver is told once, here, rather than once per handle: the
         * hardware is gone, and asking it to close each open would be asking it
         * to touch what is no longer there.
         */
        dev->ops = NULL;
        dev->class_ops = NULL;
    }
    return still_open;
}

uint32_t ag_dev_revoke(ag_device_t *dev)
{
    if (dev == NULL) {
        return 0;
    }

    lock();
    const uint32_t still_open = revoke_locked(dev);
    unlock();
    return still_open;
}

uint32_t ag_dev_calls_of_owner(const void *owner)
{
    uint32_t n = 0;

    lock();
    for (int i = 0; i < AG_DEV_MAX; i++) {
        if (s_devices[i].used && s_devices[i].owner == owner) {
            count_lock();
            n += s_devices[i].calls + s_devices[i].direct;
            count_unlock();
        }
    }
    unlock();
    return n;
}

/*
 * NOT UNDER THE REGISTRY LOCK, and that is the point.  The first version took
 * it for a moment on every call, and so tied every link to it: the console
 * holds the registry across a remote screen's class op, which waits for the
 * far board's answer - and the answer comes in through the link's reader,
 * which now waited for the registry.  Each such op sat out its whole timeout;
 * a program with a screen next door took 80 s to start, and the picture
 * never came (Zero-C3, 25 Sep 2026).  A small lock of its own, taken inside
 * the registry's and never around it.
 */
bool ag_dev_call_begin(ag_device_t *dev)
{
    if (dev == NULL) {
        return false;
    }
    count_lock();
    const bool ok = dev->used && !dev->revoked;
    if (ok) {
        dev->direct++;
    }
    count_unlock();
    return ok;
}

void ag_dev_call_end(ag_device_t *dev)
{
    if (dev == NULL) {
        return;
    }
    count_lock();
    if (dev->direct > 0) {
        dev->direct--;
    }
    const bool last = dev->direct == 0 && dev->revoked;
    count_unlock();
    if (last) {
        /* the device went while this call was out: its slot is freed by the
         * last one to leave - rare, and nobody waits on the registry for it */
        lock();
        reap_locked(dev);
        unlock();
    }
}

uint32_t ag_dev_revoke_owner(const void *owner)
{
    uint32_t gone = 0;

    lock();
    for (int i = 0; i < AG_DEV_MAX; i++) {
        ag_device_t *d = &s_devices[i];
        if (!d->used || d->revoked || d->owner != owner) {
            continue;
        }
        (void)revoke_locked(d);
        gone++;
    }
    unlock();
    return gone;
}

ag_device_t *ag_dev_find(const char *name)
{
    if (!s_initialised || name == NULL) {
        return NULL;
    }

    lock();
    ag_device_t *d = find_locked(name);
    unlock();
    return d;
}

ag_err_t ag_dev_info(uint32_t index, ag_dev_class_t filter, ag_devinfo_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    lock();
    uint32_t seen = 0;
    for (int i = 0; i < AG_DEV_MAX; i++) {
        const ag_device_t *d = &s_devices[i];
        if (!d->used || d->revoked) {
            continue;
        }
        if (filter != AG_DEV_ANY && d->cls != filter) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }

        fill_info(out, d);
        unlock();
        return AG_OK;
    }
    unlock();
    return -AG_ENOENT;
}

uint32_t ag_dev_count(void)
{
    uint32_t n = 0;
    lock();
    for (int i = 0; i < AG_DEV_MAX; i++) {
        if (s_devices[i].used && !s_devices[i].revoked) {
            n++;
        }
    }
    unlock();
    return n;
}

/* ---------------------------------------------------------------------- */
/* Operations                                                             */
/* ---------------------------------------------------------------------- */

/*
 * Every call goes through the same gate.  A revoked device answers -AG_ENODEV
 * rather than reaching a driver that has already given its hardware back, and
 * that answer is the same on every entry point so a holder cannot get one
 * behaviour from read and another from ioctl.  Caller holds the lock.
 */
static ag_err_t usable(const ag_device_t *dev)
{
    if (dev == NULL || !dev->used) {
        return -AG_EINVAL;
    }
    if (dev->revoked || dev->ops == NULL) {
        return -AG_ENODEV;
    }
    return AG_OK;
}

ag_err_t ag_dev_open(ag_device_t *dev, uint32_t flags)
{
    return ag_dev_open2(dev, flags, NULL);
}

ag_err_t ag_dev_open2(ag_device_t *dev, uint32_t flags, void **session_out)
{
    lock();
    ag_err_t err = usable(dev);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if ((dev->flags & AG_DEVF_EXCLUSIVE) && dev->open_count > 0) {
        unlock();
        return -AG_EBUSY;
    }
    if ((dev->flags & AG_DEVF_READONLY) &&
        (flags & (AG_O_WRONLY | AG_O_RDWR | AG_O_TRUNC | AG_O_APPEND)) != 0) {
        unlock();
        return -AG_EROFS;
    }

    /*
     * OPENING IS A DRIVER CALL LIKE ANY OTHER, and on this branch it is the one
     * that matters most.  Reading and writing were let out from under the
     * registry lock when sound made it obvious; opening was left behind, and
     * opening is what a mixer does when it reaches for a converter on the NEXT
     * BOARD - a round trip over the wire, inside which every task here that
     * wanted any device, the console included, was stuck.  That froze node A in
     * about half of runs and is why the remote sink was turned off.
     */
    ag_err_t (*open_session)(ag_device_t *, uint32_t, void **) =
        dev->ops->open_session;
    ag_err_t (*open_plain)(ag_device_t *, uint32_t) =
        (open_session == NULL) ? dev->ops->open : NULL;
    if (open_session != NULL && session_out == NULL) {
        unlock();
        return -AG_EINVAL;
    }

    /*
     * Counted BEFORE the call and undone if it fails.  The registry lock is let
     * go for the length of the driver's open, and an exclusive device must not
     * admit a second opener through that window - which counting afterwards
     * would let it do.
     */
    dev->open_count++;

    void      *session = NULL;
    const bool entered = call_enter(dev);
    if (open_session != NULL) {
        err = open_session(dev, flags, &session);
    } else if (open_plain != NULL) {
        err = open_plain(dev, flags);
    } else {
        err = AG_OK; /* a device with nothing to do to be opened */
    }
    call_leave_held(dev, entered);

    /*
     * The device may have been revoked while we were inside it.  Then the open
     * did succeed, into hardware that is now gone, and the honest answer is the
     * one every other entry point gives for a revoked device.  Its close is not
     * called, for the same reason ag_dev_close2 does not call it: the driver
     * has already been told the hardware went away.
     */
    if (err == AG_OK && (dev->revoked || dev->ops == NULL)) {
        err = -AG_ENODEV;
    }
    if (err != AG_OK) {
        if (dev->open_count > 0) {
            dev->open_count--;
        }
        reap_locked(dev);
        unlock();
        return err;
    }

    if (session_out != NULL) {
        *session_out = session;
    }
    unlock();
    return AG_OK;
}

ag_err_t ag_dev_close(ag_device_t *dev)
{
    return ag_dev_close2(dev, NULL);
}

ag_err_t ag_dev_close2(ag_device_t *dev, void *session)
{
    lock();
    if (dev == NULL || !dev->used || dev->open_count == 0) {
        unlock();
        return -AG_EBADF;
    }

    /*
     * Resolved under the lock, called without it - see ag_dev_open2.  Closing a
     * far device is a round trip too, and the rest of the machine has no more
     * business waiting for it than it had waiting for the open.
     */
    ag_err_t (*close_session)(ag_device_t *, void *) = NULL;
    ag_err_t (*close_plain)(ag_device_t *) = NULL;
    if (!dev->revoked && dev->ops != NULL) {
        if (session != NULL && dev->ops->close_session != NULL) {
            close_session = dev->ops->close_session;
        } else if (dev->ops->close != NULL) {
            close_plain = dev->ops->close;
        }
    }

    ag_err_t   err = AG_OK;
    const bool entered = call_enter(dev);
    if (close_session != NULL) {
        err = close_session(dev, session);
    } else if (close_plain != NULL) {
        err = close_plain(dev);
    }
    call_leave_held(dev, entered);

    if (dev->open_count > 0) {
        dev->open_count--;
    }
    /* A device pulled while it was open disappears with its last handle - or
     * with the last call still inside it, whichever ends later. */
    reap_locked(dev);
    unlock();
    return err;
}

int32_t ag_dev_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    return ag_dev_read2(dev, NULL, buf, len, off);
}

int32_t ag_dev_read2(ag_device_t *dev, void *session, void *buf, size_t len,
                     uint64_t off)
{
    lock();
    ag_err_t err = usable(dev);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (len == 0) {
        unlock();
        return 0;
    }
    int32_t (*fn_session)(ag_device_t *, void *, void *, size_t, uint64_t) =
        (session != NULL) ? dev->ops->read_session : NULL;
    int32_t (*fn)(ag_device_t *, void *, size_t, uint64_t) =
        (fn_session == NULL) ? dev->ops->read : NULL;
    if (fn_session == NULL && fn == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const bool entered = call_enter(dev);
    const int32_t n = (fn_session != NULL)
                          ? fn_session(dev, session, buf, len, off)
                          : fn(dev, buf, len, off);
    call_leave(dev, entered);
    return n;
}

int32_t ag_dev_write(ag_device_t *dev, const void *buf, size_t len,
                     uint64_t off)
{
    return ag_dev_write2(dev, NULL, buf, len, off);
}

int32_t ag_dev_write2(ag_device_t *dev, void *session, const void *buf,
                      size_t len, uint64_t off)
{
    lock();
    ag_err_t err = usable(dev);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (dev->flags & AG_DEVF_READONLY) {
        unlock();
        return -AG_EROFS;
    }
    if (len == 0) {
        unlock();
        return 0;
    }
    int32_t (*fn_session)(ag_device_t *, void *, const void *, size_t,
                          uint64_t) =
        (session != NULL) ? dev->ops->write_session : NULL;
    int32_t (*fn)(ag_device_t *, const void *, size_t, uint64_t) =
        (fn_session == NULL) ? dev->ops->write : NULL;
    if (fn_session == NULL && fn == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const bool entered = call_enter(dev);
    const int32_t n = (fn_session != NULL)
                          ? fn_session(dev, session, buf, len, off)
                          : fn(dev, buf, len, off);
    call_leave(dev, entered);
    return n;
}

ag_err_t ag_dev_ioctl(ag_device_t *dev, uint32_t cmd, void *arg, size_t arglen)
{
    return ag_dev_ioctl2(dev, NULL, cmd, arg, arglen);
}

ag_err_t ag_dev_ioctl2(ag_device_t *dev, void *session, uint32_t cmd, void *arg,
                       size_t arglen)
{
    lock();
    ag_err_t err = usable(dev);
    if (err != AG_OK) {
        unlock();
        return err;
    }

    /*
     * The generic command every device answers, so that a caller holding a
     * handle can ask what it is holding without going back to the name.
     */
    if (cmd == AG_IOC_INFO) {
        if (arg == NULL || arglen < sizeof(ag_devinfo_t)) {
            unlock();
            return -AG_EINVAL;
        }
        fill_info((ag_devinfo_t *)arg, dev);
        unlock();
        return AG_OK;
    }

    ag_err_t (*fn_session)(ag_device_t *, void *, uint32_t, void *, size_t) =
        (session != NULL) ? dev->ops->ioctl_session : NULL;
    ag_err_t (*fn)(ag_device_t *, uint32_t, void *, size_t) =
        (fn_session == NULL) ? dev->ops->ioctl : NULL;
    if (fn_session == NULL && fn == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const bool entered = call_enter(dev);
    err = (fn_session != NULL) ? fn_session(dev, session, cmd, arg, arglen)
                               : fn(dev, cmd, arg, arglen);
    call_leave(dev, entered);
    return err;
}

uint64_t ag_dev_size(ag_device_t *dev)
{
    lock();
    uint64_t n = 0;
    if (usable(dev) == AG_OK && dev->ops->size != NULL) {
        n = dev->ops->size(dev);
    }
    unlock();
    return n;
}

const char *ag_dev_class_name(ag_dev_class_t cls)
{
    switch (cls) {
    case AG_DEV_ANY:     return "any";
    case AG_DEV_BUS:     return "bus";
    case AG_DEV_BLOCK:   return "block";
    case AG_DEV_CHAR:    return "char";
    case AG_DEV_DISPLAY: return "display";
    case AG_DEV_INPUT:   return "input";
    case AG_DEV_SENSOR:  return "sensor";
    case AG_DEV_NET:     return "net";
    case AG_DEV_GPIO:    return "gpio";
    case AG_DEV_AUDIO:   return "audio";
    case AG_DEV_STORAGE: return "storage";
    case AG_DEV_MOTOR:   return "motor";
    case AG_DEV_LINK:    return "link";
    default:             return "?";
    }
}
