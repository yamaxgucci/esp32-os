/*
 * ArgonOS - HostFS wire protocol (guest UART1 ↔ host helper).
 *
 * Little-endian.  Request and response share the same header; `status` is 0
 * on requests and an ag_err_t (0 or negative) on responses.
 *
 * WHICH WAY A FRAME IS GOING is in the top bit of `op` (HSFS_F_FROM_HOST), and
 * it is not decoration.  Without it a request and a successful reply are the
 * same bytes - same magic, same op, same seq, status zero in both - and a guest
 * waiting for a reply accepts anything that matches its own request.
 *
 * That is fine against a host helper, which never sends a request.  It is not
 * fine against ANOTHER GUEST: two ArgonOS boards wired UART1 to UART1 both
 * probe with PING seq 1, each reads the other's probe as its own answer, and
 * both mount the other as a live host folder.  Found exactly that way, on two
 * emulators joined by a node link - and on real boards that wiring is the
 * normal one, so it would have happened there too.
 *
 * The guest therefore ignores any frame without the bit, and tools/hostfsd.py
 * sets it on everything it sends.  The two move together in this tree; an old
 * helper against a new guest simply never mounts.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_HSFS_PROTO_H
#define ARGON_HSFS_PROTO_H

#include <stdint.h>

#define HSFS_MAGIC     0x53465348u /* 'HSFS' */
#define HSFS_MAX_PATH  512u
#define HSFS_MAX_DATA  4096u
#define HSFS_MAX_NAME  255u

enum hsfs_op {
    HSFS_OP_PING = 0,
    HSFS_OP_STAT = 1,
    HSFS_OP_OPENDIR = 2,
    HSFS_OP_READDIR = 3,
    HSFS_OP_CLOSEDIR = 4,
    /*
     * OPEN request: a0 = guest AG_O_* flags (0 = read-only, legacy).
     * Response: a0 = host handle, a1 = size (0 for a new/truncated file).
     */
    HSFS_OP_OPEN = 5,
    /* READ request: a0 = handle, a1 = offset, data_len = max bytes (no payload). */
    HSFS_OP_READ = 6,
    HSFS_OP_CLOSE = 7,
    /*
     * Host → guest only (no request).  Pad snapshot:
     *   data_len == 3 (legacy): pad0, pad1, sys
     *   data_len == 6:          pad0, pad1, sys, pad0hi, pad1hi, ver
     * pad0/pad1 bits: UP DOWN LEFT RIGHT B1 B2.
     * hi bits:        C START X Y Z MODE.
     * sys bits:       PAUSE/START(compat) QUIT.
     * Guest demuxes these while waiting for RPC replies and in an idle
     * drain task; the input layer caches them for inp->pad /joy0.
     */
    HSFS_OP_PADPUSH = 8,
    /*
     * WRITE request: a0 = handle, a1 = offset, payload = bytes to write
     * (data_len <= HSFS_MAX_DATA).  Response: a0 = bytes written.
     */
    HSFS_OP_WRITE = 9,
    /* UNLINK request: path only.  Response: status. */
    HSFS_OP_UNLINK = 10,
};

/*
 * Set in `op` on every frame the HOST sends - replies and PADPUSH alike.  The
 * top bit, because the ops are 0..10 and will not reach it.  Mask it off before
 * comparing an op; require it before believing a frame is an answer.
 */
#define HSFS_F_FROM_HOST 0x8000u
#define HSFS_OP_MASK     0x7FFFu

/* a0 on STAT/OPEN responses: bit0 = directory */
#define HSFS_MODE_DIR 1u

typedef struct {
    uint32_t magic;
    uint16_t op;
    uint16_t seq;
    int32_t  status;
    uint32_t a0;
    uint32_t a1;
    uint32_t path_len;
    uint32_t data_len;
} hsfs_hdr_t;

#endif /* ARGON_HSFS_PROTO_H */
