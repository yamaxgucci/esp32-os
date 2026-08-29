#!/usr/bin/env python3
"""Build FALLOUT.AXE - the fallout1-ce engine as an ArgonOS application.

The engine is not in this repository and will not be: it is under the
Sustainable Use License, which is not an open licence and forbids commercial
use.  What lives in apps/fallout is the port - the SDL subset, the platform
glue, this script - and all of it is Apache-2.0.  The engine and its two
dependencies are cloned into third_party/ on first run, where .gitignore keeps
them.  The same arrangement as apps/gfxbench and LVGL.

The game data - MASTER.DAT, CRITTER.DAT and the rest, some 400 MB - is
nobody's to distribute.  Supply your own from a copy of the game, on the SD
card.  This script neither fetches nor looks for it.

    python apps/fallout/build.py            fetch what is missing, then build
    python apps/fallout/build.py --survey   compile every file, report and stop
    python apps/fallout/build.py --no-fetch fail rather than clone

--survey is the useful one while the port is unfinished: it says how many of
the engine's translation units the Xtensa compiler accepts and what they weigh,
without needing the shim to be complete enough to link.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PORT = os.path.join(ROOT, "apps", "fallout", "port")
MKAXE = os.path.join(ROOT, "tools", "mkaxe.py")

# Pinned to the same commits the engine's own CMake fetches, so that a build
# here is the build upstream describes.
SOURCES = [
    ("fallout1-ce", "https://github.com/alexbatalov/fallout1-ce.git", "main"),
    ("adecode-src", "https://github.com/alexbatalov/adecode", "main"),
    ("fpattern-src", "https://github.com/alexbatalov/fpattern", "main"),
]

ENGINE = os.path.join(ROOT, "third_party", "fallout1-ce")
ADECODE = os.path.join(ROOT, "third_party", "adecode-src")
FPATTERN = os.path.join(ROOT, "third_party", "fpattern-src")

GCC = "xtensa-esp32s3-elf-gcc"
GXX = "xtensa-esp32s3-elf-g++"

# -fpermissive: the engine is C compiled as C++ in places, and 1997 was
# relaxed about it.  -w: it is not our code and the noise buries ours.
#
# -D_DEBUG is not about assertions.  The engine's own progress reporting -
# debug_printf, which is how it says ">gsound_init" and "Failed on iso_init" -
# is compiled out without it, and then a port that stops halfway stops
# silently.  It is the difference between knowing where the engine got to and
# guessing.  Turn it off when there is a reason to.
CFLAGS = ("-Os -ffunction-sections -fdata-sections -fno-builtin -mlongcalls "
          "-fno-exceptions -fno-rtti -std=c++17 -fpermissive -w -D_DEBUG")

# Files that only exist for a platform this is not.
SKIP = (
    "src/platform/",
    "os/",
)


def rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace("\\", "/")


def fetch(no_fetch: bool) -> None:
    for name, url, branch in SOURCES:
        path = os.path.join(ROOT, "third_party", name)
        if os.path.isdir(path):
            continue
        if no_fetch:
            raise SystemExit("fallout: %s is missing and --no-fetch was given"
                             % rel(path))
        print("fetching %s" % name)
        subprocess.check_call(
            ["git", "clone", "--depth", "1", "--branch", branch, url, path])


def engine_sources() -> list[str]:
    out = []
    for dirpath, _dirs, files in os.walk(os.path.join(ENGINE, "src")):
        for name in files:
            if not name.endswith(".cc"):
                continue
            full = os.path.join(dirpath, name)
            r = os.path.relpath(full, ENGINE).replace("\\", "/")
            if any(r.startswith(p) for p in SKIP):
                continue
            out.append(full)
    out.sort()
    return out


def includes() -> list[str]:
    return [
        PORT,
        os.path.join(ENGINE, "src"),
        os.path.join(ADECODE, "include"),
        os.path.join(FPATTERN, "include"),
        os.path.join(ROOT, "sdk", "include"),
        os.path.join(ROOT, "apps", "common", "cxx"),
    ]


def compile_one(src: str, obj: str) -> subprocess.CompletedProcess:
    driver = GXX if src.endswith((".cc", ".cpp", ".cxx")) else GCC
    cmd = [driver, "-c"] + CFLAGS.split()
    if driver == GCC:
        # The C flags are a subset; the C++-only ones would be rejected.
        cmd = [driver, "-c", "-Os", "-ffunction-sections", "-fdata-sections",
               "-fno-builtin", "-mlongcalls", "-w"]
    # fpattern refuses to compile until it is told which family of operating
    # system it is on, and there are only two.  DOS is the right answer twice
    # over: ArgonOS separates paths with a backslash and compares names without
    # case, and so do the .DAT files this is used to search.
    if os.path.basename(src) == "fpattern.c":
        cmd += ["-D__MSDOS__=1"]
    for inc in includes():
        cmd += ["-I", inc]
    cmd += [src, "-o", obj]
    return subprocess.run(cmd, capture_output=True, text=True)


def survey(objdir: str) -> int:
    os.makedirs(objdir, exist_ok=True)
    sources = engine_sources()
    ok, bad = 0, []
    sizes = {"text": 0, "rodata": 0, "data": 0, "bss": 0}

    for i, src in enumerate(sources):
        obj = os.path.join(objdir, "e%03d.o" % i)
        p = compile_one(src, obj)
        if p.returncode != 0:
            first = next((l.strip() for l in p.stderr.splitlines()
                          if " error: " in l), p.stderr.strip()[:200])
            bad.append((rel(src), first))
            continue
        ok += 1
        for name, size in sections(obj).items():
            sizes[name] = sizes.get(name, 0) + size

    print("\ncompiled %d of %d translation units" % (ok, len(sources)))
    print("code (text+literal): %d KB" % (sizes["text"] // 1024))
    print("rodata:              %d KB" % (sizes["rodata"] // 1024))
    print("data:                %d KB" % (sizes["data"] // 1024))
    print("bss:                 %d KB" % (sizes["bss"] // 1024))
    for name, err in bad:
        print("\nFAIL %s\n     %s" % (name, err))
    return 0 if not bad else 1


def sections(obj: str) -> dict:
    """text/rodata/data/bss of one object, in bytes."""
    out = {"text": 0, "rodata": 0, "data": 0, "bss": 0}
    p = subprocess.run([GCC.replace("-gcc", "-size"), "-A", obj],
                       capture_output=True, text=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2 or not parts[1].isdigit():
            continue
        name, size = parts[0], int(parts[1])
        if name.startswith(".text") or name.startswith(".literal"):
            out["text"] += size
        elif name.startswith(".rodata"):
            out["rodata"] += size
        elif name.startswith(".data"):
            out["data"] += size
        elif name.startswith(".bss"):
            out["bss"] += size
    return out


def port_sources() -> list[str]:
    """Ours, plus the two MIT libraries the engine calls its own."""
    return [
        os.path.join(ROOT, "apps", "fallout", "fallout_main.cc"),
        os.path.join(PORT, "sdl_shim.cc"),
        os.path.join(PORT, "platform.cc"),
        os.path.join(PORT, "posix_io.cc"),
        os.path.join(ADECODE, "src", "adecode.c"),
        os.path.join(FPATTERN, "src", "fpattern.c"),
        os.path.join(ROOT, "apps", "common", "cxx", "cxx_rt.cpp"),
        os.path.join(ROOT, "apps", "common", "cxx", "cxx_throw.cpp"),
        os.path.join(ROOT, "apps", "common", "cxx", "cxx_hosted.c"),
        os.path.join(ROOT, "apps", "common", "libc", "libc_shim.c"),
    ]


def link(objdir: str, output: str) -> int:
    """Compile everything, then hand the objects to mkaxe.

    mkaxe takes .o files as readily as sources, which is what makes this
    possible at all: the engine wants the toolchain's own headers and the libc
    shim wants its own, and the two include paths cannot both be in one
    command.  Compiling separately settles it.
    """
    os.makedirs(objdir, exist_ok=True)
    objects = []
    failed = []

    for i, src in enumerate(engine_sources() + port_sources()):
        obj = os.path.join(objdir, "o%03d.o" % i)
        p = compile_one(src, obj)
        if p.returncode != 0:
            first = next((l.strip() for l in p.stderr.splitlines()
                          if " error: " in l), p.stderr.strip()[:200])
            failed.append((rel(src), first))
            continue
        objects.append(obj)

    if failed:
        for name, err in failed:
            print("FAIL %s\n     %s" % (name, err))
        return 1

    print("compiled %d objects; linking" % len(objects))
    cmd = [sys.executable, MKAXE, "--arch", "xtensa", "--gcc", GCC,
           # newlib after libstdc++, for the parts of C the shim in
           # apps/common/libc does not carry: setjmp, the maths, qsort,
           # strtok and their neighbours.  Only the members that resolve
           # something still undefined come in, so the shim's malloc and
           # printf still win - they are already defined by then.
           # -lm for sqrt, pow and atan2: the isometric projection and the
           # line of sight are the only floating point in the game.
           "--libs", "stdc++", "--libs", "c", "--libs", "m",
           # Fallout is half a megabyte of code before the linker has had a
           # look; without this every unreferenced function in 128k lines is
           # carried into an arena that cannot hold them.
           # Written as one word: argparse reads a value that starts with a
           # dash as the next option otherwise.
           # ARGON_FALLOUT_LDFLAGS is the escape hatch for a link that has gone
           # wrong: -Wl,-y,<symbol> makes ld say which object asked for it,
           # which is the only practical way to find out why an archive member
           # arrived and collided with the shim.
           "--ldflags=-Wl,--gc-sections " +
           os.environ.get("ARGON_FALLOUT_LDFLAGS", ""),
           "-o", output] + objects
    return subprocess.call(cmd)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--survey", action="store_true",
                    help="compile every file, report sizes, do not link")
    ap.add_argument("--no-fetch", action="store_true",
                    help="fail if the engine is not already cloned")
    ap.add_argument("--objdir", default=os.path.join(ROOT, "build", "fallout"))
    ap.add_argument("-o", "--output",
                    default=os.path.join(ROOT, "build", "apps", "FALLOUT.AXE"))
    args = ap.parse_args()

    fetch(args.no_fetch)

    if args.survey:
        return survey(args.objdir)

    return link(args.objdir, args.output)


if __name__ == "__main__":
    sys.exit(main())
