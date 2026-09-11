# Can a slot have a shell of its own?
#
# The slots have always been described as "a shell per slot, plus an
# application" - and until today that was one shell that moved between them
# as the focus did, swapping its working directory in and out as it went.
# That works precisely because only one slot is ever live, and it is not
# enough for a prompt in a window, which has to be somebody's prompt while
# the desktop is in front of it.
#
# `prompt 2` gives slot 2 a shell of its own: its own task, its own screen
# (ag_console_bind_task) and its own directory.  This is the test that says
# the two are genuinely separate.
#
#   .\tools\twoprompts.ps1
#
# The question it asks is the one that fails when they are not: after
# sending each prompt to a different directory, and switching away and back,
# does each still show its own?  A single shell swapping state answers "C:\"
# on both sides, because there is one `cd` and the last one wins.
#
# Alt+digit arrives at a serial terminal as ESC and the digit, which is why
# the switches are raw bytes rather than keystrokes.
#
# Two prompts also read one keyboard, and that is not a detail: with both
# believing they are in front they do not take turns, they take a character
# each - `cd c:\two` arrived at slot 2 as `c c:\two` before the following
# prompt learned to stand aside for a slot that has its own.
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

    $log = 'build\twoprompts.log'
    $send = @(
        # A directory for slot 2 to stand in.  C: is formatted fresh in the
        # emulator, so nothing may be assumed to exist.
        'md c:\two',
        'prompt 2',
        '~\x1b2',
        'cd c:\two',
        # Away and back, which is what a shell swapping one set of state
        # cannot survive.
        '~\x1b1',
        'cd',
        '~\x1b2',
        'cd'
    )

    & .\tools\qemu-boot.ps1 -LogPath $log -TimeoutSec $TimeoutSec `
        -BudgetSec 300 -Send $send | Out-Host

    if (-not (Test-Path $log)) { throw "no transcript at $log" }
    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $log))
    $text = -join ($bytes | ForEach-Object {
        if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { "`n" } })

    $fail = @()

    # Slot 2 has a prompt of its own at all: the prompt itself says which
    # slot it belongs to, so `(2) ` can only have been written by one.
    if ($text -notmatch '\(2\) ') {
        $fail += 'slot 2 never printed a prompt of its own'
    }

    # Each prompt in its own directory, after the switch away and back.
    if ($text -notmatch '\(2\) C:\\two>') {
        $fail += "slot 2 did not keep its own directory"
    }
    if ($text -notmatch '\(1\) C:\\>') {
        $fail += "slot 1 did not keep the root: the two share one directory"
    }

    # And no character went to the wrong reader on the way.
    if ($text -match '\(2\) C:\\>c c:') {
        $fail += 'a character of slot 2 s command went to the other prompt'
    }

    if ($fail.Count -gt 0) {
        foreach ($f in $fail) { Write-Host "FAIL: $f" }
        Write-Host "transcript: $log"
        exit 1
    }
    Write-Host 'twoprompts: ok - two shells, two directories, one keyboard'
} finally {
    Pop-Location
}
