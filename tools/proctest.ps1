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
# The one check that is a loop, because it is a race:
#
#   storm  spawn SPIN log, kill a moment later, wait - forty times
#                                          -> 40 of 40 killed, no [lockwatch]
#
# SPIN log is inside the journal's lock or the console's nearly all the time.
# A kill used to ask "is it in the kernel?" and then delete it, with the child
# running on the other core in between; on 1 Oct 2026 it was deleted holding
# the journal and the board printed nothing more.  Before the fix this storm
# hung on its first kill.
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
            'ps',
            'run t:\reapwait.axe storm t:\spin.axe log', '=reapwait storm:') |
    Out-Host

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
# Forty kills of a child that is inside the journal's lock or the console's
# nearly all the time.  A kill that deleted it there left the lock held for
# ever (1 Oct 2026: "[lockwatch] the journal lock held 30 s by ''") and the
# board printed nothing more; now the kill stops the child first and deletes
# it only outside every kernel lock.
Check $t 'reapwait storm: 40 of 40 killed, 0 refused' 'killed-mid-log-leaves-no-lock'
# From the ps before the storm on: recv holds the console for as long as a
# file takes to arrive, and the watch says so while the images go onto t:.
# Not from the storm's own command line - the screen is sent as differences,
# and a line that scrolls away before the next paint never reaches the
# transcript at all.  The watch itself is raw output and always does.
#
# Less one report.  The watch samples once a second and says "held 3 s" when
# three samples running found the same holder, held through or not; the
# console task painting SPIN's flood takes the console lock hundreds of times
# a second and is found holding it three samples running in about every second
# run, while the kills go on around it.  A console really stopped is said
# again at 30 s - the storm's wait outlasts that - and the hang this is about
# was the journal, held by the deleted child, which nothing here forgives.
$watch = [regex]::Match($(if ($ps -ge 0) { $t.Substring($ps) } else { '' }),
                        "\[lockwatch\](?! the console lock held 3 s by 'ag_con')[^\n]*")
Note ($ps -ge 0 -and -not $watch.Success) 'no-lock-held-for-seconds' `
    $(if ($ps -lt 0) { 'ps never ran' } elseif ($watch.Success) { $watch.Value })

$failed = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ''
Write-Host "proctest: $($results.Count) checks, $failed failed"
exit $(if ($failed -eq 0) { 0 } else { 1 })
