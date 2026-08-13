#!/usr/bin/env python3
"""Build DOOM.AXE from doomgeneric + the Argon port layer."""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    os.chdir(ROOT)
    cores = sorted(glob.glob(os.path.join("apps", "doom", "core", "*.c")))
    sources = [
        os.path.join("apps", "doom", "doom_main.c"),
        os.path.join("apps", "doom", "port", "doomgeneric_argon.c"),
        os.path.join("apps", "common", "libc", "libc_shim.c"),
    ] + cores
    cmd = [
        sys.executable,
        os.path.join("tools", "mkaxe.py"),
        "--arch",
        "xtensa",
        "--gcc",
        "xtensa-esp32s3-elf-gcc",
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
    print(" ".join(cmd[:20]), "...", len(sources), "sources")
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
