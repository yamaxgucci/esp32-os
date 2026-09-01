# Licensing

ArgonOS uses a split license so the kernel stays free software while
applications and the public SDK stay easy to adopt.

| Tree | License | File |
|---|---|---|
| Kernel (`components/argon_kernel/`, `main/`) and host-tests | **GPL-3.0-or-later** | [LICENSE](LICENSE) |
| Public SDK (`sdk/`), build tools (`tools/`), sample/apps under `apps/` (unless a subdirectory says otherwise) | **Apache-2.0** | [LICENSE.Apache-2.0](LICENSE.Apache-2.0), also [sdk/LICENSE](sdk/LICENSE) |
| The valve amplifier emulator (see the list below) | **PolyForm-Noncommercial-1.0.0** | [LICENSE.PolyForm-Noncommercial-1.0.0](LICENSE.PolyForm-Noncommercial-1.0.0) |
| Vendored code (`third_party/`, some `apps/*/core`) | Their own licenses | Keep the local `LICENSE` / notices |

GitHub’s repository license field should be set to **GPL-3.0** (covers the kernel,
the primary work distributed as the OS image).

## The amplifier emulator is noncommercial

Everything that models the amplifier is under the **PolyForm Noncommercial
License 1.0.0**: use it, modify it, share it, for any purpose that is not
commercial.  Selling it, or selling something built on it, needs a separate
licence from the copyright holder.

    apps/common/tube/         the valve model, the voicing banks, the tone stack
    apps/common/ckt/          the circuit solver: MNA, Newton, the device models
    apps/cktbench/            the bench that measures the solver
    apps/tubebench/           the bench that measures the chain on the chip
    tools/tube_render.c       the tone matching: the walk, the objective, the report
    tools/tube_live.c         the same chain with its knobs, live
    host-tests/test_tube.c    the tests that hold both of those to their numbers
    host-tests/test_ckt.c

Each source file says so in its `SPDX-License-Identifier`, and the two
directories carry a `LICENSE` note beside the code.

### What is deliberately NOT in that list

`tools/nam.c`, `tools/nam.h` and `tools/nam_run.c` stay **Apache-2.0**.  They are
a port of Neural Amp Modeler and of nlpodyssey/waveny, both Apache-2.0, and other
people's work cannot be put under a noncommercial licence by whoever ports it.

`apps/common/ir/` stays Apache-2.0 as well.  It is a partitioned FFT convolution
with no knowledge of amplifiers - `irfx` uses it for reverb - and the fact that
the cabinet happens to be its largest user is not a reason to restrict it.
`apps/common/ckt/ag_mathf.c` is the exception in the other direction: it is a
plain exp/log/pow/sqrt without libm, but it was written for the solver, lives
with it and is only used by it, so it travels with the directory.

`apps/amp/` is a Winamp-style media player and has nothing to do with valves,
whatever its name suggests.

### What this does and does not restrict

A licence grants rights to other people; it does not bind the copyright holder.
The author keeps every right in this code and may sell it, license it
commercially to anyone, or release it under different terms later.  What the
noncommercial licence does is stop **other people** selling it while leaving them
free to use and modify it for themselves.

The emulator is not compiled into the firmware image.  It is app-side code -
nothing under `main/` or `components/` includes it - so the GPL-3.0 image and the
noncommercial emulator are never linked into one distributed binary.  The one
place they do meet is `argon_tests`, which links the kernel and every test
together; that binary is built locally and never shipped, and building it for
yourself is what both licences permit.

## Syscall / ABI boundary

The public ABI in `sdk/include/argon/` (headers such as `abi.h`, `argon.h`,
and the thin `libargon` wrappers) is Apache-2.0.

An application or loadable driver that only:

- includes those headers,
- calls the published syscall / API table,
- and does not copy GPL kernel source into itself,

is **not** considered a derivative work of the kernel for licensing purposes.
Such programs may use any license, including proprietary ones.

Linking a program into the same binary as the kernel, or copying GPL kernel
code into an application, does make that combined work subject to the GPL.

## Why GPL-3.0 (not GPL-2.0)

The firmware builds on **ESP-IDF**, which is Apache-2.0. Apache-2.0 is
compatible with GPL-3.0 and not with GPL-2.0 alone.

## Ported code, and where it came from

`tools/nam.c`, `tools/nam.h` and `tools/nam_run.c` play the NAM/TONE3000 amplifier
captures the valve model is measured against. The algorithm, the parameter layout
and the buffering scheme are those of **Neural Amp Modeler** and of
**nlpodyssey/waveny** (with the newer-schema additions from a local fork of the
latter), both **Apache-2.0**; the C here is a port rather than a copy, written
loop-for-loop so that a render can be checked against the original bit for bit.
Kept under `tools/`, so it is Apache-2.0 like the rest of that directory, and the
files say so in their headers.

The capture files themselves (`assets/**/*.nam`) are third-party work by the people
who made them and are **not** in the repository - `.gitignore` excludes them.

## SPDX

Source files carry `SPDX-License-Identifier` headers matching the table above.
