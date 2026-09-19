/*
 * ArgonOS - loading and running an application.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_LOADER_H
#define ARGON_LOADER_H

#include <argon/axeload.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The guard past an application's data - see `guard` below and guard_arm in
 * loader.c.  Sixteen bytes keeps the data part's 16-byte alignment for
 * whatever the heap hands out next, and is wide enough that a plausible
 * overrun lands in it rather than stepping over it.
 */
#define AG_APP_GUARD_BYTES 16u
#define AG_APP_GUARD_BYTE  0xA5u

typedef struct {
    ag_axe_header_t  header;
    ag_axe_place_t   place;
    ag_axe_binding_t binding;

    /*
     * The data part's allocation, when it has one of its own.  NULL for a
     * contiguous image, whose data lives inside the code allocation and is
     * released with it.
     */
    void *data_owned;

    /*
     * Sixteen bytes of a known pattern immediately past the application's
     * data, inside an allocation this loader owns.  This chip has no memory
     * protection unit, so an application that walks off the end of one of its
     * own buffers writes into whatever the heap put next - and the board then
     * dies minutes later, in an allocator, with nothing to point at.  The
     * guard cannot stop that write; it is how the system finds out whose it
     * was.  NULL for an image with no data part.
     */
    void *guard;

    /*
     * R-1 flash XIP: code runs from a mapped appfs slot.  `code_scratch` held
     * the relocated bytes until they were programmed; it is freed after mmap.
     * `xip_slot` is an opaque ag_appfs_slot_t*.
     */
    void *code_scratch;
    void *xip_slot;
    bool  code_from_xip;
    /*
     * The chunked XIP path (a small rolling scratch instead of a code-size one)
     * relocates and programs the appfs slot itself, so the orchestrator must not
     * program it again - it only maps it.
     */
    bool  xip_programmed;

    /*
     * S-1: code placed in the PSRAM arena.  It is written through
     * place.code_writable (the data window) and executed at place.code (the
     * instruction window); the block is freed back to the PSRAM arena on
     * release, and the whole-arena mapping stays for the next image.
     */
    bool  code_from_psram;
} ag_loaded_app_t;

/*
 * Reads a .AXE, places its code in executable memory and its data in writable
 * memory, and binds the syscall table into it.  Nothing runs yet.
 */
ag_err_t ag_loader_load(const char *path, const char *cwd,
                        ag_loaded_app_t *out);

/*
 * Just the header, validated, without placing or relocating anything.  For a
 * caller that has to size something before the image can be loaded - the stack
 * it will run on, the arena it asked for.
 */
ag_err_t ag_loader_peek(const char *path, const char *cwd,
                        ag_axe_header_t *out);

void ag_loader_unload(ag_loaded_app_t *app);

/*
 * Has anything written past this application's data?
 *
 * Cheap enough to ask on a timer (sixteen bytes), and safe to ask from
 * anywhere that may read the application's memory: it takes no lock and
 * allocates nothing.  `bytes_past`, when given, receives how far into the
 * guard the damage reaches - a floor, not a measurement: a write that cleared
 * the whole guard went further than the guard can see.
 */
bool ag_loader_guard_broken(const ag_loaded_app_t *app, size_t *bytes_past);

/*
 * The code arena.  Running an image is the process layer's business (argon/proc.h);
 * what belongs here is how much room there is for one.
 *
 * The physical buffer is linked at CONFIG_ARGON_APP_ARENA_KB.  SYSTEM.CFG may
 * shrink the usable size via [memory] app_arena_kb= (see ag_loader_set_arena_kb);
 * the unused tail stays reserved and is not returned to the IDF heap.
 */
size_t ag_loader_arena_size(void);
size_t ag_loader_arena_free(void);
size_t ag_loader_arena_largest(void);
bool   ag_loader_arena_busy(void);

/*
 * Clip usable arena size before the first load.  `kb` is clamped to
 * [4 .. linked ceiling].  Returns the usable size actually selected (bytes).
 * No-op (returns current usable) once the arena has been initialised.
 */
size_t ag_loader_set_arena_kb(uint32_t kb);

/* The syscall table handed to applications. */
const ag_api_t *ag_loader_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LOADER_H */
