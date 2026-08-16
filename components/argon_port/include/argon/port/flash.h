/*
 * ArgonOS port contract - the flash the system lives in.
 *
 * Two different things are wanted from it and they are worth keeping apart.
 *
 * Reading and writing a named region is the ordinary one: a block device the
 * shell can hexdump, and the store an application image is written into.
 *
 * Mapping a region so the processor can fetch instructions out of it is the
 * other, and it is what makes an application larger than the code arena
 * possible at all (R-1, execute in place).  A port with no such mapping returns
 * -AG_ENOTSUP from map(), and the system then runs only what fits in the arena
 * - which is a smaller system, not a broken one.
 *
 * What a port must supply:
 *
 *   ag_port_part_t   a handle to a named region, NULL when there is none
 *   ag_port_map_t    a handle to a live mapping
 *
 *   ag_port_part_t ag_port_part_find(const char *label)
 *   uint64_t ag_port_part_size(ag_port_part_t p)
 *   uint32_t ag_port_part_erase_size(ag_port_part_t p)
 *
 *   ag_err_t ag_port_part_read(ag_port_part_t p, uint64_t off, void *buf,
 *                              size_t len)
 *   ag_err_t ag_port_part_write(ag_port_part_t p, uint64_t off,
 *                               const void *buf, size_t len)
 *   ag_err_t ag_port_part_erase(ag_port_part_t p, uint64_t off, size_t len)
 *
 *   ag_err_t ag_port_part_map_exec(ag_port_part_t p, uint64_t off, size_t len,
 *                                  const void **addr, ag_port_map_t *out)
 *   void     ag_port_part_unmap(ag_port_map_t h)
 *
 * Contract, not advice:
 *
 * - erase_size is what a write actually costs, not the read granularity.  The
 *   shell reports it as the sector size, because that is the number that tells
 *   an operator what writing one byte will do.
 * - write() may assume the region was erased.  The caller erases.
 * - map_exec() gives an address instructions can be fetched from.  A port that
 *   can only map for reading fails rather than returning a data address: the
 *   loader would jump to it.
 * - Distinguish the failures rather than returning -AG_EIO for everything.  The
 *   caller prints the number and nothing else - there is no way to hand a
 *   vendor error string across this boundary, and inventing one would put a
 *   foreign error space back into the kernel - so the number is the whole of
 *   the diagnosis and -AG_ENOSPC has to mean what it says.
 *
 * A warning worth carrying across ports: on the S3 the window that flash and
 * PSRAM are mapped through is one 32 MB window shared between them.  With 32 MB
 * of PSRAM fitted there are no addresses left, and the partition table cannot be
 * read at all - which looks like a corrupt table while the raw reads are
 * perfectly fine (pitfall 1 in docs/05-status.md).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_FLASH_H
#define ARGON_PORT_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

typedef const void *ag_port_part_t;
typedef uintptr_t   ag_port_map_t;

ag_port_part_t ag_port_part_find(const char *label);
uint64_t       ag_port_part_size(ag_port_part_t p);
uint32_t       ag_port_part_erase_size(ag_port_part_t p);

ag_err_t ag_port_part_read(ag_port_part_t p, uint64_t off, void *buf,
                           size_t len);
ag_err_t ag_port_part_write(ag_port_part_t p, uint64_t off, const void *buf,
                            size_t len);
ag_err_t ag_port_part_erase(ag_port_part_t p, uint64_t off, size_t len);

ag_err_t ag_port_part_map_exec(ag_port_part_t p, uint64_t off, size_t len,
                               const void **addr, ag_port_map_t *out);
void     ag_port_part_unmap(ag_port_map_t h);

#endif /* ARGON_PORT_FLASH_H */
