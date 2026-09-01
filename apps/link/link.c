/*
 * ArgonOS - the wire between two boards, measured rather than assumed.
 *
 * A machine with no screen and a machine that is nothing but a screen are
 * joined here by two soldered pins and a ground.  Everything that will later
 * ride that pair - a console, a framebuffer, a filesystem - depends on one
 * number nobody has: how many bytes a second it actually carries before it
 * starts losing them.  Datasheets answer a different question (what the UART
 * can be programmed to) and say nothing about a hand-soldered pair with a stub
 * on it.
 *
 * So this sends framed blocks one way and counts what arrives at the other end.
 * Framed, not a plain stream, because the two failures look identical in a
 * stream and have opposite fixes: a byte that arrives wrong means the line is
 * too fast for the wiring, and a byte that never arrives means the receiver was
 * not there to take it.  A block carries a sequence number and a checksum, so
 * corruption is counted separately from loss, and a receiver that has lost its
 * place finds the next block instead of reporting every byte after it as bad.
 *
 *   link send <baud> [kb]      push kb kilobytes (default 256) and time it
 *   link recv <baud> [secs]    take what comes for secs seconds (default 15)
 *   link info                  what BOARD.CFG says uart1 is wired to
 *
 * Start the receiver first.  It reports as it goes, so a run that gets nothing
 * is distinguishable from a run that gets rubbish.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("LINK", "1.0", "argon", 0);

/*
 * UART1.  UART0 is the console on both of these boards, and the port numbering
 * is the chip's, not an index into anything - see docs/03-board-config.md.
 */
#define LINK_PORT 1

#define BLK_PAYLOAD 248u
#define BLK_TOTAL   (8u + BLK_PAYLOAD + 4u) /* magic, seq, payload, sum */

/*
 * Two bytes would be enough to frame this, but four alternating ones are much
 * harder to hit by accident in a stream of payload - and a receiver that has
 * just lost synchronisation is reading payload as if it were a header.
 */
static const uint8_t k_magic[4] = {0xA5, 0x5A, 0xA5, 0x5A};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/*
 * The payload is a function of the sequence number, so the receiver needs no
 * copy of what was sent and a block can be checked on its own.  Anything that
 * repeats every byte - all zeros, all 0x55 - would hide a stuck line, which is
 * exactly the failure a new solder joint has.
 */
static uint32_t payload_fill(uint8_t *p, uint32_t seq)
{
    uint32_t sum = 0;
    for (uint32_t j = 0; j < BLK_PAYLOAD; ++j) {
        const uint8_t b = (uint8_t)(seq * 13u + j);
        p[j] = b;
        sum += b;
    }
    return sum;
}

static uint32_t payload_sum(const uint8_t *p)
{
    uint32_t sum = 0;
    for (uint32_t j = 0; j < BLK_PAYLOAD; ++j) {
        sum += p[j];
    }
    return sum;
}

/*
 * The SDK has no atoi and does not want one: a shim that silently returns zero
 * for "2M" is worse than no shim, because the caller cannot tell a rejected
 * argument from a rate of zero.  This one says whether it consumed the whole
 * word.
 */
static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(*s - '0');
        if (v > (0xffffffffu - digit) / 10u) {
            return false; /* would wrap; a baud rate never needs to */
        }
        v = v * 10u + digit;
    }
    *out = v;
    return true;
}

/* Bytes per second from a microsecond span, without floating point. */
static uint32_t rate_bps(uint32_t bytes, uint64_t us)
{
    if (us == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)bytes * 1000000u) / us);
}

static ag_err_t open_link(uint32_t baud)
{
    const ag_err_t err = g_ag_api->io->uart_config(LINK_PORT, baud, 8, 0, 1);
    if (err != AG_OK) {
        ag_printf("uart%d at %u baud: %s\n", LINK_PORT, (unsigned)baud,
                  ag_strerror(err));
        ag_printf("is [uart%d] tx/rx set in C:\\BOARD.CFG?\n", LINK_PORT);
    }
    return err;
}

static int do_send(uint32_t baud, uint32_t kb)
{
    if (open_link(baud) != AG_OK) {
        return 1;
    }

    uint8_t blk[BLK_TOTAL];
    memcpy(blk, k_magic, sizeof(k_magic));

    const uint32_t target = kb * 1024u;
    uint32_t       sent = 0;
    uint32_t       seq = 0;
    bool           stopped = false;

    ag_printf("send: %u KB at %u baud, blocks of %u\n", (unsigned)kb,
              (unsigned)baud, (unsigned)BLK_TOTAL);

    const ag_time_t t0 = g_ag_api->time->us();
    while (sent < target) {
        put32(blk + 4, seq);
        const uint32_t sum = payload_fill(blk + 8, seq);
        put32(blk + 8 + BLK_PAYLOAD, sum);

        const int32_t n = ag_uart_write(LINK_PORT, blk, BLK_TOTAL);
        if (n < 0) {
            ag_printf("write failed at block %u: %s\n", (unsigned)seq,
                      ag_strerror((ag_err_t)n));
            return 1;
        }
        sent += (uint32_t)n;
        seq++;

        /* Every sixty-odd blocks: let the watchdog see us, and let a key out. */
        if ((seq & 0x3fu) == 0) {
            ag_heartbeat();
            if (ag_kbhit()) {
                (void)ag_getch();
                stopped = true;
                break;
            }
        }
    }
    const uint64_t us = (uint64_t)(g_ag_api->time->us() - t0);

    ag_printf("sent %u bytes in %u ms = %u B/s%s\n", (unsigned)sent,
              (unsigned)(us / 1000u), (unsigned)rate_bps(sent, us),
              stopped ? " (stopped)" : "");
    return 0;
}

static int do_recv(uint32_t baud, uint32_t secs)
{
    if (open_link(baud) != AG_OK) {
        return 1;
    }

    ag_printf("recv: %u s at %u baud, any key stops\n", (unsigned)secs,
              (unsigned)baud);

    uint8_t  blk[BLK_TOTAL];
    uint32_t have = 0;   /* bytes of the current block already in blk    */
    uint32_t ok = 0;     /* blocks whose checksum came out               */
    uint32_t bad = 0;    /* blocks that arrived corrupt                  */
    uint32_t lost = 0;   /* blocks the sequence numbers say never came   */
    uint32_t stray = 0;  /* bytes thrown away hunting for a header       */
    uint32_t bytes = 0;  /* everything that came off the wire            */
    uint32_t next = 0;   /* the sequence number expected next            */
    bool     synced = false;

    const ag_time_t t0 = g_ag_api->time->us();
    const uint64_t  window = (uint64_t)secs * 1000000u;
    ag_time_t       first = 0;
    ag_time_t       last = 0;

    for (;;) {
        if ((uint64_t)(g_ag_api->time->us() - t0) >= window) {
            break;
        }
        if (ag_kbhit()) {
            (void)ag_getch();
            break;
        }

        /*
         * Read into the block, then decide.  A short read is the normal case:
         * the line delivers when it delivers, and a block spans several.
         */
        const int32_t n = ag_uart_read(LINK_PORT, blk + have,
                                       (size_t)(BLK_TOTAL - have), 200);
        if (n < 0) {
            ag_printf("read failed: %s\n", ag_strerror((ag_err_t)n));
            return 1;
        }
        ag_heartbeat();
        if (n == 0) {
            continue;
        }
        if (first == 0) {
            first = g_ag_api->time->us();
        }
        last = g_ag_api->time->us();
        bytes += (uint32_t)n;
        have += (uint32_t)n;

        /*
         * Hunting the header.  Only the first four bytes are checked here; a
         * mismatch drops one byte and slides, which is what lets a receiver
         * that came up mid-block find the start of the next one.
         */
        while (have >= 4 && (blk[0] != k_magic[0] || blk[1] != k_magic[1] ||
                             blk[2] != k_magic[2] || blk[3] != k_magic[3])) {
            for (uint32_t i = 1; i < have; ++i) {
                blk[i - 1] = blk[i];
            }
            have--;
            stray++;
        }
        if (have < BLK_TOTAL) {
            continue;
        }

        const uint32_t seq = get32(blk + 4);
        const uint32_t claimed = get32(blk + 8 + BLK_PAYLOAD);
        if (payload_sum(blk + 8) == claimed) {
            ok++;
            if (synced && seq > next) {
                lost += seq - next;
            }
            next = seq + 1;
            synced = true;
        } else {
            bad++;
            /* Do not trust this sequence number to count losses from. */
            synced = false;
        }
        have = 0;
    }

    const uint64_t span = (first != 0 && last > first) ? (uint64_t)(last - first)
                                                       : 0;
    ag_printf("got %u bytes, %u blocks ok, %u corrupt, %u lost, %u stray\n",
              (unsigned)bytes, (unsigned)ok, (unsigned)bad, (unsigned)lost,
              (unsigned)stray);
    if (span != 0) {
        ag_printf("while it was flowing: %u ms = %u B/s\n",
                  (unsigned)(span / 1000u), (unsigned)rate_bps(bytes, span));
    }
    if (bytes == 0) {
        ag_printf("nothing arrived - check tx/rx are crossed and grounds tied\n");
    }
    return 0;
}

static int do_info(void)
{
    /*
     * There is no ABI call that reads BOARD.CFG back, so this says what the
     * port answers rather than what the file says: bringing it up at its own
     * default rate either works or names the reason.
     */
    const ag_err_t err = g_ag_api->io->uart_config(LINK_PORT, 115200, 8, 0, 1);
    ag_printf("uart%d: %s\n", LINK_PORT,
              (err == AG_OK) ? "configured (pins are set)" : ag_strerror(err));
    ag_printf("`io` in the shell lists which pins the port took.\n");
    return (err == AG_OK) ? 0 : 1;
}

static void usage(void)
{
    ag_printf("usage: link send <baud> [kb]\n");
    ag_printf("       link recv <baud> [secs]\n");
    ag_printf("       link info\n");
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (argv[1][0] == 'i') {
        return do_info();
    }

    if (argc < 3) {
        usage();
        return 1;
    }
    uint32_t baud = 0;
    if (!parse_u32(argv[2], &baud) || baud == 0) {
        ag_printf("link: '%s' is not a baud rate\n", argv[2]);
        return 1;
    }

    if (argv[1][0] == 's') {
        uint32_t kb = 256u;
        if (argc >= 4 && (!parse_u32(argv[3], &kb) || kb == 0)) {
            ag_printf("link: '%s' is not a size in KB\n", argv[3]);
            return 1;
        }
        return do_send(baud, kb);
    }
    if (argv[1][0] == 'r') {
        uint32_t secs = 15u;
        if (argc >= 4 && (!parse_u32(argv[3], &secs) || secs == 0)) {
            ag_printf("link: '%s' is not a number of seconds\n", argv[3]);
            return 1;
        }
        return do_recv(baud, secs);
    }

    usage();
    return 1;
}
