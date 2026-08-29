# `apps/fallout/testdata` — running the engine without the game

There is no Fallout data in this repository and there never will be. This is
what makes the engine run far enough to be tested anyway.

`fallout.cfg` leaves `master_dat` and `critter_dat` empty. The engine then
builds its database from the patches directory alone - `data\` - instead of
looking for a 400 MB archive, and `db_init` succeeds with nothing in it. That
one line is the difference between stopping at "Could not find the master
datafile" and running the whole of `game_init`.

The fonts come from `tools/mkfon.py`, which writes the ArgonOS console font
out in Fallout's `.FON` format. `win_init` loads the text fonts *before* it
opens the window, so without them there is no video path to reach at all.

```
python apps/fallout/tools/mkfon.py -o build/sd_card/data
copy apps\fallout\testdata\fallout.cfg build\sd_card\
argon sync build\sd_card
```

```
argon test -Sd "run a:\fallout.axe"
```

## How far that gets

```
SDL: display 640x480 with a framebuffer, 1280 bytes per row
Fade time is 956
Fade steps are 43
Chi squared is 18.75, P = 4000.00 at 0.05
Sequence is random, 95% confidence.
SDL: audio 22050 Hz, 2 channels, blocks of 1024 frames
>gsound_init  >initMovie  >gmovie_init  >moviefx_init
cache_init failed in art_init
art_init failed in iso_init
Failed on iso_init
```

Everything in `game_init` up to the art. The window is opened, the display is
acquired, the palette fade runs sixty frames at about 57 fps, the engine's own
random number generator passes its chi-squared self-test, the audio device is
opened and the mixer thread starts. `iso_init` is where the missing art stops
it, and that is the correct answer for a machine with no art on it.

Those messages exist because `build.py` compiles with `-D_DEBUG`: the engine's
`debug_printf` is compiled out otherwise, and a port that stops halfway stops
silently. That cost an afternoon of guessing before it was noticed.

## Seeing the frame

An automated run reads the text screen, which is exactly what a game holding
the display is not using. So the port can write the framebuffer out:

```
argon test -Sd "run a:\fallout.axe -shot a:\shot.ppm -frames 2"
```

```
argon get a:\shot.ppm
```

`-shot` and `-frames` are ours, stripped from `argv` before the engine sees it.
`SDLTEST.AXE` does the same thing without the engine at all, and is the test
that says whether the video path itself is right.

## What is still missing

`MASTER.DAT`, `CRITTER.DAT` and the `DATA` folder, from a copy of the game, at
the root of the card next to `FALLOUT.AXE`. Then `master_dat` and `critter_dat`
go back to their real values and this directory stops being needed.
