# ArgonOS - build (and optionally flash) the whole USB-KVM dongle in one go:
# the Windows agent, KVM.AXE, the panel driver, the C: image and the flash-drive
# image.  The board firmware itself is separate and rarely changes - build it
# with `argon target esp32s3-kvm; argon build; argon flash -port COMn`.
#
#   tools\kvm-pack.ps1                         build the images into build\
#   tools\kvm-pack.ps1 -Flash -Port COM4       build, then write C: and the drive
#   tools\kvm-pack.ps1 -Board boards\esp32s3-dongle -Flash -Port COM17
#                                              the LilyGo T-Dongle-S3 instead of
#                                              the S3-CAM: its own board pack and
#                                              its own panel driver (read from
#                                              the pack's modules.txt).
#   tools\kvm-pack.ps1 -Flash -Config dev.txt  also bake a KVMCFG.TXT (ssid /
#                                              pass / webpass, three lines) so the
#                                              board boots already configured -
#                                              a development convenience, never a
#                                              shipped image.
#   tools\kvm-pack.ps1 -Flash -Relay relay.txt bake a KVMRLY.TXT (VPS host / port
#                                              / secret, three lines) so the board
#                                              dials a relay and is reachable from
#                                              anywhere.  The secret is confidential
#                                              - keep that file out of git.
param(
    [switch]$Flash,
    [string]$Port = 'COM4',
    [string]$Config,
    [string]$Relay,
    [string]$Board = 'boards\esp32s3-kvm'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$xtensa = 'D:\Espressif\tools\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'
if (Test-Path $xtensa) { $env:PATH = "$xtensa;$env:PATH" }
$py = 'D:\dev\python312\python.exe'
$idfpy = 'D:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
$fatgen = 'D:\Espressif\esp-idf\components\fatfs\wl_fatfsgen.py'
$pack = $Board
if (-not (Test-Path $pack)) { throw "board pack not found: $pack" }

# The panel driver this board loads, read from its own modules.txt (the S3-CAM
# loads ST7789S3.SYS; the T-Dongle-S3 loads ST7735TD.SYS) - one packer, any
# board, no driver name hard-coded here.
$panel = (Get-Content "$pack\modules.txt" | Where-Object { $_ -match '\.SYS' } |
          Select-Object -First 1).Trim()

Write-Host '[1/4] agent (kvmagent.exe)'
cmd /c 'tools\kvmagent\build.cmd'
if ($LASTEXITCODE -ne 0) { throw 'agent build failed' }

Write-Host "[2/4] KVM.AXE + $panel"
& $py tools\build_apps.py --only KVM.AXE
& $py tools\build_apps.py --only $panel

Write-Host '[3/4] C: image (build\sysfs_kvm.bin)'
$addargs = @('--add', 'build\sd_card\KVM.AXE=KVM.AXE')
if ($Config) {
    if (-not (Test-Path $Config)) { throw "config file not found: $Config" }
    $addargs += @('--add', "$Config=KVMCFG.TXT")
}
if ($Relay) {
    if (-not (Test-Path $Relay)) { throw "relay file not found: $Relay" }
    $addargs += @('--add', "$Relay=KVMRLY.TXT")
}
# The generated sdkconfig (not the fragment): it carries CONFIG_IDF_TARGET, which
# mksysfs checks against the board's chip.  It exists once the firmware target is
# set - argon target esp32s3-kvm.
& $py tools\mksysfs.py --board $pack --apps build\sd_card `
    --partitions partitions_kvm.csv --sdkconfig sdkconfig `
    @addargs --out build\sysfs_kvm.bin

Write-Host '[4/4] flash-drive image (build\usbdisk.img) with the agent on it'
New-Item -ItemType Directory -Force build\usbdisk | Out-Null
Copy-Item build\kvmagent.exe build\usbdisk\KVMAGENT.EXE -Force
# The agent prints a "reachable from anywhere" URL when a RELAY.TXT sits beside
# it, holding just the relay's public host (the first line of the -Relay file -
# never the port or the secret, which belong only on the board's C:).
Remove-Item build\usbdisk\RELAY.TXT -ErrorAction SilentlyContinue
if ($Relay) {
    $relayHost = (Get-Content $Relay | Where-Object { $_.Trim() -ne '' } |
                  Select-Object -First 1).Trim()
    if ($relayHost) {
        Set-Content -Path build\usbdisk\RELAY.TXT -Value $relayHost -NoNewline `
            -Encoding ascii
        Write-Host "      RELAY.TXT on the drive -> $relayHost"
    }
}
& $idfpy $fatgen build\usbdisk --output_file build\usbdisk.img `
    --partition_size 0x80000 --sector_size 4096 --long_name_support

if ($Flash) {
    Write-Host "flashing C: (0x310000) and the drive (0x780000) on $Port"
    & $idfpy -m esptool --chip esp32s3 -p $Port -b 921600 `
        --before default_reset --after hard_reset write_flash `
        0x310000 build\sysfs_kvm.bin 0x780000 build\usbdisk.img
}

Write-Host ''
Write-Host 'done:'
Write-Host '  build\sysfs_kvm.bin  -> C: (flash at 0x310000)'
Write-Host '  build\usbdisk.img    -> flash drive (flash at 0x780000)'
Write-Host '  build\kvmagent.exe   -> the Windows agent (also on the drive)'
