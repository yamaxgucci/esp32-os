# Runs ArgonOS in QEMU and attaches your terminal to its console.
#
#   . .\tools\idf-env.ps1
#   .\tools\qemu-run.ps1
#
# You get the A:\> prompt in this window and can type at it.
# To quit the emulator press Ctrl+A then X.
#
# Use -Tcp to expose the console on a TCP port instead, for connecting with
# PuTTY (Connection type: Raw, 127.0.0.1 port 5556) or any terminal program:
#
#   .\tools\qemu-run.ps1 -Tcp
[CmdletBinding()]
param(
    [switch]$Tcp,
    [int]$Port = 5556,
    [switch]$NoBuild,
    [switch]$Sd,
    [string]$SdImage = 'build\sdcard.img'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

if (-not $NoBuild) {
    & idf.py build | Out-Null
    if (-not $?) { throw 'Build failed.' }
}

$qemu = Resolve-Qemu
Update-FlashImage
$efuse = Initialize-EfuseFile

$qemuArgs = Get-QemuMachineArgs -EfusePath $efuse

if ($Sd) {
    $qemuArgs += Get-QemuSdArgs -Path $SdImage
}

if ($Tcp) {
    # wait=on: the board holds still until you connect, so you see the boot
    # rather than joining after it. A TCP serial port with no peer discards
    # everything it is given.
    $qemuArgs += @('-display', 'none', '-monitor', 'none',
                   '-serial', "tcp:127.0.0.1:$Port,server=on,wait=on")
    Write-Host "ArgonOS console on 127.0.0.1:$Port (raw TCP)."
    Write-Host 'Connect with PuTTY in Raw mode, or any terminal program.'
    Write-Host 'The board waits for your connection before booting.'
    Write-Host 'Press Ctrl+C here to stop the emulator.'
} else {
    $qemuArgs += @('-nographic', '-serial', 'mon:stdio')
    if (-not (Enable-ConsoleVt)) {
        Write-Host 'Warning: this console cannot display the screen properly.'
        Write-Host 'Use Windows Terminal, or run with -Tcp and connect PuTTY.'
    }
    Write-Host 'ArgonOS console attached to this window.  Ctrl+A then X quits.'
}

try {
    & $qemu @qemuArgs
} finally {
    # Leaving echo disabled would make the shell look broken after we exit.
    Restore-ConsoleVt
}
