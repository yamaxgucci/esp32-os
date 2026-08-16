/*
 * ArgonOS port contract - the two filesystems a board is born with.
 *
 * ArgonOS has its own VFS and its own path layer; what it does not have, and
 * deliberately does not want, is its own FAT and its own wear levelling.  Those
 * exist below, tested by more people than this project will ever have, and
 * rewriting them would be work with no product in it.  So the port mounts them
 * somewhere and the kernel adapts what it finds - through a POSIX-shaped
 * backend (src/fs/idfvfs.c), which is why nothing above this line knows the
 * difference between littlefs and FAT.
 *
 * That is also the reason this contract is about mounting rather than about
 * blocks.  A port that has a filesystem gets to keep it; a port that has none
 * has to bring one, and that is a real piece of work rather than a wrapper -
 * which is worth knowing before starting rather than after.
 *
 * The internal filesystem
 * -----------------------
 *
 *   ag_err_t ag_port_sysfs_mount(const char *label, const char *base_path,
 *                                bool format_if_needed)
 *   ag_err_t ag_port_sysfs_unmount(const char *label)
 *   ag_err_t ag_port_sysfs_space(const char *label, uint64_t *total,
 *                                uint64_t *used)
 *
 * It must survive a power cut in the middle of a write.  This is where the
 * configuration and the crash log live, and a board that loses its
 * configuration because the power went out while it was being written is a
 * board somebody has to visit.  A blank or foreign partition is formatted on
 * first mount, because there is nothing on it to lose - which is the opposite
 * of the rule for the card below.
 *
 * Removable media
 * ---------------
 *
 *   ag_err_t ag_port_sd_mount(const ag_port_sd_cfg_t *cfg,
 *                             const char *base_path, bool allow_format,
 *                             ag_port_sd_t *out)
 *   ag_err_t ag_port_sd_unmount(const char *base_path, ag_port_sd_t card)
 *   uint32_t ag_port_sd_sector_size(ag_port_sd_t card)
 *   uint64_t ag_port_sd_sectors(ag_port_sd_t card)
 *   ag_err_t ag_port_sd_read_sectors(ag_port_sd_t card, uint64_t sector,
 *                                    void *buf, uint32_t count)
 *   ag_err_t ag_port_media_space(const char *base_path, uint64_t *total,
 *                                uint64_t *avail)
 *
 * A card is somebody's data.  `allow_format` is true only when the operator
 * asked for it by name, and a port must never format as a way of recovering
 * from a mount that failed.
 *
 * spi_host is the number the schematic writes, which is the number the board
 * file uses and the number an operator reads off the datasheet.  Whatever the
 * chip calls it internally is the port's business.
 *
 * A port with no removable media returns -AG_ENODEV from mount and is finished:
 * the system boots without a card every day.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_STORAGE_H
#define ARGON_PORT_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

typedef enum {
    AG_PORT_SD_NONE = 0,
    AG_PORT_SD_NATIVE, /* the chip's own SD host, 1 or 4 bit */
    AG_PORT_SD_SPI     /* a card wired to an SPI bus         */
} ag_port_sd_kind_t;

typedef struct {
    ag_port_sd_kind_t kind;
    uint8_t           width;   /* 1 or 4, native only          */
    uint32_t          max_khz;
    /* Native pins.  Negative means "the chip's default for this signal". */
    int16_t clk, cmd, d0, d1, d2, d3;
    /* SPI pins, and the host as the schematic numbers it. */
    int16_t sck, mosi, miso, cs;
    uint8_t spi_host;
} ag_port_sd_cfg_t;

typedef void *ag_port_sd_t;

ag_err_t ag_port_sysfs_mount(const char *label, const char *base_path,
                             bool format_if_needed);
ag_err_t ag_port_sysfs_unmount(const char *label);
ag_err_t ag_port_sysfs_space(const char *label, uint64_t *total,
                             uint64_t *used);

ag_err_t ag_port_sd_mount(const ag_port_sd_cfg_t *cfg, const char *base_path,
                          bool allow_format, ag_port_sd_t *out);
ag_err_t ag_port_sd_unmount(const char *base_path, ag_port_sd_t card);
uint32_t ag_port_sd_sector_size(ag_port_sd_t card);
uint64_t ag_port_sd_sectors(ag_port_sd_t card);
ag_err_t ag_port_sd_read_sectors(ag_port_sd_t card, uint64_t sector, void *buf,
                                 uint32_t count);

ag_err_t ag_port_media_space(const char *base_path, uint64_t *total,
                             uint64_t *avail);

#endif /* ARGON_PORT_STORAGE_H */
