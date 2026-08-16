/*
 * ArgonOS - the devices the system itself provides.
 *
 * Three of them, and they are the three DOS had for the same reasons: a sink
 * that swallows output, a source that produces nothing but zeros, and the
 * console as a file so that `copy d:\con t:\notes.txt` needs no new code.
 * Everything else comes from a driver - built into the image, like the card
 * reader in storage.c, or loaded later as a .SYS module.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dev/devices.h"

#include <string.h>

#include <argon/codepage.h>
#include <argon/console.h>
#include <argon/devfs.h>
#include <argon/device.h>
#include <argon/audio.h>
#include <argon/display.h>
#include <argon/input.h>
#include <argon/keys.h>
#include <argon/log.h>
#include <argon/net.h>
#include <argon/vfs.h>

#include <argon/port/sync.h>
#include <argon/port/task.h>

#include "dev/io.h"
#include "fs/storage.h"

static ag_port_mutex_t s_dev_mutex;

/*
 * Recursive, because a device operation is entitled to ask the registry about
 * itself, and because the shell reads the registry from inside a listing it is
 * already walking.
 */
static void dev_lock(void *ctx)
{
    (void)ctx;
    ag_port_mutex_take_recursive(s_dev_mutex, AG_PORT_FOREVER);
}

static void dev_unlock(void *ctx)
{
    (void)ctx;
    ag_port_mutex_give_recursive(s_dev_mutex);
}

/* ---------------------------------------------------------------------- */
/* null - everything written to it is gone, and there is nothing to read   */
/* ---------------------------------------------------------------------- */

static int32_t null_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    (void)buf;
    (void)len;
    (void)off;
    return 0; /* end of file, immediately and always */
}

static int32_t null_write(ag_device_t *dev, const void *buf, size_t len,
                          uint64_t off)
{
    (void)dev;
    (void)buf;
    (void)off;
    return (int32_t)len; /* accepted, and forgotten */
}

static const ag_dev_ops_t k_null_ops = {
    .read = null_read,
    .write = null_write,
};

/* ---------------------------------------------------------------------- */
/* zero - an endless supply of nothing, for wiping and for measuring       */
/* ---------------------------------------------------------------------- */

static int32_t zero_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    (void)off;
    memset(buf, 0, len);
    return (int32_t)len;
}

static const ag_dev_ops_t k_zero_ops = {
    .read = zero_read,
    .write = null_write,
};

/* ---------------------------------------------------------------------- */
/* con - the console, as a file                                            */
/* ---------------------------------------------------------------------- */

static int32_t con_write(ag_device_t *dev, const void *buf, size_t len,
                         uint64_t off)
{
    (void)dev;
    (void)off;
    ag_console_write((const char *)buf, len);
    return (int32_t)len;
}

/*
 * Reads a line, echoing it, and treats Ctrl+Z as the end of the input - which
 * is what DOS did and what anyone typing `copy d:\con t:\notes.txt` expects.
 * Returning 0 is the end of file the copy is waiting for.
 *
 * Events rather than characters, because Ctrl+Z is not a character: the input
 * layer reports control codes as the key that was pressed, with no character at
 * all, so a reader that only asks for characters can never see the one thing it
 * has to see.  That cost an evening of `copy` never ending.
 *
 * This blocks until the line is typed, and it blocks with the filesystem lock
 * held, so a background process reading a file waits for the operator.  Said
 * out loud in docs/05-status.md rather than hidden: the fix is for the VFS to
 * let go of its lock around a backend transfer, which is the same fix the
 * console renderer needs and is not this change.
 */
static int32_t con_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    (void)off;

    char  *out = (char *)buf;
    size_t n = 0;

    while (n < len) {
        ag_event_t ev;
        if (!ag_console_read_event(&ev, UINT32_MAX)) {
            return (n > 0) ? (int32_t)n : -AG_EIO;
        }
        /* Being asked to stop ends the input, with whatever arrived so far. */
        if (ev.type == AG_EV_QUIT) {
            break;
        }
        if (ev.type != AG_EV_KEY_DOWN) {
            continue;
        }

        /* Ctrl+Z, the end of typed input since CP/M. */
        if ((ev.key.mods & AG_MOD_CTRL) && ev.key.keycode == AG_KEY_Z) {
            break;
        }
        if (ev.key.unicode == 0) {
            continue; /* a key with nothing to put in the file */
        }
        if (ev.key.unicode == '\r' || ev.key.unicode == '\n') {
            ag_console_puts("\r\n");
            out[n++] = '\n';
            break;
        }
        if (ev.key.unicode == 0x08) {
            if (n > 0) {
                n--;
                ag_console_puts("\b \b");
            }
            continue;
        }

        /* A byte in the active code page, for the same reason getch gives one:
         * what goes into a file is bytes, and the page says which. */
        const int32_t byte = ag_cp_from_unicode(ag_cp_active(),
                                                ev.key.unicode);
        if (byte < 0) {
            continue;
        }
        const char ch = (char)byte;
        out[n++] = ch;
        ag_console_write(&ch, 1);
    }
    return (int32_t)n;
}

static const ag_dev_ops_t k_con_ops = {
    .read = con_read,
    .write = con_write,
};

/* ---------------------------------------------------------------------- */

static void register_builtin(const char *name, ag_dev_class_t cls,
                             const ag_dev_ops_t *ops, uint32_t flags)
{
    const ag_dev_desc_t desc = {
        .name = name,
        .driver = "builtin",
        .cls = cls,
        .flags = flags,
        .ops = ops,
    };

    const ag_err_t err = ag_dev_register(&desc, NULL);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "dev", "cannot register '%s': %d", name,
               (int)err);
    }
}

ag_err_t ag_devices_init(void)
{
    if (s_dev_mutex == NULL) {
        s_dev_mutex = ag_port_mutex_new_recursive();
        if (s_dev_mutex == NULL) {
            return -AG_ENOMEM;
        }
    }

    const ag_dev_lock_t lock = {
        .lock = dev_lock,
        .unlock = dev_unlock,
        .ctx = NULL,
    };
    ag_err_t err = ag_dev_init(&lock);
    if (err != AG_OK) {
        return err;
    }

    ag_devfs_reset();
    err = ag_vfs_mount("/dev", ag_devfs_ops(), NULL, 0);
    if (err != AG_OK) {
        return err;
    }

    register_builtin("null", AG_DEV_CHAR, &k_null_ops, 0);
    register_builtin("zero", AG_DEV_CHAR, &k_zero_ops, 0);
    register_builtin("con", AG_DEV_CHAR, &k_con_ops, 0);

    /*
     * Pins and buses.  No bus is touched here - ag_io_init only writes down
     * which pins the system is already using, so that nothing can take them.
     * A bus comes up the first time somebody uses it.
     */
    err = ag_io_init();
    if (err != AG_OK) {
        return err;
    }
    ag_io_register_devices();

    /* Whatever storage found while mounting: the flash partition, and later
     * the card, which registers itself when it is mounted. */
    ag_storage_register_devices();

    /* Soft RGB565 framebuffer (or later a panel driver).  BOARD.CFG sizes it. */
    err = ag_display_init();
    if (err != AG_OK) {
        return err;
    }

    /* PCM out: I2S when BOARD.CFG pins are set, else discard stub. */
    err = ag_audio_init();
    if (err != AG_OK) {
        return err;
    }

    /* Pad layer and /dev/joy0.  HostFS PADPUSH is one source into it. */
    err = ag_input_init();
    if (err != AG_OK) {
        return err;
    }

    /*
     * Networking is optional and non-fatal: OpenEth only exists under QEMU.
     * api->net is still present when the build enables it; ready() stays
     * false until DHCP succeeds.
     */
    err = ag_net_init();
    if (err != AG_OK && err != -AG_ENOSYS) {
        ag_log(AG_LOG_WARN, "dev", "net init failed (%d)", (int)err);
    }

    ag_log(AG_LOG_INFO, "dev", "%u devices", (unsigned)ag_dev_count());
    return AG_OK;
}
