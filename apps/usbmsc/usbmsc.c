/*
 * ArgonOS - the board as a USB card reader (stage 1: read-only).
 *
 * Build it with the SDK's image tool and run it from the shell:
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o USBMSC.AXE apps/usbmsc/usbmsc.c
 *   run usbmsc.axe [seconds]
 *
 * It lends the card that is already mounted as /dev/sd0 to a PC as a USB
 * mass-storage volume, read-only: the host mounts it read-only, so files can be
 * copied off with no card reader and nothing on the card can be corrupted.
 *
 * The price, stated before it happens: the board has ONE USB port, and it is
 * the console.  While the card reader runs, that console is gone - there is no
 * way to have both, and no error is raised, so the stop condition can never be
 * a typed command.  The session ends on a host-side eject, a USB unplug, or the
 * timeout (default 60 seconds, override with argv[1]), whichever comes first,
 * and then the console comes back.
 *
 * All policy lives here, in the application, exactly as the plan said: which
 * device to lend (sd0), read-only versus writable (read-only), the timeout, and
 * what is printed.  The port only installed TinyUSB and forwards the MSC
 * callbacks to the table below; the kernel only detached and re-attached the
 * console endpoint around it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("USBMSC", "1.0", "argon", 0);

#define USBMSA_DEFAULT_TIMEOUT_S 60u

/* Shared with the TinyUSB task, which runs the callbacks below.  Single-word
 * flags only, so a plain volatile read and write is enough - the same shape the
 * port's own usb_hw.c uses for cross-task state. */
static ag_handle_t      s_dev;
static volatile uint32_t s_block_size;
static volatile uint32_t s_block_count;
static volatile bool     s_ejected;   /* the host ejected the volume            */
static volatile bool     s_host_gone; /* the host unplugged the USB             */

/* ---- the MSC backend, in the application -------------------------------- */
/*
 * These run on the port's TinyUSB task, not on this process's thread, so they
 * must not block and must not print.  They read the card through the existing
 * block device /dev/sd0 - which is how the backend stays behind argon_port's
 * own SD mount and its measured cs_hold fix, instead of reaching the SPI bus
 * directly.
 */

static int32_t msc_capacity(uint32_t lun, uint32_t *block_count,
                            uint32_t *block_size)
{
    (void)lun;
    *block_count = s_block_count;
    *block_size = s_block_size;
    return 0;
}

static int32_t msc_read10(uint32_t lun, uint32_t lba, uint32_t offset,
                          void *buf, uint32_t bufsize)
{
    (void)lun;
    const int64_t off = (int64_t)lba * (int64_t)s_block_size + (int64_t)offset;
    if (ag_seek(s_dev, off, AG_SEEK_SET) != off) {
        return -1;
    }
    const int32_t n = ag_read(s_dev, buf, bufsize);
    return (n < 0) ? -1 : n;
}

/* Stage 1 is read-only: refuse the write, and say so in the host's own terms. */
static int32_t msc_write10(uint32_t lun, uint32_t lba, uint32_t offset,
                           const void *buf, uint32_t bufsize)
{
    (void)lun;
    (void)lba;
    (void)offset;
    (void)buf;
    (void)bufsize;
    return 0;
}

static bool msc_is_writable(uint32_t lun)
{
    (void)lun;
    return false;
}

/* The presence probe: a cheap re-read of sector 0.  There is no card-detect
 * line on this board, so "media present" is "sector 0 still answers". */
static bool msc_unit_ready(uint32_t lun)
{
    (void)lun;
    uint8_t b;
    if (ag_seek(s_dev, 0, AG_SEEK_SET) != 0) {
        return false;
    }
    return ag_read(s_dev, &b, 1) == 1;
}

/* SCSI START STOP UNIT: the host asking the media to load or eject.  The eject
 * is the clean host-side stop. */
static bool msc_start_stop(uint32_t lun, bool start, bool eject)
{
    (void)lun;
    if (eject) {
        s_ejected = true;
    }
    return start;
}

static void msc_mounted(void) { /* the host attached; nothing to do */ }

static void msc_unmounted(void)
{
    s_host_gone = true;
}

static const ag_usbd_msc_cbs_t k_cbs = {
    .capacity = msc_capacity,
    .read10 = msc_read10,
    .write10 = msc_write10,
    .is_writable = msc_is_writable,
    .unit_ready = msc_unit_ready,
    .start_stop = msc_start_stop,
    .mounted = msc_mounted,
    .unmounted = msc_unmounted,
};

/* Small unsigned decimal; no libc, and a timeout never needs more than this. */
static uint32_t arg_seconds(const char *s, uint32_t fallback)
{
    if (s == NULL || *s == '\0') {
        return fallback;
    }
    uint32_t v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return fallback;
        }
        v = v * 10u + (uint32_t)(*p - '0');
    }
    return (v == 0) ? fallback : v;
}

int ag_main(int argc, char **argv)
{
    const uint32_t timeout_s =
        (argc > 1) ? arg_seconds(argv[1], USBMSA_DEFAULT_TIMEOUT_S)
                   : USBMSA_DEFAULT_TIMEOUT_S;

    if (!ag_usbd_available()) {
        ag_print("card reader: this firmware has no USB device support "
                 "(CONFIG_ARGON_USB_MSC is off)\n");
        return 1;
    }

    /* No card means no session: the block device only exists while the card is
     * mounted, so a missing /dev/sd0 is the refusal. */
    s_dev = ag_dev_open("sd0");
    if (s_dev < 0) {
        ag_printf("card reader: no card to lend (open sd0: %s)\n",
                  ag_strerror(s_dev));
        return 1;
    }

    ag_geometry_t geo;
    const ag_err_t gerr =
        ag_dev_ioctl(s_dev, AG_IOC_GEOMETRY, &geo, sizeof(geo));
    if (gerr != AG_OK || geo.sectors == 0) {
        ag_printf("card reader: card reports no usable geometry (%s)\n",
                  ag_strerror(gerr));
        ag_dev_close(s_dev);
        return 1;
    }
    s_block_size = geo.sector_size;
    s_block_count = (uint32_t)geo.sectors;

    /* Everything after this point is printed BEFORE the port takes the PHY,
     * because the moment start() succeeds the console is gone from its USB port
     * and prints stop arriving. */
    ag_printf("card reader: lending sd0 (%u sectors of %u bytes), read-only\n",
              (unsigned)s_block_count, (unsigned)s_block_size);
    ag_print("  THIS BOARD'S USB PORT IS THE CONSOLE, so the console now goes "
             "away.\n");
    ag_print("  On the PC, copy files off the new drive, then EJECT it (or "
             "unplug)\n");
    ag_printf("  to end early; otherwise it stops by itself in %u second(s).\n",
              (unsigned)timeout_s);
    ag_print("  If the console does not come back: power-cycle the board "
             "(usbmsc\n");
    ag_print("  is a command, never the boot path, so a reset restores it).\n");

    const ag_err_t start = ag_usbd_start(&k_cbs);
    if (start != AG_OK) {
        ag_printf("card reader: could not take the USB port (%s)\n",
                  ag_strerror(start));
        ag_dev_close(s_dev);
        return 1;
    }

    /* The session.  Nothing here can be a typed command - there is no console -
     * so the only ends are the host ejecting/unplugging, or the timeout. */
    const uint32_t began = ag_millis();
    while (!s_ejected && !s_host_gone) {
        if (ag_millis() - began >= timeout_s * 1000u) {
            break;
        }
        ag_delay(100);
    }

    (void)ag_usbd_stop(); /* console restored here */

    ag_print("card reader: session over, console restored.\n");
    if (s_ejected) {
        ag_print("  ended by the host ejecting the drive.\n");
    } else if (s_host_gone) {
        ag_print("  ended by the host unplugging.\n");
    } else {
        ag_print("  ended by the timeout.\n");
    }

    ag_dev_close(s_dev);
    return 0;
}
