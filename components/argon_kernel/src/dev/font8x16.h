/*
 * ArgonOS - built-in 8x16 bitmap font.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FONT8X16_H
#define ARGON_FONT8X16_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_FONT8X16_W 8
#define AG_FONT8X16_H 16

/* One bit per pixel, MSB is the leftmost column; 256 CP437 cells. */
extern const uint8_t ag_font8x16[256][AG_FONT8X16_H];

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FONT8X16_H */
