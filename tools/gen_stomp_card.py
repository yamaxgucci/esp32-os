#!/usr/bin/env python3
"""Put what STOMP needs on the card: presets, cabinets, and a take to play.

    python tools/gen_stomp_card.py
    argon sync build\\sd_card

Three kinds of file, all under `build/sd_card/stomp/`, all outputs of the fit
and so under `build/` rather than in the repository.  When one is missing this
says which command makes it rather than staging silence.

**Presets** (`<MODEL>.PRESET`) are the whole amplifier as bytes - component
values, voicing, baked curves, and the loudspeaker inside it.  Loading one needs
no circuit solver and no axis fitting, which is the entire point: the host bakes
once and the box reads bytes and plays.  `tube_render preset <out> <drive>
<model>` writes them.

**Cabinets** (`<MODEL>.WAV`) are `ir_<model>_bank.wav`, not
`ir_<model>_fitted.wav`.  The rule is that the biquad banks do the tone matching
and the impulse is the loudspeaker; the `_bank` file is the impulse fitted with
the matching bank still in the chain, so it is a cabinet and can be put in front
of another amplifier.  The `_fitted` file carries the voicing as well, which is
a fine thing to listen to and a poor thing to call a cabinet.  A preset already
carries its own, so these are for the chooser and for putting one amplifier's
speaker on another.

**A take** (`DI.WAV`) is a dry guitar at 22.05 kHz, which is what `play` wants.
STOMP refuses a take at another rate rather than resampling it, because a chain
fitted at one rate playing a take at another sounds wrong in a way that is easy
to mistake for the model being wrong.
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
# In the order they would be reached for: a chord says most about a chain, a
# single note says most about one stage.
TAKES = ("chord_di_22050.wav", "gc1_di_22050.wav", "e2_di_22050.wav")


def stage(src: Path, dst: Path) -> str:
    shutil.copyfile(src, dst)
    return f"{dst.relative_to(ROOT)} ({src.stat().st_size} bytes)"


def main() -> int:
    DST.mkdir(parents=True, exist_ok=True)
    staged: list[str] = []
    notes: list[str] = []

    missing = []
    for model in MODELS:
        src = SRC / f"{model}.preset"
        if src.is_file():
            staged.append(stage(src, DST / f"{model.upper()}.PRESET"))
        else:
            missing.append(model)
    if missing:
        notes.append(
            "no preset for: " + ", ".join(missing) + "\n"
            "  build-host/tube_render preset build/listen/<model>.preset 2.0 <model>"
        )

    # Whichever models share an impulse share a file.  The fit overwrites
    # `ir_<model>_bank.wav` for the model it was run on, so a tree where three
    # of them are the same bytes has had the fit run once - and four names over
    # two cabinets is a card that lies about what is on it.
    seen: dict[bytes, str] = {}
    same: dict[str, list[str]] = {}
    missing = []
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
        staged.append(stage(src, DST / f"{model.upper()}.WAV"))
    for owner, sharers in same.items():
        notes.append(
            f"not staged: {', '.join(sharers)} - the same bytes as {owner}; "
            "run the fit per model to tell them apart"
        )
    if missing:
        notes.append(
            "no impulse for: " + ", ".join(missing) + "\n"
            "  `tube_render match capture.nam di.wav` writes it; `polish` and\n"
            "  `iter4` overwrite it.  The walk is apps/common/tube/README.md."
        )

    for name in TAKES:
        src = SRC / name
        if src.is_file():
            staged.append(stage(src, DST / "DI.WAV"))
            break
    else:
        notes.append(
            "no take at 22.05 kHz: expected one of " + ", ".join(TAKES) + "\n"
            "  in build/listen.  `tube_render` writes them from the dry\n"
            "  material in assets/audio/guitar-di."
        )

    for line in staged:
        print("staged", line)
    if notes:
        print()
        for note in notes:
            print(note)
    return 0 if staged else 1


if __name__ == "__main__":
    sys.exit(main())
