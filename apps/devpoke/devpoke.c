/*
 * ArgonOS - open a device by name and put bytes into it.
 *
 *   run t:\devpoke.axe <device> [bytes]
 *
 * It exists because the shell can LIST devices and cannot USE one, and the
 * difference matters the moment a device is not local: a listing crosses the
 * wire as a description, an open crosses it as a conversation, and only the
 * second proves anything.  `dev` saying `sound.pcmvirt` is there is not the
 * same claim as a write to it returning the number of bytes it took.
 *
 * Deliberately dumb about what it writes.  The point is the path in and the
 * count back, not the content: a device that gives a plausible count for bytes
 * it never received would pass a cleverer test just as well.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

/*
 * EIGHT KILOBYTES, NAMED RATHER THAN TAKEN FROM THE DEFAULT.
 *
 * This program writes a pattern into one device and says what it took, so 8 KB
 * is not a measurement of its own needs - it is AG_PROC_LOAD_STACK_MIN, the
 * floor the kernel names for the task an image is read and relocated on, and
 * nothing here wants more.
 *
 * WHY NOT THE DEFAULT: it is 16 KB (CONFIG_ARGON_APP_STACK_KB, which no board
 * in this tree overrides), and the board this program is the sound test FOR is
 * the C3 - the one with no converter, whose only way to make a noise is to pour
 * it at its neighbour.  Measured on COM8: one default-sized guest leaves the C3
 * with 18 KB free and a 9 KB largest block, so DEVPOKRV is refused with -12
 * whenever anything else is running - which, on the middle board of a cluster,
 * is whenever the test is worth running.  Two 8 KB stacks fit in what one 16 KB
 * stack leaves behind.
 */
AG_APP_SIZED("DEVPOKE", "1.0", "argon", 0, 8 * 1024, 0);

static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        ag_print("usage: devpoke <device> [bytes] [times]\n");
        ag_print("  opens it by name, writes a pattern, says what it took\n");
        return 2;
    }

    uint32_t want = 64;
    if (argc > 2 && !parse_u32(argv[2], &want)) {
        ag_printf("devpoke: %s is not a number of bytes\n", argv[2]);
        return 2;
    }
    if (want > 192u) {
        want = 192u; /* one message on the wire; more is a different test */
    }
    /*
     * Repeats, because some devices only do anything once enough has arrived:
     * the mixer works a period at a time and does not choose an output until it
     * has one to send.  A single small write into it is indistinguishable from
     * a device that ignored it.
     */
    uint32_t times = 1;
    if (argc > 3 && !parse_u32(argv[3], &times)) {
        ag_printf("devpoke: %s is not a count\n", argv[3]);
        return 2;
    }
    if (times == 0u || times > 1000u) {
        times = 1;
    }

    const ag_handle_t h = ag_dev_open(argv[1]);
    if (h < 0) {
        ag_printf("devpoke: %s: %s (%d)\n", argv[1], ag_strerror((ag_err_t)h),
                  (int)h);
        return 1;
    }
    ag_printf("devpoke: %s is open\n", argv[1]);

    uint8_t buf[192];
    for (uint32_t i = 0; i < want; i++) {
        buf[i] = (uint8_t)(i * 7u + 1u);
    }

    uint32_t total = 0;
    for (uint32_t i = 0; i < times; i++) {
        /*
         * Before each write, not after: a device that never returns is a
         * different fault from one that returns an error, and a program that
         * only reports at the end cannot tell them apart - which is how a hang
         * on the sixth write of twelve looked like a hang at the start.
         *
         * Every eighth, though, not every one.  The journal is a four-kilobyte
         * RING: forty of these overwrote the very line they were printed to
         * help find, and the run then failed for want of evidence it had
         * itself destroyed.  Enough to locate a stop, few enough to leave room
         * for what stopped.
         */
        if (times > 1u && (i % 8u) == 0u) {
            ag_printf("devpoke: write %u\n", (unsigned)(i + 1u));
        }
        const int32_t n = ag_dev_write(h, buf, want);
        if (n < 0) {
            ag_printf("devpoke: write: %s (%d)\n", ag_strerror((ag_err_t)n),
                      (int)n);
            (void)ag_dev_close(h);
            return 1;
        }
        total += (uint32_t)n;
    }
    ag_printf("devpoke: %u of %u bytes taken\n", (unsigned)total,
              (unsigned)(want * times));

    (void)ag_dev_close(h);
    ag_print("devpoke: closed\n");
    return (total == want * times) ? 0 : 1;
}
