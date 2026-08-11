/*
 * Argon CC g2d — implementations (include AFTER all globals).
 *
 * Requires: g2d_globals.h + ABI 0.17 blit_bind/copy/keyed.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */

/* View rectangle on the soft FB; scroll starts at 0. */
void g2d_init(int vx, int vy, int vw, int vh)
{
    g2d_vx = vx;
    g2d_vy = vy;
    g2d_vw = vw;
    g2d_vh = vh;
    g2d_scroll_x = 0;
    g2d_scroll_y = 0;
    g2d_tile_w = 16;
    g2d_tile_h = 16;
    g2d_atlas_w = 1;
    g2d_atlas_stride = 0;
    g2d_tiles = 0;
    g2d_map = 0;
    g2d_map_w = 0;
    g2d_map_h = 0;
    g2d_key = 16711935; /* 0x00FF00FF magenta */
}

void g2d_set_scroll(int sx, int sy)
{
    g2d_scroll_x = sx;
    g2d_scroll_y = sy;
}

/* atlas: RGB565 LE packed; atlas_w = tiles per row; tw/th = tile size. */
void g2d_tileset(char *atlas, int atlas_w, int tw, int th)
{
    g2d_tiles = atlas;
    g2d_atlas_w = atlas_w;
    g2d_tile_w = tw;
    g2d_tile_h = th;
    g2d_atlas_stride = atlas_w * tw * 2;
}

void g2d_tilemap(char *map, int mw, int mh)
{
    g2d_map = map;
    g2d_map_w = mw;
    g2d_map_h = mh;
}

void g2d_set_key(int key_rgb)
{
    g2d_key = key_rgb;
}

/* Pointer to tile `id` pixels inside the atlas (char*). */
int g2d_tile_ptr(int id)
{
    int tx;
    int ty;
    int off;
    int p;
    if (g2d_tiles == 0) {
        return 0;
    }
    if (g2d_atlas_w < 1) {
        return 0;
    }
    tx = id % g2d_atlas_w;
    ty = id / g2d_atlas_w;
    off = ty * g2d_tile_h * g2d_atlas_stride + tx * g2d_tile_w * 2;
    /* Address as int — Mini-C functions return int/void only. */
    p = g2d_tiles + off;
    return p;
}

void g2d_draw_tile(int id, int dx, int dy)
{
    int src;
    src = g2d_tile_ptr(id);
    if (src == 0) {
        return;
    }
    ag_gfx_blit_bind(src, g2d_atlas_stride);
    ag_gfx_blit_copy(dx, dy, g2d_tile_w, g2d_tile_h);
}

void g2d_draw_sprite(int id, int dx, int dy)
{
    int src;
    src = g2d_tile_ptr(id);
    if (src == 0) {
        return;
    }
    ag_gfx_blit_bind(src, g2d_atlas_stride);
    ag_gfx_blit_keyed(dx, dy, g2d_tile_w, g2d_tile_h, g2d_key);
}

/* Draw the visible map window under the current scroll. */
void g2d_map_draw(void)
{
    int tw;
    int th;
    int x0;
    int y0;
    int x1;
    int y1;
    int ty;
    int tx;
    int id;
    int dx;
    int dy;
    if (g2d_map == 0 || g2d_tiles == 0) {
        return;
    }
    tw = g2d_tile_w;
    th = g2d_tile_h;
    if (tw < 1 || th < 1) {
        return;
    }
    x0 = g2d_scroll_x / tw;
    y0 = g2d_scroll_y / th;
    x1 = (g2d_scroll_x + g2d_vw) / tw;
    y1 = (g2d_scroll_y + g2d_vh) / th;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= g2d_map_w) {
        x1 = g2d_map_w - 1;
    }
    if (y1 >= g2d_map_h) {
        y1 = g2d_map_h - 1;
    }
    ty = y0;
    while (ty <= y1) {
        tx = x0;
        while (tx <= x1) {
            id = g2d_map[ty * g2d_map_w + tx];
            id = id & 255;
            dx = g2d_vx + tx * tw - g2d_scroll_x;
            dy = g2d_vy + ty * th - g2d_scroll_y;
            g2d_draw_tile(id, dx, dy);
            tx = tx + 1;
        }
        ty = ty + 1;
    }
}

/* Sprite in world pixels → screen (applies scroll). */
void g2d_sprite_world(int id, int wx, int wy)
{
    g2d_draw_sprite(id, g2d_vx + wx - g2d_scroll_x, g2d_vy + wy - g2d_scroll_y);
}

void g2d_present(void)
{
    ag_gfx_flush(g2d_vx, g2d_vy, g2d_vw, g2d_vh);
}

/* Pack one RGB565 pixel into atlas[byte_index..+1] (LE). c565 is 0..65535. */
void g2d_put565(char *atlas, int byte_index, int c565)
{
    atlas[byte_index] = c565 & 255;
    atlas[byte_index + 1] = (c565 >> 8) & 255;
}

/* Fill a tw×th tile in the atlas with solid c565. */
void g2d_fill_tile(char *atlas, int atlas_w, int tw, int th, int id, int c565)
{
    int tx;
    int ty;
    int row;
    int col;
    int stride;
    int base;
    tx = id % atlas_w;
    ty = id / atlas_w;
    stride = atlas_w * tw * 2;
    base = ty * th * stride + tx * tw * 2;
    row = 0;
    while (row < th) {
        col = 0;
        while (col < tw) {
            g2d_put565(atlas, base + row * stride + col * 2, c565);
            col = col + 1;
        }
        row = row + 1;
    }
}

/*
 * Paint helpers use g2d_tiles / atlas_w / tile_w / tile_h (call g2d_tileset
 * or assign those globals before building an atlas). Max 6 args for Mini-C.
 */
void g2d_tpix(int id, int lx, int ly, int c565)
{
    int tx;
    int ty;
    int base;
    if (g2d_tiles == 0) {
        return;
    }
    if (lx < 0 || ly < 0 || lx >= g2d_tile_w || ly >= g2d_tile_h) {
        return;
    }
    tx = id % g2d_atlas_w;
    ty = id / g2d_atlas_w;
    base = ty * g2d_tile_h * g2d_atlas_stride + tx * g2d_tile_w * 2;
    g2d_put565(g2d_tiles, base + ly * g2d_atlas_stride + lx * 2, c565);
}

void g2d_thline(int id, int x0, int x1, int y, int c565)
{
    int x;
    if (x0 > x1) {
        x = x0;
        x0 = x1;
        x1 = x;
    }
    x = x0;
    while (x <= x1) {
        g2d_tpix(id, x, y, c565);
        x = x + 1;
    }
}

void g2d_tvline(int id, int x, int y0, int y1, int c565)
{
    int y;
    if (y0 > y1) {
        y = y0;
        y0 = y1;
        y1 = y;
    }
    y = y0;
    while (y <= y1) {
        g2d_tpix(id, x, y, c565);
        y = y + 1;
    }
}

void g2d_trect(int id, int x0, int y0, int x1, int y1, int c565)
{
    int y;
    y = y0;
    while (y <= y1) {
        g2d_thline(id, x0, x1, y, c565);
        y = y + 1;
    }
}
