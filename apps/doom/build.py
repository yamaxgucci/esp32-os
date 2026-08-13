#!/usr/bin/env python3
"""Build DOOM.AXE, KBDVIRT.SYS, and bake build/sdcard.img from build/sd_card."""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def run(cmd):
    return subprocess.call(cmd)


def main():
    os.chdir(ROOT)
    gcc = "xtensa-esp32s3-elf-gcc"
    mkaxe = os.path.join("tools", "mkaxe.py")

    kbd = [
        sys.executable,
        mkaxe,
        "--arch",
        "xtensa",
        "--gcc",
        gcc,
        "--include",
        "sdk/include",
        "-o",
        os.path.join("build", "apps", "KBDVIRT.SYS"),
        os.path.join("apps", "kbdvirt", "kbdvirt.c"),
    ]
    print("KBDVIRT.SYS")
    err = run(kbd)
    if err:
        return err

    cores = sorted(glob.glob(os.path.join("apps", "doom", "core", "*.c")))
    sources = [
        os.path.join("apps", "doom", "doom_main.c"),
        os.path.join("apps", "doom", "port", "doomgeneric_argon.c"),
        os.path.join("apps", "common", "libc", "libc_shim.c"),
    ] + cores
    doom = [
        sys.executable,
        mkaxe,
        "--arch",
        "xtensa",
        "--gcc",
        gcc,
        "--include",
        "sdk/include",
        "--include",
        "apps/common",
        "--include",
        "apps/common/libc",
        "--include",
        "apps/doom",
        "--include",
        "apps/doom/port",
        "--include",
        "apps/doom/core",
        "--cflags",
        "-Os -ffunction-sections -fdata-sections -fno-builtin "
        "-include doom_cfg.h -Wno-unused -Wno-sign-compare "
        "-Wno-pointer-sign -Wno-maybe-uninitialized -Wno-implicit-function-declaration",
        "-o",
        os.path.join("build", "apps", "DOOM.AXE"),
    ] + sources
    print(" ".join(doom[:20]), "...", len(sources), "sources")
    err = run(doom)
    if err:
        return err

    share = os.path.join("build", "sd_card")
    os.makedirs(share, exist_ok=True)
    img = [
        sys.executable,
        os.path.join("tools", "mkfatimg.py"),
        "-o",
        os.path.join("build", "sdcard.img"),
        "-s",
        "64",
        share,
    ]
    print("bake", img[-1], "-> build/sdcard.img")
    return run(img)


if __name__ == "__main__":
    sys.exit(main())
