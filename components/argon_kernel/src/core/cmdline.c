/*
 * ArgonOS - command line splitting.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/cmdline.h>

#include <stdbool.h>

static inline bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int ag_cmdline_split(char *line, char **argv, int max_argv)
{
    if (line == NULL || argv == NULL || max_argv <= 0) {
        return 0;
    }

    int   argc = 0;
    char *read = line;

    while (*read != '\0') {
        while (is_space(*read)) {
            read++;
        }
        if (*read == '\0') {
            break;
        }

        /*
         * Writing the unquoted text back over the input lets quotes be
         * removed without a second buffer.
         */
        char *write = read;
        char *start = read;
        bool  quoted = false;

        while (*read != '\0' && (quoted || !is_space(*read))) {
            if (*read == '"') {
                quoted = !quoted;
                read++;
                continue;
            }
            *write++ = *read++;
        }

        const bool at_end = (*read == '\0');
        *write = '\0';
        if (!at_end) {
            read++; /* step over the separator we stopped on */
        }

        if (argc < max_argv) {
            argv[argc++] = start;
        } else {
            break; /* no room; the rest of the line is dropped */
        }
    }

    return argc;
}
