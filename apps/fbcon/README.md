# FBCON — soft framebuffer preview in the text console

Reads `d:\fb0` (RGB565) and draws it with CP437 half-blocks. Does **not** call
`ag_gfx_acquire`, so it can run next to `SMS.AXE`.

This is a **debug preview**, not a real display. For real pixels see below.

## Build

Normally just `argon apps` (or `argon apps --only FBCON.AXE`) — the
authoritative build line for this image lives in [`tools/apps.json`](../../tools/apps.json)
and is compiled by CI. The command below is the same thing, spelled out:

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include -o build/apps/FBCON.AXE apps/fbcon/fbcon.c
```

## Run (Esc must reach fbcon)

Keyboard input goes only to the **foreground** process. Start SMS in the
background, then fbcon in the foreground:

```
A:\> run /b sms.axe rambo.sms
A:\> run fbcon.axe
```

Then `Esc` or `Q` quits fbcon. If you used `run /b fbcon.axe`, Esc goes to the
shell — use `ps` and `kill <pid>` instead.

Prefer `argon run -Gfx` for real-time pixels; fbcon is only a text peek.

Optional step: `fbcon.axe 2` (finer) … `fbcon.axe 8` (chunkier).

## Real pixels (not characters)

| Way | What you get |
|-----|----------------|
| `argon run -Gfx` | Live SDL window (QEMU RGB panel) |
| `gfxdump` + `argon get` | PPM file on the PC |
| Real LCD (ST7789 / RGB panel) | Hardware — drivers not in yet |
