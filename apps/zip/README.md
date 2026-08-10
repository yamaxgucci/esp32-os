# ZIP — create / update / list / extract archives

Full archiver as a loadable `.AXE`. The shell builtin `unzip` lists and extracts
(store + deflate). Use this app to **create** or **update** archives (store).

## Build

```
python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
  --include sdk/include `
  --cflags "-Os -ffunction-sections -fdata-sections" `
  -o build/apps/ZIP.AXE apps/zip/zip.c
```

No libc shim or miniz link is required for create/list/store-extract.

Copy `ZIP.AXE` onto a disk the guest can see (`H:` HostFS share, `A:`, or `T:`).

## Usage

```
A:\> run zip.axe t:\demo.zip t:\hello.txt
A:\> run zip.axe -l t:\demo.zip
A:\> run zip.axe -u t:\demo.zip t:\notes.txt
A:\> run zip.axe -x t:\demo.zip t:\out
A:\> unzip t:\packed.zip t:\out2      (builtin — also handles deflate)
```

Create/update write **store** (method 0). `-x` extracts store entries; for
deflate members use the builtin `unzip`.
