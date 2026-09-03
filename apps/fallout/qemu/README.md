# Running Fallout 2 under QEMU

The two files beside this one go on the card next to the game; the emulator has
no other way to be told what to do at boot. Copy them into the folder that
`argon sync` packs — the same folder that holds `master.dat` — then install
them onto `C:` once, from the guest.

## The recipe

```
python apps/fallout/build.py                 # FALLOUT2.AXE, 6340 KB arena
python tools/build_apps.py --only PCMVIRT.SYS KBDVIRT.SYS MOUSEVIRT.SYS
copy build\apps\FALLOUT2.AXE  <card folder>
copy build\apps\*VIRT.SYS     <card folder>
copy apps\fallout\qemu\*      <card folder>
argon sync <card folder> -SizeMB 1024
argon test -Sd "copy a:\system.cfg c:\system.cfg" "copy a:\start.bat c:\autoexec.bat"
argon run -Gfx -Virt -Sd
```

For the 16 MB arena, build with `--psram 16` and run with `-Psram 16`; the two
have to agree or the loader refuses the image.

## Why each piece is here

**The card image must be a power of two.** QEMU's `if=sd` drive silently
refuses anything else — the guest then prints *nothing at all*, which reads
like the emulator never started. 563 MB of game data therefore needs 1024,
and above 512 `mkfatimg` builds FAT32, which is what a real card is anyway.

**`SYSTEM.CFG` asks for 640x480.** The default surface follows the console —
80x25 cells of 8x16 is 640x400 — and Fallout's interface bar lives in the
80 rows that fall off the bottom. 480 is the most `board.c` accepts and the
panel takes it.

**`START.BAT` loads three drivers before the game.** Without them the shim
reports `input none` and the mixer stays on `pcmnull`: no keyboard, no mouse,
no sound. `argon run -Virt` starts the matching host helpers, and the guest
side is these three `drv load` lines. It is installed as `C:\AUTOEXEC.BAT`
because the shell runs `/sys/AUTOEXEC.BAT` at boot, which is the only way to
start the game in a window nobody can type into.

**Both files live on `C:`, not on the card.** The QEMU flash image has no
staged sysfs, so `C:` starts empty and littlefs formats itself; what is
written afterwards persists in `build\qemu_flash.bin` **until the firmware
changes**, at which point the image is regenerated and both files are gone.
Reinstall them after every `argon build`.

**Do not add `-shot` to the game's line.** It writes a 900 KB PPM onto the
card on the way out, and the next run then dies in `proto_init` with
"Error: Loading proto message files!" — which looks like missing data and is
not.

## What the numbers should look like

Playing, on the main map: **60 fps**, `eng 14 blit 0 wire 1 ms`, arena around
5800 KB used. The main menu holds ~64.

Left alone the menu starts the attract movies, and decoding MVE costs 66 ms a
frame — **15 fps, and it is the video decoder, not the port**. Press a key
before reading the frame rate.
