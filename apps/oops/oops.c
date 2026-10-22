/*
 * OOPS - an application that writes past the end of its own data, on purpose.
 *
 * There is exactly one reason for this to exist: the kernel now puts a guard
 * behind every application's data and checks it on the supervisor's tick, and
 * a safety net nobody ever drops anything into is a safety net nobody knows is
 * torn.  This drops something into it.
 *
 * What it does is what took a day to find by accident: it walks off the end of
 * one of its own buffers into whatever the heap put next.  With the guard in
 * place the system should name it, stop it, and stay up; with `guard watch`
 * armed first the board should stop at the instruction below rather than
 * later, somewhere else, in an allocator.
 *
 * Nothing else should ever link this. It is a test instrument.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>

AG_APP("OOPS", "1.0", "argon", 0);

/*
 * The only data this image has, so that the end of this array and the end of
 * the image's data part are as close together as a compiler will allow - the
 * point being to step over that end and nothing else.
 */
static volatile unsigned char s_buf[1024];

int ag_main(int argc, char **argv)
{
    /*
     * How far past this buffer to write, and from where.
     *
     * Measured on the board: this array ends four bytes before the image's
     * data part does, and those four bytes hold something the C runtime keeps
     * - writing over them made the next line read a pointer of 0xEE and the
     * program faulted before it ever reached the guard.  That is the ordinary
     * shape of the accident and it is worth being able to stage, but it is
     * not the interesting one: it proves the check that runs on the way out,
     * not the one that runs on the tick.
     *
     * So by default the walk starts past that (skip = 4) and lands in the
     * guard and nowhere else: the program keeps running, and the supervisor
     * has to be the one to notice.  `oops N self` includes them.
     */
    unsigned past = 8;
    unsigned skip = 4;

    if (argc >= 2 && argv[1] != NULL) {
        unsigned n = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            n = n * 10u + (unsigned)(*p - '0');
        }
        if (n > 0u && n <= 512u) {
            past = n;
        }
    }
    if (argc >= 3 && argv[2] != NULL && argv[2][0] == 's') {
        skip = 0;
    }

    ag_printf("OOPS: writing %u bytes, starting %u past a %u byte buffer of "
              "my own.\n", past, skip, (unsigned)sizeof(s_buf));
    ag_printf("this is deliberate: it is how the data guard gets tested.\n");
    /* Where this buffer sits, so its end can be compared with the loader's
     * report of where the image's data begins and how big it is. */
    ag_printf("OOPS: buffer %p..%p\n", (void *)s_buf,
              (void *)(s_buf + sizeof(s_buf)));

    /* Its own buffer first, so the walk is a walk and not a wild pointer. */
    for (size_t i = 0; i < sizeof(s_buf); i++) {
        s_buf[i] = (unsigned char)i;
    }

    /*
     * And then past it.  Written through a moving pointer so that the compiler
     * cannot fold the whole thing away as undefined - which it is, and which is
     * the point.
     */
    volatile unsigned char *p = s_buf + sizeof(s_buf) + skip;
    for (unsigned i = 0; i < past; i++) {
        p[i] = 0xEEu;
    }

    ag_printf("OOPS: done writing; waiting to be noticed.\n");

    /*
     * Still here.  Whether that lasts is the test: the supervisor should find
     * the broken guard on its next tick and stop this program by name.
     */
    for (unsigned i = 0; i < 100u; i++) {
        ag_delay(100);
        if (ag_interrupted()) {
            break;
        }
    }
    ag_printf("OOPS: nobody noticed in ten seconds.\n");
    return 0;
}
