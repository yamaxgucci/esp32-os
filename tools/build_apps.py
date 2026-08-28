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
        return json.load(f)


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
    cmd += ["-o", "build/apps/" + app["out"]]
    cmd += app["src"]
    return cmd


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
    started = time.time()
    for i, app in enumerate(apps, 1):
        name = app["out"]
        print(f"[{i}/{len(apps)}] {name}", flush=True)
        proc = subprocess.run(
            command_for(app, defaults, extra, target_pair, args.gcc), cwd=ROOT)
        if proc.returncode != 0:
            failed.append(name)

    took = time.time() - started
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
