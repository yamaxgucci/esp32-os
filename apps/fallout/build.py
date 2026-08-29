#!/usr/bin/env python3
"""Build Fallout 1 or Fallout 2 as an ArgonOS application.

Neither engine is in this repository and neither will be: both are under the
Sustainable Use License, which is not an open licence and forbids commercial
use.  What lives in apps/fallout is the port - the SDL subset, the platform
glue, this script - and all of it is Apache-2.0.  The engines and their
dependencies are cloned into third_party/ on first run, where .gitignore keeps
them.  The same arrangement as apps/gfxbench and LVGL.

The game data - MASTER.DAT, CRITTER.DAT and the rest, half a gigabyte of it -
is nobody's to distribute.  Supply your own from a copy of the game, on the SD
card.  This script neither fetches nor looks for it.

    python apps/fallout/build.py             Fallout 2 -> FALLOUT2.AXE
    python apps/fallout/build.py --game 1    Fallout 1 -> FALLOUT.AXE
    python apps/fallout/build.py --survey    compile every file, report, stop
    python apps/fallout/build.py --no-fetch  fail rather than clone

The default is the second game because it is the one whose data is on the
machine this was written on.  The two engines are the same author and the same
lineage; they differ by four names in the SDL surface, which the shim answers
for both, and by zlib, which Fallout 2 needs because its .DAT entries are
deflated where Fallout 1's are not.

--survey is the useful one while a port is unfinished: it says how many of the
engine's translation units the Xtensa compiler accepts and what they weigh,
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

# Where each clone comes from.  The dependency versions are the ones each
# engine's own CMake fetches, so that a build here is the build upstream
# describes.
REPOS = {
    "fallout1-ce": ("https://github.com/alexbatalov/fallout1-ce.git", "main"),
    "fallout2-ce": ("https://github.com/alexbatalov/fallout2-ce.git", "main"),
    "adecode-src": ("https://github.com/alexbatalov/adecode", "main"),
    "fpattern-src": ("https://github.com/alexbatalov/fpattern", "main"),
    "zlib-src": ("https://github.com/madler/zlib", "v1.3.1"),
}


def tp(name: str) -> str:
    return os.path.join(ROOT, "third_party", name)


# The two games, and what each needs beyond the port.
#
# adecode is Fallout 1's ACM audio decoder.  Fallout 2 carries its own and
# wants zlib instead: its .DAT entries are deflated, and it reaches for the
# gz* file layer rather than raw inflate, so the whole library comes in.
GAMES = {
    "1": {
        "n": "1",
        "engine": "fallout1-ce",
        "output": "FALLOUT.AXE",
        "deps": ["adecode-src", "fpattern-src"],
    },
    "2": {
        "n": "2",
        "engine": "fallout2-ce",
        "output": "FALLOUT2.AXE",
        "deps": ["fpattern-src", "zlib-src"],
    },
}

# Filled in by main() once --game is known.
GAME = GAMES["2"]
ENGINE = tp(GAME["engine"])
FPATTERN = tp("fpattern-src")
ADECODE = tp("adecode-src")
ZLIB = tp("zlib-src")

# The parts of zlib Fallout 2 reaches: the deflate stream, and the gz* file
# layer on top of it, which goes through open/read/write in port/posix_io.cc.
ZLIB_SOURCES = [
    "adler32.c", "crc32.c", "deflate.c", "trees.c", "zutil.c",
    "inflate.c", "inftrees.c", "inffast.c", "infback.c",
    "compress.c", "uncompr.c",
    "gzclose.c", "gzlib.c", "gzread.c", "gzwrite.c",
]

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

# Forced in front of every C++ translation unit; see the file itself.  Kept out
# of CFLAGS because that is split on spaces and this path has some in it.
PRELUDE = os.path.join(PORT, "engine_prelude.h")

# How much of extended memory the process asks for.  See the comment on
# AG_APP_SIZED in fallout_main.cc; --arena-kb overrides it.
ARENA_KB = 5875

# Files that only exist for a platform this is not.
SKIP = (
    "src/platform/",
    "os/",
)


def rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace("\\", "/")


def fetch(no_fetch: bool) -> None:
    for name in [GAME["engine"]] + GAME["deps"]:
        path = tp(name)
        if os.path.isdir(path):
            continue
        if no_fetch:
            raise SystemExit("fallout: %s is missing and --no-fetch was given"
                             % rel(path))
        url, branch = REPOS[name]
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
    out = [
        PORT,
        os.path.join(ENGINE, "src"),
        os.path.join(FPATTERN, "include"),
        os.path.join(ROOT, "sdk", "include"),
        os.path.join(ROOT, "apps", "common", "cxx"),
    ]
    if "adecode-src" in GAME["deps"]:
        out.insert(2, os.path.join(ADECODE, "include"))
    if "zlib-src" in GAME["deps"]:
        out.append(ZLIB)
    return out


def compile_one(src: str, obj: str) -> subprocess.CompletedProcess:
    driver = GXX if src.endswith((".cc", ".cpp", ".cxx")) else GCC
    cmd = [driver, "-c"] + CFLAGS.split() + [
        "-include", PRELUDE,
        "-DAG_FALLOUT_GAME=" + GAME["n"],
        "-DAG_FALLOUT_ARENA_KB=%d" % ARENA_KB]
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
    # zlib's gz* layer opens files with the POSIX calls rather than stdio, and
    # only declares them when told the platform has <unistd.h>.  It does: they
    # come out of newlib and land in port/posix_io.cc.  Normally CMake works
    # this out by compiling a test program; there is no CMake here.
    if os.path.dirname(src) == ZLIB:
        cmd += ["-DZ_HAVE_UNISTD_H=1"]
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
    """Ours, plus the libraries each engine calls its own."""
    out = [
        os.path.join(ROOT, "apps", "fallout", "fallout_main.cc"),
        os.path.join(PORT, "sdl_shim.cc"),
        os.path.join(PORT, "platform.cc"),
        os.path.join(PORT, "posix_io.cc"),
        os.path.join(FPATTERN, "src", "fpattern.c"),
        os.path.join(ROOT, "apps", "common", "cxx", "cxx_rt.cpp"),
        os.path.join(ROOT, "apps", "common", "cxx", "cxx_throw.cpp"),
        os.path.join(ROOT, "apps", "common", "cxx", "cxx_hosted.c"),
        os.path.join(ROOT, "apps", "common", "libc", "libc_shim.c"),
    ]
    if "adecode-src" in GAME["deps"]:
        out.append(os.path.join(ADECODE, "src", "adecode.c"))
    if "zlib-src" in GAME["deps"]:
        out += [os.path.join(ZLIB, name) for name in ZLIB_SOURCES]
    return out


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
    rc = run_link(objects, output, extra="")
    if rc == 0:
        return 0

    # Fallout 2 uses std::ofstream in display_monitor.cc and std::random_device
    # in sfall_arrays.cc.  Either one drags in the whole of libstdc++'s
    # iostreams and locale, and that machinery calls newlib's stdio and its
    # string-to-number conversions - which the shim in apps/common/libc also
    # defines.  The two collide.
    #
    # The shim has to win: it is the one that knows about the process arena and
    # the ArgonOS console, and the newlib versions want a reentrancy structure
    # this program only pretends to have.  Objects come before archives on the
    # command line, so taking the first definition is taking ours.
    #
    # --allow-multiple-definition does that, and would also swallow a genuine
    # collision between two of our own files.  So it is not simply passed: the
    # link is tried without it first, and the flag is only used when every
    # complaint is of the expected shape - ours first, the toolchain's second.
    dups, other = classify_duplicates(rc_output, objdir)
    if not dups or other:
        sys.stderr.write(rc_output)
        return rc

    print("allowing %d duplicate symbol(s) the toolchain also defines: %s"
          % (len(dups), ", ".join(sorted(dups))))
    return run_link(objects, output, extra=" -Wl,--allow-multiple-definition")


# Set by run_link so the retry above can look at what went wrong.
rc_output = ""


def classify_duplicates(text: str, objdir: str) -> tuple:
    """Split 'multiple definition' complaints into expected and not.

    ld reports each one as

        <second definer>: multiple definition of `X'; <first definer>: first
        defined here

    and the test is which of the two is ours.  If our object defined it first,
    then --allow-multiple-definition keeps ours - the shim standing in for a
    libc that has now arrived underneath it, which is the case this is for.

    If it defined it second, or neither did, then two files disagree about
    something and the flag would hide it.  Those are returned separately and
    the link is left failed.
    """
    marker = os.path.normcase(os.path.abspath(objdir))
    dups = set()
    other = []
    for line in text.splitlines():
        if "multiple definition of" not in line:
            continue
        head, _, tail = line.partition("multiple definition of")
        name = tail.strip().lstrip("`").split("'")[0]
        first = tail.split(";", 1)[1] if ";" in tail else ""
        if marker in os.path.normcase(first) and marker not in os.path.normcase(head):
            dups.add(name)
        else:
            other.append(line.strip())
    return dups, other


def run_link(objects: list, output: str, extra: str) -> int:
    global rc_output
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
           "--ldflags=-Wl,--gc-sections " + extra + " " +
           os.environ.get("ARGON_FALLOUT_LDFLAGS", ""),
           "-o", output] + objects
    p = subprocess.run(cmd, capture_output=True, text=True)
    rc_output = p.stdout + p.stderr
    if p.returncode != 0:
        # Held back until the caller has decided whether the failure was the
        # expected one; printing it here would bury the retry that follows.
        return p.returncode
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    return 0


def main() -> int:
    global GAME, ENGINE, ARENA_KB

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", choices=("1", "2"), default="2",
                    help="which engine (default 2, the one whose data is at "
                         "hand)")
    ap.add_argument("--arena-kb", type=int, default=ARENA_KB,
                    help="extended memory the process asks for, in KB "
                         "(default %d, which is what fits beside the image on "
                         "8 MB of PSRAM)" % ARENA_KB)
    ap.add_argument("--survey", action="store_true",
                    help="compile every file, report sizes, do not link")
    ap.add_argument("--no-fetch", action="store_true",
                    help="fail if the engine is not already cloned")
    ap.add_argument("--objdir", default=None)
    ap.add_argument("-o", "--output", default=None)
    args = ap.parse_args()

    GAME = GAMES[args.game]
    ENGINE = tp(GAME["engine"])
    ARENA_KB = args.arena_kb

    # Separate object directories, or a switch between games would link one
    # engine's objects into the other's image and fail in ways that look like
    # the port's fault.
    objdir = args.objdir or os.path.join(ROOT, "build", GAME["engine"])
    output = args.output or os.path.join(ROOT, "build", "apps",
                                         GAME["output"])

    fetch(args.no_fetch)

    if args.survey:
        return survey(objdir)

    return link(objdir, output)


if __name__ == "__main__":
    sys.exit(main())
