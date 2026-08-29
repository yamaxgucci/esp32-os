/*
 * MARAUDER - ESP32-Marauder as an ArgonOS loadable application (.AXE).
 *
 * This is a port of justcallmekoko's ESP32-Marauder to run as an ordinary
 * ArgonOS application - Maxim's requirement: a loadable .AXE, not a builtin
 * kernel module.  It talks to the machine only through the public ABI
 * (argon/argon.h): the panel through gfx->present, the touch screen through the
 * input queue, and the radio through api->wifimon (ABI 0.38).
 *
 * First slice: the whole menu tree 1:1 with the upstream, touch + key
 * navigation, and one screen taken all the way through - WiFi > Sniffers >
 * Scan AP/STA, which lists the access points around it by sniffing beacons.
 * Every other leaf is a faithful placeholder so the tree is whole.
 *
 * Runs on the CYD board (no system framebuffer: [display] driver = panel) and,
 * for development, in QEMU -Gfx (a system surface exists, so the same bands are
 * blitted in and gfxdump can capture the UI without a board; the radio is not
 * modelled, so Scan APs reports that wifimon is unavailable there).
 *
 * Build (host gate uses the S3 toolchain; the board build uses xtensa-esp32):
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include --include apps/marauder \
 *       -o build/apps/MARAUDER.AXE \
 *       apps/marauder/marauder.c apps/marauder/mrd_gfx.c \
 *       apps/marauder/mrd_menu.c apps/marauder/mrd_scan.c
 *
 * The image is split (code / data), which is what lets its code run from flash
 * XIP on the board rather than the tiny IRAM arena - the loader chooses XIP
 * automatically for a non-contiguous image whose code will not fit the arena.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"

/*
 * Eight kilobytes of stack rather than the default sixteen: the stack lives in
 * internal SRAM for the life of the process, and with the radio up this board
 * has little of it to spare.  The app draws from a static band and recurses
 * nowhere, so eight is generous.  Heap default (0): almost nothing is malloc'd -
 * the band, the AP list and the menu view are all static.
 */
/*
 * Small on purpose - stack 7 KB, heap 1 KB.  On this no-PSRAM board the whole
 * Wi-Fi driver wants a ~36 KB slice of internal RAM, and the board has ~62 KB
 * of it: the application plus the radio only both fit if the application stays
 * lean.  It allocates nothing from its arena (band, AP list and screen state
 * are static), so the arena is tiny; the stack is only as deep as the wifimon
 * bring-up call chain needs.  Every kilobyte the image does not hold is one the
 * radio can.
 */
/*
 * AG_AXE_WANT_XIP: run from flash, keeping the IRAM arena free for the radio.
 * On this ESP32 the 10 KB of code exceeds the 8 KB arena and the loader chooses
 * flash anyway; the flag makes that intentional rather than incidental, so the
 * app still XIPs on a part with a larger arena (the S3) where its code would
 * otherwise be parked in the internal SRAM a Wi-Fi bring-up needs whole.
 */
/*
 * Stack 6 KB, heap 0.  The app allocates nothing from its own arena - the
 * render band, the AP lists and every screen's state are static (and the big
 * ones share one scratch buffer, mrd_scratch()).  Asking for a heap the loader
 * then cannot place after Wi-Fi is up would refuse the whole app for memory it
 * never uses; heap 0 lets it start regardless (the kernel only warns), and any
 * stray malloc would fail loudly rather than the app not running at all.
 */
AG_APP_SIZED("MARAUDER", "0.1", "ArgonOS",
             AG_AXE_NEEDS_GFX | AG_AXE_WANT_XIP, 6144, 0);

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (ag_api()->gfx == NULL) {
        ag_printf("marauder: this build has no display\n");
        return 1;
    }

    /*
     * A scanner is a long-running loop, not a realtime path: any clock will do,
     * it just hops slower.  Say so once, so `power eco` does not end it.
     */
    (void)ag_power_declare(AG_POWER_FIT_ANY, "wifi scanner");

    mrd_disp_t d;
    if (!mrd_gfx_begin(&d)) {
        ag_printf("marauder: could not take the display\n");
        return 1;
    }

    /*
     * A serial console is the one input the panel does not have, so it is also
     * how the app is driven and watched without touch: arrow keys / Enter / Esc
     * navigate, and the app mirrors its state to the console as "[mrd] ..."
     * lines.  On the panel-only board this text goes to the UART (the panel is
     * held by the app), so it is a control+telemetry channel over the cable.
     */
    ag_printf("[mrd] MARAUDER %ux%u - keys: up/down, enter=open, esc=back\n",
              (unsigned)d.W, (unsigned)d.H);

    /*
     * `marauder shot [path]` paints one screen and exits, leaving the frame for
     * gfxdump - so the UI can be checked on a machine with a system surface
     * (QEMU -Gfx) without a board.  Everything else runs the interactive UI.
     */
    if (argc >= 2 && argv[1] != NULL && argv[1][0] == 's' &&
        argv[1][1] == 'h') {
        mrd_menu_shot(&d, argc >= 3 ? argv[2] : "");
        mrd_gfx_end(&d);
        return 0;
    }

    /*
     * `marauder dump <file.ppm> [menu.path]` renders one screen straight to a
     * PPM and exits - the reliable headless capture, since QEMU's framebuffer
     * keeps no snapshot for gfxdump to read after the app lets go.
     */
    if (argc >= 3 && argv[1] != NULL && argv[1][0] == 'd' &&
        argv[1][1] == 'u') {
        mrd_menu_dump(&d, argv[2], argc >= 4 ? argv[3] : "");
        mrd_gfx_end(&d);
        return 0;
    }

    /*
     * `marauder wifitest` exercises every api->wifi call once and prints each
     * result over the console (the UART on the panel-only board), so the whole
     * station / access-point / ESP-NOW surface can be verified without touch.
     */
    if (argc >= 2 && argv[1] != NULL && argv[1][0] == 'w' &&
        argv[1][1] == 'i') {
        mrd_wifi_selftest();
        mrd_gfx_end(&d);
        return 0;
    }

    /*
     * `marauder bttest` exercises the BLE surface (scan, and connect+discover
     * if something is connectable) and prints each result over the console -
     * the same headless verification path as wifitest, for api->ble.
     */
    if (argc >= 2 && argv[1] != NULL && argv[1][0] == 'b' &&
        argv[1][1] == 't') {
        mrd_bt_selftest();
        mrd_gfx_end(&d);
        return 0;
    }

    mrd_menu_run(&d);

    /*
     * The radio screens leave the monitor running (starting/stopping it per
     * screen leaked the interface and eventually faulted).  Tear it down once,
     * here, on the way out - harmless if it was never started.
     */
    if (ag_wifimon_available()) {
        (void)ag_wifimon_stop();
    }

    mrd_gfx_end(&d);
    return 0;
}
