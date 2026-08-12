# Licensing

ArgonOS uses a split license so the kernel stays free software while
applications and the public SDK stay easy to adopt.

| Tree | License | File |
|---|---|---|
| Kernel (`components/argon_kernel/`, `main/`) and host-tests | **GPL-3.0-or-later** | [LICENSE](LICENSE) |
| Public SDK (`sdk/`), build tools (`tools/`), sample/apps under `apps/` (unless a subdirectory says otherwise) | **Apache-2.0** | [LICENSE.Apache-2.0](LICENSE.Apache-2.0), also [sdk/LICENSE](sdk/LICENSE) |
| Vendored code (`third_party/`, some `apps/*/core`) | Their own licenses | Keep the local `LICENSE` / notices |

GitHub’s repository license field should be set to **GPL-3.0** (covers the kernel,
the primary work distributed as the OS image).

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

## SPDX

Source files carry `SPDX-License-Identifier` headers matching the table above.
