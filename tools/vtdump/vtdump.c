/*
 * ArgonOS - render a raw terminal transcript into the screen it produces.
 *
 * A QEMU or serial capture is a stream of escape sequences and is unreadable
 * as text.  This runs it through the kernel's own screen module and prints
 * the resulting grid, which is both useful for reading test output and a
 * check of that module against real traffic rather than crafted input.
 *
 *   vtdump [cols rows] < transcript.log
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/screen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    int cols = 80;
    int rows = 25;

    if (argc >= 3) {
        cols = atoi(argv[1]);
        rows = atoi(argv[2]);
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

    char   chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
        ag_screen_write(&screen, chunk, n);
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
            putchar((c < 0x20 || c == 0x7f) ? ' ' : c);
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
