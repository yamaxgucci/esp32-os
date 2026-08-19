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
GCC_FOR_TARGET = {
    "esp32": "xtensa-esp32-elf-gcc",
    "esp32s3": "xtensa-esp32s3-elf-gcc",
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


def command_for(app, defaults, extra_cflags, gcc=None):
    include = list(defaults.get("include", [])) + list(app.get("include", []))
    cflags = app.get("cflags", defaults.get("cflags", ""))
    if extra_cflags:
        cflags = (cflags + " " + extra_cflags).strip()

    cmd = [sys.executable, MKAXE,
           "--arch", app.get("arch", defaults["arch"]),
           "--gcc", gcc or app.get("gcc", defaults["gcc"])]
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
                         "refusal at build time (see GCC_FOR_TARGET)")
    args = ap.parse_args()

    manifest = load_manifest()
    defaults = manifest["defaults"]
    apps = manifest["apps"]

    # The manifest names a compiler, but the chip decides, and the chip is
    # whatever `argon target` last configured.  An explicit --gcc still wins.
    gcc = args.gcc
    if gcc is None:
        target = target_from_sdkconfig()
        gcc = GCC_FOR_TARGET.get(target)
        if gcc is None and target is not None:
            print(f"build_apps: unknown target '{target}', using the manifest's "
                  f"{defaults['gcc']}")
    if gcc is None:
        gcc = defaults["gcc"]

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
        proc = subprocess.run(command_for(app, defaults, extra, gcc),
                              cwd=ROOT)
        if proc.returncode != 0:
            failed.append(name)

    took = time.time() - started
    if failed:
        print(f"\napps: {len(failed)} of {len(apps)} FAILED "
              f"({', '.join(failed)}) with {gcc} in {took:.0f}s",
              file=sys.stderr)
        return 1
    print(f"\napps: {len(apps)} images built by {gcc} in {took:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
