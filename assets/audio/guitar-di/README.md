# Clean guitar DI

Raw guitar signal, straight off the pickup into an interface. No amplifier, no
cabinet, no effects — this is what `apps/common/ckt` is supposed to be fed.

Everything here is 48 kHz mono, matching the audio path, so nothing resamples
on the way in.

## What is here

| File | Source | Length | Format |
| --- | --- | --- | --- |
| `nam-clean-di.wav` | Neural Amp Modeler capture signal | 3:07 | 32-bit float |
| `prvt-clean-di.wav` | ToneTwist AFx, private guitar recording | 5:15 | 32-bit float |
| `idmt-ibanez2820-clean-di.wav` | IDMT-SMT-Guitar dataset 4, Ibanez 2820 | 5:10 | 32-bit float |
| `E2_s1_01.wav`, `F2_s1_01.wav`, `A2_s2_01.wav`, `C2_s1_01.wav`, `E2_s1_soft_01.wav` | FreePats, Fender direct | ~30 s each | 24-bit |

The three long files came out of the ToneTwist AFx dry-input collection, which
is the set of signals that collection uses to drive real amplifiers when it
records its dry/wet pairs. The two sync impulses ToneTwist adds at 0.5 s and
1.5 s from each end are full-scale clicks; they have been trimmed off here.

## Measured noise floor

The number that matters for a distortion stage is not the peak, it is how far
the noise between phrases sits below the played note — a stage with 40 dB of
gain lifts that floor by 40 dB too. Quietest and loudest are 85 ms windows;
hum is measured inside the quietest one.

| File | peak | signal − noise | 50 Hz | 60 Hz |
| --- | --- | --- | --- | --- |
| `nam-clean-di.wav` | −0.29 dBFS | 190 dB (digital silence in the gaps) | — | — |
| `prvt-clean-di.wav` | −0.25 dBFS | 108 dB | −128 dB | −130 dB |
| `idmt-ibanez2820-clean-di.wav` | −0.37 dBFS | 95 dB | −68 dB | −66 dB |
| `E2_s1_01.wav` (FreePats) | 0.00 dBFS | 62 dB | −110 dB | −106 dB |

For comparison, EGFxSet's clean Stratocaster notes measure 28 dB of
signal-to-noise and are audibly hissy on a clean patch. They are not here.

The IDMT Ibanez file carries mains hum at about −67 dBFS. Under heavy drive
that is audible; the other two are not.

Every file is peak-normalised to within a third of a decibel of full scale.
There is no headroom: attenuate before the first gain stage rather than
feeding these in at unity.

## Licences

Not uniform, and not all of them are compatible with this repository's
Apache-2.0:

* **FreePats** (`E2_*`, `F2_*`, `A2_*`, `C2_*`) — CC0 1.0, public domain.
  <https://freepats.zenvoid.org/ElectricGuitar/clean-electric-guitar.html>
* **ToneTwist AFx** (`nam-*`, `prvt-*`, `idmt-*`) — CC BY-NC 4.0.
  **Non-commercial**, attribution required.
  <https://zenodo.org/records/10455730>
* `idmt-ibanez2820-clean-di.wav` additionally derives from IDMT-SMT-Guitar,
  CC BY-NC-ND 4.0. <https://zenodo.org/record/7544110>

The NC terms are the reason these three should stay out of any distributed
build. Keep them as local test material.

## Not in the repository

`.gitignore` excludes `assets/**/*.wav`, `*.nam` and `*.pk` — the recordings,
the cabinet impulses and the NAM captures together are 177 MB, and the licences
above are the other reason. This file is tracked so that the folder documents
itself; the audio is local, and the paths in `docs/08-circuit-simulation.md`
assume it sits here under these names.
