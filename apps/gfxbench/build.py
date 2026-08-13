#!/usr/bin/env python3
"""Build GFXBENCH.AXE (native), LVGLBENCH.AXE (widgets), LVGLPLAIN.AXE (rects)."""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LVGL = os.path.join(ROOT, "third_party", "lvgl")
LVGL_TAG = "v9.3.0"
LVGL_URL = "https://github.com/lvgl/lvgl.git"
MKAXE = os.path.join(ROOT, "tools", "mkaxe.py")

KEEP_PREFIX = (
    "src/lv_init.c",
    "src/core/",
    "src/display/",
    "src/draw/lv_draw",
    "src/draw/lv_image_decoder.c",
    "src/draw/sw/",
    "src/font/lv_font.c",
    "src/font/lv_font_fmt_txt.c",
    "src/font/lv_font_montserrat_14.c",
    "src/indev/",
    "src/layouts/lv_layout.c",
    "src/layouts/flex/",
    "src/libs/bin_decoder/",
    "src/misc/",
    "src/osal/lv_os.c",
    "src/osal/lv_os_none.c",
    "src/stdlib/lv_mem.c",
    "src/stdlib/builtin/lv_string_builtin.c",
    "src/stdlib/builtin/lv_sprintf_builtin.c",
    "src/stdlib/clib/lv_mem_core_clib.c",
    "src/themes/lv_theme.c",
    "src/themes/default/",
    "src/tick/",
    "src/widgets/bar/",
    "src/widgets/button/",
    "src/widgets/label/",
    "src/widgets/slider/",
)

KEEP_PREFIX_PLAIN = (
    "src/lv_init.c",
    "src/core/",
    "src/display/",
    "src/draw/lv_draw",
    "src/draw/lv_image_decoder.c",
    "src/draw/sw/",
    "src/font/lv_font.c",
    "src/font/lv_font_fmt_txt.c",
    "src/font/lv_font_montserrat_14.c",
    "src/indev/",
    "src/layouts/lv_layout.c",
    "src/libs/bin_decoder/",
    "src/misc/",
    "src/osal/lv_os.c",
    "src/osal/lv_os_none.c",
    "src/stdlib/lv_mem.c",
    "src/stdlib/builtin/lv_string_builtin.c",
    "src/stdlib/builtin/lv_sprintf_builtin.c",
    "src/stdlib/clib/lv_mem_core_clib.c",
    "src/themes/lv_theme.c",
    "src/tick/",
    "src/widgets/label/",
)

SKIP_SUB = (
    "src/core/lv_obj_id_builtin.c",
    "src/core/lv_obj_property.c",
    "src/draw/lv_draw_3d.c",
    "src/draw/lv_draw_vector.c",
    "src/draw/sw/lv_draw_sw_vector.c",
    "src/indev/lv_indev_gesture.c",
    "src/misc/lv_bidi.c",
    "src/misc/lv_matrix.c",
    "src/misc/lv_text_ap.c",
    "src/misc/lv_profiler_builtin.c",
    "src/misc/lv_templ.c",
)


def rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace("\\", "/")


def ensure_lvgl() -> None:
    hdr = os.path.join(LVGL, "lvgl.h")
    if os.path.isfile(hdr):
        return
    os.makedirs(os.path.dirname(LVGL), exist_ok=True)
    subprocess.check_call(
        ["git", "clone", "--depth", "1", "--branch", LVGL_TAG, LVGL_URL, LVGL]
    )


def lvgl_sources(keep: tuple[str, ...]) -> list[str]:
    out = []
    src_root = os.path.join(LVGL, "src")
    for dirpath, _, files in os.walk(src_root):
        for name in files:
            if not name.endswith(".c"):
                continue
            full = os.path.join(dirpath, name)
            r = rel(full)
            inner = r[len("third_party/lvgl/") :]
            inner = inner.replace("\\", "/")
            if any(inner == s or inner.startswith(s) for s in SKIP_SUB):
                continue
            if not any(inner == p or inner.startswith(p) for p in keep):
                continue
            out.append(r)
    out.sort()
    return out


def run_mkaxe(args: list[str]) -> None:
    cmd = [sys.executable, MKAXE] + args
    print("+", " ".join(cmd[:8]), "...", flush=True)
    subprocess.check_call(cmd, cwd=ROOT)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc", default="xtensa-esp32s3-elf-gcc")
    ap.add_argument("--arch", default="xtensa")
    ap.add_argument("--native-only", action="store_true")
    ap.add_argument("--lvgl-only", action="store_true")
    ap.add_argument("--plain-only", action="store_true")
    args = ap.parse_args()

    base_cflags = (
        "-Os -ffunction-sections -fdata-sections -fno-builtin "
        "-Wno-unused -Wno-sign-compare -Wno-missing-prototypes"
    )
    common = [
        "--arch", args.arch, "--gcc", args.gcc,
        "--include", "sdk/include",
        "--include", "apps/common/libc",
        "--include", "apps/gfxbench",
    ]

    if not args.lvgl_only and not args.plain_only:
        run_mkaxe(common + [
            "--cflags", base_cflags,
            "-o", "build/apps/GFXBENCH.AXE",
            "apps/gfxbench/gfxbench.c",
            "apps/gfxbench/native_draw.c",
            "apps/common/libc/libc_shim.c",
        ])

    if not args.native_only and not args.plain_only:
        ensure_lvgl()
        srcs = lvgl_sources(KEEP_PREFIX)
        if not srcs:
            raise SystemExit("no LVGL sources matched; is third_party/lvgl present?")
        print("lvgl widgets: %d source files" % len(srcs), flush=True)
        run_mkaxe(common + [
            "--cflags",
            base_cflags + " -DGFXBENCH_LVGL -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE",
            "--include", "third_party/lvgl",
            "-o", "build/apps/LVGLBENCH.AXE",
            "apps/gfxbench/gfxbench.c",
            "apps/gfxbench/lvgl_draw.c",
            "apps/common/libc/libc_shim.c",
        ] + srcs)

    if not args.native_only and not args.lvgl_only:
        ensure_lvgl()
        srcs = lvgl_sources(KEEP_PREFIX_PLAIN)
        if not srcs:
            raise SystemExit("no LVGL plain sources matched")
        print("lvgl plain: %d source files" % len(srcs), flush=True)
        run_mkaxe([
            "--arch", args.arch, "--gcc", args.gcc,
            "--include", "sdk/include",
            "--include", "apps/common/libc",
            "--include", "apps/gfxbench/plain",
            "--include", "apps/gfxbench",
            "--cflags",
            base_cflags
            + " -DGFXBENCH_LVGL -DGFXBENCH_LVGL_PLAIN -DLV_CONF_INCLUDE_SIMPLE"
            + " -DLV_KCONFIG_IGNORE",
            "--include", "third_party/lvgl",
            "-o", "build/apps/LVGLPLAIN.AXE",
            "apps/gfxbench/gfxbench.c",
            "apps/gfxbench/lvgl_plain.c",
            "apps/common/libc/libc_shim.c",
        ] + srcs)

    cfg_src = os.path.join(ROOT, "apps", "gfxbench", "arena192.cfg")
    share = os.path.join(ROOT, "build", "sd_card")
    os.makedirs(share, exist_ok=True)
    shutil.copy2(cfg_src, os.path.join(share, "arena192.cfg"))
    print("staged -> build/sd_card/arena192.cfg", flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
