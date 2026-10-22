/*
 * ArgonOS - /dev/usbcdc0, a serial port to the machine at the far end.
 *
 * The other half of the cable that /dev/usbhid0 types into.  There, this board
 * pretends to be a keyboard so the far machine needs no software; here it
 * pretends to be a serial port so that software over there, when there is any,
 * has a way to talk back.
 *
 * The only reason it exists is the picture.  A PC does not send its screen out
 * over USB - there is no class for it - so the only way to see that screen is a
 * program running over there, and this is the wire that program uses.  What
 * comes down it is a stream of messages this file does not look inside: it is a
 * byte pipe, and the framing belongs to the application, which is where the
 * knowledge of what a message means already lives.
 *
 * Thin on purpose, the same way the HID device is: no buffering of its own, no
 * framing, no queue.  A read takes what has arrived, a write takes what fits,
 * and both say how much.  Anything cleverer here would be a second buffer in
 * front of TinyUSB's, paid for in the internal RAM that the radio also wants.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/usbhid.h>

#if AG_PORT_HAS_USB_CDC

#include <string.h>

#include <argon/device.h>
#include <argon/log.h>

static int32_t usbcdc_read(ag_device_t *dev, void *buf, size_t len,
                           uint64_t off)
{
    (void)dev;
    (void)off;
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (len > 0x7FFFFFFFu) {
        len = 0x7FFFFFFFu;
    }
    return ag_port_usbdev_cdc_read(buf, (uint32_t)len);
}

static int32_t usbcdc_write(ag_device_t *dev, const void *buf, size_t len,
                            uint64_t off)
{
    (void)dev;
    (void)off;
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (len > 0x7FFFFFFFu) {
        len = 0x7FFFFFFFu;
    }
    return ag_port_usbdev_cdc_write(buf, (uint32_t)len);
}

static ag_err_t usbcdc_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                             size_t arglen)
{
    (void)dev;

    if (cmd != AG_IOC_USBCDC_STATUS) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_usbcdc_status_t)) {
        return -AG_EINVAL;
    }

    ag_port_usbdev_cdc_status_t st;
    const ag_err_t              err = ag_port_usbdev_cdc_status(&st);
    if (err != AG_OK) {
        return err;
    }

    ag_usbcdc_status_t out;
    memset(&out, 0, sizeof(out));
    out.state = (uint32_t)st.state;
    out.rx = st.rx;
    out.tx = st.tx;
    memcpy(arg, &out, sizeof(out));
    return AG_OK;
}

static const ag_dev_ops_t k_usbcdc_ops = {
    .read = usbcdc_read,
    .write = usbcdc_write,
    .ioctl = usbcdc_ioctl,
};

ag_err_t ag_usbcdc_init(void)
{
    const ag_dev_desc_t desc = {
        .name = "usbcdc0",
        .driver = "builtin",
        .cls = AG_DEV_CHAR,
        .flags = AG_DEVF_EXCLUSIVE,
        .ops = &k_usbcdc_ops,
    };

    const ag_err_t err = ag_dev_register(&desc, NULL);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "usbcdc", "cannot register usbcdc0: %d",
               (int)err);
    }
    return err;
}

#endif /* AG_PORT_HAS_USB_CDC */
