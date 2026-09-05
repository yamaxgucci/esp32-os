# The remote keyboard, end to end, in the emulator.
#
#   .\argon.cmd kvmtest
#
# Boots the firmware in QEMU, installs USBHIDVIRT.SYS, starts KVM.AXE, and then
# plays both ends against each other: tools\kvmtest.py is the phone on one side
# and the PC at the far end of the cable on the other, and it says whether the
# reports that came out are the reports those events should have produced.
#
# What this cannot check is the last inch - whether a real host agrees these
# bytes are a keyboard - because QEMU models no OTG peripheral at all.  That is
# what plugging the board into a PC is for; see docs\14-usb-kvm.md.
#
# Not part of `argon check`, for the same reason `nettest` is not: it wants
# several ports and a Python that can bind them, and a firewall prompt is not a
# test failure.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$Http = 8080,
    [int]$Hid = 5562,
    [int]$ConsolePort = 5556,
    # Leave the emulator running afterwards, for poking at the page by hand.
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

foreach ($img in @('build\apps\KVM.AXE', 'build\apps\USBHIDVIRT.SYS')) {
    if (-not (Test-Path $img)) {
        Write-Error "$img is missing - run: argon apps --only KVM.AXE USBHIDVIRT.SYS"
    }
}

$python = if ($env:ARGON_PYTHON) { $env:ARGON_PYTHON } else {
    (Get-Command python -ErrorAction Stop).Source
}

# Anything already emulating is somebody else's, and killing it at the end
# would be rude - so remember what was there and only stop what we started.
$before = @(Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id)

Write-Host 'booting the emulator...'
$qemu = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
        ". '$PSScriptRoot\idf-env.ps1'; & '$PSScriptRoot\qemu-run.ps1' -Tcp " +
        "-Port $ConsolePort -NoBuild -HostFs 'build\sd_card'")

try {
    & $python (Join-Path $PSScriptRoot 'kvmtest.py') `
        '--http' $Http '--hid' $Hid '--console' $ConsolePort
    $code = $LASTEXITCODE
} finally {
    if (-not $Keep) {
        Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
            Where-Object { $before -notcontains $_.Id } |
            Stop-Process -Force -ErrorAction SilentlyContinue
        if ($qemu -and -not $qemu.HasExited) {
            Stop-Process -Id $qemu.Id -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "the emulator is still running; the page is on http://127.0.0.1:$Http/"
    }
}

exit $code
