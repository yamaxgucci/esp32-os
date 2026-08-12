/*
 * ArgonOS - the device registry.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/device.h>

#include <string.h>

#include <argon/path.h>

static ag_device_t  s_devices[AG_DEV_MAX];
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
    if (desc->cls <= AG_DEV_ANY || desc->cls > AG_DEV_MOTOR) {
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
    if (dev->open_count > 0) {
        unlock();
        return -AG_EBUSY;
    }
    memset(dev, 0, sizeof(*dev));
    unlock();
    return AG_OK;
}

/* Caller holds the lock.  Returns how many handles are still open on it. */
static uint32_t revoke_locked(ag_device_t *dev)
{
    if (!dev->used) {
        return 0;
    }

    const uint32_t still_open = dev->open_count;
    if (still_open == 0) {
        memset(dev, 0, sizeof(*dev));
    } else {
        dev->revoked = true;
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

    void *session = NULL;
    if (dev->ops->open_session != NULL) {
        if (session_out == NULL) {
            unlock();
            return -AG_EINVAL;
        }
        err = dev->ops->open_session(dev, flags, &session);
        if (err != AG_OK) {
            unlock();
            return err;
        }
    } else if (dev->ops->open != NULL) {
        err = dev->ops->open(dev, flags);
        if (err != AG_OK) {
            unlock();
            return err;
        }
    }
    if (session_out != NULL) {
        *session_out = session;
    }
    dev->open_count++;
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

    ag_err_t err = AG_OK;
    if (!dev->revoked && dev->ops != NULL) {
        if (session != NULL && dev->ops->close_session != NULL) {
            err = dev->ops->close_session(dev, session);
        } else if (dev->ops->close != NULL) {
            err = dev->ops->close(dev);
        }
    }

    dev->open_count--;
    /* A device pulled while it was open disappears with its last handle. */
    if (dev->revoked && dev->open_count == 0) {
        memset(dev, 0, sizeof(*dev));
    }
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
    int32_t n;
    if (session != NULL && dev->ops->read_session != NULL) {
        n = dev->ops->read_session(dev, session, buf, len, off);
    } else if (dev->ops->read != NULL) {
        n = dev->ops->read(dev, buf, len, off);
    } else {
        unlock();
        return -AG_ENOTSUP;
    }
    unlock();
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
    int32_t n;
    if (session != NULL && dev->ops->write_session != NULL) {
        n = dev->ops->write_session(dev, session, buf, len, off);
    } else if (dev->ops->write != NULL) {
        n = dev->ops->write(dev, buf, len, off);
    } else {
        unlock();
        return -AG_ENOTSUP;
    }
    unlock();
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

    if (session != NULL && dev->ops->ioctl_session != NULL) {
        err = dev->ops->ioctl_session(dev, session, cmd, arg, arglen);
    } else if (dev->ops->ioctl != NULL) {
        err = dev->ops->ioctl(dev, cmd, arg, arglen);
    } else {
        err = -AG_ENOTSUP;
    }
    unlock();
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
    default:             return "?";
    }
}
