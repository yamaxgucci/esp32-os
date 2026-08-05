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

# Decide where the console goes before building the arguments: a window that
# cannot render the screen is worse than no window, so it is not offered.
if (-not $Tcp) {
    $vtOk = Enable-ConsoleVt
    Write-Host "console: $(Get-ConsoleVtReport)"

    if (-not $vtOk) {
        Write-Host ''
        Write-Host 'This window cannot display the ArgonOS screen, so the'
        Write-Host 'console is being put on a TCP port instead of here.'
        $Tcp = $true
    }
}

if ($Tcp) {
    # wait=on: the board holds still until you connect, so you see the boot
    # rather than joining after it. A TCP serial port with no peer discards
    # everything it is given.
    $qemuArgs += @('-display', 'none', '-monitor', 'none',
                   '-serial', "tcp:127.0.0.1:$Port,server=on,wait=on")
    Write-Host ''
    Write-Host "ArgonOS console on 127.0.0.1:$Port (raw TCP)."
    Write-Host 'Connect with PuTTY: Session, Connection type Raw,'
    Write-Host "  Host Name 127.0.0.1, Port $Port, then Open."
    Write-Host 'The board waits for your connection before booting.'
    Write-Host 'Press Ctrl+C here to stop the emulator.'
} else {
    $qemuArgs += @('-nographic', '-serial', 'mon:stdio')
    Write-Host 'ArgonOS console attached to this window.  Ctrl+A then X quits.'
}

#
# Start-Process -NoNewWindow hands this console's own handles to the child.
# The call operator does not: PowerShell can sit in the middle of a native
# command's output, read it as text and print it again through its own
# rendering, which does not act on escape sequences - so the screen arrives as
# a wall of "<-[2J" even though the console mode is set correctly.
#
function Start-Qemu {
    param([string]$Exe, [string[]]$Arguments)
    return Start-Process -FilePath $Exe -ArgumentList $Arguments `
        -NoNewWindow -PassThru
}

try {
    $proc = Start-Qemu -Exe $qemu -Arguments $qemuArgs
    $proc.WaitForExit()
} finally {
    # Leaving echo disabled would make the shell look broken after we exit.
    Restore-ConsoleVt
}
