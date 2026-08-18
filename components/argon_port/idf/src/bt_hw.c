/*
 * ArgonOS port: ESP-IDF - Bluetooth Low Energy, host side.
 *
 * NimBLE for the link and ESP-IDF's esp_hid host for the profile.  The whole
 * point of it is a keyboard: this chip has no USB, so there is no other way to
 * attach one, and a machine you cannot type on is a machine with a serial
 * cable permanently hanging out of it.
 *
 * What comes out of here is HID reports.  Turning a report into a keystroke is
 * the kernel's business (src/dev/btinput.c), because the mapping from a
 * usage code to a key is the same on every port and the way to reach the
 * radio is not.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_ENABLE_BLE) && CONFIG_ARGON_ENABLE_BLE

#include <argon/port/bt.h>

#include <string.h>

#include "esp_bt.h"
#include "esp_hidh.h"
#include "esp_hidh_nimble.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define BLE_HID_SVC_UUID 0x1812

/* Appearance values a keyboard and a mouse advertise (BLE assigned numbers). */
#define APPEARANCE_KEYBOARD 0x03c1
#define APPEARANCE_MOUSE    0x03c2

#define SCAN_MAX 16

static volatile ag_bt_state_t s_state = AG_BT_OFF;
static volatile bool          s_synced;
static ag_port_bt_report_fn   s_on_report;

/* What the scan collects, and the lock that keeps the radio task out of it. */
static ag_port_bt_dev_t s_seen[SCAN_MAX];
static volatile uint32_t s_seen_n;
static SemaphoreHandle_t s_lock;

/* The device that is open, if any. */
static esp_hidh_dev_t  *s_dev;
static char             s_dev_name[AG_BT_NAME_MAX + 1];
static uint8_t          s_dev_addr[6];
static volatile uint32_t s_reports;
static volatile int8_t   s_rssi;

static void lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

/* ---- the host task ----------------------------------------------------- */

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run(); /* returns when the host is stopped */
    nimble_port_freertos_deinit();
}

/* ---- discovery --------------------------------------------------------- */

/*
 * One advertisement.  Kept if it is new, and updated if it is not: a device
 * advertises several times a second, and a list that grows by one each time is
 * a list of one device.
 */
static void note_device(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }

    char name[AG_BT_NAME_MAX + 1] = {0};
    if (fields.name != NULL && fields.name_len > 0) {
        const uint8_t n = (fields.name_len > AG_BT_NAME_MAX)
                              ? AG_BT_NAME_MAX
                              : fields.name_len;
        memcpy(name, fields.name, n);
        name[n] = '\0';
    }

    bool hid = false;
    if (fields.appearance_is_present &&
        (fields.appearance == APPEARANCE_KEYBOARD ||
         fields.appearance == APPEARANCE_MOUSE)) {
        hid = true;
    }
    for (int i = 0; !hid && i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == BLE_HID_SVC_UUID) {
            hid = true;
        }
    }

    lock();
    for (uint32_t i = 0; i < s_seen_n; i++) {
        if (memcmp(s_seen[i].addr, disc->addr.val, 6) == 0) {
            /* A later advertisement may carry the name the first one had no
             * room for; keep whichever is more informative. */
            if (s_seen[i].name[0] == '\0' && name[0] != '\0') {
                memcpy(s_seen[i].name, name, sizeof(name));
            }
            s_seen[i].rssi = disc->rssi;
            s_seen[i].hid = s_seen[i].hid || hid;
            unlock();
            return;
        }
    }
    ag_port_bt_dev_t *d = NULL;
    if (s_seen_n < SCAN_MAX) {
        d = &s_seen[s_seen_n++];
    } else {
        /*
         * Full, and what fills it is not what anybody is looking for: the air
         * is full of beacons and phones with random addresses that advertise
         * nothing but a number, several times a second, and they arrive first
         * because there are so many of them.  A device with a name, or one
         * that says it is a keyboard, takes the place of the weakest one that
         * is neither.  Without this the list is sixteen lines of "(no name)"
         * and the keyboard is not on it.
         */
        int8_t worst = 127;
        for (uint32_t i = 0; i < s_seen_n; i++) {
            if (s_seen[i].hid || s_seen[i].name[0] != '\0') {
                continue;
            }
            if (s_seen[i].rssi <= worst) {
                worst = s_seen[i].rssi;
                d = &s_seen[i];
            }
        }
        if (d != NULL && !hid && name[0] == '\0') {
            d = NULL; /* the newcomer is no better than what is there */
        }
    }
    if (d != NULL) {
        memset(d, 0, sizeof(*d));
        memcpy(d->name, name, sizeof(name));
        memcpy(d->addr, disc->addr.val, 6);
        d->addr_type = disc->addr.type;
        d->rssi = disc->rssi;
        d->hid = hid;
    }
    unlock();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        note_device(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_state == AG_BT_SCANNING) {
            s_state = AG_BT_IDLE;
        }
        break;
    default:
        break;
    }
    return 0;
}

/* ---- HID reports ------------------------------------------------------- */

static ag_bt_usage_t map_usage(esp_hid_usage_t u)
{
    switch (u) {
    case ESP_HID_USAGE_KEYBOARD: return AG_BT_USAGE_KEYBOARD;
    case ESP_HID_USAGE_MOUSE:    return AG_BT_USAGE_MOUSE;
    default:                     return AG_BT_USAGE_OTHER;
    }
}

static void hidh_event(void *handler_args, esp_event_base_t base, int32_t id,
                       void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) {
            s_dev = p->open.dev;
            s_state = AG_BT_OPEN;
            s_reports = 0;
            const uint8_t *bda = esp_hidh_dev_bda_get(p->open.dev);
            if (bda != NULL) {
                memcpy(s_dev_addr, bda, sizeof(s_dev_addr));
            }
            const char *n = esp_hidh_dev_name_get(p->open.dev);
            snprintf(s_dev_name, sizeof(s_dev_name), "%s",
                     (n != NULL) ? n : "");
        } else {
            s_state = AG_BT_IDLE;
        }
        break;

    case ESP_HIDH_INPUT_EVENT:
        s_reports++;
        if (s_on_report != NULL) {
            s_on_report(map_usage(p->input.usage), (uint8_t)p->input.report_id,
                        p->input.data, p->input.length);
        }
        break;

    case ESP_HIDH_CLOSE_EVENT:
        /*
         * The memory belongs to the caller of this event and nobody else will
         * free it.  Doing that here and then reconnecting is what makes a
         * keyboard that walked out of range come back by itself.
         */
        if (p->close.dev != NULL) {
            (void)esp_hidh_dev_free(p->close.dev);
        }
        s_dev = NULL;
        s_state = AG_BT_IDLE;
        break;

    default:
        break;
    }
}

/* ---- the contract ------------------------------------------------------ */

void ag_port_bt_on_report(ag_port_bt_report_fn fn) { s_on_report = fn; }

ag_err_t ag_port_bt_start(void)
{
    if (s_state != AG_BT_OFF) {
        return AG_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return -AG_ENOMEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    /* Bonds live in NVS.  Without it a keyboard has to be paired again after
     * every power cut, and pairing needs a keyboard. */

    /*
     * Hand the classic Bluetooth half of the controller's memory back before
     * initialising it.  On this chip that is about 30 KB, and this build has
     * no use for classic at all: what it wants is a keyboard, and a modern
     * keyboard is BLE.
     */
    (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.mode = ESP_BT_MODE_BLE;
    if (esp_bt_controller_init(&cfg) != ESP_OK) {
        return -AG_ENOMEM;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        return -AG_EIO;
    }
    if (esp_nimble_init() != ESP_OK) {
        return -AG_EIO;
    }

    const esp_hidh_config_t hcfg = {
        .callback = hidh_event,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    if (esp_hidh_init(&hcfg) != ESP_OK) {
        return -AG_EIO;
    }

    if (esp_nimble_enable(nimble_host_task) != ESP_OK) {
        return -AG_EIO;
    }

    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    esp_log_level_set("ESP_HIDH_NIMBLE", ESP_LOG_WARN);

    s_state = AG_BT_IDLE;
    return AG_OK;
}

/*
 * Off, and giving the memory back - the other half of what makes both radios
 * possible on this chip.  The controller alone holds tens of kilobytes while
 * it is enabled and none of it once it is not, which is the difference between
 * "linked" and "running" and the reason both can be in one image.
 */
ag_err_t ag_port_bt_stop(void)
{
    if (s_state == AG_BT_OFF) {
        return AG_OK;
    }
    if (s_dev != NULL) {
        (void)esp_hidh_dev_close(s_dev);
        s_dev = NULL;
    }
    (void)ble_gap_disc_cancel();
    (void)esp_hidh_deinit();

    /* Order matters and is the reverse of start: host, then controller. */
    (void)nimble_port_stop();
    (void)esp_nimble_deinit();
    (void)esp_bt_controller_disable();
    (void)esp_bt_controller_deinit();

    s_state = AG_BT_OFF;
    s_seen_n = 0;
    s_reports = 0;
    s_dev_name[0] = '\0';
    return AG_OK;
}

ag_err_t ag_port_bt_scan(ag_port_bt_dev_t *out, uint32_t max, uint32_t *found,
                         uint32_t seconds)
{
    if (s_state == AG_BT_OFF) {
        return -AG_ENODEV;
    }
    if (s_state == AG_BT_SCANNING || s_state == AG_BT_OPENING) {
        return -AG_EBUSY;
    }
    if (seconds == 0) {
        seconds = 4;
    }

    /*
     * The host synchronises with the controller a moment after it starts, and
     * every GAP call before that fails with an error that says nothing useful.
     * Waiting here rather than in the caller: this is a fact about the radio.
     */
    for (int i = 0; i < 100 && !ble_hs_synced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!ble_hs_synced()) {
        return -AG_ETIMEDOUT;
    }

    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return -AG_EIO;
    }

    lock();
    s_seen_n = 0;
    unlock();

    struct ble_gap_disc_params params = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0, /* active: ask for the name, which passive does not get */
        /*
         * Duplicate filtering off, and that is what makes names appear.  The
         * name usually arrives in the scan response, which is a second packet
         * from the same address - exactly what the controller's duplicate
         * filter throws away.  Every device then reports several times a
         * second and note_device() merges them, which it has to do anyway.
         */
        .filter_duplicates = 0,
    };

    s_state = AG_BT_SCANNING;
    const int rc = ble_gap_disc(own_addr_type, (int32_t)(seconds * 1000u),
                                &params, gap_event, NULL);
    if (rc != 0) {
        s_state = AG_BT_IDLE;
        return -AG_EIO;
    }

    /* Blocking, as the contract says: there is nothing to return until the
     * listening window closes. */
    for (uint32_t i = 0; i < seconds * 50u + 50u; i++) {
        if (s_state != AG_BT_SCANNING) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    (void)ble_gap_disc_cancel();
    if (s_state == AG_BT_SCANNING) {
        s_state = AG_BT_IDLE;
    }

    lock();
    const uint32_t n = s_seen_n;
    for (uint32_t i = 0; i < n && i < max; i++) {
        out[i] = s_seen[i];
    }
    unlock();

    if (found != NULL) {
        *found = n;
    }
    return AG_OK;
}

ag_err_t ag_port_bt_open(const uint8_t addr[6], int addr_type)
{
    if (s_state == AG_BT_OFF) {
        return -AG_ENODEV;
    }
    if (s_state == AG_BT_OPEN || s_state == AG_BT_OPENING) {
        return -AG_EBUSY;
    }

    uint8_t bda[6];
    memcpy(bda, addr, sizeof(bda));
    memcpy(s_dev_addr, addr, sizeof(s_dev_addr));

    s_state = AG_BT_OPENING;
    esp_hidh_dev_t *dev =
        esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, (uint8_t)addr_type);
    if (dev == NULL) {
        s_state = AG_BT_IDLE;
        return -AG_EIO;
    }
    /* The OPEN event decides whether this actually worked; it may want a
     * passkey first, and that conversation is not over yet. */
    return AG_OK;
}

ag_err_t ag_port_bt_close(void)
{
    if (s_dev != NULL) {
        (void)esp_hidh_dev_close(s_dev);
    }
    s_state = (s_state == AG_BT_OFF) ? AG_BT_OFF : AG_BT_IDLE;
    return AG_OK;
}

ag_err_t ag_port_bt_status(ag_port_bt_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->reports = s_reports;
    out->rssi = s_rssi;
    memcpy(out->addr, s_dev_addr, sizeof(out->addr));
    snprintf(out->name, sizeof(out->name), "%s", s_dev_name);
    return AG_OK;
}

#endif /* CONFIG_ARGON_ENABLE_BLE */
