# Host share — files from Windows into the guest

Two ways to see a Windows folder from ArgonOS in QEMU.

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

### Живая картинка в QEMU (играть SMS)

Нужен **HostFS** с `--pad-cfg` (так делает `argon run -HostFs`): хост **пушит**
состояние клавиш ~60 Гц, гость читает кэш — диагонали и «идти + стрелять»
без лагов UART.

```bat
argon run -Gfx -HostFs build\sms_share
```

В папку шары положи `SMS.AXE`, ROM и при желании свой `sms.cfg`. В госте:

```
H:\> run sms.axe game.sms
```

Жди `sms: controls = live pad (inp / HostFS PADPUSH)` (или то же у `MD.AXE`).
Фокус можно на SDL — хост ловит клавиши низкоуровневым хуком.

### Важно

* Меняли файлы на Windows → снова `argon sync` (или `run -Share`).
* Образ по умолчанию **64 МиБ**, FAT16, VFAT.
* FatFs патч: тип тома из BPB, не только из числа кластеров.
* На плате нужна настоящая microSD.

Реализация: `tools/mkfatimg.py`, `tools/fatget.py`.

---

## 2. HostFS — live folder on `H:` (done for QEMU)

**HostFS** is a VFS backend on UART1 talking to `tools/hostfsd.py`. The guest
drive `H:` maps to `/host`. No image rebuild: drop a file on Windows, `dir` on
`H:` sees it.

```bat
argon run -Gfx -HostFs D:\roms
```

Optional together with a packed card:

```bat
argon run -Gfx -Share build\sd_card -HostFs D:\roms
```

Guest:

```
A:\> h:
H:\> dir
H:\> run a:\sms.axe sonic.sms
```

(`sonic.sms` lives under `D:\roms` on the host.)

### How it works

| Side | Role |
|------|------|
| `hostfsd.py` | TCP server on `127.0.0.1:5557`, serves `--root` |
| QEMU 2nd `-serial` | UART1 as TCP client to that port |
| Guest `hostfs.c` | RPC over UART1, mount `/host` (read + file write) |

Console stays on the first serial (`mon:stdio` / `-Tcp`). HostFS never shares
the console byte stream.

Supported: `dir`, `cd`, `type`, `run`, `copy` onto `H:` (create/overwrite via
`OPEN` + `HSFS_OP_WRITE`), `del` (`HSFS_OP_UNLINK`). Not yet: mkdir, rename.
With `--pad-cfg`, hostfsd **pushes** live pad snapshots (`HSFS_OP_PADPUSH`,
6 bytes: pad0, pad1, sys, pad0hi, pad1hi, ver). The guest input layer caches
them for `inp->pad` / `btn` / `btnp` and `/dev/joy0`. `H:\sms.pad` is the same
cache as a read-only compatibility file. Bindings come from `sms.cfg`
(`pad0.c`, `pad0.start`, …).

Small files: `copy` onto `H:` and `del` work. **Large files** (e.g. SMS WAV,
hundreds of KB) are still unreliable on `H:` while PADPUSH is live — use the
SD image instead:

```
run h:\SMS.AXE … a:\sms.wav
```

Then on the host: `argon get a:\sms.wav build\sms.wav`. Fixing large `H:`
copies is backlog item 9 in [`04-roadmap.md`](../04-roadmap.md).

Without `-HostFs`, boot tries a short UART1 ping and skips `H:` if nobody answers.
Guest firmware and `hostfsd.py` must match for writes (old helper opens read-only).

### Sync vs HostFS

| | Sync (`A:`) | HostFS (`H:`) |
|---|---|---|
| When changes appear | after `argon sync` | immediately |
| Where code lives | host tools only | kernel + `hostfsd.py` |
| On a real board | use a real SD | needs another transport later |
| Complexity | low | medium |

### Why not 9p/virtfs

QEMU `esp32s3` has no virtio-9p. HostFS uses the second UART instead.
