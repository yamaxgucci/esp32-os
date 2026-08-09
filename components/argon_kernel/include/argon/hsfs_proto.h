/*
 * ArgonOS - HostFS wire protocol (guest UART1 ↔ host helper).
 *
 * Little-endian.  Request and response share the same header; `status` is 0
 * on requests and an ag_err_t (0 or negative) on responses.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
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
     * Host → guest only (no request).  data_len == 3: pad0, pad1, sys.
     * Guest demuxes these while waiting for RPC replies and in an idle
     * drain task so SMS can read a RAM cache without a UART round-trip.
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
