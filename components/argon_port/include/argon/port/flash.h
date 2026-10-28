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
 *   ag_err_t ag_port_part_map_data(ag_port_part_t p, uint64_t off, size_t len,
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

typedef const void *ag_port_part_t;
typedef uintptr_t   ag_port_map_t;

ag_port_part_t ag_port_part_find(const char *label);
uint64_t       ag_port_part_size(ag_port_part_t p);
uint32_t       ag_port_part_erase_size(ag_port_part_t p);

/*
 * The partition's subtype byte, 0xFF when there is no partition.
 *
 * find() already refuses anything that is not a data partition, so the
 * firmware itself is out of reach - but C: (littlefs) and nvs are data too,
 * and an application that could erase those could destroy the machine's
 * configuration by typing one wrong label.  The subtype is how a caller tells
 * "a raw store nobody else owns" from "someone's filesystem".
 */
uint8_t        ag_port_part_subtype(ag_port_part_t p);

ag_err_t ag_port_part_read(ag_port_part_t p, uint64_t off, void *buf,
                           size_t len);
ag_err_t ag_port_part_write(ag_port_part_t p, uint64_t off, const void *buf,
                            size_t len);

/*
 * The same flash, in the data address space instead of the instruction one.
 *
 * map_exec puts a range where the processor can fetch from it, which is what
 * code needs and what data cannot use: on this class of part an instruction-bus
 * address cannot be read as bytes at all.  A cartridge, a font, a table of
 * samples - anything the program reads rather than runs - wants this one.
 *
 * Read only, and free of charge in memory: the bytes stay in flash and go
 * through the same cache the processor's own code does.
 */
ag_err_t ag_port_part_map_data(ag_port_part_t p, uint64_t off, size_t len,
                               const void **addr, ag_port_map_t *out);
ag_err_t ag_port_part_erase(ag_port_part_t p, uint64_t off, size_t len);

ag_err_t ag_port_part_map_exec(ag_port_part_t p, uint64_t off, size_t len,
                               const void **addr, ag_port_map_t *out);
void     ag_port_part_unmap(ag_port_map_t h);

/*
 * WHILE SOUND IS PLAYING, THE FLASH MUST STAND STILL.
 *
 * Every read, write and erase of this flash begins by switching the cache off
 * for the whole chip - both cores, instructions and data, and the external RAM
 * with them, because there is one cache and it serves all of it.  For as long
 * as the operation lasts, a core fetching its own code stops dead.  That is
 * why an interrupt handler must live in IRAM, and it is why a block of audio
 * is lost when a file is read underneath it.
 *
 * External RAM has no such property.  An access to it is an ordinary cache
 * miss: dear, but nothing stops, the other core keeps running and interrupts
 * are served.  So a board with PSRAM can put everything it needs somewhere
 * that never has to stop, and then leave the flash alone.
 *
 * The rule belongs to the system rather than to the program, because the
 * program is not who breaks it: the Wi-Fi stack writes its calibration to
 * NVS, a log line is flushed to a file, something reads a configuration -
 * none of them know an amplifier is running.  So the guard is raised by the
 * converter itself, around the time it is actually clocking frames out
 * (ag_port_flash_guard, from the pcm driver's open and close), and it covers
 * everybody.
 *
 * Two levels, in this order on purpose:
 *
 *   watching  - the default.  Every flash operation attempted while sound is
 *               playing is counted and the task that made it is named in the
 *               journal.  Nothing is refused: a refusal inside the Wi-Fi
 *               stack is a new class of failure, and the first thing wanted
 *               is the list of who turns up at all.
 *   strict    - `[flash] strict = yes`.  Those operations are refused with
 *               -AG_EBUSY.  A caller that cannot take no for an answer must
 *               stop the sound first, which is the honest cost of the rule.
 *
 * Mapping is not reading: a region already mapped for execution is fetched
 * through the cache like any other code and never switches it off.  Code
 * mapped before the sound started keeps running either way.
 */
void ag_port_flash_guard(int delta);
bool ag_port_flash_guarded(void);
void ag_port_flash_strict(bool on);
bool ag_port_flash_is_strict(void);

/*
 * How many flash operations have been attempted while sound was playing, and
 * the name of the task that made the last one.  `who` may be NULL.  The count
 * is since boot: it is a defect counter, and one that resets itself when read
 * is one nobody can quote.
 */
uint32_t ag_port_flash_violations(const char **who);

/*
 * THE MIRROR: the filesystem in RAM, the flash only its source.
 *
 * Reading this flash is what stops the machine, and the system reads it all
 * the time without meaning to - a file's size, a directory entry, a
 * configuration, the page a browser asked for.  Forbidding that is the wrong
 * cure, because every one of those is something that genuinely has to work
 * while sound is playing.
 *
 * So the partition is copied into external RAM once, at boot, and every read
 * after that is answered from the copy: no cache off, no stall, no rule for
 * anybody to remember.  A write goes to the flash as it always did and to the
 * copy with it, so the two cannot drift apart - a write is rare and deliberate
 * and nobody plays through one.
 *
 * It costs the partition's size in external RAM (1 MB for C: on this board,
 * against 4 MB fitted).  A board without the memory to spare keeps reading
 * the flash exactly as before: mirror() returns false and nothing else
 * changes.
 */
bool     ag_port_flash_mirror(const char *label);
uint32_t ag_port_flash_mirror_reads(uint32_t *bytes);
bool     ag_port_flash_mirrored(void);

/*
 * HOLDING A WRITE UNTIL THE SOUND STOPS.
 *
 * A write is rarer than a read and nobody saves a preset in the middle of a
 * chord - but when it does happen while a converter is clocking frames, it
 * costs the same erase-and-write with the cache off, and the blocks it eats
 * are not recoverable.
 *
 * With the mirror in place there is somewhere to put it.  The write goes
 * into the mirror at once, so the file IS changed for everything that reads
 * it, and the chip is left alone; the sector is remembered as dirty and
 * written out as soon as the last converter closes - or on `flash sync`, or
 * before an orderly restart.
 *
 * What this costs, said plainly: power cut off during a performance, with a
 * write made during that same performance, loses that write.  Nothing else
 * is at risk - a write made while nothing played went to the chip
 * immediately, as it always did - and the system says what is outstanding
 * rather than hiding it.  Set `[flash] hold = no` and every write goes
 * straight through, at the cost of the blocks.
 */
void     ag_port_flash_hold(bool on);
uint32_t ag_port_flash_dirty(void);
uint32_t ag_port_flash_flush(void);

#endif /* ARGON_PORT_FLASH_H */
