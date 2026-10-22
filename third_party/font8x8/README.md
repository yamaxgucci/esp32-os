# font8x8 — the glyphs both ArgonOS fonts are made of

`font8x8_basic.h` as published, byte for byte. Its own header says:

    8x8 monochrome bitmap fonts for rendering
    Author: Daniel Hepper <daniel@hepper.net>

    License: Public Domain

    Based on:
    // Author:
    //     Marcel Sondaar
    //     International Business Machines (public domain VGA fonts)
    // License:
    //     Public Domain

Upstream: <https://github.com/dhepper/font8x8>, file `font8x8_basic.h`.

## Why it is in the repository rather than downloaded

`tools/gen_font8x16.py` used to fetch this file from the network on every run
that had no cache, which is two problems: a build that reaches the internet is
a build that can be given something else, and the only evidence for the
public-domain claim in `LICENSING.md` lived outside the tree. Nine kilobytes
settles both.

The generator reads this copy and downloads only if it is missing.

## What is made from it

    components/argon_kernel/src/dev/font8x16.c   every scanline doubled
    apps/common/font8x8.h                        the doubling undone again
    apps/marauder/mrd_font.h                     the printable rows, copied

None of those operations adds authorship, so all of them are public domain
too. See [LICENSING.md](../../LICENSING.md).
