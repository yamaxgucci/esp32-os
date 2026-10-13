# argon remote <argon command ...> - run an argon command on the test PC.
#
#   argon remote check
#   argon remote test ver mem
#   argon remote nodetest -Scenario link
#   argon remote -Probe          exit 0 when the PC answers, 3 when it does not
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
# WHAT COMES BACK.  The console output streams here as it runs and the exit
# code is the remote one.  Files the run wrote under build\ (top level) and
# logs\ are copied into logs\remote\<tree>\ afterwards.
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
$sshOpts = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'ServerAliveInterval=30')
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

function Test-Remote {
    & ssh @sshOpts $remote 'exit 0' 2>$null | Out-Null
    return ($LASTEXITCODE -eq 0)
}

# PowerShell on the far side, as base64: no quoting survives cmd + ssh +
# PowerShell intact, and Cyrillic does not survive it at all.
function Invoke-Remote([string]$Script) {
    $full = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; `$ProgressPreference = 'SilentlyContinue'`n" + $Script
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($full))
    # Out-Host: the output streams to this console as it comes, and the
    # function returns the exit code alone rather than everything printed.
    & ssh @sshOpts $remote "powershell -NoProfile -NonInteractive -EncodedCommand $b64" | Out-Host
    return $LASTEXITCODE
}

function Quote([string]$s) { "'" + $s.Replace("'", "''") + "'" }

if ($Argv.Count -ge 1 -and $Argv[0] -ieq '-Probe') {
    if (Test-Remote) { Write-Host "remote: $remote answers."; exit 0 }
    Write-Host "remote: $remote does not answer."
    exit 3
}
if ($Argv.Count -eq 0) {
    Write-Host 'usage: argon remote <argon command ...>   (argon remote -Probe)'
    exit 2
}
if (-not (Test-Remote)) {
    Write-Host "remote: $remote does not answer - run it here instead."
    exit 3
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
"@
if ($rc -ne 0) { throw 'remote: could not create the repository on the PC.' }

# `git receive-pack` with a space: the far side is PowerShell, which finds
# git.exe on PATH but not the dashed git-receive-pack inside Git's own folder.
$ref = "refs/heads/remote/$tree"
& git push -q --force --receive-pack='git receive-pack' "${remote}:$repo" "${commit}:$ref" 2>&1 |
    Where-Object { $_ -notmatch '^\s*$' } | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'remote: push to the PC failed.' }

# --- the files git does not carry -------------------------------------------
$copy = @()
foreach ($f in @('sdkconfig', '.argon-board')) { if (Test-Path $f) { $copy += $f } }
$stage = "$base/incoming/$tree"
Invoke-Remote "New-Item -ItemType Directory -Force '$stage' | Out-Null; Get-ChildItem '$stage' -Force | Remove-Item -Force; Remove-Item '$base/incoming/$tree.tar' -ErrorAction SilentlyContinue" | Out-Null
if ($copy.Count) {
    & scp -q @sshOpts @copy "${remote}:$stage/"
    if ($LASTEXITCODE -ne 0) { throw 'remote: copying sdkconfig to the PC failed.' }
}

# --- check out, then run ----------------------------------------------------
$argList = ($Argv | ForEach-Object { Quote $_ }) -join ' '
$script = @"
`$ErrorActionPreference = 'Continue'
`$work = '$work'
if (-not (Test-Path "`$work\.git")) { git clone -q --no-checkout '$repo' `$work }
Set-Location `$work
git fetch -q origin '+${ref}:refs/remotes/origin/remote/$tree'
git checkout -q -f --detach '$commit'
if (`$LASTEXITCODE -ne 0) { Write-Host 'remote: checkout failed on the PC.'; exit 4 }
git clean -q -fd
# What this tree is built for.  A different chip is what argon target is for,
# and it starts from an empty build\ - do the same rather than let IDF try.
`$stage = '$stage'
if (Test-Path "`$stage\sdkconfig") {
    `$was = if (Test-Path sdkconfig) { (Select-String sdkconfig -Pattern '^CONFIG_IDF_TARGET=' | Select-Object -First 1).Line } else { '' }
    `$now = (Select-String "`$stage\sdkconfig" -Pattern '^CONFIG_IDF_TARGET=' | Select-Object -First 1).Line
    if (`$was -ne `$now -and (Test-Path build)) { Write-Host "remote: target `$was -> `$now, emptying build\"; Remove-Item -Recurse -Force build }
}
Get-ChildItem `$stage -Force | Copy-Item -Destination . -Force
Copy-Item -Force '$base/local-env.ps1' tools\local-env.ps1
`$started = Get-Date
& cmd /c argon.cmd $argList
`$code = `$LASTEXITCODE
# What the run wrote, for the trip back.
`$list = @()
if (Test-Path build) { `$list += Get-ChildItem build -File | Where-Object { `$_.LastWriteTime -ge `$started -and `$_.Length -lt 64MB } | ForEach-Object { 'build/' + `$_.Name } }
if (Test-Path logs) { `$list += Get-ChildItem logs -File -Recurse | Where-Object { `$_.LastWriteTime -ge `$started } | ForEach-Object { 'logs/' + `$_.FullName.Substring((Resolve-Path logs).Path.Length + 1).Replace('\','/') } }
`$tar = "`$stage\..\$tree.tar"
Remove-Item `$tar -ErrorAction SilentlyContinue
if (`$list.Count) {
    Set-Content -Path "`$stage\..\$tree.files" -Value `$list -Encoding ascii
    & tar.exe -cf `$tar -T "`$stage\..\$tree.files"
}
exit `$code
"@
$code = Invoke-Remote $script

# --- the evidence back ------------------------------------------------------
# One tar, one copy: a nodetest writes dozens of files and scp per file is a
# second each.
$tarFile = (& git rev-parse --path-format=absolute --git-path "argon-remote-$PID.tar").Trim()
& scp -q @sshOpts "${remote}:$base/incoming/$tree.tar" $tarFile 2>$null
if ($LASTEXITCODE -eq 0 -and (Test-Path $tarFile)) {
    $dest = Join-Path $root "logs\remote\$tree"
    New-Item -ItemType Directory -Force $dest | Out-Null
    $n = @(& tar.exe -tf $tarFile).Count
    & tar.exe -xf $tarFile -C $dest
    Remove-Item $tarFile -ErrorAction SilentlyContinue
    Write-Host ("remote: {0} file(s) the run wrote -> logs\remote\{1}\" -f $n, $tree)
}
Write-Host "remote: exit $code"
exit $code
