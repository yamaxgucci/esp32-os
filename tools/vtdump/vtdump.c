/*
 * ArgonOS - render a raw terminal transcript into the screen it produces.
 *
 * A QEMU or serial capture is a stream of escape sequences and is unreadable
 * as text.  This runs it through the kernel's own screen module and prints
 * the resulting grid, which is both useful for reading test output and a
 * check of that module against real traffic rather than crafted input.
 *
 *   vtdump [cols rows [codepage]] < transcript.log
 *
 * The transcript is UTF-8, because that is what a terminal is given; the screen
 * holds one code page byte per cell.  So this converts on the way in and back on
 * the way out, which is the same pair of conversions the console does - and the
 * reason a Cyrillic screen comes out with its columns still lined up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/screen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/codepage.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/*
 * Turns the UTF-8 of the transcript back into code page bytes, one at a time, so
 * the screen sees what the guest's cells held.  Escape sequences are ASCII and
 * pass through untouched; anything the page has no byte for becomes '?', which is
 * visible in a dump rather than silently absent.
 */
typedef struct {
    uint32_t pending;   /* code point being assembled  */
    int      remaining; /* continuation bytes still due */
} utf8_in_t;

static void feed_byte(ag_screen_t *s, utf8_in_t *u, unsigned char b)
{
    char out;

    if (u->remaining > 0) {
        if ((b & 0xc0u) != 0x80u) {
            u->remaining = 0; /* malformed: fall through and reinterpret */
        } else {
            u->pending = (u->pending << 6) | (uint32_t)(b & 0x3fu);
            if (--u->remaining == 0) {
                const int32_t byte =
                    ag_cp_from_unicode(ag_cp_active(), u->pending);
                out = (byte >= 0) ? (char)byte : '?';
                ag_screen_write(s, &out, 1);
            }
            return;
        }
    }

    if (b < 0x80u) {
        out = (char)b;
        ag_screen_write(s, &out, 1);
        return;
    }
    if ((b & 0xe0u) == 0xc0u) {
        u->pending = b & 0x1fu;
        u->remaining = 1;
    } else if ((b & 0xf0u) == 0xe0u) {
        u->pending = b & 0x0fu;
        u->remaining = 2;
    } else if ((b & 0xf8u) == 0xf0u) {
        u->pending = b & 0x07u;
        u->remaining = 3;
    }
    /* A stray continuation byte is dropped, as it is in the console. */
}

int main(int argc, char **argv)
{
    int cols = 80;
    int rows = 25;

    if (argc >= 3) {
        cols = atoi(argv[1]);
        rows = atoi(argv[2]);
    }
    if (argc >= 4) {
        ag_cp_t cp;
        if (!ag_cp_from_number((uint16_t)atoi(argv[3]), &cp)) {
            fprintf(stderr, "vtdump: no such code page: %s\n", argv[3]);
            return 2;
        }
        ag_cp_set_active(cp);
    }
    if (cols <= 0 || cols > AG_SCREEN_MAX_COLS || rows <= 0 ||
        rows > AG_SCREEN_MAX_ROWS) {
        fprintf(stderr, "vtdump: bad screen size\n");
        return 2;
    }

#ifdef _WIN32
    /* Otherwise 0x1a in the stream is taken for end of file. */
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    const size_t need = ag_screen_memsize((uint16_t)cols, (uint16_t)rows);
    void *mem = malloc(need);
    if (mem == NULL) {
        fprintf(stderr, "vtdump: out of memory\n");
        return 2;
    }

    ag_screen_t screen;
    if (ag_screen_init(&screen, mem, need, (uint16_t)cols, (uint16_t)rows) !=
        AG_OK) {
        fprintf(stderr, "vtdump: screen init failed\n");
        return 2;
    }

    unsigned char chunk[4096];
    utf8_in_t     utf8 = {0, 0};
    size_t        n;
    while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
        for (size_t i = 0; i < n; i++) {
            feed_byte(&screen, &utf8, chunk[i]);
        }
    }

    printf("+");
    for (int x = 0; x < cols; x++) {
        printf("-");
    }
    printf("+\n");

    for (uint16_t y = 0; y < screen.rows; y++) {
        const ag_cell_t *row = ag_screen_row(&screen, y);
        printf("|");
        for (uint16_t x = 0; x < screen.cols; x++) {
            const unsigned char c = (unsigned char)row[x].ch;
            if (c < 0x20 || c == 0x7f) {
                putchar(' ');
                continue;
            }
            /* Out as UTF-8, so the dump reads like the screen looked. */
            char           utf8_out[4];
            const uint32_t point = ag_cp_to_unicode(ag_cp_active(), c);
            const size_t   len = (point != 0) ? ag_utf8_encode(point, utf8_out)
                                              : 0;
            if (len == 0) {
                putchar(' ');
            } else {
                fwrite(utf8_out, 1, len, stdout);
            }
        }
        printf("|\n");
    }

    printf("+");
    for (int x = 0; x < cols; x++) {
        printf("-");
    }
    printf("+\n");
    printf("cursor at column %u, row %u\n", (unsigned)screen.cur_x,
           (unsigned)screen.cur_y);

    free(mem);
    return 0;
}
