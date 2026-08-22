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
#include <argon/port/ble.h>

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
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#if AG_PORT_HAS_BLE_PERIPH
#include "esp_timer.h" /* esp_timer_get_time for the BLE-MIDI timestamp */
#include "os/os_mbuf.h" /* os_mbuf_append for the GATT server's read path */
#endif
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

/*
 * The host synchronises with the controller a moment after it starts, and every
 * GAP call before that fails with an error that says nothing useful.  Waiting
 * here rather than in the caller: this is a fact about the radio.
 *
 * Five seconds, not two.  Two was enough from the shell on an idle machine and
 * not enough with an application running and the heap thin - the pad reported
 * "timed out" when the radio was merely slow, which is a complaint about the
 * wrong thing.  Nothing is lost by waiting: the alternative is failing.
 */
static bool wait_synced(void)
{
    for (int i = 0; i < 250 && !ble_hs_synced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ble_hs_synced();
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

#if AG_PORT_HAS_BLE_PERIPH
static void ble_periph_register(void);
#endif

/*
 * The HID host, brought up the first time a device is opened.  See the note in
 * ag_port_bt_start: this costs a task and four kilobytes, and only a keyboard
 * needs it.
 */
static bool s_hidh_ready;

static ag_err_t hidh_ensure(void)
{
    if (s_hidh_ready) {
        return AG_OK;
    }
    const esp_hidh_config_t hcfg = {
        .callback = hidh_event,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    if (esp_hidh_init(&hcfg) != ESP_OK) {
        return -AG_ENOMEM;
    }
    esp_log_level_set("ESP_HIDH_NIMBLE", ESP_LOG_WARN);
    s_hidh_ready = true;
    return AG_OK;
}

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

    /*
     * The HID host is NOT started here, and that is the difference between a
     * board that can be a MIDI controller and one that cannot.
     *
     * esp_hidh_init brings up an event loop with a task and a four kilobyte
     * stack, and it exists for exactly one purpose: attaching a keyboard.  A
     * program that only wants to advertise - a MIDI pad, say - was paying for it
     * anyway, and on this chip that payment is the one that fails: with both
     * radios linked, an application running and BLE coming up, the loop's task
     * could not be created and the whole radio start failed with it.  Started on
     * demand in ag_port_bt_open instead, which is the only caller that needs it.
     */

#if AG_PORT_HAS_BLE_PERIPH
    /*
     * Add the peripheral's GATT service now, before the host is enabled: this
     * is the window NimBLE registers services in (they are started on sync).
     * Non-fatal - a board that fails to register a server can still be a
     * keyboard host and a scanner.
     */
    ble_periph_register();
#endif

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
    if (s_hidh_ready) {
        (void)esp_hidh_deinit();
        s_hidh_ready = false;
    }

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
    if (!wait_synced()) {
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

    /* The HID host, now that something is actually being opened. */
    const ag_err_t herr = hidh_ensure();
    if (herr != AG_OK) {
        return herr;
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

/* ---- general BLE: the observer ----------------------------------------- */

#if AG_PORT_HAS_BLE_CENTRAL

/*
 * A second scan, next to the HID one, and separate on purpose.  The HID scan
 * (ag_port_bt_scan above) throws away everything that is not a possible
 * keyboard; this keeps everything and says what each thing is.  Both use the
 * one radio, so they cannot run at once - s_state guards that - but each fills
 * its own list, because what they keep about a device is not the same.
 */
static ag_port_ble_dev_t s_ble_seen[SCAN_MAX];
static volatile uint32_t s_ble_seen_n;

static void note_ble_device(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }

    char name[AG_BLE_NAME_MAX + 1] = {0};
    if (fields.name != NULL && fields.name_len > 0) {
        const uint8_t n = (fields.name_len > AG_BLE_NAME_MAX)
                              ? AG_BLE_NAME_MAX
                              : fields.name_len;
        memcpy(name, fields.name, n);
        name[n] = '\0';
    }
    /*
     * Advertising report event type (BLE core): 0 = ADV_IND and
     * 1 = ADV_DIRECT_IND are the connectable ones; the rest are beacons and
     * scan responses you cannot open.
     */
    const bool connectable = (disc->event_type <= 1);

    lock();
    ag_port_ble_dev_t *d = NULL;
    for (uint32_t i = 0; i < s_ble_seen_n; i++) {
        if (memcmp(s_ble_seen[i].addr, disc->addr.val, 6) == 0) {
            d = &s_ble_seen[i];
            break;
        }
    }
    bool is_new = false;
    if (d == NULL) {
        if (s_ble_seen_n < SCAN_MAX) {
            d = &s_ble_seen[s_ble_seen_n++];
            is_new = true;
        } else {
            /* Full: the weakest device with no name gives way to a newcomer,
             * the same reasoning as the HID scan - the air is full of nameless
             * beacons and the one you want usually has a name. */
            int8_t worst = 127;
            for (uint32_t i = 0; i < s_ble_seen_n; i++) {
                if (s_ble_seen[i].name[0] == '\0' &&
                    s_ble_seen[i].rssi <= worst) {
                    worst = s_ble_seen[i].rssi;
                    d = &s_ble_seen[i];
                }
            }
            if (d != NULL && name[0] == '\0' && d->rssi >= disc->rssi) {
                d = NULL; /* the newcomer is no better than what is there */
            }
            if (d != NULL) {
                is_new = true;
            }
        }
    }
    if (d != NULL) {
        if (is_new) {
            memset(d, 0, sizeof(*d));
            memcpy(d->addr, disc->addr.val, 6);
            d->addr_type = disc->addr.type;
            d->company = 0xffff;
        }
        d->rssi = disc->rssi;
        d->connectable = d->connectable || connectable;
        if (d->name[0] == '\0' && name[0] != '\0') {
            memcpy(d->name, name, sizeof(name));
        }
        if (fields.appearance_is_present) {
            d->appearance = fields.appearance;
        }
        d->flags = fields.flags;
        for (int i = 0; i < fields.num_uuids16 && d->n_uuids < AG_BLE_UUIDS_MAX;
             i++) {
            const uint16_t u = ble_uuid_u16(&fields.uuids16[i].u);
            bool           dup = false;
            for (uint8_t k = 0; k < d->n_uuids; k++) {
                if (d->uuids[k] == u) {
                    dup = true;
                }
            }
            if (!dup) {
                d->uuids[d->n_uuids++] = u;
            }
        }
        if (fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
            d->company = (uint16_t)(fields.mfg_data[0] |
                                    ((uint16_t)fields.mfg_data[1] << 8));
        }
    }
    unlock();
}

static int gap_event_ble(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        note_ble_device(&event->disc);
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

ag_err_t ag_port_ble_scan(ag_port_ble_dev_t *out, uint32_t max, uint32_t *found,
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

    if (!wait_synced()) {
        return -AG_ETIMEDOUT;
    }

    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return -AG_EIO;
    }

    lock();
    s_ble_seen_n = 0;
    unlock();

    struct ble_gap_disc_params params = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,          /* active: ask for the name in the scan response */
        .filter_duplicates = 0, /* merge here, so scan responses are not lost   */
    };

    s_state = AG_BT_SCANNING;
    const int rc = ble_gap_disc(own_addr_type, (int32_t)(seconds * 1000u),
                                &params, gap_event_ble, NULL);
    if (rc != 0) {
        s_state = AG_BT_IDLE;
        return -AG_EIO;
    }

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
    const uint32_t n = s_ble_seen_n;
    for (uint32_t i = 0; i < n && i < max; i++) {
        out[i] = s_ble_seen[i];
    }
    unlock();

    if (found != NULL) {
        *found = n;
    }
    return AG_OK;
}

/* ---- general BLE: the GATT client --------------------------------------- */

/*
 * One connection at a time, driven from the shell task.  NimBLE does its work
 * on the host task and answers through callbacks; the pattern here is the same
 * as the scan - start an operation, then block the caller polling a flag the
 * callback sets, because the shell command that asked wants a result and there
 * is no state machine above it to hand the wait to.
 */
#define BLE_GATT_SVCS_MAX 12
#define BLE_GATT_CHRS_MAX 24
#define BLE_CONN_NONE     0xffffu

static volatile uint16_t s_conn = BLE_CONN_NONE;
static volatile bool     s_op_done;
static volatile int      s_op_status;

static ag_ble_svc_t      s_svcs[BLE_GATT_SVCS_MAX];
static volatile uint32_t s_nsvc;
static ag_ble_chr_t      s_chrs[BLE_GATT_CHRS_MAX];
static volatile uint32_t s_nchr;

static uint8_t           s_read_buf[AG_BLE_VAL_MAX];
static volatile uint32_t s_read_len;

/* Wait for the callback to say the current operation is finished. */
static bool wait_op(uint32_t timeout_ms)
{
    const uint32_t iters = timeout_ms / 20u + 1u;
    for (uint32_t i = 0; i < iters; i++) {
        if (s_op_done) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_op_done;
}

static int gatt_conn_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_op_status = event->connect.status;
        s_conn = (event->connect.status == 0) ? event->connect.conn_handle
                                               : BLE_CONN_NONE;
        s_op_done = true;
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        /* The peer or the link went away; the session is over and the next
         * call finds no connection.  Nothing here reconnects. */
        s_conn = BLE_CONN_NONE;
        break;
    default:
        break;
    }
    return 0;
}

ag_err_t ag_port_ble_connect(const uint8_t addr[6], int addr_type,
                             uint32_t timeout_ms)
{
    if (s_state == AG_BT_OFF) {
        return -AG_ENODEV;
    }
    if (s_state != AG_BT_IDLE) {
        return -AG_EBUSY; /* scanning, or a keyboard is opening/open */
    }
    if (s_conn != BLE_CONN_NONE) {
        return -AG_EBUSY;
    }
    if (addr == NULL) {
        return -AG_EINVAL;
    }
    if (timeout_ms == 0u) {
        timeout_ms = 10000u;
    }

    if (!wait_synced()) {
        return -AG_ETIMEDOUT;
    }

    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return -AG_EIO;
    }

    ble_addr_t peer;
    peer.type = (uint8_t)addr_type;
    memcpy(peer.val, addr, 6);

    s_op_done = false;
    s_op_status = 0;
    const int rc =
        ble_gap_connect(own_addr_type, &peer, 30000, NULL, gatt_conn_cb, NULL);
    if (rc != 0) {
        return -AG_EIO;
    }

    if (!wait_op(timeout_ms)) {
        (void)ble_gap_conn_cancel();
        return -AG_ETIMEDOUT;
    }
    if (s_op_status != 0 || s_conn == BLE_CONN_NONE) {
        return -AG_EIO;
    }
    return AG_OK;
}

ag_err_t ag_port_ble_disconnect(void)
{
    if (s_conn == BLE_CONN_NONE) {
        return AG_OK;
    }
    (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int i = 0; i < 50 && s_conn != BLE_CONN_NONE; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_conn = BLE_CONN_NONE;
    s_nsvc = 0;
    s_nchr = 0;
    return AG_OK;
}

bool ag_port_ble_connected(void) { return s_conn != BLE_CONN_NONE; }

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && svc != NULL) {
        if (s_nsvc < BLE_GATT_SVCS_MAX) {
            ag_ble_svc_t *s = &s_svcs[s_nsvc++];
            s->start = svc->start_handle;
            s->end = svc->end_handle;
            ble_uuid_to_str(&svc->uuid.u, s->uuid);
        }
    } else {
        s_op_status = (error->status == BLE_HS_EDONE) ? 0 : error->status;
        s_op_done = true;
    }
    return 0;
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        if (s_nchr < BLE_GATT_CHRS_MAX) {
            ag_ble_chr_t *c = &s_chrs[s_nchr++];
            c->handle = chr->val_handle;
            c->props = chr->properties;
            ble_uuid_to_str(&chr->uuid.u, c->uuid);
        }
    } else {
        s_op_status = (error->status == BLE_HS_EDONE) ? 0 : error->status;
        s_op_done = true;
    }
    return 0;
}

ag_err_t ag_port_ble_discover(uint32_t timeout_ms)
{
    if (s_conn == BLE_CONN_NONE) {
        return -AG_ENODEV;
    }
    if (timeout_ms == 0u) {
        timeout_ms = 8000u;
    }

    s_nsvc = 0;
    s_op_done = false;
    s_op_status = 0;
    if (ble_gattc_disc_all_svcs(s_conn, svc_disc_cb, NULL) != 0) {
        return -AG_EIO;
    }
    if (!wait_op(timeout_ms)) {
        return -AG_ETIMEDOUT;
    }

    s_nchr = 0;
    for (uint32_t i = 0; i < s_nsvc; i++) {
        s_op_done = false;
        if (ble_gattc_disc_all_chrs(s_conn, s_svcs[i].start, s_svcs[i].end,
                                    chr_disc_cb, NULL) != 0) {
            continue;
        }
        (void)wait_op(timeout_ms);
        if (s_conn == BLE_CONN_NONE) {
            return -AG_ENODEV; /* the link dropped mid-discovery */
        }
    }
    return AG_OK;
}

uint32_t ag_port_ble_services(ag_ble_svc_t *out, uint32_t max)
{
    const uint32_t n = s_nsvc;
    for (uint32_t i = 0; i < n && i < max; i++) {
        out[i] = s_svcs[i];
    }
    return n;
}

uint32_t ag_port_ble_chars(ag_ble_chr_t *out, uint32_t max)
{
    const uint32_t n = s_nchr;
    for (uint32_t i = 0; i < n && i < max; i++) {
        out[i] = s_chrs[i];
    }
    return n;
}

static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    s_op_status = error->status;
    s_read_len = 0;
    if (error->status == 0 && attr != NULL && attr->om != NULL) {
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(attr->om, s_read_buf, sizeof(s_read_buf),
                                &got) == 0) {
            s_read_len = got;
        }
    }
    s_op_done = true;
    return 0;
}

int32_t ag_port_ble_read(uint16_t handle, uint8_t *out, uint32_t max,
                         uint32_t timeout_ms)
{
    if (s_conn == BLE_CONN_NONE) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    if (timeout_ms == 0u) {
        timeout_ms = 5000u;
    }

    s_op_done = false;
    s_op_status = 0;
    s_read_len = 0;
    if (ble_gattc_read(s_conn, handle, read_cb, NULL) != 0) {
        return -AG_EIO;
    }
    if (!wait_op(timeout_ms)) {
        return -AG_ETIMEDOUT;
    }
    if (s_op_status != 0) {
        return -AG_EIO;
    }
    uint32_t n = s_read_len;
    if (n > max) {
        n = max;
    }
    memcpy(out, s_read_buf, n);
    return (int32_t)n;
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    s_op_status = error->status;
    s_op_done = true;
    return 0;
}

ag_err_t ag_port_ble_write(uint16_t handle, const void *data, uint32_t len,
                           bool with_response, uint32_t timeout_ms)
{
    if (s_conn == BLE_CONN_NONE) {
        return -AG_ENODEV;
    }
    if (data == NULL && len > 0u) {
        return -AG_EINVAL;
    }
    if (len > 512u) {
        return -AG_EINVAL;
    }
    if (timeout_ms == 0u) {
        timeout_ms = 5000u;
    }

    if (!with_response) {
        /* Fire and forget: no acknowledgement to wait for, by request. */
        return (ble_gattc_write_no_rsp_flat(s_conn, handle, data,
                                            (uint16_t)len) == 0)
                   ? AG_OK
                   : -AG_EIO;
    }

    s_op_done = false;
    s_op_status = 0;
    if (ble_gattc_write_flat(s_conn, handle, data, (uint16_t)len, write_cb,
                             NULL) != 0) {
        return -AG_EIO;
    }
    if (!wait_op(timeout_ms)) {
        return -AG_ETIMEDOUT;
    }
    return (s_op_status == 0) ? AG_OK : -AG_EIO;
}

#endif /* AG_PORT_HAS_BLE_CENTRAL */

/* ---- general BLE: the peripheral (advertise + GATT server) -------------- */

#if AG_PORT_HAS_BLE_PERIPH

#define BLE_PERIPH_VAL_MAX 128

static const ble_uuid16_t s_periph_svc_uuid = BLE_UUID16_INIT(0xfff0);
static const ble_uuid16_t s_periph_rd_uuid = BLE_UUID16_INIT(0xfff1);
static const ble_uuid16_t s_periph_wr_uuid = BLE_UUID16_INIT(0xfff2);

/*
 * The Apple BLE-MIDI service and its one characteristic, so a music app sees
 * the board as a MIDI device.  128-bit UUIDs, stored least-significant byte
 * first as NimBLE wants:
 *   service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 *   char    7772E5DB-3868-4112-A1A9-F2669D106BF3
 */
static const ble_uuid128_t s_midi_svc_uuid = BLE_UUID128_INIT(
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed,
    0x5a, 0x0e, 0xb8, 0x03);
static const ble_uuid128_t s_midi_chr_uuid = BLE_UUID128_INIT(
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38,
    0xdb, 0xe5, 0x72, 0x77);

static volatile bool     s_adv_on;
static volatile bool     s_adv_is_midi; /* advertising as a MIDI device       */
static volatile bool     s_adv_client;
static volatile uint16_t s_periph_conn = 0xffff; /* the connected client       */
static uint16_t          s_midi_val_handle;      /* set by ble_gatts_add_svcs  */
static volatile uint32_t s_adv_writes;
static uint8_t           s_adv_read[BLE_PERIPH_VAL_MAX];
static volatile uint32_t s_adv_read_len;
static uint8_t           s_adv_last[BLE_PERIPH_VAL_MAX];
static volatile uint32_t s_adv_last_len;
static char              s_adv_name[32];
static uint8_t           s_adv_own_addr_type;

/*
 * The one place a client's read and write are served.  Read hands back whatever
 * the kernel last set; write keeps the newest bytes and counts it.  Short, as a
 * radio callback must be.
 */
static int periph_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return (os_mbuf_append(ctxt->om, s_adv_read,
                               (uint16_t)s_adv_read_len) == 0)
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_adv_last, sizeof(s_adv_last),
                                &got) == 0) {
            s_adv_last_len = got;
            s_adv_writes++;
        }
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/*
 * The MIDI characteristic's own access: a read gives back nothing (a MIDI
 * controller has no state to read) and a write from the host is accepted and
 * ignored for now - this board is a controller, not a synth.  Notes go the
 * other way, as notifications from midi_send().
 */
static int midi_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_periph_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_periph_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_periph_rd_uuid.u,
                .access_cb = periph_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_periph_wr_uuid.u,
                .access_cb = periph_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_midi_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_midi_chr_uuid.u,
                .access_cb = midi_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_midi_val_handle,
            },
            {0},
        },
    },
    {0},
};

static void ble_periph_register(void)
{
    static const char def[] = "ArgonOS";
    memcpy(s_adv_read, def, sizeof(def) - 1u);
    s_adv_read_len = sizeof(def) - 1u;
    (void)ble_gatts_count_cfg(s_periph_svcs);
    (void)ble_gatts_add_svcs(s_periph_svcs);
}

static int gap_event_periph(struct ble_gap_event *event, void *arg);

static int periph_adv_begin(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_adv_name;
    fields.name_len = (uint8_t)strlen(s_adv_name);
    fields.name_is_complete = 1;
    if (s_adv_is_midi) {
        /* A music app scans for the MIDI service UUID.  It is 128 bits - 16
         * bytes in the advertisement - so with flags and the name there is
         * little of the 31 left, which is why the MIDI name is kept short. */
        fields.uuids128 = &s_midi_svc_uuid;
        fields.num_uuids128 = 1;
        fields.uuids128_is_complete = 1;
    } else {
        fields.uuids16 = &s_periph_svc_uuid;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
    }
    if (ble_gap_adv_set_fields(&fields) != 0) {
        return -1;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(s_adv_own_addr_type, NULL, BLE_HS_FOREVER,
                             &adv_params, gap_event_periph, NULL);
}

static int gap_event_periph(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_adv_client = (event->connect.status == 0);
        if (event->connect.status == 0) {
            s_periph_conn = event->connect.conn_handle;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        /* A peripheral that goes quiet the moment its first client leaves can
         * be reached exactly once.  Advertise again so the next one can find
         * it. */
        s_adv_client = false;
        s_periph_conn = 0xffff;
        if (s_adv_on) {
            (void)periph_adv_begin();
        }
        break;
    default:
        break;
    }
    return 0;
}

ag_err_t ag_port_ble_adv_start(const char *name)
{
    if (s_state == AG_BT_OFF) {
        return -AG_ENODEV;
    }
    if (name == NULL || name[0] == '\0') {
        name = "ArgonOS";
    }

    if (!wait_synced()) {
        return -AG_ETIMEDOUT;
    }
    if (ble_hs_id_infer_auto(0, &s_adv_own_addr_type) != 0) {
        return -AG_EIO;
    }

    snprintf(s_adv_name, sizeof(s_adv_name), "%s", name);
    s_adv_is_midi = false;
    (void)ble_gap_adv_stop(); /* in case it was already up */
    if (periph_adv_begin() != 0) {
        return -AG_EIO; /* usually the name made the advert exceed 31 bytes */
    }
    s_adv_on = true;
    return AG_OK;
}

ag_err_t ag_port_ble_adv_stop(void)
{
    s_adv_on = false;
    (void)ble_gap_adv_stop();
    return AG_OK;
}

ag_err_t ag_port_ble_adv_status(ag_port_ble_adv_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    out->advertising = s_adv_on;
    out->connected = s_adv_client;
    out->writes = s_adv_writes;
    out->read_len = s_adv_read_len;
    return AG_OK;
}

void ag_port_ble_adv_set_read(const void *data, uint32_t len)
{
    if (len > sizeof(s_adv_read)) {
        len = sizeof(s_adv_read);
    }
    if (data != NULL && len > 0u) {
        memcpy(s_adv_read, data, len);
    }
    s_adv_read_len = len;
}

int32_t ag_port_ble_adv_last_write(uint8_t *out, uint32_t max)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    uint32_t n = s_adv_last_len;
    if (n > max) {
        n = max;
    }
    memcpy(out, s_adv_last, n);
    return (int32_t)n;
}

ag_err_t ag_port_ble_midi_advertise(const char *name)
{
    if (s_state == AG_BT_OFF) {
        return -AG_ENODEV;
    }
    if (name == NULL || name[0] == '\0') {
        name = "ArgMIDI";
    }

    if (!wait_synced()) {
        return -AG_ETIMEDOUT;
    }
    if (ble_hs_id_infer_auto(0, &s_adv_own_addr_type) != 0) {
        return -AG_EIO;
    }

    /* The 128-bit MIDI UUID leaves room for only a short name in a 31-byte
     * advertisement, so keep it short here rather than fail set_fields. */
    snprintf(s_adv_name, sizeof(s_adv_name), "%.8s", name);
    s_adv_is_midi = true;
    (void)ble_gap_adv_stop();
    if (periph_adv_begin() != 0) {
        return -AG_EIO;
    }
    s_adv_on = true;
    return AG_OK;
}

bool ag_port_ble_midi_ready(void)
{
    /* Connected is enough to report "ready": whether the client armed its
     * CCCD is a detail the notify below reports on its own, and the subscribe
     * event's handle does not always match cleanly across NimBLE versions. */
    return s_periph_conn != 0xffff;
}

ag_err_t ag_port_ble_midi_send(uint8_t status, uint8_t data1, uint8_t data2)
{
    if (s_periph_conn == 0xffff) {
        return -AG_ENODEV; /* nobody is connected */
    }

    /*
     * One MIDI message in a BLE-MIDI packet: a header byte and a timestamp
     * byte, then the message.  The timestamp is milliseconds, low 13 bits,
     * split 6 over 7 across the two bytes, each with the high bit set.
     */
    const uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000) & 0x1fffu;
    uint8_t        pkt[5];
    pkt[0] = (uint8_t)(0x80u | ((ts >> 7) & 0x3fu));
    pkt[1] = (uint8_t)(0x80u | (ts & 0x7fu));
    pkt[2] = status;
    pkt[3] = data1;
    pkt[4] = data2;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
    if (om == NULL) {
        return -AG_ENOMEM;
    }
    /* notify_custom takes ownership of the mbuf either way. */
    return (ble_gatts_notify_custom(s_periph_conn, s_midi_val_handle, om) == 0)
               ? AG_OK
               : -AG_EIO;
}

#endif /* AG_PORT_HAS_BLE_PERIPH */

#endif /* CONFIG_ARGON_ENABLE_BLE */
