/*
 * ArgonOS - the device example.
 *
 * Build it with the SDK's image tool and run it from the shell:
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o DEVS.AXE apps/devs/devs.c
 *   run t:\devs.axe
 *
 * Everything an application does with devices: find out what exists, open one
 * by name, read and write it, ask it about itself with ioctl, and be told no
 * when it asks for something the device does not do.  It checks each answer and
 * returns non-zero if any of them was wrong, so `run` plus `errorlevel` is a
 * test of the dev sub-table - and, because this is a loaded image rather than
 * something built into the kernel, a test that the sub-table is really the way
 * in and not a shortcut the built-ins take.
 *
 * The habit worth copying: ask AG_HAS before using anything, because a profile
 * without devices is a real profile and "it crashed" is a worse answer than
 * "this board has no device manager".
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("DEVS", "1.0", "argon", 0);

static int s_failures;

static void failed(const char *what, ag_err_t err)
{
    ag_printf("  %s: %s (%d)\n", what, ag_strerror(err), (int)err);
    s_failures++;
}

static void wrong(const char *what)
{
    ag_printf("  %s\n", what);
    s_failures++;
}

/* ---- what this board has ------------------------------------------------ */

static const char *class_name(ag_dev_class_t cls)
{
    switch (cls) {
    case AG_DEV_BUS:     return "bus";
    case AG_DEV_BLOCK:   return "block";
    case AG_DEV_CHAR:    return "char";
    case AG_DEV_DISPLAY: return "display";
    case AG_DEV_INPUT:   return "input";
    case AG_DEV_SENSOR:  return "sensor";
    case AG_DEV_NET:     return "net";
    case AG_DEV_GPIO:    return "gpio";
    case AG_DEV_AUDIO:   return "audio";
    case AG_DEV_STORAGE: return "storage";
    case AG_DEV_MOTOR:   return "motor";
    default:             return "?";
    }
}

static void list_devices(void)
{
    ag_print("devices:\n");

    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_enumerate(i, AG_DEV_ANY, &info) != AG_OK) {
            break;
        }
        ag_printf("  %-8s %-8s %-10s%s\n", info.name, class_name(info.cls),
                  info.driver,
                  (info.flags & AG_DEVF_READONLY) ? " read-only" : "");
    }
}

/* Filtering is not a convenience: it is how an application finds the one it
 * needs without knowing what anybody called it. */
static void list_storage(void)
{
    uint32_t found = 0;

    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_enumerate(i, AG_DEV_STORAGE, &info) != AG_OK) {
            break;
        }
        if (info.cls != AG_DEV_STORAGE) {
            wrong("the storage filter returned something else");
        }
        found++;
    }
    ag_printf("storage devices: %u\n", (unsigned)found);
}

/* ---- reading and writing ------------------------------------------------ */

static void read_zero(void)
{
    const ag_handle_t h = ag_dev_open("zero");
    if (h < 0) {
        failed("open zero", h);
        return;
    }

    uint8_t buf[32];
    memset(buf, 0xa5, sizeof(buf));

    const int32_t n = ag_read(h, buf, sizeof(buf));
    if (n != (int32_t)sizeof(buf)) {
        ag_printf("  zero gave %d bytes, wanted %u\n", (int)n,
                  (unsigned)sizeof(buf));
        s_failures++;
    } else {
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0) {
                wrong("zero produced something other than zero");
                break;
            }
        }
    }

    /* A device that is a file: the same close as a file, and the same handle
     * table, which is why nothing leaks when this process ends. */
    ag_close(h);
    ag_printf("read %d bytes from zero\n", (int)n);
}

static void write_null(void)
{
    const ag_handle_t h = ag_dev_open("null");
    if (h < 0) {
        failed("open null", h);
        return;
    }

    const char text[] = "this goes nowhere";
    const int32_t n = ag_write(h, text, sizeof(text) - 1);
    if (n != (int32_t)(sizeof(text) - 1)) {
        failed("write null", n);
    }

    /* And nothing comes back out of it, which is the end of file. */
    char    buf[8];
    const int32_t got = ag_read(h, buf, sizeof(buf));
    if (got != 0) {
        ag_printf("  null returned %d bytes; it should return none\n",
                  (int)got);
        s_failures++;
    }

    ag_close(h);
    ag_printf("wrote %d bytes to null\n", (int)n);
}

/* ---- asking a device about itself --------------------------------------- */

static void ask_flash(void)
{
    const ag_handle_t h = ag_dev_open("flash0");
    if (h < 0) {
        /* Not a failure: a board without one is a board without one. */
        ag_print("no flash0 on this board\n");
        return;
    }

    /* Every device answers this one, whatever else it does or does not. */
    ag_devinfo_t info;
    const ag_err_t err = ag_dev_ioctl(h, AG_IOC_INFO, &info, sizeof(info));
    if (err != AG_OK) {
        failed("ioctl info", err);
    } else {
        ag_printf("flash0 is a %s device from %s\n", class_name(info.cls),
                  info.driver);
    }

    ag_geometry_t geo;
    if (ag_dev_ioctl(h, AG_IOC_GEOMETRY, &geo, sizeof(geo)) == AG_OK) {
        ag_printf("  %u sectors of %u bytes\n", (unsigned)geo.sectors,
                  (unsigned)geo.sector_size);
    } else {
        wrong("flash0 did not report its geometry");
    }

    /* A command the device does not implement is refused, not guessed at. */
    if (ag_dev_ioctl(h, AG_IOC_RESET, 0, 0) != -AG_ENOTSUP) {
        wrong("an unimplemented ioctl should answer -AG_ENOTSUP");
    }

    /* Read-only means it: the write is refused rather than quietly dropped. */
    const int32_t written = ag_write(h, "x", 1);
    if (written != -AG_EROFS) {
        ag_printf("  writing to flash0 gave %d, wanted -AG_EROFS\n",
                  (int)written);
        s_failures++;
    }

    /* Reading is a file read, positions and all. */
    uint8_t head[16];
    if (ag_read(h, head, sizeof(head)) != (int32_t)sizeof(head)) {
        wrong("could not read the first bytes of flash0");
    }
    if (ag_seek(h, 0, AG_SEEK_END) <= 0) {
        wrong("flash0 has no length");
    }

    ag_close(h);
}

static void refusals(void)
{
    const ag_handle_t h = ag_dev_open("no-such-device");
    if (h >= 0) {
        wrong("opening a device that does not exist should fail");
        ag_close(h);
    } else if (h != -AG_ENOENT) {
        failed("opening a missing device gave the wrong reason", h);
    }

    /* ioctl on something that is not a device handle is a bad handle, not a
     * crash: a file and a device are both handles, and only one of them has an
     * ioctl. */
    const ag_handle_t f = ag_open("T:\\devs.tmp", AG_O_RDWR | AG_O_CREATE);
    if (f >= 0) {
        ag_devinfo_t info;
        if (ag_dev_ioctl(f, AG_IOC_INFO, &info, sizeof(info)) != -AG_EBADF) {
            wrong("ioctl on a plain file should answer -AG_EBADF");
        }
        ag_close(f);
        ag_unlink("T:\\devs.tmp");
    }
}

/* ---- the pins themselves ------------------------------------------------ */

/*
 * Direct access is allowed, and it is also the one place where an application
 * can quietly break the machine it is running on.  Everything here is either a
 * read, which changes nothing, or a claim of a pin the system has said is free.
 */
static void use_pins(void)
{
    const ag_io_api_t *io = ag_api()->io;
    if (io == NULL) {
        ag_print("no direct hardware access in this build\n");
        return;
    }

    /*
     * The console's own pin.  Refused, and that refusal is the whole point of
     * the pin table: an application that took it would end the conversation it
     * is reporting over, and nobody would ever see why.
     */
    const ag_err_t console = io->gpio_config(43, AG_GPIO_OUT);
    if (console != -AG_EACCES) {
        ag_printf("  driving the console pin gave %d, wanted -AG_EACCES\n",
                  (int)console);
        s_failures++;
    } else {
        ag_print("the console pin is refused, as it should be\n");
    }

    /* A pin that is nobody's.  Configure it, drive it, read it back. */
    const int pin = 5;
    ag_err_t  err = io->gpio_config(pin, AG_GPIO_OUT);
    if (err != AG_OK) {
        failed("gpio_config", err);
        return;
    }

    io->gpio_write(pin, 1);
    const int high = io->gpio_read(pin);
    io->gpio_write(pin, 0);
    const int low = io->gpio_read(pin);
    ag_printf("pin %d reads %d when driven high and %d when driven low\n", pin,
              high, low);

    /* Reading is allowed anywhere - it is how a wiring problem gets looked at
     * rather than guessed at - so a reserved pin still answers. */
    if (io->gpio_read(43) < 0) {
        wrong("reading a reserved pin should still work");
    }
    /* A pin the chip does not have is out of range, not a crash. */
    if (io->gpio_read(9999) != -AG_ERANGE) {
        wrong("a pin that does not exist should answer -AG_ERANGE");
    }

    /*
     * Not released on purpose: the point being made is that ending the process
     * gives it back anyway.  `io` at the prompt afterwards shows pin 5 free.
     */
}

static void buses(void)
{
    const ag_io_api_t *io = ag_api()->io;
    if (io == NULL) {
        return;
    }

    /*
     * On a board whose BOARD.CFG never named the pins there is no bus, and the
     * answer says exactly that rather than pretending the bus is empty.  The
     * two are different: -AG_ENODEV is "no such bus", -AG_ENOENT is "the bus is
     * fine and nothing is at that address".
     */
    const ag_err_t err = io->i2c_probe(0, 0x50);
    if (err == AG_OK) {
        ag_print("something answers at 0x50 on i2c0\n");
    } else if (err == -AG_ENOENT) {
        ag_print("i2c0 works and has nothing at 0x50\n");
    } else if (err == -AG_ENODEV) {
        ag_print("this board has no i2c0 in BOARD.CFG\n");
    } else {
        failed("i2c_probe", err);
    }

    /* The analogue input is a build option, so ask rather than assume. */
    if (AG_HAS(io, adc_read)) {
        ag_printf("adc channel 0 reads %d\n", (int)io->adc_read(0));
    } else {
        ag_print("no analogue input in this build\n");
    }
}

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!AG_HAS(ag_api()->dev, enumerate)) {
        ag_print("this build has no device manager\n");
        return 1;
    }

    list_devices();
    ag_print("\n");
    list_storage();
    read_zero();
    write_null();
    ask_flash();
    refusals();
    ag_print("\n");
    use_pins();
    buses();

    ag_printf("\n%s\n", (s_failures == 0) ? "all checks passed"
                                          : "some checks failed");
    return (s_failures == 0) ? 0 : 1;
}
