# Two boards of DIFFERENT instruction sets, as one cluster, with nothing
# plugged in.
#
#   .\argon.cmd xclutest
#
# This is what needed the CYD and the C3 on the desk until 18 Sep 2026.  It is
# the whole of claim 3b in docs/plans/cluster.md - a cluster that spans
# instruction sets can share a wire, devices and messages freely, but CODE only
# runs where its instruction set is - and the interesting part is what happens
# at that line rather than either side of it:
#
#   * an image for the other processor is REFUSED IN WORDS, not loaded and run
#     into an illegal instruction somewhere inside;
#   * a run that lands on a node which cannot execute the image does not fail:
#     the node reads the PROGRAM NAME out of the header and runs its own build
#     of that program.  That is rung 1 of the cascade, and it only means
#     anything across two instruction sets.
#
# Node A is an ESP32-S3 (Xtensa) under qemu-system-xtensa; node B is an
# ESP32-C3 (RISC-V) under qemu-system-riscv32.  One build directory cannot hold
# two firmwares, so B's is built first and its flash image kept aside in
# build-bank\ - see -FlashAsIs and -Chip in qemu-boot.ps1.
#
# IT SWITCHES THE TARGET TWICE and ends on esp32s3, the primary one.  Expect
# twenty minutes.
#
# THE FAULT THIS FILE USED TO DESCRIBE WAS THIS SCRIPT'S, AND IT IS FIXED.
#
# It read: "on this pair a node serves exactly one remote run per session; the
# second is retransmitted to the ceiling and answered -60 while the far node's
# counters stand still".  Every word of the evidence was true and the
# conclusion was wrong.  The far node's counters stood still because there was
# no far node: it had reached the end of its own script and exited.
#
# What kept it waiting was silence in the shell.  `run x.axe` that lands here
# prints "returned N"; `run x.axe` that crosses the wire printed the program's
# output and nothing else, while `link run` - the same thing asked for
# differently - has always said "returned N from the other node".  qemu-boot
# waits for "returned " to know a run is over, so every remote run in a
# scripted scenario sat for the whole command timeout, seven minutes, and the
# node it was talking to had a hundred and twenty seconds of `link poll` to
# live.  Everything asked after that got no answer, correctly.
#
# Found by measuring rather than by re-running: tools\xcluprobe.ps1 asks for
# four remote runs of three kinds in one session with the frame counters
# printed between them.  All four are served - and the same probe is what
# showed that the first shape of the question ("does the SECOND one fail?")
# had the wrong subject, because two runs in a row passed.
#
# So this scenario is ONE SESSION again, and it now proves something the two
# could not: the same node answers two different asks, and the cascade picks a
# DIFFERENT rung for each of them - a native build of its own for one program,
# the portable build for another - without being reset in between.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 420,
    [int]$SendDelaySec = 25,
    # Use what is already built and banked.  For iterating; a green run of that
    # is not a green run of this.
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$bank = 'build-bank'
$flashB = Join-Path $bank 'flash-esp32c3.bin'
$linkPort = 5590

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
    return ($raw -replace "$([char]27)\[[0-9;]*[A-Za-z]", "`n")
}

# The console is 80 columns and a sentence that crosses the edge arrives split
# across two rows, so a phrase is looked for with the whitespace flattened.
# The hardware harness learned this on the CYD's 40-column screen.
function Flat {
    param([string]$Text)
    return ($Text -replace '\s+', ' ')
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    $f = Flat $Text
    Note ($f.Contains($Needle)) $Name `
        $(if (-not $f.Contains($Needle)) { "no '$Needle'" })
}

# For a phrase that contains a PATH.
#
# Flattening rescues a sentence broken at a space; it does not rescue a word
# broken in the middle, which is what the eightieth column does to
# "/sys/unsigned.bc" - the break becomes a space and the phrase is no longer
# there.  So this one compares with every space removed from both sides, which
# is safe for a path and would not be for prose.
function CheckTight {
    param([string]$Text, [string]$Needle, [string]$Name)
    $t = ($Text -replace '\s+', '')
    $n = ($Needle -replace '\s+', '')
    Note ($t.Contains($n)) $Name $(if (-not $t.Contains($n)) { "no '$Needle'" })
}

function Set-Target {
    param([string]$Name)
    if ($NoBuild) { return }
    Write-Host "`n-- building for $Name --" -ForegroundColor DarkGray
    & "$root\argon.cmd" target $Name | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "argon target $Name failed." }
    # Twice: a new symbol in the defaults makes the first configure write a
    # fresh sdkconfig and stop, after which build\ holds what was made from the
    # config BEFORE the change.  The second run is a ninja no-op otherwise.
    for ($i = 0; $i -lt 2; $i++) {
        & "$root\argon.cmd" build | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "argon build for $Name failed." }
    }
}

# ---------------------------------------------------- the two firmwares ---

if (-not $NoBuild) {
    Set-Target 'esp32c3-qemu'
    Update-FlashImage
    New-Item -ItemType Directory -Force -Path $bank | Out-Null
    Copy-Item 'build\qemu_flash.bin' $flashB -Force
    # A blank efuse of its own, beside the image, for the same reason a pair
    # needs two flash files: they are written to.
    $efuseB = [IO.Path]::ChangeExtension($flashB, $null) + '_efuse.bin'
    [IO.File]::WriteAllBytes((Join-Path (Get-Location) $efuseB),
                             (New-Object byte[] 1024))
    Write-Host "banked $flashB" -ForegroundColor DarkGray

    Set-Target 'esp32s3'
}
if (-not (Test-Path $flashB)) {
    throw "no banked C3 image at $flashB - run without -NoBuild once."
}

# Every image is arch-tagged in tools\apps.json, so these build whatever the
# firmware target happens to be.
$only = @('NODELINK.SYS', 'NODELNRV.SYS', 'NODEHI.AXE', 'NODEHIRV.AXE',
          'AGVMRV.AXE')
& python (Join-Path $PSScriptRoot 'build_apps.py') --only @only | Out-Null
foreach ($img in $only) {
    if (-not (Test-Path "build\apps\$img")) { throw "$img did not build." }
}

# THE THIRD RUNG NEEDS A PROGRAM THAT EXISTS TWICE AND NOT THREE TIMES:
# as an Xtensa image, which is what node A asks for, and as BYTECODE on node B
# - and NOT as a RISC-V build, or rung one would answer first and rung three
# would never be reached.  unsigned.c is the one to use because its seven
# answers are the ones where signedness changes the answer, so "it ran" and "it
# ran correctly" are the same check.
if (-not (Test-Path 'build-host\cchost.exe')) {
    cmake -S host-tests -B build-host -G Ninja | Out-Null
    cmake --build build-host --target cchost | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cchost did not build.' }
}
New-Item -ItemType Directory -Force -Path $bank | Out-Null
& '.\build-host\cchost.exe' 'apps\cc\examples\unsigned.c' `
    (Join-Path $bank 'xt-unsigned.axe') -xtensa | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cchost could not compile unsigned.c.' }
& '.\build-host\cchost.exe' 'apps\cc\examples\unsigned.c' `
    (Join-Path $bank 'bc-unsigned.bc') -bc | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cchost could not compile unsigned.c to bytecode.' }

Stop-Emulators
Start-Sleep -Milliseconds 500

# --------------------------------------------------------------- the pair ---
#
# B in a process of its own, driven by a generated script.  The quoting
# problems that shape makes unavoidable are written up at Invoke-Pair in
# tools\nodetest.ps1; the difference here is that the two guests are two
# different machines, so B carries its own -Chip and a flash image built before
# the target moved on.

function Invoke-XPair {
    param(
        [string]$Tag,
        [string[]]$PutB,
        [string[]]$SendB,
        [string[]]$PutA,
        [string[]]$SendA
    )
    # And B does not start talking until A has ITS files: the other half of the
    # wait below, written up at Invoke-Pair in tools\nodetest.ps1 (26 Sep 2026:
    # a delivery to A sent again, B gone by the time A spoke, 12 of 22 checks).
    $readyA = "build\xclutest-$Tag-a.ready"
    Remove-Item $readyA -ErrorAction SilentlyContinue
    $SendB = @("!powershell -NoProfile -Command `"for (;;) { if (Test-Path $readyA) { break }; Start-Sleep -Milliseconds 500 }`"") + @($SendB)
    $runner = "build\xclutest-$Tag-b.ps1"
    $q = { param($a) '@(' + (($a | ForEach-Object { "'$_'" }) -join ',') + ')' }
    $lines = @("Set-Location '$root'",
               "& '$(Join-Path $PSScriptRoot 'qemu-boot.ps1')' ``")
    $lines += "    -Port 5586 -TimeoutSec $TimeoutSec -NoNet ``"
    $lines += "    -LinkPort $linkPort -LinkListen -Chip 'esp32c3' ``"
    $lines += "    -Flash '$flashB' -FlashAsIs -KeepOtherEmulators ``"
    $lines += "    -Tag 'b-$Tag' -LogPath 'build\xclutest-$Tag-b.log' ``"
    $lines += "    -Put $(& $q $PutB) ``"
    $lines += "    -Send $(& $q $SendB)"
    $lines += 'exit $LASTEXITCODE'
    Set-Content -Path $runner -Value $lines -Encoding ascii

    $procB = Start-Process -FilePath 'powershell' -PassThru -NoNewWindow `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                        '-File', ('"{0}"' -f (Join-Path (Get-Location) $runner))) `
        -RedirectStandardOutput "build\xclutest-$Tag-b.out" `
        -RedirectStandardError "build\xclutest-$Tag-b.err"
    Start-Sleep -Milliseconds 500
    if ($procB.HasExited) {
        throw ("node B exited at once (code $($procB.ExitCode)); " +
               "see build\xclutest-$Tag-b.err")
    }

    # A DOES NOT START UNTIL B HAS ITS FILES, for the reason written up at
    # Invoke-Pair in tools\nodetest.ps1: both guests deliver their own over the
    # console, B's are the many, and a transfer that has to be retried costs
    # about two minutes - after which A is asking a node whose listener has not
    # started.  Waiting for what B SAYS rather than for a number of seconds.
    if ($PutB.Count -gt 0) {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        $ready = $false
        while ((Get-Date) -lt $deadline -and -not $procB.HasExited) {
            $out = Get-Content "build\xclutest-$Tag-b.out" -Raw `
                -ErrorAction SilentlyContinue
            if ($out -and $out.Contains('--- deliveries done ---')) {
                $ready = $true
                break
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $ready) {
            Write-Host ('node B has not finished its deliveries; starting A ' +
                        'anyway') -ForegroundColor Yellow
        }
    }

    try {
        & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port 5576 `
            -TimeoutSec $TimeoutSec -LinkPort $linkPort -KeepOtherEmulators `
            -Flash "build\qemu_flash_xclu_a.bin" `
            -Tag "a-$Tag" -LogPath "build\xclutest-$Tag-a.log" `
            -ReadyFile $readyA `
            -Put $PutA -Send $SendA | Out-Null
    } finally {
        if (-not $procB.WaitForExit($TimeoutSec * 2000)) {
            Write-Host 'node B did not finish; stopping it.' -ForegroundColor Yellow
            Stop-Process -Id $procB.Id -Force -ErrorAction SilentlyContinue
        }
        if ((Test-Path "build\xclutest-$Tag-b.err") -and
            (Get-Item "build\xclutest-$Tag-b.err").Length -gt 0) {
            Write-Host "--- build\xclutest-$Tag-b.err ---" -ForegroundColor DarkGray
            Get-Content "build\xclutest-$Tag-b.err" | Select-Object -Last 12 |
                Out-Host
        }
        Stop-Emulators
        Start-Sleep -Milliseconds 500
    }
}

# The polls that hold node B's shell open while A does its half.  SIX SHORT
# ONES rather than one long one, and the reason is what a transcript is worth:
# a single `link poll 120000` prints nothing for two minutes, so a node that
# stopped at second twenty and a node that worked throughout leave the same
# transcript.  Each of these returns a prompt, which is a heartbeat.  Then the
# frame counters and the journal, so that a failure carries its own evidence.
$hold = @('link poll 20000', 'link poll 20000', 'link poll 20000',
          'link poll 20000', 'link poll 20000', 'link poll 20000',
          'link', 'log -n 40')

# ---- 1. the line, and the line crossed by identity ------------------------

Write-Host "`n== an Xtensa node and a RISC-V node, one cluster ==" `
    -ForegroundColor Cyan

# NODEHIRV GOES ON C:, and that is not a preference.  ag_loader_find_by_name
# searches /sys - the internal flash, which is C: - and nowhere else, because
# that is where a board keeps the images it can run.  Put on T: it is invisible
# to the substitution and the far node answers "no build of that program here",
# which is true and reads like the cascade is broken.
#
# TWO PROGRAMS, TWO RUNGS, ONE SESSION.  Node B is given a native build of its
# own of one program (NODEHI) and only the portable build of the other
# (UNSIGNED), so the same node answers the same kind of ask two different ways
# within a few seconds - which is the cascade being a cascade rather than a
# single trick, and cannot be shown by two sessions of one run each.
Invoke-XPair -Tag 'run' `
    -PutB @('build\apps\NODELNRV.SYS=t:\nodelnrv.sys',
            'build\apps\NODEHIRV.AXE=c:\nodehirv.axe',
            'build\apps\NODEHI.AXE=t:\nodehi.axe',
            'build\apps\NODEHI.AXE=t:\xeno.axe',
            'build\apps\AGVMRV.AXE=c:\agvm.axe',
            "$bank\bc-unsigned.bc=c:\unsigned.bc",
            "$bank\xt-unsigned.axe=t:\xeno2.axe") `
    -SendB (@('drv load t:\nodelnrv.sys',
              'link name c3',
              'link use node0',
              # An image for the other processor, run where it cannot run.
              'run t:\nodehi.axe',
              'log -n 14') + $hold) `
    -PutA @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
    -SendA @('drv load t:\nodelink.sys',
             'link name s3',
             'link use node0',
             "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
             'link nodes',
             '=cores in all',
             # THE FAR END, ASKED SOMETHING IT CANNOT ANSWER FROM MEMORY.
             # `link nodes` prints what the hello exchange left behind and
             # would look the same if the wire had died since.
             'link devs',
             '=are in this registry now',
             # A IS NOT GIVEN THIS FILE.  Placement sends the run to whichever
             # node has it, which is B - and B cannot execute it.  RUNG ONE:
             # B has a build of its own.
             'run t:\xeno.axe',
             '=nodehi: here',
             # RUNG THREE, on the same node without resetting anything: this
             # program B can neither execute nor build, and what it has is the
             # portable one.
             'run t:\xeno2.axe',
             '=shr20')

$a = Transcript 'build\xclutest-run-a.log'
$b = Transcript 'build\xclutest-run-b.log'

# ------------------------------------------------------------- the checks ---

# Two machines, and each says which it is.  Without this every check below
# could be passing on two copies of the same guest.
Check $a 'esp32s3' 'xclu-node-a-is-xtensa'
Check $b 'esp32c3' 'xclu-node-b-is-riscv'

Check $a 'link is node0' 'xclu-wire-bound-on-a'
Check $b 'link is node0' 'xclu-wire-bound-on-b'
Check $a '2 nodes' 'xclu-both-nodes-visible'
Check $a 'c3' 'xclu-far-node-named'
# A round trip at that moment, and a real cluster feature in its own right:
# the neighbour's device registry, fetched over the wire between two machines
# that share no instruction set.
Check $a 'are in this registry now' 'xclu-far-registry-came-over-the-wire'

# THE LINE, and that it is refused rather than crossed by accident.
Note (-not (Flat $b).Contains('Guru') -and -not (Flat $b).Contains('panic')) `
    'xclu-foreign-image-did-not-crash-it'
# In the journal, not on the screen: the refusal always happens, but whether
# its words are still visible depends on what else wanted the screen in the
# same half second.  This tree wrote that rule down for driver messages and it
# holds here for the same reason.
Check $b 'built for a different processor' 'xclu-foreign-image-refused-in-words'

# THE LINE CROSSED - by sending the program rather than the code.  A asks for
# an image it has not got; B has it, cannot execute it, reads the PROGRAM NAME
# out of the header and runs its own build of that program.
Check $a 'not here' 'xclu-run-crossed-the-wire'
Check $a 'nodehi: here' 'xclu-far-node-ran-its-own-build'
# NOT "the refusal is absent": the first attempt IS the refusal, and that is
# what starts the substitution.  What must not survive is the asker being told
# the run came to nothing.
Note (-not (Flat $a).Contains('did not run there either')) `
    'xclu-asker-was-not-told-it-failed'

# THE SAME NODE SERVED A SECOND ASK, and said so in the words a local run uses.
#
# This line is the one the fault at the top of this file was made of: it did
# not exist, so a scripted remote run had nothing to finish on and waited out
# its whole timeout while the far node's script ran out.  Checked here rather
# than assumed, because everything below it depends on the session still being
# alive.
Check $a 'returned 0 from node0' 'xclu-remote-run-reported-its-exit'

# ---- 2. and the rung below that, on the same node, seconds later ----------
#
# The far node cannot execute the image it was handed AND has no build of its
# own it can execute either.  What it has is the portable one, and that is the
# whole of the third rung: it runs, and the answers come back.
#
# unsigned.c is the program to use: its seven answers are the ones where
# signedness changes the result, so "it ran" and "it ran correctly" are the
# same check.

# Its own words for the choice, not the file transfer: "running my own
# /sys/unsigned.bc".  The first draft of this check looked for the name alone
# and passed on the line where the file was DELIVERED, which proved nothing.
CheckTight $b 'running my own /sys/unsigned.bc' `
    'xclu-bc-far-node-chose-the-portable-build'
# On the ASKING node, because that is where the far node's loader line arrives
# along with everything else the program said.
Check $a 'AGVM: riscv32' 'xclu-bc-interpreter-loaded-there'
# The seven where signedness changes the answer, so "it ran" and "it ran
# correctly" are one check.  On the ASKING node, because that is where the far
# node's output has to arrive for any of this to be worth anything.
foreach ($k in @('shr 08000000', 'shr_s f8000000', 'div 55555555',
                 'rem 5', 'gt yes', 'gt_s no', 'shr20 00000800')) {
    Check $a $k ('xclu-bc-' + ($k -replace '\s+', '-'))
}

$failed = @($results | Where-Object { $_ -like 'FAIL *' })
Write-Host ''
Write-Host ("xclutest: {0} checks, {1} failed" -f $results.Count, $failed.Count)
Write-Host 'target left at esp32s3.' -ForegroundColor DarkGray
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
