# Argon CC g2d — tiny 2D helpers for games

Mini-C library (not in the kernel). Builds on ABI **0.17** stateful blit:

`ag_gfx_blit_bind` → `ag_gfx_blit_copy` / `ag_gfx_blit_keyed`.

## Layout

| File | When to include |
|------|-----------------|
| `g2d_globals.h` | Before any function (with your globals) |
| `g2d_impl.h` | After **all** globals — ends the globals window |

Guest HostFS: copy these headers **beside** your `.c` (CC resolves bare `#include` next to the main file).

## API

```c
g2d_init(vx, vy, vw, vh);           /* view on framebuffer */
g2d_tileset(atlas, atlas_w, tw, th);/* RGB565 LE atlas, atlas_w tiles/row */
g2d_tilemap(map, mw, mh);           /* byte cell = tile id */
g2d_set_key(0xFF00FF);              /* sprite chroma (RRGGBB) */
g2d_set_scroll(sx, sy);
g2d_map_draw();                     /* tiles under camera */
g2d_sprite_world(id, wx, wy);       /* sprite in world pixels */
g2d_present();                      /* flush view rect */
```

Helpers to build atlases in Mini-C:

- `g2d_put565` / `g2d_fill_tile` — raw fill
- After `g2d_tileset` (or assign `g2d_tiles` / size globals): `g2d_tpix`,
  `g2d_thline`, `g2d_tvline`, `g2d_trect` — paint patterns inside a tile
  (≤6 args, Mini-C limit)

## Examples

- [`../examples/tile_demo.c`](../examples/tile_demo.c) — scroll + sprite smoke
- [`../examples/wetspot.c`](../examples/wetspot.c) — Wetspot-like acceptance game

```text
run h:\cc.axe h:\tile_demo.c h:\tile_demo.axe
run h:\tile_demo.axe
```

Needs `argon run -Gfx -HostFs …` and PADPUSH for movement demos.
