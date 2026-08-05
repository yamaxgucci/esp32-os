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
    [switch]$NoBuild
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

if ($Tcp) {
    $qemuArgs += @('-display', 'none', '-monitor', 'none',
                   '-serial', "tcp:127.0.0.1:$Port,server=on,wait=off")
    Write-Host "ArgonOS console on 127.0.0.1:$Port (raw TCP)."
    Write-Host 'Connect with PuTTY in Raw mode, or any terminal program.'
    Write-Host 'Press Ctrl+C here to stop the emulator.'
} else {
    $qemuArgs += @('-nographic', '-serial', 'mon:stdio')
    Write-Host 'ArgonOS console attached to this window.  Ctrl+A then X quits.'
}

& $qemu @qemuArgs
