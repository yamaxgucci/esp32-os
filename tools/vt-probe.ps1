# Reports whether this console can display the ArgonOS screen.
#
#   argon vt
#
# Exists because getting this wrong is invisible: the emulator runs, the board
# boots, and the only symptom is a wall of escape sequences.  The result is
# also written to build\vt-probe.txt so it can be read from a console that
# closes itself.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
$ok = Enable-ConsoleVt
$report = Get-ConsoleVtReport

# The end-to-end check belongs here rather than in the launcher: writing to
# somebody's terminal to double-check the console mode is fine when they asked
# for a diagnosis, and gratuitous when they asked to run the emulator.
$renders = if ($ok) { Test-ConsoleRendering } else { $false }

$lines = @(
    "virtual terminal: $(if ($ok) { 'yes' } else { 'no' })"
    "detail: $report"
    "renders escape sequences: $(if ($renders) { 'yes' } else { 'no' })"
    "host: $($Host.Name) $($Host.Version)"
    "term: $env:WT_SESSION$env:TERM"
)

New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null
$lines | Set-Content -Path (Join-Path $root 'build\vt-probe.txt') -Encoding ascii

# PowerShell 5.1 has no `e escape, so ESC is built by code point.
$esc = [char]27
if ($ok -and $renders) {
    [Console]::Out.Write("$esc[2K`ranswer: escape sequences work here`n")
} else {
    Write-Host 'answer: this console cannot display the screen; use -Tcp with PuTTY'
}

$lines | ForEach-Object { Write-Host $_ }
Restore-ConsoleVt

exit $(if ($ok -and $renders) { 0 } else { 1 })
