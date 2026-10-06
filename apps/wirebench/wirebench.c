/*
 * ArgonOS - how fast the wire between two boards carries ANYBODY'S bytes.
 *
 * The screen between two boards moves 2.68 MB/s over four soldered wires; the
 * cluster link over the same four moves about 20 KB/s.  The obvious question
 * is whether the difference is the DATA - pixels against files - or the shape
 * of the transfer, and the only way to answer it is to put something that is
 * not a picture through the path that works.
 *
 * So this writes bytes to /dev/wire0, which REMDISP publishes beside its
 * display: the same framing, the same one-transaction-per-frame, the same
 * checksum at the far end, and nothing about pixels anywhere in it.  The far
 * end (REMTERM) counts what arrives and what failed its checksum.
 *
 *   wirebench [seconds] [chunk]
 *
 * `chunk` is how much is handed to one write() call, 4096 by default.  The
 * driver cuts it into frames of its own size, so this is about the cost of the
 * call rather than of the wire.
 *
 * The bytes are noise, unseeded, for the same reason GFXNOISE's are: nothing
 * in the path compresses, and two runs send the same thing.
 */
#include <argon/argon.h>

AG_APP("WIREBENCH", "1.0", "argon", 0);

#define CHUNK_MAX 8192u

static uint8_t  s_buf[CHUNK_MAX];
static uint32_t s_rng = 0x2545F491u;

static uint32_t xorshift(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
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

int ag_main(int argc, char **argv)
{
    uint32_t secs = (argc >= 2) ? number(argv[1]) : 10u;
    uint32_t chunk = (argc >= 3) ? number(argv[2]) : 4096u;

    if (secs == 0u) {
        secs = 10u;
    }
    if (chunk == 0u || chunk > CHUNK_MAX) {
        chunk = 4096u;
    }

    for (uint32_t i = 0; i < chunk; i++) {
        s_buf[i] = (uint8_t)xorshift();
    }

    const ag_handle_t h = ag_open("/dev/wire0", AG_O_WRONLY);
    if (h < 0) {
        ag_printf("wirebench: /dev/wire0: %s\n", ag_strerror((ag_err_t)h));
        ag_printf("wirebench: load c:\\drv\\remdisp.sys first\n");
        return 1;
    }

    ag_printf("wirebench: %u s, %u B per write\n", (unsigned)secs,
              (unsigned)chunk);

    uint32_t        bytes = 0;
    uint32_t        calls = 0;
    uint32_t        refused = 0;
    const ag_time_t t0 = ag_micros();
    const ag_time_t until = t0 + (ag_time_t)secs * 1000000u;

    while (ag_micros() < until) {
        const int32_t n = ag_write(h, s_buf, chunk);
        calls++;
        if (n > 0) {
            bytes += (uint32_t)n;
        } else {
            refused++;
        }
    }

    const uint32_t ms = (uint32_t)((ag_micros() - t0) / 1000u);
    ag_close(h);

    ag_printf("wirebench: %u KB in %u ms, %u calls, %u refused\n",
              (unsigned)(bytes / 1024u), (unsigned)ms, (unsigned)calls,
              (unsigned)refused);
    if (ms != 0u) {
        ag_printf("wirebench: %u KB/s offered\n",
                  (unsigned)(bytes / ms * 1000u / 1024u));
    }
    return 0;
}
