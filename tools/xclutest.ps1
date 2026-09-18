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
# ONE THING SEEN HERE IS NOT EXPLAINED, and it is written down rather than
# tidied away.  Twice in nine runs, while this scenario was still being
# written, node A's remote run answered -60 (AG_ETIMEDOUT) after the full
# sixty seconds while everything else about the wire was healthy: the hello
# exchange had named the far node and counted its cores, and node B's journal
# showed nothing at all for the whole of that minute.  It has not happened in
# the five runs since the shape below (six short polls, and a live round trip
# before the ask), but five runs is not a proof and nothing was changed in the
# system - only in what this script asks for.
#
# So the scenario is arranged to carry its own evidence next time: `link devs`
# proves the wire is alive AT THAT MOMENT rather than from memory, the polls
# are short enough to leave a heartbeat in the transcript, and node B ends by
# printing its frame counters and its journal.  A failure here should say
# whether the request arrived and was dropped or never arrived.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 300,
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
    return ($raw -replace "`e\[[0-9;]*[A-Za-z]", "`n")
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
$only = @('NODELINK.SYS', 'NODELNRV.SYS', 'NODEHI.AXE', 'NODEHIRV.AXE')
& python (Join-Path $PSScriptRoot 'build_apps.py') --only @only | Out-Null
foreach ($img in $only) {
    if (-not (Test-Path "build\apps\$img")) { throw "$img did not build." }
}

Stop-Emulators
Start-Sleep -Milliseconds 500

# --------------------------------------------------------------- the pair ---
#
# B in a process of its own, driven by a generated script.  The quoting
# problems that shape makes unavoidable are written up at Invoke-Pair in
# tools\nodetest.ps1; the difference here is that the two guests are two
# different machines, so B carries its own -Chip and a flash image built before
# the target moved on.

$runner = 'build\xclutest-b.ps1'
$lines = @("Set-Location '$root'",
           "& '$(Join-Path $PSScriptRoot 'qemu-boot.ps1')' ``")
$lines += "    -Port 5586 -TimeoutSec $TimeoutSec -NoNet ``"
$lines += "    -LinkPort $linkPort -LinkListen -Chip 'esp32c3' ``"
$lines += "    -Flash '$flashB' -FlashAsIs -KeepOtherEmulators ``"
$lines += "    -Tag 'b-xclu' -LogPath 'build\xclutest-b.log' ``"
# NODEHIRV GOES ON C:, and that is not a preference.
#
# ag_loader_find_by_name searches /sys - the internal flash, which is C: - and
# nowhere else, because that is where a board keeps the images it can run.  Put
# on T: it is invisible to the substitution and the far node answers "no build
# of that program here", which is true and reads like the cascade is broken.
#
# The two foreign images stay on T:, which is a RAM disk and the right place
# for something that is only there to be refused.
$lines += ("    -Put @('build\apps\NODELNRV.SYS=t:\nodelnrv.sys'," +
           "'build\apps\NODEHIRV.AXE=c:\nodehirv.axe'," +
           "'build\apps\NODEHI.AXE=t:\nodehi.axe'," +
           "'build\apps\NODEHI.AXE=t:\xeno.axe') ``")
$lines += ("    -Send @('drv load t:\nodelnrv.sys'," +
           "'link name c3'," +
           "'link use node0'," +
           # An image for the other processor, run where it cannot run.  Before
           # anything else, so its refusal is in the journal by the time the
           # far node starts asking for things.
           "'run t:\nodehi.axe'," +
           "'log -n 14'," +
           # And then hold the shell while A does its half.  The serving
           # happens on the link's own reader task regardless; this only keeps
           # the guest alive, because qemu-boot stops an emulator once its
           # command list is done and a node that has gone looks exactly like
           # one that never answered.
           # SIX SHORT POLLS RATHER THAN ONE LONG ONE, and the reason is
           # what a transcript is worth: a single `link poll 115000` prints
           # nothing for two minutes, so a node that stopped at second twenty
           # and a node that was working throughout leave the SAME
           # transcript.  Each of these returns a prompt, which is a
           # heartbeat.
           "'link poll 20000','link poll 20000','link poll 20000'," +
           "'link poll 20000','link poll 20000','link poll 20000'," +
           # The frame counters, so that a failure carries its own evidence:
           # whether something arrived and was thrown away, or never arrived.
           "'link'," +
           # AND THE JOURNAL AFTERWARDS, which is the only place the far
           # node's side of a remote run is written down: the program it
           # substituted, or the reason it could not.  Without this the
           # evidence for the one thing this scenario exists to show is on the
           # asking node only, as a number.
           "'log -n 40')")
$lines += 'exit $LASTEXITCODE'
Set-Content -Path $runner -Value $lines -Encoding ascii

Write-Host "`n== an Xtensa node and a RISC-V node, one cluster ==" `
    -ForegroundColor Cyan

$procB = Start-Process -FilePath 'powershell' -PassThru -NoNewWindow `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                    '-File', ('"{0}"' -f (Join-Path (Get-Location) $runner))) `
    -RedirectStandardOutput 'build\xclutest-b.out' `
    -RedirectStandardError 'build\xclutest-b.err'
Start-Sleep -Milliseconds 500
if ($procB.HasExited) {
    throw ("node B exited at once (code $($procB.ExitCode)); " +
           "see build\xclutest-b.err")
}

try {
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port 5576 `
        -TimeoutSec $TimeoutSec -LinkPort $linkPort -KeepOtherEmulators `
        -Flash 'build\qemu_flash_xclu_a.bin' `
        -Tag 'a-xclu' -LogPath 'build\xclutest-a.log' `
        -Put @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -Send @('drv load t:\nodelink.sys',
                'link name s3',
                'link use node0',
                "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                'link nodes',
                '=cores in all',
                # THE FAR END, ASKED SOMETHING IT CANNOT ANSWER FROM MEMORY.
                #
                # `link nodes` prints what the hello exchange left behind, and
                # would look exactly the same if the wire had died since.
                # Pulling the neighbour's device registry is a round trip NOW,
                # so a failure below this line is about the run path rather
                # than about the wire - which is a distinction this scenario
                # had to make once already.
                'link devs',
                '=are in this registry now',

                # A IS NOT GIVEN THIS FILE.  Placement sends the run to
                # whichever node has it, which is B - and B cannot execute it.
                'run t:\xeno.axe',
                '=nodehi: here') | Out-Null
} finally {
    if (-not $procB.WaitForExit($TimeoutSec * 2000)) {
        Write-Host 'node B did not finish; stopping it.' -ForegroundColor Yellow
        Stop-Process -Id $procB.Id -Force -ErrorAction SilentlyContinue
    }
    if ((Test-Path 'build\xclutest-b.err') -and
        (Get-Item 'build\xclutest-b.err').Length -gt 0) {
        Write-Host '--- build\xclutest-b.err ---' -ForegroundColor DarkGray
        Get-Content 'build\xclutest-b.err' | Select-Object -Last 12 | Out-Host
    }
    Stop-Emulators
    Start-Sleep -Milliseconds 500
}

$a = Transcript 'build\xclutest-a.log'
$b = Transcript 'build\xclutest-b.log'

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

$failed = @($results | Where-Object { $_ -like 'FAIL *' })
Write-Host ''
Write-Host ("xclutest: {0} checks, {1} failed" -f $results.Count, $failed.Count)
Write-Host 'target left at esp32s3.' -ForegroundColor DarkGray
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
