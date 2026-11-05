# MD.AXE for a board, the way docs/user/08-mega-drive.md tells it.  From PowerShell,
# in the repository's root:
#
#   .\apps\md\build.ps1 -Chip esp32      # the CYD and other ESP32 boards
#   .\apps\md\build.ps1 -Chip esp32s3    # an ESP32-S3 with PSRAM (the S3-CAM)
#
# Both are the CYD's build (ARGON_MD_SMALL: the picture drawn by the second core,
# the Z80 and the sound chips there too, the cartridge mapped out of flash); the
# S3's adds MD_PSRAM_PAGES, so what internal memory cannot give comes from PSRAM.
# The image is build\md\<chip>\MD.AXE.
param([ValidateSet('esp32', 'esp32s3')][string]$Chip = 'esp32',
      [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
. .\tools\idf-env.ps1 | Out-Null

$gcc = @{ esp32 = 'xtensa-esp32-elf-gcc'; esp32s3 = 'xtensa-esp32s3-elf-gcc' }[$Chip]
$extra = @{ esp32 = ''; esp32s3 = '-DMD_PSRAM_PAGES' }[$Chip]
if (-not $Out) { $Out = "build\md\$Chip\MD.AXE" }
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null

$include = 'sdk/include', 'apps/common', 'apps/common/libc', 'apps/md', 'apps/md/port',
           'apps/md/core/bus', 'apps/md/core/cpus/M68K', 'apps/md/core/vdp', 'apps/md/core/io',
           'apps/md/core/savestate', 'apps/md/core/sound', 'apps/sms/core/z80',
           'apps/sms/core/sound', 'apps/common/fm', 'apps/common/dsp' |
           ForEach-Object { '--include', $_ }
# the Z80's opcode tables, its interpreter's and the font stay in flash with the code
$rodata = '.rodata.op_cb', '.rodata.op_dd', '.rodata.op_ed', '.rodata.op_fd', '.rodata.dd_cb',
          '.rodata.fd_cb', '.rodata.sms_z80_execute', '.rodata.k_font8x16', '.rodata.k_city' |
          ForEach-Object { '--code-rodata', $_ }
$src = 'apps/md/md_main.c', 'apps/md/port/md_z80.c', 'apps/md/port/md_ym_native.c',
       'apps/md/core/sound/ym2612.c', 'apps/md/port/md_psg.c', 'apps/md/port/md_sound_out.c',
       'apps/md/port/md_snd_async.c', 'apps/md/port/md_raster.c', 'apps/md/port/md_savestate.c',
       'apps/common/libc/libc_shim.c', 'apps/sms/core/z80/z80.c', 'apps/sms/core/sound/sn76489.c',
       'apps/md/core/bus/gwenesis_bus.c', 'apps/md/core/cpus/M68K/m68kcpu.c',
       'apps/md/core/vdp/gwenesis_vdp_gfx.c', 'apps/md/core/vdp/gwenesis_vdp_mem.c',
       'apps/md/core/io/gwenesis_io.c', 'apps/md/core/savestate/gwenesis_savestate.c'
$cflags = "-Os -DMD_SPLIT $extra -ffunction-sections -fdata-sections -fno-builtin " +
          '-include md_cfg.h -DARGON_MD_Z80 -DARGON_MD_SMALL -DARGON_SMS_SMALL -DLSB_FIRST ' +
          '-Wno-unused -Wno-sign-compare'

& python tools/mkaxe.py --arch xtensa --gcc $gcc --no-stage @include --cflags $cflags `
    @rodata --libs c --split-hot -o $Out @src
if ($LASTEXITCODE -ne 0) { throw "mkaxe failed ($LASTEXITCODE)" }
"MD for $Chip -> $Out"
