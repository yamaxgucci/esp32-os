/*
 * ArgonOS - the device registry.
 *
 * One table of everything the system can talk to, whether it is a built-in
 * (the console, the null sink), a controller inside the chip, or a chip on a
 * bus brought up by a loadable .SYS module.  A device has a name, a class, a
 * driver that owns it and a small set of operations; nothing else is required
 * of it, and nothing here knows what any particular device does.
 *
 * A device is a byte addressable object, and the position belongs to the
 * handle rather than to the device.  That is what lets the same registry serve
 * both a stream (the console, which ignores the offset) and a disk (the card,
 * where the offset is where to read), and it is why devices can be reached
 * through the filesystem as /dev/name - see devfs.h.  DOS did the same thing
 * with CON, PRN and NUL, and for the same reason: one set of file calls is
 * fewer than two.
 *
 * No dependency on FreeRTOS or the chip, so this is built and tested on the
 * host.  Matching a .SYS to a chip on a bus lives next door, in probe.h: the
 * registry only stores what was registered, not how the driver was chosen.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DEVICE_H
#define ARGON_DEVICE_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many devices can exist at once.  A bound rather than a list that grows,
 * for the same reason the resource list has one: a bound that is reached is a
 * diagnosable event, and the registry has to work when memory is short.
 */
#define AG_DEV_MAX 24

/*
 * Public because a driver needs its own priv pointer back in every callback,
 * and because the shell and the tests read the bookkeeping.  The last three
 * fields belong to the registry; a driver must not write them.
 *
 * The ops table and AG_DEV_NAME_MAX live in the ABI: a .SYS sees the same
 * definitions through <argon/abi.h>.
 */
struct ag_device {
    char           name[AG_DEV_NAME_MAX];
    char           driver[AG_DEV_NAME_MAX];
    ag_dev_class_t cls;
    uint32_t       flags; /* ag_dev_flags                                   */

    const ag_dev_ops_t *ops;
    /*
     * The class vtable, handed back by dev->ops() in the ABI: ag_display_ops_t
     * for a display, ag_sensor_ops_t for a sensor.  NULL when the class calls
     * are all the device has.
     */
    const void *class_ops;
    void       *priv;
    /*
     * Whoever registered it.  NULL is the kernel; a loaded .SYS passes its
     * module, so unloading the module can take its devices with it.
     */
    const void *owner;

    uint32_t open_count;
    bool     used;
    bool     revoked; /* the hardware went away, handles are still open     */
};

typedef struct {
    const char         *name;   /* required, unique, no separators          */
    const char         *driver; /* who implements it, for `dev` and the log */
    ag_dev_class_t      cls;
    uint32_t            flags;
    const ag_dev_ops_t *ops;
    const void         *class_ops;
    void               *priv;
    const void         *owner;
} ag_dev_desc_t;

/*
 * Serialisation is supplied rather than assumed, exactly as for the VFS, so the
 * registry can be built and tested without FreeRTOS.  Pass NULL for no locking.
 *
 * The lock must be recursive: a device operation is entitled to ask the
 * registry about itself.
 */
typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} ag_dev_lock_t;

ag_err_t ag_dev_init(const ag_dev_lock_t *lock);

/*
 * Adds a device.  -AG_EEXIST for a name already taken, -AG_EINVAL for a name
 * that is empty, too long or contains a path separator, -AG_ENFILE when the
 * table is full.  The descriptor is copied; `out` may be NULL.
 */
ag_err_t ag_dev_register(const ag_dev_desc_t *desc, ag_device_t **out);

/*
 * Removes a device that nobody is using.  -AG_EBUSY while a handle is open:
 * a driver unloading itself has to be told, not obeyed.
 */
ag_err_t ag_dev_unregister(ag_device_t *dev);

/*
 * The card was pulled.  The device stops being findable immediately and every
 * operation on an already open handle returns -AG_ENODEV, so the holders learn
 * about it on their next call instead of reaching a driver that is gone.  The
 * slot is freed by the last close.  Returns the number of handles still open.
 *
 * The same treatment the VFS gives an ejected mount, and for the same reason:
 * waiting for the holders to close would mean waiting for a process that may
 * never run again.
 */
uint32_t ag_dev_revoke(ag_device_t *dev);

/* Revokes everything one module registered.  Returns how many devices went. */
uint32_t ag_dev_revoke_owner(const void *owner);

/* NULL when there is no such device, or when it has been revoked. */
ag_device_t *ag_dev_find(const char *name);

/*
 * Iterates the registry.  `filter` is AG_DEV_ANY for everything.  Returns
 * -AG_ENOENT past the end, which is how a caller knows to stop.
 */
ag_err_t ag_dev_info(uint32_t index, ag_dev_class_t filter, ag_devinfo_t *out);

uint32_t ag_dev_count(void);

/*
 * Open accounting lives here rather than in the caller, because exclusivity is
 * a property of the device: AG_DEVF_EXCLUSIVE makes a second open fail with
 * -AG_EBUSY, which is the whole point of the flag for a bus or a port.
 */
ag_err_t ag_dev_open(ag_device_t *dev, uint32_t flags);
ag_err_t ag_dev_close(ag_device_t *dev);

int32_t  ag_dev_read(ag_device_t *dev, void *buf, size_t len, uint64_t off);
int32_t  ag_dev_write(ag_device_t *dev, const void *buf, size_t len,
                      uint64_t off);
ag_err_t ag_dev_ioctl(ag_device_t *dev, uint32_t cmd, void *arg, size_t arglen);
uint64_t ag_dev_size(ag_device_t *dev);

/* "char", "storage", "display" - for `dev` and for the journal. */
const char *ag_dev_class_name(ag_dev_class_t cls);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_DEVICE_H */
