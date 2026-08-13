/*
 * Forced into every translation unit of DOOM.AXE (-include).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DOOM_CFG_H
#define ARGON_DOOM_CFG_H

#define ARGON_TARGET 1

/* Engine stays 320x200 pal8; the port 2x-nearest's onto the soft fb. */
#define DOOMGENERIC_RESX 320
#define DOOMGENERIC_RESY 200

/* Mute milestone: no SDL mixer / FEATURE_SOUND. */
#undef FEATURE_SOUND

#endif
