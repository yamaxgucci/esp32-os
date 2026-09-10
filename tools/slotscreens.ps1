# Does a slot remember what was on it?
#
# Until today the answer was no, and not by oversight: there was one screen
# for every slot, so the only way to stop slot 2 reading as the tail of slot 1
# was to wipe it on every switch.  Everything a program had said was therefore
# gone the moment you looked at something else - and on this machine looking
# at something else is how you start the next thing.
#
# Each slot has its own screen now (ag_console_use_slot), and this is the test
# that says so.  It is a whole-system question - console, session, panel - and
# there is no host test that can ask it, so it is asked in the emulator:
#
#   .\tools\slotscreens.ps1
#
# Alt+digit arrives at a serial terminal as ESC followed by the digit, which
# is why the switches below are raw bytes rather than keystrokes.
param(
    [switch]$NoBuild,
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not $NoBuild) {
        & .\argon.cmd build | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'the firmware did not build' }
    }

    $log = 'build\slotscreens.log'
    $send = @(
        # Slot 1 says something only slot 1 could have said.
        'echo SLOTONEMARK',
        # Alt+2: a slot nobody has looked at yet, so its screen is new.
        '~\x1b2',
        'echo SLOTTWOMARK',
        # Alt+1: back.  What is on the glass now is the question.
        '~\x1b1',
        'ver'
    )

    & .\tools\qemu-boot.ps1 -LogPath $log -TimeoutSec $TimeoutSec `
        -BudgetSec 300 -Send $send | Out-Host

    if (-not (Test-Path $log)) { throw "no transcript at $log" }
    $text = Get-Content -Raw $log

    $fail = @()
    if (-not (Test-Path 'build-host\vtdump.exe')) {
        throw 'build-host\vtdump.exe is missing; run argon check first'
    }
    $screen = (& cmd /c "build-host\vtdump.exe 80 25 437 < $log") -join "`n"

    # Both marks were typed, so both must be in the transcript: a test that
    # only looked at the screen could pass because neither ever happened.
    if ($text -notmatch 'SLOTONEMARK') { $fail += 'slot 1 never echoed' }
    if ($text -notmatch 'SLOTTWOMARK') { $fail += 'slot 2 never echoed' }

    # And on the screen, after coming back: slot 1's own words, and none of
    # slot 2's.  The second half is the half that fails when the slots share
    # one screen after all.
    if ($screen -notmatch 'SLOTONEMARK') {
        $fail += 'slot 1 came back blank: its screen was not kept'
    }
    if ($screen -match 'SLOTTWOMARK') {
        $fail += "slot 2's output is on slot 1's screen: they are sharing one"
    }

    if ($fail.Count -gt 0) {
        foreach ($f in $fail) { Write-Host "FAIL: $f" }
        Write-Host "transcript: $log"
        exit 1
    }
    Write-Host 'slotscreens: ok - a slot keeps its own screen across a switch'
} finally {
    Pop-Location
}
