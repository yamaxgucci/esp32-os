/*
 * ArgonOS port: executable memory outside the internal SRAM arena.
 *
 * On the ESP32-S3 the code arena and the radios compete for the same ~330 KB
 * of internal SRAM, because instruction RAM and the DMA-capable data RAM are
 * one physical memory seen through two buses.  A 192 KB arena leaves no room
 * for Wi-Fi and Bluetooth at once - see the arena note in the S3 defaults.
 *
 * This is the way out that only this chip has: PSRAM reaches the instruction
 * bus as well as the data bus, so a block of it can be mapped twice - once
 * writable (the data window, where the loader assembles an image) and once
 * executable (the instruction window, where the processor fetches it).  The
 * arena then lives in the 8 MB of PSRAM and the internal SRAM it used to hold
 * goes back to the radios and the heap.
 *
 *   ag_err_t ag_port_execmem_map(size_t bytes, void **exec, void **writable,
 *                                ag_port_map_t *out)
 *       Reserve `bytes` of PSRAM and map it into both windows.  `exec` is where
 *       code runs from and is what an image's addresses must point at; the
 *       loader writes bytes through `writable` and calls sync() before the
 *       first fetch.  The two differ by a fixed offset, so an allocation at
 *       writable + k executes at exec + k.
 *   void     ag_port_execmem_sync(void *writable, size_t bytes)
 *       Make writes through `writable` visible to instruction fetch: push the
 *       data cache to PSRAM and drop the stale instruction cache.  Called after
 *       an image is placed and relocated, before it is entered.
 *   void     ag_port_execmem_unmap(ag_port_map_t h)
 *       Give the region back.  The arena is mapped once for the life of the
 *       system, so this is really only for the failure path of map().
 *
 * A port with no such memory - the original ESP32, or any build without PSRAM -
 * returns -AG_ENOTSUP from map(), and the loader keeps its internal SRAM arena.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_EXECMEM_H
#define ARGON_PORT_EXECMEM_H

#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>       /* ag_err_t */
#include <argon/port/flash.h> /* ag_port_map_t */

ag_err_t ag_port_execmem_map(size_t bytes, void **exec, void **writable,
                             ag_port_map_t *out);
void     ag_port_execmem_sync(void *writable, size_t bytes);
void     ag_port_execmem_unmap(ag_port_map_t h);

#endif /* ARGON_PORT_EXECMEM_H */
