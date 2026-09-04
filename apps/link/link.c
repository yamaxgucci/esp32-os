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
 *   link send <baud> [kb] [block]    push kb kilobytes (default 256), time it
 *   link recv <baud> [secs] [block]  take what comes for secs seconds
 *   link info                        what BOARD.CFG says uart1 is wired to
 *
 * Start the receiver first.  It reports as it goes, so a run that gets nothing
 * is distinguishable from a run that gets rubbish.
 *
 * The block size is the whole block on the wire, header and checksum included,
 * and it is an argument rather than a constant because it is the one dial worth
 * sweeping: a corrupt block costs the rest of itself plus the hunt for the next
 * header, so what a line's error rate really costs depends on how big the
 * blocks are.  Both ends must be given the same number - a mismatch reads as
 * everything corrupt, which is the honest answer for a receiver framing at the
 * wrong size.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("LINK", "1.2", "argon", 0);

/*
 * UART1.  UART0 is the console on both of these boards, and the port numbering
 * is the chip's, not an index into anything - see docs/03-board-config.md.
 */
#define LINK_PORT 1

#define BLK_OVERHEAD 12u   /* magic 4, seq 4, sum 4                          */
#define BLK_MIN      16u   /* the overhead plus four bytes worth checking     */
#define BLK_MAX      1040u /* one block on the stack; the UART FIFO is 128    */
#define BLK_DEFAULT  260u  /* what every measurement before 4 Sep 2026 used   */

/*
 * Set once from the command line before anything touches the wire, and read
 * everywhere after.  A file-scope pair rather than a parameter threaded through
 * payload_fill and payload_sum: those two are the inner loop, and the loop
 * around them is the thing being measured.
 */
static uint32_t g_total = BLK_DEFAULT;
static uint32_t g_payload = BLK_DEFAULT - BLK_OVERHEAD;

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
static void payload_fill(uint8_t *p, uint32_t seq)
{
    for (uint32_t j = 0; j < g_payload; ++j) {
        p[j] = (uint8_t)(seq * 13u + j);
    }
}

/*
 * Over the sequence number as well as the payload, and that is not tidiness.
 * With the checksum covering only the payload, a block whose *number* was
 * mangled still passed - and since the number is what loss is counted from, a
 * single flipped bit in it reported four billion blocks lost.  Whatever a
 * checksum does not cover is a field nobody is checking.
 */
static uint32_t block_sum(const uint8_t *p)
{
    uint32_t sum = 0;
    for (uint32_t j = 0; j < 4u + g_payload; ++j) {
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

/*
 * Both ends move a buffer, not a block.
 *
 * The block size is the thing being swept, and if each block were also one
 * call to the port then a run with small blocks would be measuring the cost of
 * a system call rather than the cost of the wire - at 800 KB/s a 20-byte block
 * is forty thousand calls a second at each end, and the answer would come out
 * "small blocks are slow" for a reason that has nothing to do with the line.
 * So the sender packs whole blocks into a buffer of about a kilobyte and
 * writes that, and the receiver reads whatever has arrived and frames what it
 * finds inside it.  Which is also what a real driver does.
 */
#define IO_BUF 1024u

static int do_send(uint32_t baud, uint32_t kb)
{
    if (open_link(baud) != AG_OK) {
        return 1;
    }

    static uint8_t buf[IO_BUF + BLK_MAX];
    const uint32_t per_write = (g_total >= IO_BUF) ? 1u : (IO_BUF / g_total);
    const uint32_t chunk = per_write * g_total;

    const uint32_t target = kb * 1024u;
    uint32_t       sent = 0;
    uint32_t       seq = 0;
    bool           stopped = false;

    /*
     * Every line this prints is kept under forty characters on purpose: the
     * CYD's console is forty columns wide, and a line that wraps arrives at
     * the laptop cut in half with a cursor move through the middle of it.
     */
    ag_printf("send: %u KB %u baud blk %u\n", (unsigned)kb, (unsigned)baud,
              (unsigned)g_total);

    const ag_time_t t0 = g_ag_api->time->us();
    while (sent < target) {
        for (uint32_t i = 0; i < per_write; ++i) {
            uint8_t *const blk = buf + i * g_total;
            memcpy(blk, k_magic, sizeof(k_magic));
            put32(blk + 4, seq);
            payload_fill(blk + 8, seq);
            put32(blk + 8 + g_payload, block_sum(blk + 4));
            seq++;
        }

        const int32_t n = ag_uart_write(LINK_PORT, buf, chunk);
        if (n < 0) {
            ag_printf("tx failed at %u: %s\n", (unsigned)seq,
                      ag_strerror((ag_err_t)n));
            return 1;
        }
        sent += (uint32_t)n;

        /* Let the watchdog see us, and let a key out. */
        ag_heartbeat();
        if (ag_kbhit()) {
            (void)ag_getch();
            stopped = true;
            break;
        }
    }
    const uint64_t us = (uint64_t)(g_ag_api->time->us() - t0);

    ag_printf("tx bytes=%u ms=%u bps=%u%s\n", (unsigned)sent,
              (unsigned)(us / 1000u), (unsigned)rate_bps(sent, us),
              stopped ? " cut" : "");
    return 0;
}

static int do_recv(uint32_t baud, uint32_t secs)
{
    if (open_link(baud) != AG_OK) {
        return 1;
    }

    ag_printf("recv: %u s %u baud blk %u\n", (unsigned)secs, (unsigned)baud,
              (unsigned)g_total);

    static uint8_t io[IO_BUF];
    static uint8_t blk[BLK_MAX];
    uint32_t have = 0;   /* bytes of the current block already framed    */
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

        const int32_t n = ag_uart_read(LINK_PORT, io, sizeof(io), 200);
        if (n < 0) {
            ag_printf("rx failed: %s\n", ag_strerror((ag_err_t)n));
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

        /*
         * Frame what arrived, one byte at a time.  The first four bytes are
         * the only ones that have to match anything, so a receiver that has
         * lost its place spends one comparison per byte until it finds a
         * header - and a byte that is not the header byte expected may still
         * be the first byte of the next one, which is the case the naive
         * version gets wrong on a line whose corruption looks like magic.
         */
        for (int32_t i = 0; i < n; ++i) {
            const uint8_t b = io[i];
            if (have < 4u) {
                if (b == k_magic[have]) {
                    blk[have++] = b;
                } else if (b == k_magic[0]) {
                    stray += have;   /* what was held was not a header */
                    have = 1u;
                    blk[0] = b;
                } else {
                    stray += have + 1u;
                    have = 0u;
                }
                continue;
            }
            blk[have++] = b;
            if (have < g_total) {
                continue;
            }
            have = 0;
            const uint32_t seq = get32(blk + 4);
            const uint32_t claimed = get32(blk + 8 + g_payload);
            if (block_sum(blk + 4) == claimed) {
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
        }
    }

    const uint64_t span = (first != 0 && last > first) ? (uint64_t)(last - first)
                                                       : 0;
    ag_printf("rx bytes=%u ok=%u\n", (unsigned)bytes, (unsigned)ok);
    ag_printf("rx bad=%u lost=%u stray=%u\n", (unsigned)bad, (unsigned)lost,
              (unsigned)stray);
    if (bad != 0) {
        /* What one bad block really costs: itself, and then the hunt. */
        ag_printf("rx resync=%u\n", (unsigned)(stray / bad));
    }
    if (span != 0) {
        /*
         * Two rates, because they answer different questions.  bps is what
         * came off the wire; good is what a caller could have used - the
         * payload of the blocks that survived, with headers, checksums and
         * everything corrupt taken out.  A faster line that loses more can
         * still win, and only the second number says whether it did.
         */
        ag_printf("rx ms=%u bps=%u\n", (unsigned)(span / 1000u),
                  (unsigned)rate_bps(bytes, span));
        ag_printf("rx good=%u\n", (unsigned)rate_bps(ok * g_payload, span));
    }
    if (bytes == 0) {
        ag_printf("rx nothing - crossed tx/rx? common ground?\n");
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
    ag_printf("usage: link send <baud> [kb] [block]\n");
    ag_printf("       link recv <baud> [secs] [block]\n");
    ag_printf("       link info\n");
    ag_printf("block is the whole block on the wire, %u..%u, default %u\n",
              (unsigned)BLK_MIN, (unsigned)BLK_MAX, (unsigned)BLK_DEFAULT);
}

/* The fourth argument, spelled the same for send and recv. */
static bool take_block(int argc, char **argv)
{
    if (argc < 5) {
        return true;
    }
    uint32_t n = 0;
    if (!parse_u32(argv[4], &n) || n < BLK_MIN || n > BLK_MAX) {
        ag_printf("link: '%s' is not a block size (%u..%u)\n", argv[4],
                  (unsigned)BLK_MIN, (unsigned)BLK_MAX);
        return false;
    }
    g_total = n;
    g_payload = n - BLK_OVERHEAD;
    return true;
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
        if (!take_block(argc, argv)) {
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
        if (!take_block(argc, argv)) {
            return 1;
        }
        return do_recv(baud, secs);
    }

    usage();
    return 1;
}
