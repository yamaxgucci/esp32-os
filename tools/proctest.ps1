# Who collects a finished process, in the emulator.
#
#   .\argon.cmd proctest
#
# An application's spawn and its wait are two calls, and the supervisor sweeps
# up finished processes nobody is waiting for every 250 ms.  Until 25 Sep 2026
# a child that ended between the two was swept up first, and the wait that
# followed answered -AG_ENOENT instead of the child's code; a kill between two
# slices of a wait did the same at once, and hung the desktop on Ctrl+C.  The
# fix keeps a zombie for the process that spawned it while that one lives.
#
# apps\reapwait holds each gap open for a whole second - four supervisor ticks -
# so every check here fails every time or never, and needs no loop:
#
#   late   spawn HELLO, sleep, wait        -> the child's own code, 0
#   kill   spawn SPIN, kill, sleep, wait   -> -92, AG_EKILLED
#   leave  spawn HELLO, sleep, never wait  -> the orphan is swept up anyway
#
# and after the first two a second wait must say -2: the first one collected
# the child, and nothing else may have.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

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
    # [char]27, not `e: PowerShell 5.1 has no `e (see nodetest.ps1).
    return ($raw -replace "$([char]27)\[[0-9;?]*[A-Za-z]", "`n")
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    Note ($Text.Contains($Needle)) $Name $(if (-not $Text.Contains($Needle)) { "no '$Needle'" })
}

# Named, and checked afterwards: an image missing from this list would run on
# whatever an earlier full build left in build\apps.
$only = @('REAPWAIT.AXE', 'HELLO.AXE', 'SPIN.AXE')
& python (Join-Path $PSScriptRoot 'build_apps.py') --only @only | Out-Null
foreach ($img in $only) {
    if (-not (Test-Path "build\apps\$img")) { throw "$img did not build." }
}

& idf.py build | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$log = 'build\proctest.log'
& (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec -LogPath $log `
    -Put @('build\apps\REAPWAIT.AXE=t:\reapwait.axe',
           'build\apps\HELLO.AXE=t:\hello.axe',
           'build\apps\SPIN.AXE=t:\spin.axe') `
    -Send @('run t:\reapwait.axe late t:\hello.axe', '=reapwait late:',
            'run t:\reapwait.axe kill t:\spin.axe', '=reapwait kill:',
            'run t:\reapwait.axe leave t:\hello.axe', '=reapwait leave:',
            '!powershell -NoProfile -Command Start-Sleep -Seconds 3',
            'ps') | Out-Host

$t = Transcript $log
Check $t 'reapwait late: wait 0, code 0, again -2' 'late-wait-gets-the-code'
# The wait is what is checked, not the code: SPIN listens, and a kill asks
# first, so the child may end itself (code 0) in the moment between the asking
# and the deleting - both are a killed child kept for its parent.  The fault
# this guards against is -2, the child collected before the wait.
$kill = [regex]::Match($t, 'reapwait kill: wait -92, code (-92|0), again -2')
Note $kill.Success 'killed-child-kept-for-its-parent' `
    $(if (-not $kill.Success) { "no 'reapwait kill: wait -92, code -92|0, again -2'" })
Check $t 'reapwait leave: not waiting' 'parent-left-without-waiting'
# The LAST ps, which is the one after the orphan had its tick.
$ps = $t.LastIndexOf('C:\>ps')
Note ($ps -ge 0 -and $t.IndexOf('no applications loaded', $ps) -gt $ps) `
    'orphan-swept-up' $(if ($ps -lt 0) { 'ps never ran' })

$failed = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ''
Write-Host "proctest: $($results.Count) checks, $failed failed"
exit $(if ($failed -eq 0) { 0 } else { 1 })
