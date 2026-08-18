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
                    help="compiler for every image, overriding tools/apps.json "
                         "(xtensa-esp32-elf-gcc for the original ESP32; the "
                         "manifest names the S3 one, and an S3 image loaded on "
                         "an ESP32 is a fault at the entry point, not a refusal)")
    args = ap.parse_args()

    manifest = load_manifest()
    defaults = manifest["defaults"]
    apps = manifest["apps"]

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
        proc = subprocess.run(command_for(app, defaults, extra, args.gcc),
                              cwd=ROOT)
        if proc.returncode != 0:
            failed.append(name)

    took = time.time() - started
    if failed:
        print(f"\napps: {len(failed)} of {len(apps)} FAILED "
              f"({', '.join(failed)}) in {took:.0f}s", file=sys.stderr)
        return 1
    print(f"\napps: {len(apps)} images built in {took:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
