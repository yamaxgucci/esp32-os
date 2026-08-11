/*
 * Argon CC g2d — shared globals (include before any function).
 *
 * RGB565 atlas in g2d_tiles (char*, little-endian bytes). Map cells are
 * tile indices in g2d_map (char*). Chroma key is 0x00RRGGBB like ag_gfx_*.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef G2D_GLOBALS_H
#define G2D_GLOBALS_H

/* Logical view on the framebuffer (destination). */
int g2d_vx;
int g2d_vy;
int g2d_vw;
int g2d_vh;

/* Camera scroll in map pixel space. */
int g2d_scroll_x;
int g2d_scroll_y;

/* Tile size and atlas. */
int g2d_tile_w;
int g2d_tile_h;
int g2d_atlas_w; /* tiles per row in atlas */
int g2d_atlas_stride; /* bytes per atlas row (= atlas_pixel_w * 2) */
char *g2d_tiles; /* RGB565 bytes */

/* Tilemap: one byte per cell = tile index (0..255). */
char *g2d_map;
int g2d_map_w;
int g2d_map_h;

/* Transparent colour for sprites (0x00RRGGBB). */
int g2d_key;

#endif
