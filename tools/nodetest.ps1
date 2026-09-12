# Two nodes, in the emulator, with a wire between them.
#
#   .\argon.cmd nodetest
#
# Boots TWO whole ArgonOS kernels on this PC and joins their UART1 ports to each
# other, then measures the wire between them with apps\link - the same program
# that measured the real cable between the S3 and the CYD.
#
# WHY THIS EXISTS.  A cluster needs two machines, and there is one board on the
# desk.  Everything else in this tree that needed a second party used a Python
# process pretending to be one (hostfsd, rlinkd, the AT modem), which is right
# when the other end is a coprocessor whose firmware is somebody else's.  It is
# wrong here: the other end is US, and a fake node would be a fake of the thing
# under test.  So both ends are the real image, and the only thing simulated is
# the copper.
#
# HOW IT WORKS.  A serial port in QEMU is a TCP socket, and qemu-boot already
# opens one as a server (the console) and others as clients (HostFS, the radio).
# A pair is then just the two ends of one socket: node B listens on UART1, node A
# connects to it.  Nothing in the firmware knows or can know the difference.
#
# WHAT HAD TO CHANGE for it, all in qemu-boot.ps1, and each was a way for this
# to fail confusingly rather than cleanly:
#
#   * that script kills every qemu-system-xtensa before it starts, which is
#     right for one guest and fatal for two - the second shoots the first.
#     -KeepOtherEmulators, and this script does the one kill at the top.
#   * the flash drive is writable raw MTD.  Two emulators on one file corrupt
#     each other's C:, and the damage is not seen here - it is seen weeks later
#     as a file that reads back -91 on an unrelated run.  -Flash gives B its own
#     copy of the same image.
#   * QEMU's own chatter went to one log from both processes.  -Tag.
#
# TIMING.  B receives for a long window and A waits before sending, rather than
# the two racing to be ready.  Without the wait A can finish booting first and
# push blocks into a UART nobody has opened, and what that looks like from the
# transcript is a lossy wire - a measurement, not a mistake, and one that would
# have been believed.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 120,
    # Baud is nominal: there is no line here, only a socket, so this only has to
    # be a number both ends configure their port to.  Kept at a real rate so the
    # transcript reads like the board's.
    [int]$Baud = 921600,
    [int]$Kb = 64,
    # Long enough to cover A's boot, its file transfer and the wait below.
    [int]$RecvSecs = 60,
    [int]$SendDelaySec = 25
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$linkPort = 5570
$flashB = 'build\qemu_flash_b.bin'

$results = New-Object System.Collections.Generic.List[string]
function Note {
    param([bool]$Ok, [string]$Name, [string]$Detail = '')
    $line = ($(if ($Ok) { 'PASS ' } else { 'FAIL ' })) + $Name +
            $(if ($Detail) { " - $Detail" } else { '' })
    $results.Add($line)
    if ($Ok) { Write-Host $line -ForegroundColor Green }
    else { Write-Host $line -ForegroundColor Red }
}

function Transcript {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    return ($raw -replace "`e\[[0-9;]*[A-Za-z]", "`n")
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    Note ($Text.Contains($Needle)) $Name $(if (-not $Text.Contains($Needle)) { "no '$Needle'" })
}

# The program both nodes run.
& python (Join-Path $PSScriptRoot 'build_apps.py') --only LINK.AXE | Out-Null
if (-not (Test-Path 'build\apps\LINK.AXE')) { throw 'LINK.AXE did not build.' }

# One flash image, built once, before either guest can race the other to it.
Update-FlashImage

# The one kill, here rather than in each guest.
Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

Write-Host "`n== two nodes, one wire ==" -ForegroundColor Cyan
Write-Host "node B: console 5566, UART1 listening on $linkPort, flash $flashB"
Write-Host "node A: console 5556, UART1 connecting to $linkPort"

# A separate PROCESS, not a background job.
#
# Start-Job was the obvious way and does not work: the job's runspace cannot
# dot-source tools\idf-env.ps1 - it simply never returns, and the symptom is a
# job stuck in Running for ever with nothing on its output, which reads exactly
# like a guest that failed to boot.  A process inherits $env: from this one, so
# the environment is already there and nothing has to be set up again.  It is
# also what rlinktest does for its fixture, which is the precedent.
# Quoted by hand.  Start-Process does not quote for you, and this tree lives
# under "Unity Projects" - so an unquoted script path arrives as -File
# 'D:\Work\Unity' and the child says the file has no .ps1 extension, which is
# true and says nothing about the real problem.
$argsB = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', ('"{0}"' -f (Join-Path $PSScriptRoot 'qemu-boot.ps1')),
    '-Port', '5566', '-NetPort', '5578', '-TimeoutSec', "$TimeoutSec",
    '-LinkPort', "$linkPort", '-LinkListen', '-Flash', $flashB,
    '-KeepOtherEmulators', '-Tag', 'b', '-LogPath', 'build\nodetest-b.log',
    '-Put', 'build\apps\LINK.AXE=t:\link.axe',
    '-Send', ('"run t:\link.axe recv {0} {1}"' -f $Baud, $RecvSecs)
)
$procB = Start-Process -FilePath 'powershell' -ArgumentList $argsB -PassThru `
    -NoNewWindow -RedirectStandardOutput 'build\nodetest-b.out' `
    -RedirectStandardError 'build\nodetest-b.err'
Start-Sleep -Milliseconds 500
if ($procB.HasExited) {
    throw ("node B exited at once (code $($procB.ExitCode)); " +
           'see build\nodetest-b.err')
}

try {
    # A waits on the host, not in the guest: the shell has no sleep, and a
    # delay typed as a guest command would be a program that has to exist.
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port 5556 -TimeoutSec $TimeoutSec `
        -LinkPort $linkPort -KeepOtherEmulators -Tag 'a' `
        -LogPath 'build\nodetest-a.log' `
        -Put "build\apps\LINK.AXE=t:\link.axe" `
        -Send @("!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                "run t:\link.axe send $Baud $Kb") | Out-Null
} finally {
    # B is still inside its receive window; let it finish and say what it got.
    if (-not $procB.WaitForExit($TimeoutSec * 2000)) {
        Write-Host 'node B did not finish; stopping it.' -ForegroundColor Yellow
        Stop-Process -Id $procB.Id -Force -ErrorAction SilentlyContinue
    }
    foreach ($f in @('build\nodetest-b.out', 'build\nodetest-b.err')) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
            Write-Host "--- $f ---" -ForegroundColor DarkGray
            Get-Content $f | Select-Object -Last 12 | Out-Host
        }
    }
    Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

$a = Transcript 'build\nodetest-a.log'
$b = Transcript 'build\nodetest-b.log'

# Both kernels really booted - two of them, not one twice.
Check $a 'ArgonOS' 'node-a-booted'
Check $b 'ArgonOS' 'node-b-booted'
Check $a 'send:' 'node-a-sent'
Check $b 'recv:' 'node-b-received'

# The measurement itself.  `link` reports whole blocks, corrupt ones and lost
# ones separately, which is the distinction the whole program exists for: a
# corrupt block means the line is too fast for the wiring, a lost one means the
# receiver could not keep up, and they are cured in opposite directions.
$m = [regex]::Match($b,
    'got (\d+) bytes, (\d+) blocks ok, (\d+) corrupt, (\d+) lost, (\d+) stray')
if (-not $m.Success) {
    # Fall back to whatever summary line the receiver printed, so a change in
    # its wording is a readable failure rather than a silent zero.
    $tail = ($b -split "`n" | Where-Object { $_ -match 'block|byte|B/s|arrived' } |
             Select-Object -Last 4) -join ' | '
    Note $false 'node-b-summary-parsed' "receiver said: $tail"
} else {
    $bytes  = [int]$m.Groups[1].Value
    $blocks = [int]$m.Groups[2].Value
    $bad    = [int]$m.Groups[3].Value
    $lost   = [int]$m.Groups[4].Value
    $stray  = [int]$m.Groups[5].Value
    Write-Host ("wire: $bytes bytes, $blocks blocks ok, $bad corrupt, " +
                "$lost lost, $stray stray")
    Note ($blocks -gt 0) 'wire-carried-blocks' "$blocks blocks"
    Note ($bad -eq 0) 'wire-no-corruption' "$bad corrupt"
    Note ($lost -eq 0) 'wire-nothing-lost' "$lost lost"
    # Stray bytes are the ones thrown away while hunting for a header.  A few at
    # the very start are honest - the receiver may attach mid-block - but a
    # steady stream of them means the framing is wrong rather than the line.
    Note ($stray -lt 512) 'wire-framing-held' "$stray stray bytes"

    # The check that actually says "everything arrived", as against "what
    # arrived was clean".  A receive window that closed early would report zero
    # corrupt and zero lost about half a transfer - `lost` counts gaps between
    # sequence numbers, and a missing tail is not a gap.  Comparing the two
    # nodes' own byte counts is the only statement that covers it.
    $s = [regex]::Match($a, 'sent (\d+) bytes in (\d+) ms')
    if (-not $s.Success) {
        Note $false 'node-a-summary-parsed' 'no "sent N bytes" line'
    } else {
        $sent = [int]$s.Groups[1].Value
        Note ($sent -eq $bytes) 'wire-byte-for-byte' "sent $sent, got $bytes"
    }

    # Said out loud so nobody quotes it at a board.  There is no line here and
    # no baud rate: the wire is a TCP socket between two processes on this PC,
    # and what it carries per second is a property of this PC.  The real cable
    # is measured by running the same program on two boards.
    Write-Host ('rate: QEMU only - this wire is a socket, not copper. ' +
                'For real numbers put LINK.AXE on two boards.') -ForegroundColor DarkGray
}

$failed = @($results | Where-Object { $_ -like 'FAIL *' })
Write-Host ''
Write-Host ("nodetest: {0} checks, {1} failed" -f $results.Count, $failed.Count)
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
