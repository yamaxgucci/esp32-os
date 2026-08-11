# Harbor Quest

Top-down RPG for **Argon CC + g2d** (Phantasy Star-like presentation).

English chapter-1 story beats loosely follow a short Genesis RPG opening
(Traysia-style: leave harbor town, find a companion, clear a bandit cave,
return home). Names and dialogue are original.

## HostFS layout

Stage beside each other under `build/sd_card/`:

- `harbor.c`, `hq_data.h`
- `g2d_globals.h`, `g2d_impl.h` (from `apps/cc/lib/g2d/`)
- `atlas.bin` (from `apps/cc/games/harbor/assets/` or `python tools/rpg_atlas.py`)
- `sms.cfg` (keyboard → pad map; copied from `apps/sms/sms.cfg`)
- `CC.AXE`

## Play

```text
argon run -Gfx -HostFs build\sd_card
run h:\cc.axe h:\harbor.c h:\harbr.axe
run h:\harbr.axe
```

### Controls (keyboard)

Focus the **SDL graphics window** (not the text console), then:

| Key | Action |
|-----|--------|
| Arrow keys | Move |
| **Z** or **Enter** | Talk / confirm / fight |
| **X** | Cancel / run from battle |
| **Esc** | Quit |

Older “B1/B2” labels meant pad buttons; on a keyboard that is **Z** / **X**
(`sms.cfg`: `pad0.b1=Z`, `pad0.b2=X`).

## Route

1. You start next to **Lina** — press **Z** to talk.
2. Buy a sword from the **Smith** (20 gold) — stand next to him, **Z**.
3. Walk south → **North Road** → north → **Grayfen**.
4. Talk to **Ban** (joins if you have the sword).
5. Back to the field, enter the **stairs** in the rocks → cave.
6. Talk to **Cave Chief** (boss). Return to Lina.
