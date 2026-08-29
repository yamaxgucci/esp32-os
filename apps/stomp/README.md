# STOMP — the amplifier with its knobs on a screen

```
run a:\STOMP.AXE [model]        jcm800 (default), bogner, slo, ts9
```

Arrows walk between the controls, Enter presses the one under the cursor,
Q or Escape leaves.  A tap does the same in one go, because a finger should not
have to select and then confirm.

## The knob list is not kept here

`ag_amp_pot_count` / `ag_amp_pot_id` / `ag_amp_pot_name` answer for the model,
and the screen lays out however many that is:

| model | controls |
| --- | --- |
| `jcm800` | DRIVE BASS MID TREB MASTER |
| `bogner`, `slo` | DRIVE GAIN BASS MID TREB MASTER |
| `ts9` | DRIVE TONE MASTER |

A screen carrying its own list would be wrong the first time a model changed,
and the models are still changing.

## Noon is 5, and 5 is what the fit left

Every pot starts at 0.5, which is the position each preset was voiced and
measured at: `ag_amp_pot_apply` at 0.5 must leave the configuration bit-identical
to the one the matching walk produced.  The knobs belong to the player; the
matching never turns them.

Bass, mid and treble are solved in the passive network, so they are the circuit's
own.  Drive, gain and master are still levels between blocks — a stop-gap, and
`ag_amp.c` says what replaces it.

## The three buttons

Along the bottom, and buttons rather than labels because they are pressed:

| button | Enter does |
| --- | --- |
| `CAB <name>` | opens the cabinet chooser |
| `ON` / `OFF` | takes the cabinet out of the path and puts it back |
| `PRESET <name>` | opens the preset chooser |

The switch is separate from the chooser on purpose: **switching a cabinet out is
one press**, and it keeps the chosen file, so pressing again brings the same
cabinet back.  The name goes dim while it is out.  `(no cabinet)` is also the
first line of the cabinet's list, because off is a position of that control and
not only a switch.

## The chooser

A real directory listing over the picture, walked with the arrows, `Enter` to
descend into `[a directory]` or take what is under the cursor, `Escape` to leave;
a tap outside it also leaves.  It starts in the working directory, shows `..`
unless it is at a drive's root, and filters by what the button is for — `.wav`
for a cabinet, `.preset` for a preset.  Sixty-four entries fit; past that it says
how many it did not show rather than pretending the directory ended.

The preset list carries **the models built into this image** above the files,
because those are presets too.  Picking one takes effect at once — the knobs
under it change as you pick, which is the fastest proof that the layout follows
the model.

## What the screen costs

Measured, not guessed — `run a:\STOMP.AXE bench` under `argon test -Icount`,
which is tubebench's unit: instructions with `-icount shift=0`, a floor, since
QEMU models neither the FPU's latency nor a cache miss.  640×400 soft display:

| | instructions | at 240 MHz | of an audio frame |
| --- | ---: | ---: | ---: |
| full paint | 1 042 650 | 4.34 ms | 95 samples |
| — of which the clear | 528 450 | 2.20 ms | 48 |
| flush, whole frame | 100 | — | 0 |
| turn a knob | 252 200 | 1.05 ms | 23 samples |
| move the cursor | 334 050 | 1.39 ms | 30 samples |
| chooser opening | 2 100 550 | 8.75 ms | 192 samples |

The last column is against 22.05 kHz.  `flush` is nearly free **here only**:
in QEMU the drawing target is the panel's own memory, so the call presents
without copying.  On a panel at the end of an SPI bus the same call is
width × height × 2 bytes down the wire, and that is why the redraw marks
rectangles rather than sending whole frames.

Two rules come out of this:

* **A redraw never runs inside the audio callback.**  A full paint is three
  frames of 32 samples long; no tidying of the drawing changes that.  It belongs
  in its own task, below the audio in priority.
* **Only what changed is painted.**  Turning a knob repaints one cell — a fifth
  of a full paint — and an event that changes no pixel costs nothing at all.
  There is no `swap`: `flush` already copies its rectangle to the front buffer
  and presents it, and calling both paid the whole update twice.

Against the heaviest chain: `slo` at 4× with antialiasing is 6094 instructions a
sample, 55.98% of a core, and the cabinet convolution after it another 981, so
about **65% of one core** — leaving a third of that core, on a chip with two.

## Cabinets on the card

```
python tools/gen_stomp_cabs.py
argon sync build\sd_card
```

Stages `build/listen/ir_<model>_bank.wav` as `a:\stomp\<MODEL>.WAV` — the
impulse fitted with the matching bank still in the chain, which is the
loudspeaker on its own and can go in front of another amplifier.  Not
`ir_<model>_fitted.wav`: that one carries the voicing as well, so it is not a
cabinet.  Models whose impulses are byte-identical are staged once and named,
because four names over two cabinets is a card that lies about what is on it.

## What is not here yet

The audio.  This edits a configuration and draws it; wiring it to the input and
the output is the next piece, and it is deliberately separate, because a screen
that redraws while the card is waiting is how a knob becomes a crackle.

So a chosen **file** — a `.wav` cabinet, a `.preset` off the card — is recorded
and shown on its button, and is read when there is something to play it through.
A chosen **built-in model** takes effect immediately.

## Trying it without a board

```
argon apps --only STOMP.AXE
argon sync build\sd_card
argon run -Sd -Gfx
```

`gfxdump` cannot photograph it: reaching a prompt to type that at means leaving
the application, and the console redraws over what it drew.  Grab QEMU's RGB
window instead.

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
