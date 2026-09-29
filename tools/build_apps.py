#!/usr/bin/env python3
"""Build every .AXE / .SYS listed in tools/apps.json.

Applications are not part of the firmware build, so nothing used to compile
them: the only record of how each one is built was a hand-written mkaxe line
in its README, and a change under apps/common could break half of them while
`argon check` stayed green.  This walks the manifest instead.

  python tools/build_apps.py                 # the core group
  python tools/build_apps.py --group all     # everything in the manifest
  python tools/build_apps.py --only SYNTH.AXE DX7.AXE
  python tools/build_apps.py --warnings      # add -Wall -Wextra

Exit code is non-zero if any image fails to build, which is the point.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tools", "apps.json")
MKAXE = os.path.join(ROOT, "tools", "mkaxe.py")

# Which compiler belongs to which chip.
#
# This is not a preference.  The two Xtensa cores are not the same instruction
# set: the S3 is an LX7 and the original ESP32 an LX6, and GCC for the S3 emits
# LX7 instructions the older core does not have - SALTU, for one, which is what
# it lowers a plain `a < b` value into.  Such an image loads, relocates, runs,
# and dies the moment it reaches that instruction, with EXCCAUSE 0 and a PC in
# the middle of what disassembles - under the ESP32's own objdump, which does
# not know the opcode - as something from the floating point unit.
#
# Nothing about that says "wrong compiler", which is why it cost a day.  So the
# compiler is chosen from the target the firmware is built for, and printed.
#
# The instruction set comes with it, because on the C6 they are not the same
# instruction set at all: an image built for one and loaded on the other is
# refused by the loader (the arch is in the .AXE header), which is at least an
# honest failure - unlike the LX7-on-LX6 case above, which runs until it does
# not.
TOOLCHAIN_FOR_TARGET = {
    "esp32":   ("xtensa", "xtensa-esp32-elf-gcc"),
    "esp32s3": ("xtensa", "xtensa-esp32s3-elf-gcc"),
    "esp32c6": ("riscv32", "riscv32-esp-elf-gcc"),
}


def target_from_sdkconfig():
    """The chip the current build is for, or None if there is no build yet."""
    try:
        with open(os.path.join(ROOT, "sdkconfig"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("CONFIG_IDF_TARGET="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return None


def load_manifest():
    with open(MANIFEST, encoding="utf-8") as f:
        manifest = json.load(f)

    # A JSON file has no comments, so the manifest carries them as objects -
    # at the top level, and between entries in the list where one section of
    # it needs a word of explanation.  Those are not images and have no "out";
    # dropping them here is what lets them be written at all.  Without this the
    # build dies on the first one with KeyError: 'out', which is how the merge
    # of the Fallout branch into main first failed on 23 September 2026.
    manifest["apps"] = [a for a in manifest["apps"] if "out" in a]
    return manifest


def toolchain_for(app, defaults, target_pair, forced_gcc):
    """Which (arch, gcc) builds this image.

    Three sources, in this order, and the order is the whole point:

      --gcc on the command line   wins over everything, and says so in its help.
      the entry's own arch/gcc    an entry that names a compiler means it.  This
                                  is how ST7789.SYS gets built for RISC-V while
                                  the rest of the manifest is Xtensa: they are
                                  not alternatives, they are different machines.
      the target's pair           for every entry that named neither, the chip
                                  `argon target` last configured decides.

    Before the middle rule existed, the target's compiler overrode the entry's
    and a RISC-V image was handed to an Xtensa GCC, which failed on -mcmodel.
    """
    named = "arch" in app or "gcc" in app
    if named:
        arch = app.get("arch", defaults["arch"])
        gcc = app.get("gcc", defaults["gcc"])
    elif target_pair is not None:
        arch, gcc = target_pair
    else:
        arch, gcc = defaults["arch"], defaults["gcc"]
    return arch, (forced_gcc or gcc)


def command_for(app, defaults, extra_cflags, target_pair, forced_gcc):
    include = list(defaults.get("include", [])) + list(app.get("include", []))
    cflags = app.get("cflags", defaults.get("cflags", ""))
    if extra_cflags:
        cflags = (cflags + " " + extra_cflags).strip()

    arch, gcc = toolchain_for(app, defaults, target_pair, forced_gcc)
    cmd = [sys.executable, MKAXE, "--arch", arch, "--gcc", gcc]
    for inc in include:
        cmd += ["--include", inc]
    if cflags:
        cmd += ["--cflags", cflags]
    if "cxxflags" in app:
        cmd += ["--cxxflags", app["cxxflags"]]
    # Archives beyond libgcc.  Only the STL images need one so far, and they
    # need it named here rather than guessed: -lstdc++ pulls in code compiled
    # for a hosted system, and which image is willing to carry that is a
    # decision, not a default.
    for lib in app.get("libs", []):
        cmd += ["--libs", lib]
    if app.get("split_hot"):
        # The image is built in three parts, with the functions marked AG_HOT
        # in one the loader puts in the code arena and everything else in one
        # it executes from flash.  Worth it only for an image that does not fit
        # the arena whole; mkaxe prints both sizes and the arena check below
        # measures the hot half, which is the half that has to fit.
        cmd += ["--split-hot"]
        for src in app.get("hot_src", []):
            cmd += ["--hot-src", src]
    if app.get("icon"):
        # A program's own face, carried in its file (mkaxe --icon).  Kept in
        # apps.json beside its sources because that is where everything else
        # about how this image is built already lives.
        cmd += ["--icon", app["icon"]]
    cmd += ["-o", "build/apps/" + app["out"]]
    cmd += app["src"]
    return cmd


def arena_bytes():
    """How much code arena this target has, from the configured sdkconfig.

    The loader puts an .AXE in the arena when it fits and relocates it into
    flash when it does not, and the difference was measured on the ESP32-A1S
    at a factor of two - 168% of an audio block against 79%.  It says so in one
    line at load time, and that line is easy not to read: an application that
    grows past the arena keeps working and quietly stops being fast.

    So an image that declares `"arena": true` in the manifest is checked here
    instead, where somebody is already looking.

    What this cannot know: the arena is shared.  A driver loaded before the
    application takes its share first, which is why the A1S sends drivers to
    flash (`[modules] xip`).  This is a guard against the plain case, not a
    model of the machine.
    """
    cfg = os.path.join(ROOT, "sdkconfig")
    if not os.path.exists(cfg):
        return 0
    with open(cfg, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for line in text.split("\n"):
        if line.startswith("CONFIG_ARGON_APP_ARENA_KB="):
            try:
                return int(line.split("=", 1)[1]) * 1024
            except ValueError:
                return 0
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--group", default="core",
                    help="core (default), big, or all")
    ap.add_argument("--only", nargs="*", default=None,
                    help="build just these output names")
    ap.add_argument("--warnings", action="store_true",
                    help="append -Wall -Wextra to every image")
    ap.add_argument("--list", action="store_true", help="print the list and stop")
    ap.add_argument("--gcc", default=None,
                    help="compiler for every image, overriding both the target "
                         "and tools/apps.json.  Rarely wanted: the default now "
                         "follows CONFIG_IDF_TARGET, and building for the wrong "
                         "core is an illegal instruction at run time, not a "
                         "refusal at build time (see TOOLCHAIN_FOR_TARGET)")
    args = ap.parse_args()

    manifest = load_manifest()
    defaults = manifest["defaults"]
    apps = manifest["apps"]

    # The manifest names a compiler, but the chip decides for every entry that
    # did not name one itself.  The chip is whatever `argon target` last
    # configured; an explicit --gcc still wins over both.
    target = target_from_sdkconfig()
    target_pair = TOOLCHAIN_FOR_TARGET.get(target)
    if target_pair is None and target is not None and args.gcc is None:
        print(f"build_apps: unknown target '{target}', using the manifest's "
              f"{defaults['gcc']}")

    if args.only:
        wanted = {name.upper() for name in args.only}
        apps = [a for a in apps if a["out"].upper() in wanted]
        missing = wanted - {a["out"].upper() for a in apps}
        if missing:
            print("no such image in tools/apps.json: " + ", ".join(sorted(missing)),
                  file=sys.stderr)
            return 2
    elif args.group != "all":
        apps = [a for a in apps if a.get("group", "core") == args.group]

    if not apps:
        print("nothing to build", file=sys.stderr)
        return 2

    if args.list:
        for a in apps:
            print(f"{a['out']:16s} {a.get('group', 'core'):5s} {a['src'][0]}")
        return 0

    os.makedirs(os.path.join(ROOT, "build", "apps"), exist_ok=True)

    extra = "-Wall -Wextra" if args.warnings else ""
    failed = []
    over_arena = []
    started = time.time()
    for i, app in enumerate(apps, 1):
        name = app["out"]
        print(f"[{i}/{len(apps)}] {name}", flush=True)

        # Sources some entries generate rather than keep by hand: PHONE.SYS
        # carries its client page as a C array made from the .html it is edited
        # as.  Run here rather than by hand because a generated file that can be
        # out of date is a generated file that eventually is - and the symptom
        # would be a board serving last week's page with this week's protocol.
        gen_failed = False
        for gen in app.get("generate", []):
            g = subprocess.run([sys.executable, os.path.join(ROOT, gen)],
                               cwd=ROOT)
            if g.returncode != 0:
                print(f"{name}: {gen} failed", file=sys.stderr)
                gen_failed = True
                break
        if gen_failed:
            failed.append(name)
            continue

        proc = subprocess.run(
            command_for(app, defaults, extra, target_pair, args.gcc), cwd=ROOT,
            capture_output=True, text=True)
        if proc.stdout:
            sys.stdout.write(proc.stdout)
        if proc.stderr:
            sys.stderr.write(proc.stderr)
        if proc.returncode != 0:
            failed.append(name)
            continue

        if app.get("arena"):
            # The size mkaxe just printed, rather than one read back out of the
            # header: the two would have to agree, and only one of them is what
            # the tool actually believes.
            m = re.search(r"code (\d+) bytes", proc.stdout or "")
            arena = arena_bytes()
            size = int(m.group(1)) if m else 0
            if arena > 0 and size > 0:
                if size > arena:
                    print(f"{name}: {size} bytes of code will not fit the "
                          f"{arena}-byte arena - it will run from flash, "
                          f"which on the A1S was twice as slow",
                          file=sys.stderr)
                    over_arena.append(name)
                elif size > arena - 2048:
                    print(f"{name}: {arena - size} bytes of arena left "
                          f"({size} of {arena})")

    took = time.time() - started
    if over_arena and args.warnings:
        print(f"\napps: {', '.join(over_arena)} outgrew the code arena",
              file=sys.stderr)
        return 1
    if failed:
        print(f"\napps: {len(failed)} of {len(apps)} FAILED "
              f"({', '.join(failed)}) for {target or 'no target'} "
              f"in {took:.0f}s", file=sys.stderr)
        return 1
    print(f"\napps: {len(apps)} images built for {target or 'no target'} "
          f"in {took:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
