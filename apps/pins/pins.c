/*
 * ArgonOS - ring out the wires between two boards, one pin at a time.
 *
 *   run c:\pins.axe out <secs> <pin>=<0|1> ...   drive, hold, let go
 *   run c:\pins.axe in <up|down> <pin> ...       read once, pulled up or down
 *
 * One board drives one pin to the level the others are not at; the other
 * board reads every pin it has on the bundle.  A wire that is there shows the
 * level, a wire that is not shows the pull, two wires crossed show on the
 * wrong pin, and two touching show on both.  Read once pulled down against a
 * lone high and once pulled up against a lone low: a missing ground passes
 * one of the two by luck, never both.  tools/pinring.py walks the bundle.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("PINS", "1.0", "argon", 0);

#define PINS_MAX 16

static int number(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

int ag_main(int argc, char **argv)
{
    const ag_io_api_t *io = g_ag_api->io;
    if (argc < 3) {
        ag_printf("pins out <secs> <pin>=<0|1> ... | pins in <up|down> <pin> ...\n");
        return 1;
    }
    int pin[PINS_MAX];
    int lvl[PINS_MAX];
    int n = 0;
    for (int i = 3; i < argc && n < PINS_MAX; i++) {
        const char *a = argv[i];
        pin[n] = number(a);
        while (*a != '\0' && *a != '=') {
            a++;
        }
        lvl[n] = (*a == '=' && a[1] == '1') ? 1 : 0;
        n++;
    }
    if (argv[1][0] == 'o') {
        for (int i = 0; i < n; i++) {
            /* a pin a driver holds (a UART's, say) is refused: say so, or a
             * wire that was never driven reads as a wire that is not there */
            if (io->gpio_config(pin[i], AG_GPIO_OUT) != AG_OK) {
                ag_printf("pins: %d is held by a driver\n", pin[i]);
                continue;
            }
            io->gpio_write(pin[i], lvl[i]);
        }
        ag_printf("pins out held\n");
        ag_delay((uint32_t)number(argv[2]) * 1000u);
        for (int i = 0; i < n; i++) {
            io->gpio_config(pin[i], AG_GPIO_IN);
        }
        return 0;
    }
    const int mode = (argv[2][0] == 'u') ? AG_GPIO_IN_PULLUP : AG_GPIO_IN_PULLDOWN;
    for (int i = 0; i < n; i++) {
        if (io->gpio_config(pin[i], mode) != AG_OK) {
            ag_printf("pins: %d is held by a driver\n", pin[i]);
        }
    }
    ag_delay(20);
    ag_printf("pins in");
    for (int i = 0; i < n; i++) {
        ag_printf(" %d=%d", pin[i], io->gpio_read(pin[i]) ? 1 : 0);
    }
    ag_printf(" end\n");
    for (int i = 0; i < n; i++) {
        io->gpio_config(pin[i], AG_GPIO_IN);
    }
    return 0;
}
