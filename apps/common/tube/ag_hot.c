/*
 * ag_hot - the amplifier's inner loop as one translation unit.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/*
 * WHY THIS FILE EXISTS
 *
 * ag_amp_tick calls into the valve, the oversampler and the filters on every
 * sample, and those live in files of their own.  A compiler cannot see into
 * another translation unit, so each of those is a real call - and on this
 * processor a call rotates a register window, which costs whether the callee
 * does anything or not.  At four stages and four times oversampling the
 * amplifier makes sixteen calls into the valve alone for one sample of audio.
 *
 * Compiling them together lets the compiler put the small ones where they are
 * used and keep values in registers across what used to be a call boundary.
 * Nothing here changes: the files are the same files, with the same names and
 * the same statics - checked, none of them collide.
 *
 * The .c files are still built separately by anything that does not care
 * (the host tests, the tools), so this is a build-time choice rather than a
 * rearrangement of the source.
 */
#include "ag_biq.c"
#include "ag_os.c"
#include "ag_tone.c"
#include "ag_tube.c"
#include "ag_amp.c"
