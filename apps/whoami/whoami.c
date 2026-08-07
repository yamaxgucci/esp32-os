/*
 * ArgonOS - a probe-aware .SYS example.
 *
 * When loaded by `drv probe` (or boot modules.probe), it learns the matching
 * I2C bus and address from ag_probe_hint() and publishes a character device
 * `whoami` that reads back that location.  Loaded by plain `drv load` it still
 * registers, with bus/addr left as "none".
 *
 *   [modules]
 *   probe = 0:0x76:t:\whoami.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("WHOAMI", "1.0", "argon");

typedef struct {
    char text[32];
    size_t len;
} whoami_t;

static whoami_t s_who;

static int32_t who_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    whoami_t *st = (whoami_t *)ag_dev_priv(dev);
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
    memcpy(buf, st->text + (size_t)off, n);
    return (int32_t)n;
}

static uint64_t who_size(ag_device_t *dev)
{
    whoami_t *st = (whoami_t *)ag_dev_priv(dev);
    return (st != NULL) ? st->len : 0;
}

static const ag_dev_ops_t k_ops = {
    .read = who_read,
    .size = who_size,
};

static char hex_digit(uint8_t n)
{
    return (char)((n < 10) ? ('0' + n) : ('a' + (n - 10)));
}

static void append_hex2(char *dst, size_t dst_len, uint8_t v)
{
    char tmp[3];
    tmp[0] = hex_digit((uint8_t)(v >> 4));
    tmp[1] = hex_digit((uint8_t)(v & 0x0f));
    tmp[2] = '\0';
    ag_strlcat(dst, tmp, dst_len);
}

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add) || !AG_HAS(ag_api()->dev, probe_hint)) {
        return -AG_ENOSYS;
    }

    const ag_probe_hint_t *hint = ag_probe_hint();
    if (hint != NULL) {
        char num[8];
        ag_strlcpy(s_who.text, "i2c", sizeof(s_who.text));
        ag_strlcat(s_who.text,
                   ag_utoa((uint64_t)hint->bus, num, sizeof(num), 0, false),
                   sizeof(s_who.text));
        ag_strlcat(s_who.text, ":0x", sizeof(s_who.text));
        append_hex2(s_who.text, sizeof(s_who.text), hint->addr);
        if (hint->has_id) {
            ag_strlcat(s_who.text, " id=0x", sizeof(s_who.text));
            append_hex2(s_who.text, sizeof(s_who.text), hint->id_val);
        }
        ag_strlcat(s_who.text, "\n", sizeof(s_who.text));
    } else {
        ag_strlcpy(s_who.text, "loaded without probe\n", sizeof(s_who.text));
    }
    s_who.len = strlen(s_who.text);

    const ag_dev_add_t desc = {
        .name = "whoami",
        .driver = "WHOAMI",
        .cls = AG_DEV_CHAR,
        .ops = &k_ops,
        .priv = &s_who,
    };
    return ag_dev_add(&desc);
}
