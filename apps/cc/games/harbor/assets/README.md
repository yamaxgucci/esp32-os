# Harbor Quest assets

## `atlas.bin`

Procedural RGB565 LE atlas (16×16, 16×8 tiles, chroma `#FF00FF`).

```text
python tools/rpg_atlas.py -o apps/cc/games/harbor/assets/atlas.bin --png apps/cc/games/harbor/assets/atlas.png
```

Stages to `build/sd_card/atlas.bin`. Packs under `raw/` are optional reference only;
the shipped look is the procedural generator.

## Save file

Progress is written to `h:\harbor.sav` (map, party, quest flags, gold).
Title: **Z** continue (if save exists), **X** new game.
