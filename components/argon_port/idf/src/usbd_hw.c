/*
 * ArgonOS port: ESP-IDF - the board as a USB mass-storage device, on demand.
 *
 * The counterpart of usb_hw.c, on the other side of the peripheral: where that
 * is the OTG as a HOST for a keyboard, this is it as a DEVICE a PC mounts.  It
 * is linked only behind CONFIG_ARGON_USB_MSC (off by default), and it uses bare
 * TinyUSB, not esp_tinyusb - see main/idf_component.yml for why: the convenience
 * MSC API accepts a wear-levelling handle or an sdmmc_card_t, and this board's
 * card is on SPI behind the port's own mount (storage_hw.c), so the MSC backend
 * is written by hand here and forwards every callback to a table the
 * APPLICATION registered.
 *
 * The whole of the PHY handover is here, and the reason it exists is one
 * sentence: this chip has ONE internal USB PHY and two peripherals that want
 * it - the USB-Serial-JTAG console and the OTG device.  They cannot run at
 * once (Espressif's own guide says it; the loser goes silent with no error).
 * So start() uninstalls the Serial-JTAG driver, claims the PHY for the OTG, and
 * the console is off its USB port until stop() - which uninstalls TinyUSB and
 * reinstalls the Serial-JTAG driver so the console comes back.  The KERNEL
 * detaches and re-attaches the console endpoint around these two calls; the
 * port owns only the drivers and the PHY.
 *
 * There is no board here to prove this on (the one COM9 is the user's), so the
 * sequence below is written, not measured: it follows the order the feasibility
 * study settled on and fails loudly at each step rather than half-working,
 * because a board that is flashed over this same port must not lose it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_USB_MSC) && CONFIG_ARGON_USB_MSC

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <string.h>

#include <argon/port/usbd.h>

#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "class/msc/msc_device.h"

static const char *TAG = "usbd";

/* The one OTG peripheral is root hub port 0, full speed. */
#define USBD_RHPORT 0

/* The MSC bulk endpoints: OUT 1, IN 0x81, 64-byte max packet (full speed). */
#define USBD_EP_OUT 0x01
#define USBD_EP_IN  0x81

/*
 * The Serial-JTAG console's buffer sizes, from uart_console.c / uart_hw.c.
 * stop() reinstalls the driver with the same shape so the console that comes
 * back is the console that left.
 */
#define USBD_JTAG_RX 2048
#define USBD_JTAG_TX 256

/* The application's table, copied at start and cleared on stop. */
static ag_usbd_msc_cbs_t s_cbs;
static volatile bool      s_up;
static volatile bool      s_mounted;

/* Whether the USB-Serial-JTAG driver was up when we took the PHY. */
static bool s_jtag_was_up;

static usb_phy_handle_t s_phy;
static TaskHandle_t     s_task;
static volatile bool    s_run;

/* ---- descriptors -------------------------------------------------------- */

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, /* class is per-interface, the MSC way */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,   /* Espressif's development VID */
    .idProduct = 0x4002,  /* 0x4000 | MSC bit, the TinyUSB PID map */
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

/* One MSC interface, two bulk endpoints.  Layout is the TinyUSB template. */
#define USBD_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USBD_CONFIG_TOTAL_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(0, 0, USBD_EP_OUT, USBD_EP_IN, 64),
};

static const char *const s_str_desc[] = {
    "ArgonOS",        /* 1: manufacturer */
    "SD card reader", /* 2: product       */
    "1",              /* 3: serial        */
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_dev_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_cfg_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t s_buf[32];

    if (index == 0) {
        /* Language descriptor: one code unit, English (0x0409). */
        s_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4u);
        s_buf[1] = 0x0409;
        return s_buf;
    }
    if (index - 1 >= sizeof(s_str_desc) / sizeof(s_str_desc[0])) {
        return NULL;
    }
    const char *s = s_str_desc[index - 1];
    size_t      n = strlen(s);
    if (n > 30u) {
        n = 30u;
    }
    s_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * (uint16_t)n + 2u));
    for (size_t i = 0; i < n; i++) {
        s_buf[1 + i] = (uint16_t)(unsigned char)s[i];
    }
    return s_buf;
}

/* ---- MSC callbacks, forwarded to the application's table --------------- */

/* Fixed-width SCSI strings, space padded - inquiry gives no terminator. */
static void fix_str(uint8_t *dst, size_t n, const char *src)
{
    const size_t len = strlen(src);
    const size_t copy = (len < n) ? len : n;
    memcpy(dst, src, copy);
    if (copy < n) {
        memset(dst + copy, ' ', n - copy);
    }
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    fix_str(vendor_id, 8, "ArgonOS");
    fix_str(product_id, 16, "Card reader");
    fix_str(product_rev, 4, "1.0");
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size)
{
    uint32_t bc = 0;
    uint32_t bs = 0;
    if (s_cbs.capacity != NULL && s_cbs.capacity(lun, &bc, &bs) == 0) {
        *block_count = bc;
        *block_size = (uint16_t)bs;
    } else {
        *block_count = 0;
        *block_size = 512;
    }
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    if (s_cbs.read10 == NULL) {
        return TUD_MSC_RET_ERROR;
    }
    return s_cbs.read10(lun, lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)bufsize;
    /*
     * Stage 1 is read-only on two levels: the application's own write10 is
     * expected to refuse, and a NULL entry is refused here too, so a table
     * that never said anything about writes cannot open a write path.
     */
    if (s_cbs.write10 == NULL) {
        return 0;
    }
    return s_cbs.write10(lun, lba, offset, buffer, bufsize);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    return (s_cbs.unit_ready != NULL) && s_cbs.unit_ready(lun);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject)
{
    (void)power_condition;
    if (s_cbs.start_stop == NULL) {
        return false;
    }
    return s_cbs.start_stop(lun, start, load_eject);
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    return (s_cbs.is_writable != NULL) && s_cbs.is_writable(lun);
}

void tud_mount_cb(void)
{
    s_mounted = true;
    if (s_cbs.mounted != NULL) {
        s_cbs.mounted();
    }
}

void tud_umount_cb(void)
{
    s_mounted = false;
    if (s_cbs.unmounted != NULL) {
        s_cbs.unmounted();
    }
}

/* ---- the TinyUSB task --------------------------------------------------- */

static void usbd_task(void *arg)
{
    (void)arg;
    while (s_run) {
        tud_task();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* Put the Serial-JTAG console back, the same shape it left in. */
static void reinstall_jtag(void)
{
    usb_serial_jtag_driver_config_t jc = {
        .rx_buffer_size = USBD_JTAG_RX,
        .tx_buffer_size = USBD_JTAG_TX,
    };
    (void)usb_serial_jtag_driver_install(&jc);
    s_jtag_was_up = false;
}

/* ---- the contract ------------------------------------------------------- */

ag_err_t ag_port_usbd_start(const ag_usbd_msc_cbs_t *cbs)
{
    if (s_up) {
        return -AG_EBUSY;
    }
    if (cbs == NULL) {
        return -AG_EINVAL;
    }

    /*
     * Free the internal PHY from the console.  The driver's uninstall leaves
     * the pad enable alone on purpose (its own comment: USJ stdout may still
     * want it), but it is the driver that owns the peripheral's PHY select, and
     * the OTG's usb_new_phy below re-takes the pads - this order, console first,
     * is the whole of the handover.
     *
     * uart_hw.c's s_jtag_open stays true across this on purpose: nothing may
     * touch the console port while the session runs, and stop() reinstalls the
     * driver so the flag is true again when the console re-attaches.
     */
    s_jtag_was_up = usb_serial_jtag_is_driver_installed();
    if (s_jtag_was_up) {
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        (void)usb_serial_jtag_driver_uninstall();
    }

    /*
     * Refuse if the OTG-side PHY is already held - the USB host (usb_hw.c) is
     * the only other claimant and both are build-time options, but a session
     * left up by a previous owner must not be silently stacked.  The check does
     * not see the Serial-JTAG (that driver never registers with usb_phy), which
     * is fine: it was just taken down above.
     */
    usb_phy_status_t st = USB_PHY_STATUS_FREE;
    if (usb_phy_get_phy_status(USB_PHY_TARGET_INT, &st) == ESP_OK &&
        st != USB_PHY_STATUS_FREE) {
        reinstall_jtag();
        return -AG_EBUSY;
    }

    const usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    if (usb_new_phy(&phy, &s_phy) != ESP_OK) {
        reinstall_jtag();
        return -AG_EIO;
    }

    if (!tusb_init()) {
        (void)usb_del_phy(s_phy);
        s_phy = NULL;
        reinstall_jtag();
        return -AG_EIO;
    }

    s_cbs = *cbs;
    s_up = true;
    s_mounted = false;
    s_run = true;
    if (xTaskCreate(usbd_task, "usbd", 4096, NULL, 5, &s_task) != pdPASS) {
        (void)tusb_deinit(USBD_RHPORT);
        (void)usb_del_phy(s_phy);
        s_phy = NULL;
        s_up = false;
        s_run = false;
        memset(&s_cbs, 0, sizeof(s_cbs));
        reinstall_jtag();
        return -AG_ENOMEM;
    }
    return AG_OK;
}

ag_err_t ag_port_usbd_stop(void)
{
    if (!s_up) {
        return AG_OK;
    }
    s_up = false;
    s_run = false;

    /* Drop the application's table first, so a dying application cannot leave
     * the port calling its freed code while the teardown drains. */
    memset(&s_cbs, 0, sizeof(s_cbs));

    /* Let the task see s_run and exit before the stack under it goes. */
    for (int i = 0; i < 25 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    (void)tusb_deinit(USBD_RHPORT);
    if (s_phy != NULL) {
        (void)usb_del_phy(s_phy);
        s_phy = NULL;
    }

    /* Restore the USB-Serial-JTAG console, which re-takes the PHY. */
    if (s_jtag_was_up) {
        reinstall_jtag();
    }
    return AG_OK;
}

ag_err_t ag_port_usbd_status(ag_usbd_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    out->up = s_up;
    out->mounted = s_mounted;
    return AG_OK;
}

#endif /* SOC_USB_OTG_SUPPORTED */

#endif /* CONFIG_ARGON_USB_MSC */
