/*
 * ArgonOS - /dev/usbhid0, the keyboard and mouse at the far end of the cable.
 *
 * A character device with almost nothing in it, and that is the design.  A
 * write is a whole number of AG_USBHID_FRAME-byte frames, each one report for
 * one interface, and this file's entire job is to take them apart and hand them
 * down to the port.  Nothing here knows what a keystroke is: the code that
 * turns a letter into a usage code lives in apps/common/kvm, where a host test
 * can reach it without a board.
 *
 * Two decisions worth writing down.
 *
 * Exclusive, because two writers would interleave reports on the same
 * interface, and a keyboard whose press comes from one process and whose
 * release comes from another is a keyboard that sticks.  Whoever holds this
 * device is the only person typing on the far machine.
 *
 * A short write is a real answer, not a failure.  The interrupt endpoint takes
 * one report per millisecond per interface, so a caller that offers ten at once
 * gets back the number of bytes that fitted and is expected to come again -
 * which is exactly what a caller that also has a socket to service wants to
 * hear.  Only a write whose *first* frame does not fit returns -AG_EAGAIN.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/usbhid.h>

#if AG_PORT_HAS_USB_DEVICE

#include <string.h>

#include <argon/device.h>
#include <argon/log.h>

static int32_t usbhid_write(ag_device_t *dev, const void *buf, size_t len,
                            uint64_t off)
{
    (void)dev;
    (void)off;

    if (buf == NULL) {
        return -AG_EINVAL;
    }
    if (len == 0 || (len % AG_USBHID_FRAME) != 0) {
        /*
         * A partial frame is a caller bug rather than a short write: this
         * device has no buffer to hold half a report in until the rest of it
         * turns up, and inventing one would mean holding a key down across an
         * arbitrary gap.
         */
        return -AG_EINVAL;
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t         done = 0;

    while (done < len) {
        ag_usbhid_frame_t f;
        memcpy(&f, p + done, sizeof(f));

        if (f.len > AG_USBHID_DATA_MAX) {
            return done > 0 ? (int32_t)done : -AG_EINVAL;
        }

        const ag_err_t err = ag_port_usbdev_send((ag_usbhid_kind_t)f.kind,
                                                 f.data, f.len);
        if (err != AG_OK) {
            /* Something went already: say how much, and let the caller retry
             * the rest.  Nothing at all: pass the reason up. */
            return done > 0 ? (int32_t)done : (int32_t)err;
        }
        done += AG_USBHID_FRAME;
    }
    return (int32_t)done;
}

static ag_err_t usbhid_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                             size_t arglen)
{
    (void)dev;

    if (cmd != AG_IOC_USBHID_STATUS) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_usbhid_status_t)) {
        return -AG_EINVAL;
    }

    ag_port_usbdev_status_t st;
    const ag_err_t          err = ag_port_usbdev_status(&st);
    if (err != AG_OK) {
        return err;
    }

    ag_usbhid_status_t out;
    memset(&out, 0, sizeof(out));
    out.state = (uint32_t)st.state;
    out.sent = st.sent;
    out.dropped = st.dropped;
    out.leds = st.leds;
    out.protocol = st.protocol;
    memcpy(arg, &out, sizeof(out));
    return AG_OK;
}

static const ag_dev_ops_t k_usbhid_ops = {
    .write = usbhid_write,
    .ioctl = usbhid_ioctl,
};

ag_err_t ag_usbhid_init(void)
{
    const ag_dev_desc_t desc = {
        .name = "usbhid0",
        .driver = "builtin",
        .cls = AG_DEV_CHAR,
        .flags = AG_DEVF_EXCLUSIVE,
        .ops = &k_usbhid_ops,
    };

    const ag_err_t err = ag_dev_register(&desc, NULL);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "usbhid", "cannot register usbhid0: %d",
               (int)err);
    }
    return err;
}

#endif /* AG_PORT_HAS_USB_DEVICE */
