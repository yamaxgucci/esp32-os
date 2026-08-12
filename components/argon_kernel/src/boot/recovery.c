/*
 * ArgonOS - safe boot / recovery profile.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/recovery.h>

#include <string.h>

#include <argon/cfg.h>
#include <argon/log.h>
#include <argon/vfs.h>

#include "core/sysconfig.h"
#include "esp_system.h"
#include "esp_attr.h"

#ifndef AG_BOOT_RECOVERY_DEFAULT_AFTER
#define AG_BOOT_RECOVERY_DEFAULT_AFTER 3u
#endif

typedef struct {
    uint32_t magic;
    uint32_t attempts;
} ag_boot_counter_t;

#define AG_BOOT_COUNTER_MAGIC 0xA60C071Cu

static RTC_NOINIT_ATTR ag_boot_counter_t s_counter;
static bool         s_recovery;
static const char  *s_reason = "";
static bool         s_begun;

static bool reset_is_unclean(esp_reset_reason_t rr)
{
    switch (rr) {
    case ESP_RST_POWERON:
    case ESP_RST_SW: /* esp_restart / shell reboot — intentional */
        return false;
    default:
        return true;
    }
}

static bool marker_present(void)
{
    ag_stat_t st;
    return ag_vfs_stat(AG_BOOT_SAFE_PATH, NULL, &st) == AG_OK &&
           (st.attr & AG_A_DIR) == 0;
}

void ag_boot_recovery_begin(void)
{
    if (s_begun) {
        return;
    }
    s_begun = true;
    s_recovery = false;
    s_reason = "";

    if (s_counter.magic != AG_BOOT_COUNTER_MAGIC) {
        s_counter.magic = AG_BOOT_COUNTER_MAGIC;
        s_counter.attempts = 0;
    }

    const esp_reset_reason_t rr = esp_reset_reason();
    if (reset_is_unclean(rr)) {
        if (s_counter.attempts < 1000u) {
            s_counter.attempts++;
        }
    } else if (rr == ESP_RST_POWERON) {
        /* Cold power-on: start a fresh streak. */
        s_counter.attempts = 0;
    }
    /* ESP_RST_SW keeps the counter until the shell clears it. */

    unsigned threshold = AG_BOOT_RECOVERY_DEFAULT_AFTER;
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg != NULL) {
        const int32_t n = ag_cfg_get_int(cfg, "boot.recovery_after",
                                         (int32_t)threshold);
        if (n >= 1 && n <= 100) {
            threshold = (unsigned)n;
        }
        if (ag_cfg_get_bool(cfg, "boot.safe", false)) {
            s_recovery = true;
            s_reason = "boot.safe=1 in SYSTEM.CFG";
        }
    }

    if (!s_recovery && marker_present()) {
        s_recovery = true;
        s_reason = "marker " AG_BOOT_SAFE_PATH;
    }

    if (!s_recovery && s_counter.attempts >= threshold) {
        s_recovery = true;
        s_reason = "unclean reset streak";
    }

    if (s_recovery) {
        ag_log(AG_LOG_WARN, "boot",
               "RECOVERY: modules and AUTOEXEC skipped (%s, attempts=%u)",
               s_reason, (unsigned)s_counter.attempts);
    }
}

bool ag_boot_in_recovery(void) { return s_recovery; }

const char *ag_boot_recovery_reason(void)
{
    return (s_reason != NULL) ? s_reason : "";
}

void ag_boot_recovery_shell_ok(void)
{
    s_counter.magic = AG_BOOT_COUNTER_MAGIC;
    s_counter.attempts = 0;
}

ag_err_t ag_boot_recovery_set_marker(bool on)
{
    if (on) {
        const ag_handle_t h =
            ag_vfs_open(AG_BOOT_SAFE_PATH, NULL,
                        AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (h < 0) {
            return h;
        }
        static const char k_body[] = "recovery\n";
        const int32_t n = ag_vfs_write(h, k_body, sizeof(k_body) - 1);
        ag_vfs_close(h);
        return (n < 0) ? n : AG_OK;
    }
    const ag_err_t err = ag_vfs_unlink(AG_BOOT_SAFE_PATH, NULL);
    if (err == -AG_ENOENT) {
        return AG_OK;
    }
    return err;
}

bool ag_boot_recovery_marker_present(void) { return marker_present(); }

unsigned ag_boot_recovery_attempts(void)
{
    if (s_counter.magic != AG_BOOT_COUNTER_MAGIC) {
        return 0;
    }
    return (unsigned)s_counter.attempts;
}
