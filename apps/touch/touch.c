/*
 * ArgonOS - what the touchscreen thinks you touched.
 *
 *   run a:\touch.axe
 *
 * Paints the cell under the finger and prints where it was.  It exists to
 * answer the two questions a resistive panel always raises before anything
 * else can use it - is it wired the way the schematic says, and are the axes
 * the right way round - and to be the thing that says so on the screen the
 * finger is on, rather than on a terminal somewhere else.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

AG_APP("TOUCH", "0.1", "argon", 0);

/*
 * Painting is a function because it has to happen twice.
 *
 * A process is already "focused" the moment it starts - the slot it was
 * spawned into is - but the session adopts it a fraction later, and adoption
 * repaints the slot, which wipes whatever was drawn in between.  Waiting for
 * focus does not help, because focus is already true.  What works is the same
 * thing the file manager does: draw, and draw again when FOCUS_GAINED says the
 * screen is now really ours.  Two hours went into a blank screen that was
 * blamed on the touchscreen driver.
 */
static void repaint(const ag_coninfo_t *info)
{
    /* The header only: the marks left by a finger are the record of what
     * happened and must survive a redraw. */
    ag_color(AG_LGRAY, AG_BLACK);
    ag_gotoxy(0, 0);
    ag_printf("touch: %ux%u cells. Esc to stop.", (unsigned)info->cols,
              (unsigned)info->rows);
}

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_coninfo_t info;
    ag_api()->con->info(&info);
    uint32_t taps = 0;
    uint32_t moves = 0;
    bool     running = true;

    ag_cls();
    repaint(&info);
    ag_gotoxy(0, 2);
    ag_printf("waiting for a finger");

    uint32_t idle = 0;

    while (running && !ag_interrupted()) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, 100)) {
            ag_heartbeat();
            /*
             * The header is redrawn while nothing is happening, and that is
             * not decoration.  Whatever painted the screen last is not
             * necessarily this program: the loading view lands on the slot
             * after the process has already started, and FOCUS_GAINED does not
             * arrive when the process was focused from the moment it was
             * spawned.  Redrawing twice a second is three lines; finding out
             * why the screen was blank took an evening.
             */
            if (++idle >= 5) {
                idle = 0;
                repaint(&info);
            }
            continue;
        }
        idle = 0;

        switch (ev.type) {
        case AG_EV_FOCUS_GAINED:
            repaint(&info);
            break;

        case AG_EV_KEY_DOWN:
            if (ev.key.keycode == AG_KEY_ESC || ev.key.keycode == AG_KEY_Q) {
                running = false;
            }
            break;

        case AG_EV_POINTER_DOWN:
        case AG_EV_POINTER_MOVE: {
            const bool down = (ev.type == AG_EV_POINTER_DOWN);
            if (down) {
                taps++;
            } else {
                moves++;
            }
            /*
             * A solid block where the finger is, in a colour that says which
             * of the two events it was: the difference between "the panel
             * reports a press" and "the panel reports it moving" is most of
             * what goes wrong with a resistive screen.
             */
            ag_poke((uint16_t)ev.ptr.x, (uint16_t)ev.ptr.y, 0xdb,
                    down ? AG_ATTR(AG_LGREEN, AG_BLACK)
                         : AG_ATTR(AG_CYAN, AG_BLACK));
            ag_gotoxy(0, (uint16_t)(info.rows - 1));
            ag_color(AG_YELLOW, AG_BLACK);
            ag_printf("%s at %d,%d   taps %u moves %u    ",
                      down ? "down" : "move", (int)ev.ptr.x, (int)ev.ptr.y,
                      (unsigned)taps, (unsigned)moves);
            break;
        }

        case AG_EV_POINTER_UP:
            ag_gotoxy(0, (uint16_t)(info.rows - 1));
            ag_color(AG_LGRAY, AG_BLACK);
            ag_printf("up   at %d,%d   taps %u moves %u    ", (int)ev.ptr.x,
                      (int)ev.ptr.y, (unsigned)taps, (unsigned)moves);
            break;

        default:
            break;
        }
    }

    ag_color(AG_LGRAY, AG_BLACK);
    ag_cls();
    ag_printf("%u taps, %u moves\n", (unsigned)taps, (unsigned)moves);
    ag_log(AG_LOG_INFO, "touch", "done: %u taps, %u moves", (unsigned)taps,
           (unsigned)moves);
    return (taps > 0) ? 0 : 1;
}
