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


def stage(board_dir, apps_dir, out_dir, display=None):
    """Assemble what belongs on C:, from the board pack and the built images."""
    os.makedirs(out_dir, exist_ok=True)
    staged = []

    for name in ("BOARD.CFG", "SYSTEM.CFG"):
        src = os.path.join(board_dir, name)
        if not os.path.isfile(src):
            raise SystemExit(f"mksysfs: {src} is missing")
        shutil.copy2(src, os.path.join(out_dir, name))
        staged.append(name)

    # SYSTEM.CFG is read after BOARD.CFG and wins, so the override goes there
    # rather than editing the board's own description of its hardware.
    if display is not None:
        with open(os.path.join(out_dir, "SYSTEM.CFG"), "a",
                  encoding="utf-8") as f:
            f.write("\n; Added by mksysfs --display\n"
                    "[display]\n"
                    f"driver = {display}\n")

    # Drivers, under the names SYSTEM.CFG spells: lower case, because that is
    # what `drv install` writes and littlefs does not fold case.
    drv_dir = os.path.join(out_dir, "drv")
    os.makedirs(drv_dir, exist_ok=True)
    sysfile = os.path.join(board_dir, "modules.txt")
    if os.path.isfile(sysfile):
        with open(sysfile, "r", encoding="utf-8") as f:
            wanted = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    else:
        wanted = ["ILI9341.SYS", "XPT2046.SYS"]

    for image in wanted:
        src = os.path.join(apps_dir, image)
        if not os.path.isfile(src):
            raise SystemExit(
                f"mksysfs: {src} is missing - run `argon apps --group board`")
        shutil.copy2(src, os.path.join(drv_dir, image.lower()))
        staged.append("drv/" + image.lower())

    return staged


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default=os.path.join(ROOT, "boards", "esp32-cyd"),
                    help="board pack holding BOARD.CFG and SYSTEM.CFG")
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
    ap.add_argument("--flash", action="store_true",
                    help="write the image to the board as well")
    ap.add_argument("-p", "--port", help="serial port, for --flash")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    offset, size = read_partition(args.partitions, "sysfs")
    name_max = read_config(args.sdkconfig, "CONFIG_LITTLEFS_OBJ_NAME_LEN", "64")

    staged_dir = tempfile.mkdtemp(prefix="argon-sysfs-")
    try:
        staged = stage(args.board, args.apps, staged_dir, args.display)
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

        print(f"mksysfs: {args.out}  {size} bytes at 0x{offset:x}"
              + (f"  [display] driver = {args.display}" if args.display else ""))
        for name in staged:
            print(f"  C:\\{name}")
    finally:
        shutil.rmtree(staged_dir, ignore_errors=True)

    if not args.flash:
        return 0

    if not args.port:
        raise SystemExit("mksysfs: --flash needs -p PORT")

    flash = [sys.executable, "-m", "esptool", "--chip", "esp32",
             "-p", args.port, "-b", str(args.baud),
             "--before", "default_reset", "--after", "hard_reset",
             "write_flash", hex(offset), args.out]
    return subprocess.run(flash).returncode


if __name__ == "__main__":
    sys.exit(main())
