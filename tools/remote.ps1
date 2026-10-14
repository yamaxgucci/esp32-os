# argon remote <argon command ...> - run an argon command on the test PC.
#
#   argon remote check
#   argon remote test ver mem
#   argon remote nodetest -Scenario link
#   argon remote -Probe          exit 0 when the PC answers, 3 when it does not
#   argon remote -Attach         follow this tree's run again after losing it
#
# WHY.  The emulator tests are CPU work for hours a week, and the laptop that
# runs them is also the machine with the boards, the editor and several
# sessions on it.  The PC next to it has six cores doing nothing.  So the rule
# (docs/16-test-pc.md): when the PC answers, the QEMU tests run there.
#
# WHAT IS SENT.  The working tree as it is - not HEAD.  Tracked changes and new
# files that are not ignored go into a snapshot commit built in a private index
# (the real index and the stash are not touched), pushed over ssh into the PC's
# copy of the repository, and checked out there.  sdkconfig and .argon-board are
# not in git and decide what gets built, so they are copied as well: the PC
# builds the target this tree is set to, not whatever it built last time.
#
# ONE CHECKOUT PER TREE.  D:\argon\work\<leaf of this tree's path> - so the main
# checkout and each worktree have their own build\ on the PC, incremental
# builds stay incremental, and the emulator registry (qemu-common.ps1) stays
# per tree there just as it is here.
#
# THE RUN DOES NOT LIVE IN THE SSH SESSION.  The first version ran argon.cmd
# inside the ssh command, and on 13 Oct 2026 the first full build on the PC
# died three minutes in: the PC went to sleep on its idle timer - an ssh
# session is not activity to Windows - and the Wi-Fi went with it.  So the run
# is started as its own process (through WMI, outside sshd's job object, which
# would kill it on disconnect), holds the machine awake with
# SetThreadExecutionState for exactly as long as it runs - no power setting
# is changed - and writes its console to a log.  This side follows the log
# and, when the connection drops, connects again and carries on from the line
# it had reached.  -Attach does the same by hand later.
#
# WHAT COMES BACK.  The console, as it runs, and the remote exit code.  Files
# the run wrote under build\ (top level) and logs\ are copied into
# logs\remote\<tree>\ afterwards.
#
# The host is ARGON_REMOTE (default: the ssh alias `pc`).  No ssh password
# prompts: BatchMode, so an absent key fails at once instead of hanging.
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Argv = @()
)

# Continue, not Stop: in Windows PowerShell a redirected line of a native
# program's stderr - git's CRLF notes, ssh's banner - is an error record, and
# Stop turns the first one into a failed run.  Exit codes are checked instead.
$ErrorActionPreference = 'Continue'
$remote = if ($env:ARGON_REMOTE) { $env:ARGON_REMOTE } else { 'pc' }
$base = 'D:/argon'
$repo = "$base/esp32-os.git"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$tree = Split-Path -Leaf $root
$work = "$base/work/$tree"
$in = "$base/incoming"
$stage = "$in/$tree"
$sshOpts = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=4')
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

function Test-Remote {
    & ssh @sshOpts $remote 'exit 0' 2>$null | Out-Null
    return ($LASTEXITCODE -eq 0)
}

# PowerShell on the far side, as base64: no quoting survives cmd + ssh +
# PowerShell intact, and Cyrillic does not survive it at all.
function Get-RemoteCommand([string]$Script) {
    $full = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; `$ProgressPreference = 'SilentlyContinue'`n" + $Script
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($full))
    return "powershell -NoProfile -NonInteractive -EncodedCommand $b64"
}
function Invoke-Remote([string]$Script) {
    # Out-Host: the output streams to this console as it comes, and the
    # function returns the exit code alone rather than everything printed.
    & ssh @sshOpts $remote (Get-RemoteCommand $Script) | Out-Host
    return $LASTEXITCODE
}

# Follow the run's log from line $script:seen until the run says it is done.
# Survives the connection: a drop costs a reconnect, not the run, for up to
# $GiveUpSec of the PC not answering at all.
$script:seen = 0
function Wait-Run([int]$GiveUpSec = 900) {
    $lost = $null
    while ($true) {
        $tail = @"
`$log = '$in/$tree.log'; `$done = '$in/$tree.done'; `$skip = $($script:seen)
while (-not (Test-Path `$log)) { if (Test-Path `$done) { break }; Start-Sleep -Milliseconds 300 }
`$n = 0; `$buf = ''
`$fs = [IO.File]::Open(`$log, 'Open', 'Read', 'ReadWrite, Delete')
`$sr = New-Object IO.StreamReader(`$fs, [Text.Encoding]::UTF8)
while (`$true) {
    `$fin = Test-Path `$done
    `$chunk = `$sr.ReadToEnd()
    if (`$chunk) {
        `$parts = (`$buf + `$chunk) -split "``n"
        `$buf = `$parts[-1]
        for (`$i = 0; `$i -lt `$parts.Count - 1; `$i++) {
            `$n++
            if (`$n -gt `$skip) { [Console]::Out.WriteLine(`$parts[`$i].TrimEnd([char]13)) }
        }
        [Console]::Out.Flush()
    } elseif (`$fin) {
        if (`$buf) { `$n++; if (`$n -gt `$skip) { [Console]::Out.WriteLine(`$buf) } }
        [Console]::Out.WriteLine('@@argon-remote-done ' + (Get-Content `$done -Raw).Trim())
        break
    } else { Start-Sleep -Milliseconds 300 }
}
"@
        $code = $null
        & ssh @sshOpts $remote (Get-RemoteCommand $tail) 2>$null | ForEach-Object {
            if ($_ -match '^@@argon-remote-done (-?\d+)') { $code = [int]$Matches[1] }
            else { Write-Host $_; $script:seen++ }
        }
        if ($null -ne $code) { return $code }
        if (-not $lost) { $lost = Get-Date; Write-Host "remote: lost the connection at line $($script:seen) - the run goes on, reconnecting..." }
        elseif (((Get-Date) - $lost).TotalSeconds -gt $GiveUpSec) {
            Write-Host "remote: $remote has not answered for $GiveUpSec s.  The run may still be going: argon remote -Attach"
            return 6
        }
        Start-Sleep -Seconds 5
        if (Test-Remote) { $lost = $null }
    }
}

# The evidence back: one tar, one copy - a nodetest writes dozens of files and
# scp per file is a second each.
function Get-Evidence {
    $tarFile = (& git rev-parse --path-format=absolute --git-path "argon-remote-$PID.tar").Trim()
    & scp -q @sshOpts "${remote}:$in/$tree.tar" $tarFile 2>$null
    if ($LASTEXITCODE -eq 0 -and (Test-Path $tarFile)) {
        $dest = Join-Path $root "logs\remote\$tree"
        New-Item -ItemType Directory -Force $dest | Out-Null
        $n = @(& tar.exe -tf $tarFile).Count
        & tar.exe -xf $tarFile -C $dest
        Remove-Item $tarFile -ErrorAction SilentlyContinue
        Write-Host ("remote: {0} file(s) the run wrote -> logs\remote\{1}\" -f $n, $tree)
    }
}

if ($Argv.Count -ge 1 -and $Argv[0] -ieq '-Probe') {
    if (Test-Remote) { Write-Host "remote: $remote answers."; exit 0 }
    Write-Host "remote: $remote does not answer."
    exit 3
}
if ($Argv.Count -eq 0) {
    Write-Host 'usage: argon remote <argon command ...>   (-Probe: is it up; -Attach: follow the run again)'
    exit 2
}
if (-not (Test-Remote)) {
    Write-Host "remote: $remote does not answer - run it here instead."
    exit 3
}
if ($Argv[0] -ieq '-Attach') {
    $code = Wait-Run
    if ($code -ne 6) { Get-Evidence }
    Write-Host "remote: exit $code"
    exit $code
}

# One run per tree at a time: a second one would check out over the first.
& ssh @sshOpts $remote (Get-RemoteCommand @"
`$p = '$in/$tree.pid'
if ((Test-Path `$p) -and -not (Test-Path '$in/$tree.done')) {
    if (Get-Process -Id ([int](Get-Content `$p -Raw)) -ErrorAction SilentlyContinue) { exit 5 }
}
exit 0
"@) | Out-Host
if ($LASTEXITCODE -eq 5) {
    Write-Host "remote: a run of this tree is still going on the PC.  argon remote -Attach to follow it."
    exit 5
}

# --- the snapshot -----------------------------------------------------------
# Next to the repository, not in %TEMP%: on the laptop C: is the full disk.
$index = (& git rev-parse --path-format=absolute --git-path ("argon-remote-{0}.index" -f $PID)).Trim()
$head = (& git rev-parse HEAD).Trim()
try {
    # A copy of the real index, not read-tree: its stat cache saves hashing
    # every file in the tree again.
    Copy-Item -LiteralPath (& git rev-parse --git-path index).Trim() $index
    $env:GIT_INDEX_FILE = $index
    & git -c core.safecrlf=false add -A 2>$null
    $snapTree = (& git write-tree).Trim()
} finally {
    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $index -ErrorAction SilentlyContinue
}
$headTree = (& git rev-parse "$head^{tree}").Trim()
$commit = if ($snapTree -eq $headTree) { $head }
          else { (& git commit-tree $snapTree -p $head -m "remote snapshot of $tree").Trim() }
$label = if ($commit -eq $head) { 'HEAD' } else { 'HEAD + working tree' }
Write-Host ("remote: {0} {1} -> {2}:{3}" -f $label, $commit.Substring(0, 9), $remote, $work)

$rc = Invoke-Remote @"
if (-not (Test-Path '$repo')) { git init -q --bare '$repo' }
New-Item -ItemType Directory -Force '$stage' | Out-Null
Get-ChildItem '$stage' -Force | Remove-Item -Force
foreach (`$x in 'tar','files','log','done','pid','cmd','run.ps1') { Remove-Item "$in/$tree.`$x" -ErrorAction SilentlyContinue }
# A Remove-Item of nothing still leaves `$? false, and that is the exit code.
exit 0
"@
if ($rc -ne 0) { throw 'remote: could not prepare the repository on the PC.' }

# `git receive-pack` with a space: the far side is PowerShell, which finds
# git.exe on PATH but not the dashed git-receive-pack inside Git's own folder.
$ref = "refs/heads/remote/$tree"
& git push -q --force --receive-pack='git receive-pack' "${remote}:$repo" "${commit}:$ref" 2>&1 |
    Where-Object { $_ -notmatch '^\s*$' } | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'remote: push to the PC failed.' }

# --- the files git does not carry, and the run itself ------------------------
# The run is two small files: a .cmd that runs argon (cmd's quoting is the
# one argon.cmd expects) and a PowerShell wrapper that keeps the PC awake
# around it, collects what it wrote and says it is done.  Written here and
# copied over with the rest.
$local = (& git rev-parse --path-format=absolute --git-path "argon-remote-$PID").Trim()
New-Item -ItemType Directory -Force $local | Out-Null

$winWork = $work.Replace('/', '\'); $winIn = $in.Replace('/', '\')
$cmdArgs = ($Argv | ForEach-Object { if ($_ -match '[\s&|<>^]') { '"' + $_ + '"' } else { $_ } }) -join ' '
Set-Content -LiteralPath (Join-Path $local "$tree.cmd") -Encoding ascii -Value @"
@echo off
chcp 65001 >nul
cd /d "$winWork"
call argon.cmd $cmdArgs > "$winIn\$tree.log" 2>&1
exit /b %ERRORLEVEL%
"@
Set-Content -LiteralPath (Join-Path $local "$tree.run.ps1") -Encoding utf8 -Value @"
`$ErrorActionPreference = 'Continue'
Add-Type -Namespace ArgonRemote -Name Power -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);'
# ES_CONTINUOUS | ES_SYSTEM_REQUIRED: no sleep while this process lives.
[ArgonRemote.Power]::SetThreadExecutionState([uint32]'0x80000001') | Out-Null
Set-Location '$work'
`$started = Get-Date
& cmd /c "$winIn\$tree.cmd"
`$code = `$LASTEXITCODE
`$list = @()
if (Test-Path build) { `$list += Get-ChildItem build -File | Where-Object { `$_.LastWriteTime -ge `$started -and `$_.Length -lt 64MB } | ForEach-Object { 'build/' + `$_.Name } }
if (Test-Path logs) { `$logs = (Resolve-Path logs).Path; `$list += Get-ChildItem logs -File -Recurse | Where-Object { `$_.LastWriteTime -ge `$started } | ForEach-Object { 'logs/' + `$_.FullName.Substring(`$logs.Length + 1).Replace('\','/') } }
if (`$list.Count) {
    Set-Content -Path '$in/$tree.files' -Value `$list -Encoding ascii
    & tar.exe -cf '$in/$tree.tar' -T '$in/$tree.files'
}
Set-Content -Path '$in/$tree.done' -Value `$code -Encoding ascii
"@

$copy = @(foreach ($f in @('sdkconfig', '.argon-board')) { if (Test-Path $f) { $f } })
if ($copy.Count) { & scp -q @sshOpts @copy "${remote}:$stage/" }
& scp -q @sshOpts (Join-Path $local "$tree.cmd") (Join-Path $local "$tree.run.ps1") "${remote}:$in/"
$scpCode = $LASTEXITCODE
Remove-Item -Recurse -Force -LiteralPath $local -ErrorAction SilentlyContinue
if ($scpCode -ne 0) { throw 'remote: copying the run to the PC failed.' }

# --- check out, then start --------------------------------------------------
$rc = Invoke-Remote @"
`$work = '$work'
if (-not (Test-Path "`$work\.git")) { git clone -q --no-checkout '$repo' `$work }
Set-Location `$work
git fetch -q origin '+${ref}:refs/remotes/origin/remote/$tree'
git checkout -q -f --detach '$commit'
if (`$LASTEXITCODE -ne 0) { Write-Host 'remote: checkout failed on the PC.'; exit 4 }
git clean -q -fd
# What this tree is built for.  A different chip is what argon target is for,
# and it starts from an empty build\ - do the same rather than let IDF try.
if (Test-Path '$stage\sdkconfig') {
    `$was = if (Test-Path sdkconfig) { (Select-String sdkconfig -Pattern '^CONFIG_IDF_TARGET=' | Select-Object -First 1).Line } else { '' }
    `$now = (Select-String '$stage\sdkconfig' -Pattern '^CONFIG_IDF_TARGET=' | Select-Object -First 1).Line
    if (`$was -ne `$now -and (Test-Path build)) { Write-Host "remote: target `$was -> `$now, emptying build\"; Remove-Item -Recurse -Force build }
}
Get-ChildItem '$stage' -Force | Copy-Item -Destination . -Force
Copy-Item -Force '$base/local-env.ps1' tools\local-env.ps1
# Through WMI: a process started from this ssh session is in sshd's job
# object and dies with the connection; one WMI starts is not.
`$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$in/$tree.run.ps1"'
    CurrentDirectory = `$work.Replace('/', '\') }
if (`$r.ReturnValue -ne 0) { Write-Host "remote: could not start the run (`$(`$r.ReturnValue))."; exit 4 }
Set-Content -Path '$in/$tree.pid' -Value `$r.ProcessId -Encoding ascii
"@
if ($rc -ne 0) { exit $rc }

$code = Wait-Run
if ($code -ne 6) { Get-Evidence }
Write-Host "remote: exit $code"
exit $code
