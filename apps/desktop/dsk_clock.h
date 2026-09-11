/*
 * The screen saver's face: four seven-segment digits on black.
 *
 * Segments rather than a font because the font this shell has is 8x16 and a
 * clock read from across a desk is not sixteen pixels tall.  Scaling a
 * bitmap font gives blocky, uneven strokes; seven rectangles give clean ones
 * at any size, need no glyph data at all, and are the shape a person expects
 * a clock to have.
 */
#ifndef DSK_CLOCK_H
#define DSK_CLOCK_H

#include "dsk_rect.h"

#include <stdbool.h>

/*
 * Paint HH:MM centred in `screen`, over black.
 *
 * Draws and does not flush, and draws the WHOLE face every time it is
 * called: on a board with no framebuffer the caller hands this to
 * dsk_paint_region, which calls it once per strip with the painter clipped
 * to that strip, and sends each strip as it is finished.  Painting outside
 * that machinery reaches the glass on a board that has a surface and
 * reaches nothing at all on one that has not - which is exactly how the
 * first version of this drew a clock nobody could see.
 *
 * `valid` false draws --:-- instead: the board has no battery under its
 * clock, so an unset clock is the normal state after a power cut and has to
 * look like one rather than like half past midnight.
 *
 * `nudge` shifts the whole block a few pixels; the caller advances it every
 * minute so the same pixels are not lit for hours at a time.
 */
void dsk_clock_draw(dsk_rect_t screen, int hour, int minute, bool valid,
                    unsigned nudge);

#endif /* DSK_CLOCK_H */
