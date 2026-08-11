# PCMVIRT — virtual PCM → Windows

Loadable `.SYS` that publishes `/dev/pcmvirt`. Apps write interleaved s16 PCM;
the driver streams a WAV header + samples over TCP `:5558` to the host
(`tools/pcmplay.py`). No listener → samples are dropped; the app never waits.

Canonical output paths (see [`docs/user/03-host-share.md`](../../docs/user/03-host-share.md)):

- `build/apps/PCMVIRT.SYS` — archive
- `build/sd_card/PCMVIRT.SYS` — HostFS / sync share (`mkaxe` stages both)

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include -o build\apps\PCMVIRT.SYS apps/pcmvirt/pcmvirt.c
```

## Install once (persists across reboot)

In the guest (file must be visible on a drive — use HostFS `H:` / `T:` copy):

```
drv install h:\pcmvirt.sys
```

This stores `C:\DRV\PCMVIRT.SYS` and adds `[modules] device = c:\drv\pcmvirt.sys`
to `C:\SYSTEM.CFG`. Every boot loads it automatically.

## Use from apps

```
run h:\dx7.axe pcmvirt
run h:\md.axe game.bin pcmvirt
run h:\sms.axe game.sms pcmvirt
```

Or cfg key `audio_out = /dev/pcmvirt`. Default mute path is `/dev/pcmnull`.

Host (anytime after the module is loaded; PowerShell, IDF Python):

```powershell
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
& $py tools\pcmplay.py
& $py tools\pcmplay.py --reconnect
```

`.\argon.cmd run` already hostfwd’s `127.0.0.1:5558`.

---

## How to verify (QEMU → Windows speakers)

Commands below are for **PowerShell / Windows Terminal** from the repo root.
`argon` is not on PATH — use `.\argon.cmd`. Use the IDF venv Python (not a
global `python`):

```powershell
cd "D:\Work\Unity Projects\ESP32-OS"
$py = "D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe"
```

### 0. Prerequisites on the host

- Firmware built: `.\argon.cmd build`
- Images staged under `build\sd_card\` at least:
  - `PCMVIRT.SYS`
  - one app that can open `/dev/pcmvirt` — easiest **`DX7.AXE`** (always produces sound when keys/MIDI play); or `MD.AXE` / `SMS.AXE` + a ROM
- Python with `sounddevice` (or use `--ffplay`):

```powershell
& $py -m pip install sounddevice
```

### 1. Start QEMU with the share

```powershell
.\argon.cmd run -Gfx -HostFs build\sd_card
```

Leave this terminal as the guest console. Confirm net comes up (OpenEth / DHCP);
`pcmvirt` needs `api->net` ready before TCP listen succeeds — apps still start
immediately even if net is late (samples drop until listen works).

### 2. Install the driver once

In the guest:

```
dir h:\pcmvirt.sys
drv install h:\pcmvirt.sys
dev pcmvirt
```

Expect something like `installed c:\drv\pcmvirt.sys (autoload on boot)` and
`dev` listing `pcmvirt` / driver `PCMVIRT`.

Optional persistence check: `reboot` in the guest (same QEMU process keeps
`-Gfx` / `-HostFs`; H: may take a moment to return), then `dev pcmvirt`
**without** `drv install` — both the module (`drv`) and the device must
appear. If you only see the `.SYS` on `C:\drv` but no `dev pcmvirt`, re-run
`drv install h:\pcmvirt.sys` once on a build that writes to `/sys/drv/`
(LittleFS is case-sensitive; older installs used `/sys/DRV`).

### 3. Start the host player (any time)

In a **second** host terminal (repo root; set `$py` again):

```powershell
& $py tools\pcmplay.py
```

It may print `waiting for pcmvirt on 127.0.0.1:5558...` until the guest is
listening. That is fine — unlike the old app `net` sink, the guest does **not**
block waiting for you.

Useful:

```powershell
& $py tools\pcmplay.py --reconnect
& $py tools\pcmplay.py --record build\pcm_capture.wav
```

### 4. Run an app that writes to pcmvirt

Guest (DX7 — simplest):

```
run h:\dx7.axe pcmvirt
```

Expect `dx7: sound = /dev/pcmvirt @ 22050 Hz` (no “waiting for host”).
Play keys (Z…M / Q…I) or let MIDI autoload from `h:\dx7\`.

Guest (MD example):

```
run h:\md.axe h:\game.bin pcmvirt
```

### 5. What “pass” looks like

| Check | Pass |
|-------|------|
| App start | Immediate; no “waiting for host” loop |
| Without `pcmplay` | App keeps running; silent |
| With `pcmplay` connected | Host prints `connected: 22050 Hz, 2 ch` and you hear audio |
| Disconnect / reconnect `pcmplay` | With `--reconnect`, audio resumes; app never stalls |
| `audio_out` / CLI `pcmnull` | Silence, no errors, no wait |
| After reboot | `pcmvirt` still in `dev` if `drv install` was done |

### 6. Troubleshooting

| Symptom | Likely cause |
|---------|----------------|
| `dev pcmvirt` missing | Module not loaded — `drv` list; re-`install` |
| `pcmplay` never connects | QEMU not running, wrong port, or net not up in guest |
| Connected but silence | App still on `pcmnull` / `mock`; pass `pcmvirt` |
| Stutter / gaps | Under QEMU the usual cause is guest below realtime (`late`/`resync` in DX7; `pcmplay` `xrealtime` &lt; 1). Not clipping: check peak via `--save`. Ring overflow is `tcp_ov` / log `drop_ov`; short TCP stalls are absorbed by the 32 KiB guest ring. Host live path also prints `underruns` / `ring_drop`. |
| `api->net is NULL` | Firmware built without networking |

Uninstall:

```
drv uninstall PCMVIRT
```
