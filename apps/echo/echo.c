/*
 * ArgonOS - a tiny loadable .SYS driver.
 *
 * Publishes a character device named `echo`: whatever is written to it can be
 * read back.  Small enough to prove the module path - load, register, use,
 * unload, devices gone - without needing a chip on a bus.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\ECHO.SYS apps/echo/echo.c
 *   drv load t:\echo.sys
 *   copy t:\note.txt d:\echo
 *   type d:\echo
 *   drv unload ECHO
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("ECHO", "1.0", "argon");

#define ECHO_CAP 64

typedef struct {
    char   buf[ECHO_CAP];
    size_t len;
} echo_state_t;

static echo_state_t s_echo;

static int32_t echo_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    echo_state_t *st = (echo_state_t *)ag_dev_priv(dev);
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (off >= st->len) {
        return 0;
    }
    size_t n = st->len - (size_t)off;
    if (n > len) {
        n = len;
    }
    memcpy(buf, st->buf + (size_t)off, n);
    return (int32_t)n;
}

static int32_t echo_write(ag_device_t *dev, const void *buf, size_t len,
                          uint64_t off)
{
    (void)off;
    echo_state_t *st = (echo_state_t *)ag_dev_priv(dev);
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (len > ECHO_CAP) {
        len = ECHO_CAP;
    }
    memcpy(st->buf, buf, len);
    st->len = len;
    return (int32_t)len;
}

static uint64_t echo_size(ag_device_t *dev)
{
    echo_state_t *st = (echo_state_t *)ag_dev_priv(dev);
    return (st != NULL) ? st->len : 0;
}

static const ag_dev_ops_t k_echo_ops = {
    .read = echo_read,
    .write = echo_write,
    .size = echo_size,
};

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }

    s_echo.len = 0;

    const ag_dev_add_t desc = {
        .name = "echo",
        .driver = "ECHO",
        .cls = AG_DEV_CHAR,
        .flags = 0,
        .ops = &k_echo_ops,
        .priv = &s_echo,
    };
    return ag_dev_add(&desc);
}
