#!/usr/bin/env python3
"""Build the C: partition as an image, so it can be flashed rather than typed.

C: is the board's own flash - a littlefs partition holding BOARD.CFG, the
modules SYSTEM.CFG asks for, and whatever the running system writes there.
Everything on it used to arrive one way: through the console, with the shell's
`recv` command echoing every byte back as hex.  That works, and it has two
costs that turned out to matter.

The first is cosmetic - a screen full of numbers on every upload.  The second
is not: a file delivered that way lands *after* the boot has finished, so the
driver the system loaded is the previous one.  Every driver change needs two
boots, and forgetting the second sends you looking for a bug in code that is
not running.  Worse, a driver that stops the boot cannot be replaced at all,
because there is no prompt to type `recv` at.

An image is written by the same esptool run that writes the firmware, before
anything has booted, and none of the above applies.

    python tools/mksysfs.py                    build/sysfs.bin
    python tools/mksysfs.py --flash -p COM3    and write it to the board

The layout comes from the partition table and sdkconfig, so there is nothing
here to keep in step by hand.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_partition(csv_path, name):
    """Offset and size of one partition, as the CSV declares them."""
    with open(csv_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[0] == name:
                return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"mksysfs: no '{name}' partition in {csv_path}")


def read_config(sdkconfig, key, default):
    """One CONFIG_ value out of sdkconfig, which is where littlefs's own are."""
    try:
        with open(sdkconfig, "r", encoding="utf-8") as f:
            for line in f:
                m = re.match(rf"^{re.escape(key)}=(.*)$", line.strip())
                if m:
                    return m.group(1)
    except OSError:
        pass
    return default


def stage(board_dir, apps_dir, out_dir, display=None, extra=(),
          display_size=None):
    """Assemble what belongs on C:, from the board pack and the built images.

    `board_dir` may be None, and then nothing comes from a pack: the caller
    supplies every file with --add.  That is for the emulator, which has no
    board and whose pins are nobody's - staging the CYD's BOARD.CFG there
    describes hardware that is not present and, worse, declares a 160x120
    display on a machine that could show any size.
    """
    os.makedirs(out_dir, exist_ok=True)
    staged = []

    if board_dir is not None:
        for name in ("BOARD.CFG", "SYSTEM.CFG"):
            src = os.path.join(board_dir, name)
            if not os.path.isfile(src):
                raise SystemExit(f"mksysfs: {src} is missing")
            shutil.copy2(src, os.path.join(out_dir, name))
            staged.append(name)

        # Drivers, under the names SYSTEM.CFG spells: lower case, because that
        # is what `drv install` writes and littlefs does not fold case.
        drv_dir = os.path.join(out_dir, "drv")
        os.makedirs(drv_dir, exist_ok=True)
        sysfile = os.path.join(board_dir, "modules.txt")
        if os.path.isfile(sysfile):
            with open(sysfile, "r", encoding="utf-8") as f:
                wanted = [l.strip() for l in f
                          if l.strip() and not l.startswith("#")]
        else:
            wanted = ["ILI9341.SYS", "XPT2046.SYS"]

        for image in wanted:
            src = os.path.join(apps_dir, image)
            if not os.path.isfile(src):
                raise SystemExit(
                    f"mksysfs: {src} is missing - run `argon apps --group board`")
            shutil.copy2(src, os.path.join(drv_dir, image.lower()))
            staged.append("drv/" + image.lower())

    for spec in extra:
        host, _, name = spec.partition("=")
        if not name:
            name = os.path.basename(host)
        if not os.path.isfile(host):
            raise SystemExit(f"mksysfs: {host} is missing")
        dst = os.path.join(out_dir, *name.split("/"))
        os.makedirs(os.path.dirname(dst) or out_dir, exist_ok=True)
        shutil.copy2(host, dst)
        if name not in staged:
            staged.append(name)

    # Last, and that is a fix rather than a detail: SYSTEM.CFG is read after
    # BOARD.CFG and wins, so the override belongs there rather than in the
    # board's own description of its hardware - but appending it before the
    # --add files were copied meant an `--add mine.cfg=SYSTEM.CFG` silently
    # threw the override away.  Now it is appended to whichever SYSTEM.CFG
    # ended up on the image, and one is created if none did.
    if display is not None or display_size is not None:
        path = os.path.join(out_dir, "SYSTEM.CFG")
        with open(path, "a", encoding="utf-8") as f:
            f.write("\n; Added by mksysfs\n[display]\n")
            if display is not None:
                f.write(f"driver = {display}\n")
            if display_size is not None:
                f.write(f"width  = {display_size[0]}\n"
                        f"height = {display_size[1]}\n")
        if "SYSTEM.CFG" not in staged:
            staged.append("SYSTEM.CFG")

    return staged


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default=os.path.join(ROOT, "boards", "esp32-cyd"),
                    help="board pack holding BOARD.CFG and SYSTEM.CFG; "
                         "`none` stages no pack at all and every file comes "
                         "from --add, which is what the emulator wants - it "
                         "has no pins to describe and no fixed screen size")
    ap.add_argument("--apps", default=os.path.join(ROOT, "build", "apps"),
                    help="where the built .SYS images are")
    ap.add_argument("--partitions",
                    default=os.path.join(ROOT, "partitions_4mb.csv"))
    ap.add_argument("--sdkconfig", default=os.path.join(ROOT, "sdkconfig"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sysfs.bin"))
    ap.add_argument("--display", choices=("soft", "panel", "none"),
                    help="override [display] driver in the staged SYSTEM.CFG.  "
                         "`soft` is a framebuffer the system owns and shares; "
                         "`panel` is none at all, with applications bringing "
                         "their own pixels (gfx->present) - which on this board "
                         "is 37 KB of a 320 KB machine handed back; `none` is no "
                         "graphics whatsoever")
    ap.add_argument("--display-size", metavar="WxH",
                    help="the soft framebuffer's size, into the same "
                         "[display] section.  Without this the firmware's "
                         "default (640x400) stands, and the only other way to "
                         "change it is to write C:\\SYSTEM.CFG on a running "
                         "system and reboot - which in QEMU is unreliable "
                         "enough to have wasted a session (see "
                         "apps/desktop/check.ps1)")
    ap.add_argument("--add", action="append", default=[], metavar="HOST[=NAME]",
                    help="put another file on C: as well, repeatable.  For "
                         "anything big enough that sending it through the "
                         "console would be a wait - a cartridge, an "
                         "application - since this goes in with the same "
                         "esptool run as the firmware")
    ap.add_argument("--flash", action="store_true",
                    help="write the image to the board as well")
    ap.add_argument("-p", "--port", help="serial port, for --flash")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    offset, size = read_partition(args.partitions, "sysfs")
    name_max = read_config(args.sdkconfig, "CONFIG_LITTLEFS_OBJ_NAME_LEN", "64")
    # Which chip, from the same place name-max comes from.  It used to be the
    # string "esp32" in the esptool line below, which was invisible while one
    # board existed and became `This chip is ESP32-C6, not ESP32` the day a
    # second one did.  Quoted in sdkconfig, hence the strip.
    chip = read_config(args.sdkconfig, "CONFIG_IDF_TARGET", '"esp32"').strip('"')

    size_wh = None
    if args.display_size:
        m = re.fullmatch(r"(\d+)\s*[xX]\s*(\d+)", args.display_size.strip())
        if not m:
            raise SystemExit("mksysfs: --display-size wants WxH, e.g. 320x240")
        size_wh = (int(m.group(1)), int(m.group(2)))

    board = None if args.board.lower() == "none" else args.board

    staged_dir = tempfile.mkdtemp(prefix="argon-sysfs-")
    try:
        staged = stage(board, args.apps, staged_dir, args.display,
                       args.add, size_wh)
        os.makedirs(os.path.dirname(args.out), exist_ok=True)

        # The component's own tool, invoked the way its CMake does: block size
        # is littlefs's and fixed at 4096 by esp_littlefs, and name-max has to
        # match the build or the board will not mount what this writes.
        cmd = [sys.executable, "-m", "littlefs.__main__", "create", staged_dir,
               args.out, f"--fs-size={size}", f"--name-max={name_max}",
               "--block-size=4096"]
        try:
            subprocess.run(cmd, check=True)
        except FileNotFoundError:
            raise SystemExit("mksysfs: littlefs-python is missing.  "
                             "pip install littlefs-python")

        note = ""
        if args.display:
            note += f"  [display] driver = {args.display}"
        if size_wh:
            note += f"  {size_wh[0]}x{size_wh[1]}"
        print(f"mksysfs: {args.out}  {size} bytes at 0x{offset:x}  ({chip})"
              + note)
        for name in staged:
            print(f"  C:\\{name}")
    finally:
        shutil.rmtree(staged_dir, ignore_errors=True)

    if not args.flash:
        return 0

    if not args.port:
        raise SystemExit("mksysfs: --flash needs -p PORT")

    flash = [sys.executable, "-m", "esptool", "--chip", chip,
             "-p", args.port, "-b", str(args.baud),
             "--before", "default_reset", "--after", "hard_reset",
             "write_flash", hex(offset), args.out]
    return subprocess.run(flash).returncode


if __name__ == "__main__":
    sys.exit(main())
