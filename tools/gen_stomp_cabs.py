#!/usr/bin/env python3
"""Put the cabinet impulses on the card for STOMP's cabinet chooser.

    python tools/gen_stomp_cabs.py
    argon sync build\\sd_card

The chooser browses for `.wav`, and a card with nothing to browse teaches
nothing about whether the chooser works.  So the impulses the fit already
produced are staged under `build/sd_card/stomp/`, one per model, named after the
amplifier they were fitted against.

**`ir_<model>_bank.wav`, not `ir_<model>_fitted.wav`.**  The rule is that the
biquad banks do the tone matching and the impulse is the loudspeaker; the
`_bank` file is the impulse fitted with the matching bank still in the chain, so
it is a cabinet and can be put in front of another amplifier.  The `_fitted`
file is the other split - the bank at zero and the impulse carrying the voicing
as well - which is a fine thing to listen to and a poor thing to call a cabinet.

These are outputs of `tube_render`, so they live under `build/` and are not in
the repository.  When they are missing this says which command makes them
rather than staging silence.
"""
from __future__ import annotations

import hashlib
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "build" / "listen"
DST = ROOT / "build" / "sd_card" / "stomp"

MODELS = ("jcm800", "bogner", "slo", "ts9")


def main() -> int:
    DST.mkdir(parents=True, exist_ok=True)
    staged, missing = [], []
    # Whichever models share an impulse share a file.  The fit overwrites
    # `ir_<model>_bank.wav` for the model it was run on, so a tree where three
    # of them are the same bytes has had the fit run once - and four names over
    # two cabinets is a card that lies about what is on it.
    seen: dict[bytes, str] = {}
    same: dict[str, list[str]] = {}
    for model in MODELS:
        src = SRC / f"ir_{model}_bank.wav"
        if not src.is_file():
            missing.append(model)
            continue
        digest = hashlib.sha256(src.read_bytes()).digest()
        if digest in seen:
            same.setdefault(seen[digest], []).append(model)
            continue
        seen[digest] = model
        dst = DST / f"{model.upper()}.WAV"
        shutil.copyfile(src, dst)
        staged.append(f"{dst.relative_to(ROOT)} ({src.stat().st_size} bytes)")

    for line in staged:
        print("staged", line)
    for owner, sharers in same.items():
        print(
            f"not staged: {', '.join(sharers)} - same bytes as {owner}; "
            "run the fit per model to tell them apart"
        )
    if missing:
        print()
        print("no impulse yet for: " + ", ".join(missing))
        print("`tube_render match capture.nam di.wav` writes")
        print("build/listen/ir_<model>_bank.wav; `polish` and `iter4` then")
        print("overwrite it.  The walk is apps/common/tube/README.md.")
    if not staged:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
