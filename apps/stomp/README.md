# STOMP — the amplifier with its knobs on a screen

```
run a:\STOMP.AXE [model | preset.PRESET] [play take.wav] [out sink]
                 [pot name=0..10] ...
```

Arrows walk between the controls, Enter presses the one under the cursor,
Q or Escape leaves.  A tap does the same in one go, because a finger should not
have to select and then confirm.

| word | what it does |
| --- | --- |
| `jcm800` … | a built-in model: lays the screen out, and with sound on, bakes it |
| `…\X.PRESET` | a chain off the card — curves, voicing and loudspeaker, no solver |
| `play take.wav` | a dry guitar at 22.05 kHz to run through it |
| `out pcmvirt` | where the sound goes: `pcmvirt`, `pcmmix`, `pcmnull`, or a `.wav` |
| `pot drive=8` | a knob, before anything is played |
| `os 2` | the oversampling, over whatever the preset was baked with |
| `cabms 46` | keep only that much of the cabinet's impulse |
| `vol 6` | the output trim after the cabinet, 5 being unity |
| `bench` | what the screen and the chain cost, and exit |

With no `play` and no `out` it is silent and only draws — which is the mode the
screen was built and measured in.

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

## The sound

A block is 256 frames — `AG_IR_BLOCK`, because the cabinet's overlap-add works
in exactly that and a second block size would mean a second buffer for nothing.
256 at 22.05 kHz is 11.6 ms.  Per block: the take, `ag_amp_tick` through the
valves, `ag_ir_process_block` through the loudspeaker, `ag_pcm_write` out.

**The screen only draws when the block loop is ahead of the sink** —
`ag_pcm_slack_us() >= 6000`.  That is not a formality: a full paint is 4.3 ms
against a block of 11.6, so without the check the first chooser opened would be
a dropout.  The dirty rectangles do the rest; turning a knob is 1.05 ms.

**A preset is the path a pedal is meant to take.**  It is the whole amplifier as
bytes — component values, voicing, baked curves, and the loudspeaker inside it —
and loading one needs no circuit solver and no axis fitting.  The host bakes
once, the box reads bytes and plays.

**A built-in model is the other path, and costs.**  `ag_amp_model` gives a
configuration but not a curve, so playing one means `ag_amp_build`: 212 M
instructions, 882 ms at 240 MHz, measured by tubebench.  It happens with the
screen saying so, because a bake under a ringing note is a gap, not a knob.

**Turning a knob goes through `ag_amp_set_knobs`**, which redesigns coefficients
without clearing filter state.  Zeroing eight biquads and three halfbands under a
ringing note is a click on every keypress; a coefficient changing under a running
filter is a step proportional to how far the knob moved.

### Rendering to a file, which is how this is checked

```
argon test -Sd "run a:\STOMP.AXE a:\stomp\JCM800.PRESET play a:\stomp\DI.WAV out a:\OUT.WAV pot master=2"
python tools/fatget.py build\sdcard.img OUT.WAV -o build\OUT.WAV
```

A `.wav` sink is a render, not a performance: `ag_pcm` does not pace it, and the
take is played once through rather than looped until the card fills.  `pot` is
there so that two renders at stated settings can be compared — a knob that cannot
be set without a finger cannot be tested, and "the knobs work" is a claim like
any other.

The output is clipped at the 16-bit rail and the count is printed on exit, the
same convention `tube_render` writes its listening files with.  With the master
at noon a hot DI runs into the rail; that is what the master is for.

## On a board, where the answer is time

The emulator can only count instructions.  A board answers in microseconds, and
the two differ by about a factor of two — QEMU models neither the FPU's latency
on a dependent chain nor a cache miss.  **Quote the board for anything about
keeping up.**  `run c:\stomp.axe c:\<model>.preset bench` measures the chain
against the block it has to fit in: 256 frames at 22.05 kHz is 11609 µs.

Measured on a Waveshare ESP32-S3-Zero with a CS4344, jcm800, two stages:

| | µs | of the block |
| --- | ---: | ---: |
| valves 1× + adaa | 1935 | 16% |
| valves 2× + adaa | 4071 | 35% |
| valves 4× + adaa | 7429 | 63% |
| cabinet, 200 ms, 18 partitions | 4089 | 35% |
| cabinet, 45 ms, 4 partitions | 1452 | 12% |
| cabinet, 22 ms, 2 partitions | 1297 | 11% |

Two things follow.

**The cabinet costs what its impulse is long.**  The convolution is partitioned
in blocks of `AG_IR_BLOCK`, so 200 ms is eighteen partitions and 46 ms is four.
A loudspeaker's impulse is mostly over in a few tens of milliseconds and the
rest is room — but truncating it changes what the matching walk fitted, so
`cabms` is a number to be listened to and measured, not a default to be assumed.

**Where the curve tables live decides whether it plays at all.**  Every
oversampled sample reads them.  In PSRAM they cost the valves about 11% more
than in internal SRAM, and 48 KB is what fits — which is why the tables are
sized by the preset's real stage count rather than by `AG_AMP_STAGES`.  Getting
them there needed a kernel fix: the ABI's capability bits and the port's are
different numbers with the same names, so `ag_malloc_caps(n, AG_MEM_FAST)` was
asking the heap for executable memory and being refused at every size.

With the curves in internal SRAM and a 45 ms cabinet, **4× with antialiasing
runs at 76% of the block** — 82–84% on the board including the screen, no drops.

## What is on the card

```
python tools/gen_stomp_card.py
argon sync build\sd_card
```

* `<MODEL>.PRESET` — the whole chain.  `tube_render preset <out> <drive> <model>`
  writes them.  **They carry `sizeof(ag_amp_cfg_t)` in their header and the
  loader refuses a mismatch**, so adding `pot[]` to the configuration invalidated
  every preset baked before it; the version went to 5 and the refusal says why.
* `<MODEL>.WAV` — the cabinet: `ir_<model>_bank.wav`, the impulse fitted with the
  matching bank still in the chain, which is the loudspeaker on its own and can
  go in front of another amplifier.  Not `ir_<model>_fitted.wav`, which carries
  the voicing as well.  Models whose impulses are byte-identical are staged once
  and named, because four names over two cabinets is a card that lies.
* `DI.WAV` — a dry take at 22.05 kHz.  Another rate is refused rather than
  resampled: a chain fitted at one rate playing a take at another sounds wrong in
  a way that is easy to mistake for the model being wrong.

## What is not here yet

**There is no input.**  The audio ABI has `write` and no `read`, and the port
contract has `dout` and no `din` — so a guitar cannot get in, and what plays is a
take off the card.  A pedal needs capture on the same I2S port as playback, one
clock for both directions, which is the lesson from Blackstomp in
`docs/10-watch-board.md`.  That is the next piece and it needs the board to be
believed.

Changing preset while playing reads a hundred kilobytes off the card in the event
handler, so the sink underruns for as long as that takes.  Changing amplifier is
not a knob, and it has not been made to sound like one.

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
