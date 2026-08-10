# Host share — files from Windows into the guest

## Canonical folders

| Folder | Role |
|--------|------|
| [`build/apps/`](../../build/apps) | Archive of every built `.AXE` / `.SYS` |
| [`build/sd_card/`](../../build/sd_card) | **The** share: HostFS root and `argon sync` input |

`tools/mkaxe.py` writes your `-o` path, then **always** copies the image into
both folders (unless `--no-stage`). Prefer:

```
… -o build/apps/SMS.AXE …
```

Then use **only** `build\sd_card` with QEMU:

```bat
argon run -Gfx -HostFs build\sd_card
argon sync build\sd_card
argon run -Share build\sd_card
```

Do not invent per-app shares (`sms_share`, `cc_share`, …).

---

## 1. Sync — snapshot on `A:` (done)

Pack a folder into `build\sdcard.img` (FAT16). The guest mounts it as `A:`.
Changes on Windows appear only after another sync.

```bat
argon sync build\sd_card
argon run -Sd
```

Or in one step: `argon run -Share build\sd_card`.

Pull a file back out:

```bat
argon get shot.ppm
```

### Live graphics + pad (SMS / MD)

Need **HostFS** with `--pad-cfg` (`argon run -HostFs`): host pushes pad state
~60 Hz.

```bat
argon run -Gfx -HostFs build\sd_card
```

Put `SMS.AXE` / `MD.AXE`, ROMs, and optional `sms.cfg` in `build\sd_card`. Guest:

```
H:\> run sms.axe game.sms
```

Expect `sms: controls = live pad (inp / HostFS PADPUSH)` (same idea for MD).

### Notes

* Changed files on Windows → `argon sync` again (or use HostFS for live `H:`).
* Default image **64 MiB**, FAT16, VFAT.
* On hardware, use a real microSD.

Implementation: `tools/mkfatimg.py`, `tools/fatget.py`.

---

## 2. HostFS — live folder on `H:` (done for QEMU)

**HostFS** is a VFS backend on UART1 talking to `tools/hostfsd.py`. Guest drive
`H:` maps to `/host`. Drop a file on Windows → `dir` on `H:` sees it.

```bat
argon run -Gfx -HostFs build\sd_card
```

Optional together with a packed card (same folder for sync):

```bat
argon run -Gfx -Share build\sd_card -HostFs build\sd_card
```

Guest:

```
H:\> dir
H:\> run sms.axe sonic.sms
```

### How it works

| Side | Role |
|------|------|
| `hostfsd.py` | TCP server on `127.0.0.1:5557`, serves `--root` |
| QEMU 2nd `-serial` | UART1 as TCP client to that port |
| Guest `hostfs.c` | RPC over UART1, mount `/host` (read + file write) |

Console stays on the first serial. With `--pad-cfg`, hostfsd **pushes** live pad
snapshots. Large WAV copies on `H:` while PADPUSH is live can be flaky — prefer
`A:` + `argon get` for big captures (backlog in [`04-roadmap.md`](../04-roadmap.md)).

### Sync vs HostFS

| | Sync (`A:`) | HostFS (`H:`) |
|---|---|---|
| When changes appear | after `argon sync` | immediately |
| Where code lives | host tools only | kernel + `hostfsd.py` |
| On a real board | use a real SD | needs another transport later |

### Why not 9p/virtfs

QEMU `esp32s3` has no virtio-9p. HostFS uses the second UART instead.
