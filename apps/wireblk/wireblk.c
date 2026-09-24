/*
 * ArgonOS - blocks across the wire between two boards, counted one by one.
 *
 * Not pixels and not the link protocol: fixed-size blocks, each carrying its
 * own sequence number and a payload the receiver can recompute, so every
 * block that arrives is either intact, damaged, or missing - and the three
 * counts, the block size and the time are the whole measurement.
 *
 *   wireblk tx|rx  uart|spim|spis  <rate>  <block>  <secs>  [gap_us] [bus]
 *
 *   uart   port 1, <rate> in baud; the SENDER hands blocks to the UART's own
 *          DMA (UHCI) when the kernel has it, the receiver reads the FIFO
 *   uartcpu the same port with no DMA at all: every byte through the driver
 *          ring and the interrupt, for comparison
 *   spim   this board is the SPI MASTER (it holds the clock), <rate> in kHz
 *   spis   this board is the SPI SLAVE, <rate> in kHz
 *
 *          Both ends are the kernel's framed SPI link (io->spi_link_open2,
 *          ABI 0.60): a stream carried in `frame=N`-byte transactions (4096
 *          unless told), so <block> is how the application writes and reads,
 *          exactly as on the UART, and not the size of anything on the wire.
 *
 * So Zero->LCD over SPI is `tx spim` on the Zero and `rx spis` on the LCD, and
 * LCD->Zero is `tx spis` on the LCD and `rx spim` on the Zero: the master
 * clocks in both cases, and the data rides MOSI in one and MISO in the other.
 *
 * Block: "WBLK", sequence (4 bytes, little endian), then block-8 bytes of a
 * pattern computed from the sequence.  The receiver recomputes every byte.
 * The receiver's report is also written to c:\wireblk.txt, because a console
 * that repaints is not a place to read a number from.
 */
#include <argon/argon.h>
#include <argon/libc.h>

/* 16 KB of arena, the least a named heap may be: the default is a quarter of
 * the free pool, and on the C3 the SPI link's one piece is what that took */
AG_APP_SIZED("WIREBLK", "1.0", "argon", 0, 0, 16384);

#define BLK_MAX  8192u

/* port=N on the command line: the C3's wire to the Zero is its UART0 */
static int s_uart_port = 1;
#define UART_PORT s_uart_port

static uint8_t *s_blk;   /* one block, DMA-capable for the SPI master */

/*
 * FIVE BUFFERS FOR THE UART'S DMA, and five is arithmetic: the port queues up
 * to four transfers and a submit that finds the queue full waits for one, so
 * when a submit returns at most four are the hardware's - and the fifth, the
 * one being filled, never is.  One buffer shared with the DMA would be
 * rewritten while it is still being sent.
 */
#define DMA_RING 5u
static uint8_t *s_dmabuf[DMA_RING];
/*
 * Sized by the block and taken from the heap, not 8 KB each in bss: the C3
 * has 25 KB free after boot with its largest piece 7 KB, and a static 17 KB
 * of data would not load there at all, whatever the block asked for.
 */
static uint8_t *s_rx;
static uint32_t s_rxcap;

/* a prefix test without the libc that is not here */
static bool starts(const char *a, const char *pre)
{
    while (*pre != 0) {
        if (*a++ != *pre++) {
            return false;
        }
    }
    return true;
}

static uint32_t number(const char *s)
{
    uint32_t v = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
    }
    return v;
}

static uint8_t pat(uint32_t seq, uint32_t i)
{
    return (uint8_t)(seq * 7u + i * 13u + (i >> 8));
}

static void build(uint8_t *b, uint32_t len, uint32_t seq)
{
    b[0] = 'W';
    b[1] = 'B';
    b[2] = 'L';
    b[3] = 'K';
    b[4] = (uint8_t)seq;
    b[5] = (uint8_t)(seq >> 8);
    b[6] = (uint8_t)(seq >> 16);
    b[7] = (uint8_t)(seq >> 24);
    for (uint32_t i = 8; i < len; i++) {
        b[i] = pat(seq, i);
    }
}

/* ---- the receiver's parser ---------------------------------------------- */

typedef struct {
    uint32_t block;
    uint32_t have;     /* bytes of the current block collected */
    uint32_t seq;
    uint32_t expect;
    bool     started;
    uint32_t good, bad, missing, stray;
    uint64_t first_us, last_us;
} rx_t;

static uint8_t *s_cur;   /* one block, heap, sized by the block */

/*
 * WHAT A DAMAGED BLOCK LOOKS LIKE, counted rather than guessed.  A bit
 * flipped by the line leaves one byte wrong and the rest in step; a byte the
 * receiver dropped or doubled leaves everything after it shifted by one.  The
 * two want different cures, so each damaged block is sorted into one of:
 *   flip1   exactly one byte wrong - and which bits of it (s_bit[])
 *   flipN   several bytes wrong, still in step
 *   drop    from the first wrong byte on, the block is one byte EARLY
 *   dup     ... one byte LATE
 *   other   none of those (a header hit, a mix)
 * and the first few are written out whole-ish to c:\wireblk_bad.txt.
 */
static uint32_t s_flip1, s_flipn, s_drop, s_dup, s_other;
static uint32_t s_bit[8];      /* which bit of the byte, in flip1 blocks */
static uint32_t s_rise, s_fall; /* flip1: a 0 read as 1, a 1 read as 0 */
static uint32_t s_prev_same;   /* flip1: the bit before the flipped one equal */
static char     s_diag[1600];

static bool shifted(const rx_t *r, uint32_t from, int d)
{
    uint32_t hits = 0;
    uint32_t n = 0;
    for (uint32_t i = from + 1u; i < r->block && n < 32u; i++, n++) {
        const int32_t j = (int32_t)i + d;
        if (j < 8 || (uint32_t)j >= r->block) {
            continue;
        }
        hits += (s_cur[i] == pat(r->seq, (uint32_t)j)) ? 1u : 0u;
    }
    return n >= 8u && hits * 10u >= n * 9u;
}

static void classify(const rx_t *r, uint32_t first)
{
    uint32_t wrong = 0;
    for (uint32_t i = 8; i < r->block; i++) {
        wrong += (s_cur[i] != pat(r->seq, i)) ? 1u : 0u;
    }
    const uint8_t want = pat(r->seq, first);
    const uint8_t got = s_cur[first];
    const char   *kind;
    if (wrong == 1u) {
        s_flip1++;
        kind = "flip1";
        const uint8_t x = (uint8_t)(want ^ got);
        for (int b = 0; b < 8; b++) {
            if (x & (1u << b)) {
                s_bit[b]++;
                if (got & (1u << b)) {
                    s_rise++;
                } else {
                    s_fall++;
                }
                /* on the wire bit b follows bit b-1 (LSB first); bit 0
                 * follows the start bit, which is a 0 */
                const int prev = (b == 0) ? 0 : ((want >> (b - 1)) & 1);
                if (prev == ((want >> b) & 1)) {
                    s_prev_same++;
                }
            }
        }
    } else if (shifted(r, first, 1)) {
        s_drop++;
        kind = "drop";
    } else if (shifted(r, first, -1)) {
        s_dup++;
        kind = "dup";
    } else if (wrong < r->block / 4u) {
        s_flipn++;
        kind = "flipN";
    } else {
        s_other++;
        kind = "other";
    }
    /* how much of the wrong run is the NEXT block's beginning, byte for byte */
    uint32_t nxt = 0;
    uint32_t span = 0;
    for (uint32_t j = 0; first + j < r->block && j < 128u; j++, span++) {
        uint8_t e;
        const uint32_t s1 = r->seq + 1u;
        if (j < 4u) {
            e = (uint8_t)"WBLK"[j];
        } else if (j < 8u) {
            e = (uint8_t)(s1 >> (8u * (j - 4u)));
        } else {
            e = pat(s1, j);
        }
        nxt += (s_cur[first + j] == e) ? 1u : 0u;
    }
    if (strlen(s_diag) + 140u < sizeof(s_diag)) {
        char num[16];
        (void)ag_strlcat(s_diag, "nxt=", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(nxt, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, "/", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(span, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, " ", sizeof(s_diag));
        (void)ag_strlcat(s_diag, kind, sizeof(s_diag));
        (void)ag_strlcat(s_diag, " seq=", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(r->seq, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, " at=", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(first, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, " wrong=", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(wrong, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, " want=", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(want, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, " got=", sizeof(s_diag));
        (void)ag_strlcat(s_diag, ag_utoa(got, num, sizeof(num), 0, false),
                         sizeof(s_diag));
        (void)ag_strlcat(s_diag, "\n", sizeof(s_diag));
    }
}

static void diag_write(void)
{
    char line[400];
    char num[16];
    line[0] = '\0';
    static const char *const k[] = {"flip1=", " flipN=", " drop=", " dup=",
                                    " other=", " rise=", " fall=",
                                    " prev_same=", " bits="};
    const uint32_t v[] = {s_flip1, s_flipn, s_drop, s_dup, s_other,
                          s_rise, s_fall, s_prev_same};
    for (uint32_t i = 0; i < 8u; i++) {
        (void)ag_strlcat(line, k[i], sizeof(line));
        (void)ag_strlcat(line, ag_utoa(v[i], num, sizeof(num), 0, false),
                         sizeof(line));
    }
    (void)ag_strlcat(line, k[8], sizeof(line));
    for (int b = 0; b < 8; b++) {
        (void)ag_strlcat(line, ag_utoa(s_bit[b], num, sizeof(num), 0, false),
                         sizeof(line));
        (void)ag_strlcat(line, (b < 7) ? "," : "\n", sizeof(line));
    }
    ag_printf("wireblk diag %s", line);
    const ag_handle_t f =
        ag_open("c:\\wireblk_bad.txt", AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f >= 0) {
        (void)ag_write(f, line, strlen(line));
        (void)ag_write(f, s_diag, strlen(s_diag));
        (void)ag_close(f);
    }
}

/*
 * THE CHECK HAS TO BE FASTER THAN THE WIRE, or it is the check that is
 * measured.  On the C3 the byte-at-a-time parser with pat() recomputed for
 * every byte could not take 1.8 MB/s, and the SPI link - which slows a
 * writer down instead of dropping - reported that as its own speed.  So a
 * block's body is copied in one piece, and the expected bytes are counted up
 * (13 a byte, one more every 256) rather than multiplied out.
 */
static uint32_t first_wrong(const rx_t *r)
{
    const uint32_t base = r->seq * 7u;
    for (uint32_t c = 0; c * 256u < r->block; c++) {
        uint32_t       i = (c == 0u) ? 8u : c * 256u;
        const uint32_t end = ((c + 1u) * 256u < r->block) ? (c + 1u) * 256u
                                                          : r->block;
        uint8_t        e = (uint8_t)(base + i * 13u + c);
        for (; i < end; i++, e = (uint8_t)(e + 13u)) {
            if (s_cur[i] != e) {
                return i;
            }
        }
    }
    return 0;
}

static void finish(rx_t *r)
{
    r->seq = (uint32_t)s_cur[4] | ((uint32_t)s_cur[5] << 8) |
             ((uint32_t)s_cur[6] << 16) | ((uint32_t)s_cur[7] << 24);
    const uint32_t first = first_wrong(r);
    if (first != 0u) {
        r->bad++;
        classify(r, first);
        return;
    }
    const uint64_t now = ag_micros();
    if (r->good == 0u) {
        r->first_us = now;
    }
    r->last_us = now;
    r->good++;
    if (r->started && r->seq > r->expect) {
        r->missing += r->seq - r->expect;
    }
    r->started = true;
    r->expect = r->seq + 1u;
}

static void feed(rx_t *r, const uint8_t *p, int32_t n)
{
    int32_t k = 0;
    while (k < n) {
        if (r->have < 4u) {
            const uint8_t c = p[k++];
            static const uint8_t magic[4] = {'W', 'B', 'L', 'K'};
            if (c == magic[r->have]) {
                s_cur[r->have++] = c;
            } else {
                r->stray += r->have + 1u;
                r->have = (c == 'W') ? 1u : 0u;
                if (r->have) {
                    s_cur[0] = c;
                    r->stray--;
                }
            }
            continue;
        }
        uint32_t take = r->block - r->have;
        if (take > (uint32_t)(n - k)) {
            take = (uint32_t)(n - k);
        }
        memcpy(s_cur + r->have, p + k, take);
        r->have += take;
        k += (int32_t)take;
        if (r->have == r->block) {
            finish(r);
            r->have = 0;
        }
    }
}

static void report(const char *wire, uint32_t rate, const rx_t *r)
{
    const uint64_t us = (r->last_us > r->first_us) ? (r->last_us - r->first_us)
                                                  : 1u;
    const uint64_t bytes = (uint64_t)r->good * r->block;
    const uint32_t kbs = (uint32_t)(bytes * 1000000u / us / 1024u);
    char line[200];
    line[0] = '\0';
    static const char *const k[] = {"wireblk rx ", " rate=", " block=",
                                    " good=", " bad=", " missing=",
                                    " stray=", " ms=", " kbs="};
    const uint32_t v[] = {0, rate, r->block, r->good, r->bad, r->missing,
                          r->stray, (uint32_t)(us / 1000u), kbs};
    char num[24];
    for (uint32_t i = 0; i < 9u; i++) {
        (void)ag_strlcat(line, k[i], sizeof(line));
        if (i == 0u) {
            (void)ag_strlcat(line, wire, sizeof(line));
        } else {
            (void)ag_strlcat(line, ag_utoa(v[i], num, sizeof(num), 0, false),
                             sizeof(line));
        }
    }
    (void)ag_strlcat(line, "\n", sizeof(line));
    ag_printf("%s", line);
    const ag_handle_t f =
        ag_open("c:\\wireblk.txt", AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f >= 0) {
        (void)ag_write(f, line, strlen(line));
        (void)ag_close(f);
    }
}

/* ---- a board that is busy ---------------------------------------------- */

/*
 * THE STRESS: threads that compute for as long as the transfer runs, one per
 * core, at the priority an application's threads get - so they share the
 * processor with the sending or receiving loop the way a real program's work
 * would, rather than waiting politely below it.  What they compute is
 * arithmetic nobody reads; what is measured is what it costs the wire.
 */
static volatile bool     s_burn_stop;
static volatile uint32_t s_burn_work[2];

static void burner(void *arg)
{
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    uint32_t       x = 0x9E3779B9u + id;
    uint32_t       n = 0;
    while (!s_burn_stop) {
        for (uint32_t i = 0; i < 1000u; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            x = x * 2654435761u + i;
        }
        n++;
    }
    s_burn_work[id & 1u] = n + (x & 1u);
    ag_thread_exit();
}

/*
 * Internal SRAM when there is some, the process's own arena when there is
 * not.  The kernel gives an image that names no heap a quarter of the free
 * pool as its arena - on the C3 with the radio gone that is 20 KB - and with
 * an SPI link's 40 KB taken out of the rest, 8.9 KB of internal memory was
 * all that was left outside it.  None of these is handed to a DMA engine.
 */
static uint8_t *buf_alloc(size_t n)
{
    void *p = ag_malloc_caps(n, AG_MEM_FAST);
    if (p == NULL) {
        p = ag_malloc(n);
    }
    return (uint8_t *)p;
}

/* ---- main ---------------------------------------------------------------- */

int ag_main(int argc, char **argv)
{
    if (argc < 6) {
        ag_printf("usage: wireblk tx|rx uart|spim|spis <rate> <block> <secs> "
                  "[gap_us] [bus]\n");
        return 1;
    }
    const bool     tx = (argv[1][0] == 't');
    const char    *wire = argv[2];
    const uint32_t rate = number(argv[3]);
    uint32_t       block = number(argv[4]);
    const uint32_t secs = number(argv[5]);
    uint32_t gap = 0;
    int      bus = 2;
    uint32_t burn = 0;
    uint32_t frame = 4096;
    bool     sink = false;
    uint32_t drop = 0;
    for (int i = 6, bare = 0; i < argc; i++) {
        const char *a = argv[i];
        if (starts(a, "burn=")) {
            burn = number(a + 5);
        } else if (starts(a, "gap=")) {
            gap = number(a + 4);
        } else if (starts(a, "bus=")) {
            bus = (int)number(a + 4);
        } else if (starts(a, "port=")) {
            s_uart_port = (int)number(a + 5);
        } else if (starts(a, "frame=")) {
            frame = number(a + 6);
        } else if (starts(a, "drop=")) {
            /* test: this end drops every Nth data frame on arrival */
            drop = number(a + 5);
        } else if (starts(a, "sink=")) {
            /* diagnosis: take the bytes and count them, check nothing - how
             * much a receiver can TAKE, apart from what checking costs */
            sink = (number(a + 5) != 0u);
        } else if (bare++ == 0) {
            gap = number(a);
        } else {
            bus = (int)number(a);
        }
    }
    if (burn > 2u) {
        burn = 2u;
    }
    const int      cs = 10;
    const ag_io_api_t *io = g_ag_api->io;

    const bool uartcpu = (wire[0] == 'u' && wire[4] == 'c');
    const bool uart = (wire[0] == 'u');
    const bool spim = (wire[0] == 's' && wire[3] == 'm');
    const bool spis = (wire[0] == 's' && wire[3] == 's');
    if (!uart && !spim && !spis) {
        ag_printf("wireblk: wire is uart, spim or spis\n");
        return 1;
    }
    if (block < 16u || block > BLK_MAX) {
        ag_printf("wireblk: block 16..%u\n", (unsigned)BLK_MAX);
        return 1;
    }
    block = (block + 3u) & ~3u;

    /*
     * The wire first, the application's buffers after.  An SPI link wants its
     * frames and rings as one piece (56 KB at 4 KB frames), and on the C3 the
     * one free block that size is exactly where 16 KB of receive buffers had
     * just been put when they were asked for first.
     */
    ag_err_t e = AG_OK;
    if (uart) {
        e = io->uart_config(UART_PORT, rate, 8, 0, 1);
    } else if (!AG_HAS(io, spi_link_open2)) {
        e = -AG_ENOSYS;
    } else {
        e = io->spi_link_open2(bus, cs, rate, frame,
                               AG_SPILINK_FRAMED | AG_SPILINK_DROP(drop) |
                                   (spim ? AG_SPILINK_MASTER : 0u));
    }
    if (e != AG_OK) {
        ag_printf("wireblk: open %s: %s\n", wire, ag_strerror(e));
        return 1;
    }

    /*
     * Only what the role uses: a sender builds into s_blk, a receiver reads
     * into s_rx and assembles in s_cur.  All three at 8 KB was 24 KB of a C3
     * that also has to hold the link's own frames and rings.  Internal SRAM
     * by caps, not the app's arena: the C3 has none to give an image that
     * did not ask for one, and the parser reads it every byte.
     */
    /* a read is a helping from the stream, not a block: 2 KB is plenty */
    s_rxcap = (block < 2048u) ? block : 2048u;
    if (s_rxcap < 512u) {
        s_rxcap = 512u;
    }
    s_blk = tx ? buf_alloc(block) : NULL;
    /* the block being checked first: it is read every byte, and it is the
     * one that should get internal memory when there is only room for one */
    s_cur = tx ? NULL : buf_alloc(block);
    s_rx = tx ? NULL : buf_alloc(s_rxcap);
    if (tx ? (s_blk == NULL) : (s_rx == NULL || s_cur == NULL)) {
        ag_meminfo_t mi;
        memset(&mi, 0, sizeof(mi));
        g_ag_api->mem->info(&mi);
        ag_printf("wireblk: no memory for %u-byte buffers (blk %s rx %s cur "
                  "%s; fast free %u, system free %u)\n",
                  (unsigned)block, s_blk ? "ok" : "-", s_rx ? "ok" : "-",
                  s_cur ? "ok" : "-", (unsigned)mi.fast_free,
                  (unsigned)mi.system_free);
        ag_free(s_blk);
        ag_free(s_rx);
        ag_free(s_cur);
        if (spim || spis) {
            (void)io->spi_link_close(bus);
        }
        return 1;
    }

    bool dma = false;
    if (uart && !uartcpu && tx && AG_HAS(io, uart_dma_submit) &&
        AG_HAS(io, uart_dma_drain)) {
        dma = true;
        for (uint32_t i = 0; i < DMA_RING; i++) {
            s_dmabuf[i] = (uint8_t *)ag_malloc_caps(block, AG_MEM_DMA);
            if (s_dmabuf[i] == NULL) {
                dma = false;
            }
        }
        if (!dma) {
            ag_printf("wireblk: no DMA memory for the ring; plain writes\n");
        }
    }

    ag_printf("wireblk %s %s rate=%u block=%u secs=%u gap=%u%s\n",
              tx ? "tx" : "rx", wire, (unsigned)rate, (unsigned)block,
              (unsigned)secs, (unsigned)gap, dma ? " dma" : "");

    ag_thread_t burners[2] = {0, 0};
    s_burn_stop = false;
    for (uint32_t i = 0; i < burn; i++) {
        burners[i] = ag_thread_create(
            /* 5, the priority an application's own thread runs at: with 1
             * they only ever had the idle time, and that is not a busy
             * board, it is an idle one with a hobby */
            burner, (void *)(uintptr_t)i, "burn", 2048, 5,
            (i == 0u) ? AG_THREAD_APP_CORE : AG_THREAD_SYS_CORE);
    }
    if (burn != 0u) {
        ag_printf("wireblk: %u burner thread(s) running\n", (unsigned)burn);
    }

    const uint64_t t0 = ag_micros();
    const uint64_t until = t0 + (uint64_t)secs * 1000000u;

    if (tx) {
        uint32_t seq = 0, sent = 0, refused = 0;
        while (ag_micros() < until) {
            bool ok = true;
            if (dma) {
                uint8_t *const b = s_dmabuf[seq % DMA_RING];
                build(b, block, seq);
                ok = (io->uart_dma_submit(UART_PORT, b, block) == (int32_t)block);
            } else if (uart) {
                build(s_blk, block, seq);
                ok = (io->uart_write(UART_PORT, s_blk, block) == (int32_t)block);
            } else {
                build(s_blk, block, seq);
                uint32_t off = 0;
                if (AG_HAS(io, spi_link_write_wait)) {
                    const int32_t w =
                        io->spi_link_write_wait(bus, s_blk, block, 200);
                    off = (w > 0) ? (uint32_t)w : 0u;
                }
                while (off < block && ag_micros() < until) {
                    const int32_t n =
                        io->spi_link_write(bus, s_blk + off, block - off);
                    if (n > 0) {
                        off += (uint32_t)n;
                    } else {
                        ag_delay(1);
                    }
                }
                ok = (off == block);
            }
            if (ok) {
                sent++;
                seq++;
            } else {
                refused++;
            }
            if (gap != 0u) {
                const uint64_t g0 = ag_micros();
                while (ag_micros() - g0 < gap) {
                }
            }
        }
        if (dma) {
            (void)io->uart_dma_drain(UART_PORT);
        }
        const uint32_t ms = (uint32_t)((ag_micros() - t0) / 1000u);
        /* what is still in the link's ring goes out before the close */
        if (spim || spis) {
            ag_delay(300);
        }
        ag_printf("wireblk tx %s rate=%u block=%u sent=%u refused=%u ms=%u "
                  "kbs=%u\n",
                  wire, (unsigned)rate, (unsigned)block, (unsigned)sent,
                  (unsigned)refused, (unsigned)ms,
                  (unsigned)(ms ? ((uint64_t)sent * block * 1000u / ms / 1024u)
                                : 0u));
        /*
         * The sender's count in a file too, so the receiver's can be set
         * against it: a block lost whole before the receiver's first good one,
         * or after its last, falls in no gap and is otherwise never counted.
         */
        char line[160];
        char num[24];
        line[0] = '\0';
        (void)ag_strlcat(line, "wireblk tx ", sizeof(line));
        (void)ag_strlcat(line, wire, sizeof(line));
        static const char *const k[] = {" rate=", " block=", " sent=",
                                        " refused=", " ms="};
        const uint32_t v[] = {rate, block, sent, refused, ms};
        for (uint32_t i = 0; i < 5u; i++) {
            (void)ag_strlcat(line, k[i], sizeof(line));
            (void)ag_strlcat(line, ag_utoa(v[i], num, sizeof(num), 0, false),
                             sizeof(line));
        }
        (void)ag_strlcat(line, "\n", sizeof(line));
        const ag_handle_t f = ag_open("c:\\wireblk_tx.txt",
                                      AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (f >= 0) {
            (void)ag_write(f, line, strlen(line));
            (void)ag_close(f);
        }
    } else {
        rx_t r;
        memset(&r, 0, sizeof(r));
        r.block = block;
        while (ag_micros() < until) {
            int32_t n = 0;
            if (uart && !uartcpu && AG_HAS(io, uart_dma_read)) {
                /* received by DMA straight into s_rx, a block at a time */
                n = io->uart_dma_read(UART_PORT, s_rx, s_rxcap, 200);
            } else if (uart) {
                n = io->uart_read(UART_PORT, s_rx, s_rxcap, 20);
            } else {
                n = io->spi_link_read(bus, s_rx, s_rxcap, 20);
            }
            if (n > 0 && sink) {
                const uint64_t now = ag_micros();
                if (r.first_us == 0u) {
                    r.first_us = now;
                }
                r.last_us = now;
                r.stray += (uint32_t)n; /* bytes, in the sink */
            } else if (n > 0) {
                feed(&r, s_rx, n);
            }
        }
        if (sink) {
            r.good = r.stray / block;
            r.stray = 0;
        }
        report(wire, rate, &r);
        diag_write();
    }

    s_burn_stop = true;
    for (uint32_t i = 0; i < burn; i++) {
        if (burners[i] != 0) {
            (void)ag_thread_join(burners[i], 2000u);
        }
    }
    if (burn != 0u) {
        ag_printf("wireblk: burners did %u + %u thousand rounds\n",
                  (unsigned)s_burn_work[0], (unsigned)s_burn_work[1]);
    }

    if (spim || spis) {
        ag_spilink_stats_t st;
        memset(&st, 0, sizeof(st));
        if (AG_HAS(io, spi_link_stats) &&
            io->spi_link_stats(bus, &st) == AG_OK) {
            ag_printf("wireblk link %s: frames=%u data=%u bad=%u gaps=%u "
                      "dropped=%u wire_us=%u loop_us=%u reorders=%u "
                      "short=%u unarmed=%u rewinds=%u naks=%u sd=%u sc=%u errors=%u\n",
                      wire, (unsigned)st.blocks_clocked,
                      (unsigned)st.blocks_data,
                      (unsigned)st.frames_bad, (unsigned)st.seq_gaps,
                      (unsigned)st.bytes_dropped, (unsigned)st.wire_us,
                      (unsigned)st.loop_us, (unsigned)st.reorders,
                      (unsigned)st.short_frames, (unsigned)st.blocks_unarmed,
                      (unsigned)st.rewinds, (unsigned)st.naks,
                      (unsigned)st.starve_data, (unsigned)st.starve_credit,
                      (unsigned)st.errors);
        }
        (void)io->spi_link_close(bus);
    }
    for (uint32_t i = 0; i < DMA_RING; i++) {
        if (s_dmabuf[i] != NULL) {
            ag_free(s_dmabuf[i]);
            s_dmabuf[i] = NULL;
        }
    }
    ag_free(s_blk);
    ag_free(s_rx);
    ag_free(s_cur);
    return 0;
}
