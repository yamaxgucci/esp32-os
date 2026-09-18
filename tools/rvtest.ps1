# The machines that are not this one, end to end, with no board plugged in.
#
#   .\argon.cmd rvtest
#
# Three claims, and they are separate:
#
#   1. A program the board's own compiler wrote for RISC-V RUNS on a RISC-V
#      part, and gives the same answers the Xtensa build gives.  Checked on an
#      emulated ESP32-C3 - Espressif's QEMU has that machine, which this tree
#      only started using on 18 Sep 2026; before that every RISC-V result here
#      began with "plug the C3 in".
#
#   2. The board can WRITE one.  An emulated S3 runs CC.AXE with -riscv over
#      the same source and the image it produces is compared, byte for byte,
#      against the one the host build of the same compiler wrote.  That is the
#      cluster's second rung: the board with the memory to compile does it for
#      the board that has not got it - CC wants three megabytes of heap, and no
#      C3 will ever have them.
#
#   3. And the third rung: ONE image that both machines run.  The same .bc
#      file, byte for byte, is handed to AGVM.AXE on the Xtensa guest and to
#      AGVMRV.AXE on the RISC-V one, and the answers have to be identical -
#      including the seven where signedness changes the answer, which is where
#      an interpreter that took a shortcut would show.
#
# IT SWITCHES THE TARGET TWICE, because the two halves are two different chips
# and one build directory.  It ends on esp32s3, the primary target, so the rest
# of the gate can run after it without a third rebuild.  Expect twenty minutes.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 300,
    # Skip the target switches and use what is already built.  For iterating on
    # one half; a green run of that is NOT a green run of this.
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

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
    return ($raw -replace "$([char]27)\[[0-9;]*[A-Za-z]", "`n")
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    Note ($Text.Contains($Needle)) $Name `
        $(if (-not $Text.Contains($Needle)) { "no '$Needle'" })
}

function Set-Target {
    param([string]$Name)
    if ($NoBuild) { return }
    Write-Host "`n-- building for $Name --" -ForegroundColor DarkGray
    & "$root\argon.cmd" target $Name | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "argon target $Name failed." }
    # Twice, and the second one is not superstition: a new symbol in the
    # defaults makes the first configure write a fresh sdkconfig and say
    # "Missing kconfig option.  Re-run the build process" - after which what is
    # in build\ was made from the config before the change.  The second run is
    # a ninja no-op when the first was enough.
    for ($i = 0; $i -lt 2; $i++) {
        & "$root\argon.cmd" build | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "argon build for $Name failed." }
    }
    if (-not (Test-Path 'build\argonos.bin')) {
        throw "argon build for $Name produced no image."
    }
}

# The reference images, cross-compiled here by the host build of the board's
# own compiler.
#
# Written AFTER each target switch and not once at the top, because switching
# target is a fullclean: everything in build\ goes, including these.  That cost
# a run - the first thing the C3 phase did was ask for a file the target switch
# had just deleted.
function New-RvImages {
    New-Item -ItemType Directory -Force -Path 'build' | Out-Null
    foreach ($ex in @('unsigned', 'selftest')) {
        & '.\build-host\cchost.exe' "apps\cc\examples\$ex.c" `
            "build\rv-$ex.axe" -riscv | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "cchost could not compile $ex.c for riscv."
        }
    }
}

# And the bytecode ones, which go in build-bank\ rather than build\ - not for
# tidiness but because THE SAME FILE has to reach both guests.  Switching
# target is a fullclean, so anything in build\ would be compiled twice, and
# "the same program" is a weaker claim than "the same bytes".
$bank = 'build-bank'
function New-BcImages {
    New-Item -ItemType Directory -Force -Path $bank | Out-Null
    foreach ($ex in @('unsigned', 'selftest')) {
        $out = Join-Path $bank "bc-$ex.bc"
        if (Test-Path $out) { continue }
        & '.\build-host\cchost.exe' "apps\cc\examples\$ex.c" $out -bc |
            Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "cchost could not compile $ex.c to bytecode."
        }
    }
}

# The seven answers where signedness changes the answer, and they are the same
# seven whatever the machine.  That is the claim, so it is one table.
$sevenRx = @{
    'shr   08000000' = 'unsigned-shift-right'
    'shr_s f8000000' = 'signed-shift-right'
    'div   55555555' = 'unsigned-divide'
    'rem   5'        = 'unsigned-remainder'
    'gt    yes'      = 'unsigned-compare'
    'gt_s  no'       = 'signed-compare'
    'shr20 00000800' = 'shift-past-fifteen'
}

# The host build of the board's own compiler, which is what both halves are
# measured against.
if (-not (Test-Path 'build-host\cchost.exe')) {
    cmake -S host-tests -B build-host -G Ninja | Out-Null
    cmake --build build-host --target cchost | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cchost did not build.' }
}

# ------------------------------------------- 1. a RISC-V part runs them ---

Set-Target 'esp32c3-qemu'
New-RvImages
# Fresh at the start of the run, and then left alone: phase two must hand the
# far machine THE SAME BYTES, not the same source compiled again.
Remove-Item (Join-Path $bank 'bc-*.bc') -ErrorAction SilentlyContinue
New-BcImages
& python (Join-Path $PSScriptRoot 'build_apps.py') --only AGVMRV.AXE | Out-Null
if (-not (Test-Path 'build\apps\AGVMRV.AXE')) { throw 'AGVMRV.AXE did not build.' }

Write-Host "`n== a program compiled for RISC-V, on a RISC-V part ==" `
    -ForegroundColor Cyan

& (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
    -LogPath 'build\rvtest-c3.log' `
    -Put @('build\rv-unsigned.axe=c:\rvuns.axe',
           'build\rv-selftest.axe=c:\rvst.axe',
           'build\apps\AGVMRV.AXE=c:\agvm.axe',
           "$bank\bc-unsigned.bc=c:\uns.bc",
           "$bank\bc-selftest.bc=c:\st.bc") `
    -Send @('ver',
            'run c:\rvuns.axe',
            'run c:\rvst.axe',
            # And the same two programs again, as bytecode, through the
            # interpreter.  Same source, same answers, no processor.
            'run c:\agvm.axe c:\uns.bc',
            'run c:\agvm.axe c:\st.bc',
            'errorlevel') | Out-Null
Stop-Emulators

$c3 = Transcript 'build\rvtest-c3.log'

Check $c3 'esp32c3' 'rv-emulated-c3-booted'
# The loader's own word for what it was handed.  An image for the wrong machine
# does not refuse to load - it loads and throws an illegal instruction
# somewhere inside - so this line is the only place the arch is ever said.
Check $c3 'UNSIGNED: riscv32' 'rv-loader-named-the-machine'

# The seven answers, and they are the seven the Xtensa build prints.  Each one
# is an operator that emits a DIFFERENT instruction when the value is unsigned,
# which is the class of mistake a disassembler cannot see: SRLI where SRAI
# belonged decodes perfectly and is wrong from 0x80000000 up.
$want = @{
    'shr   08000000' = 'rv-unsigned-shift-right'
    'shr_s f8000000' = 'rv-signed-shift-right'
    'div   55555555' = 'rv-unsigned-divide'
    'rem   5'        = 'rv-unsigned-remainder'
    'gt    yes'      = 'rv-unsigned-compare'
    'gt_s  no'       = 'rv-signed-compare'
    'shr20 00000800' = 'rv-shift-past-fifteen'
}
foreach ($k in $want.Keys) { Check $c3 $k $want[$k] }

# And the whole self-test: arithmetic, arrays, structs, calls, the
# preprocessor, the console.  Its exit code is the number of checks that did
# not hold, so both halves of this are the claim.
Check $c3 'cc selftest' 'rv-selftest-ran'
Note ([regex]::IsMatch($c3, 'failures:[\s\S]{0,40}?\b0\b')) `
    'rv-selftest-no-failures'
Check $c3 'returned 0' 'rv-selftest-exit-zero'

# The third rung, on this machine.  The interpreter says which processor IT is
# built for; what it runs says nothing about a processor at all.
Check $c3 'AGVM: riscv32' 'bc-interpreter-is-riscv-here'
# Twice each, because the native run above printed them once: seven answers
# from native code and seven from bytecode is fourteen occurrences.
foreach ($k in $sevenRx.Keys) {
    $n = ([regex]::Matches($c3, [regex]::Escape($k))).Count
    Note ($n -ge 2) ('bc-c3-' + $sevenRx[$k]) "$n occurrences"
}

# ------------------------------------ 2. and a board can write one ---

Set-Target 'esp32s3'
New-RvImages

Write-Host "`n== a board compiling for the other machine ==" -ForegroundColor Cyan

& python (Join-Path $PSScriptRoot 'build_apps.py') --only CC.AXE AGVM.AXE |
    Out-Null
if (-not (Test-Path 'build\apps\CC.AXE')) { throw 'CC.AXE did not build.' }
if (-not (Test-Path 'build\apps\AGVM.AXE')) { throw 'AGVM.AXE did not build.' }

$share = 'build\rvshare'
New-Item -ItemType Directory -Force -Path $share | Out-Null
Remove-Item (Join-Path $share 'fromboard.axe') -ErrorAction SilentlyContinue

& (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
    -LogPath 'build\rvtest-s3.log' -HostFs $share `
    -Put @('build\apps\CC.AXE=t:\cc.axe',
           'apps\cc\examples\selftest.c=t:\selftest.c',
           'apps\cc\examples\ppinc.h=t:\ppinc.h',
           'build\apps\AGVM.AXE=t:\agvm.axe',
           "$bank\bc-unsigned.bc=t:\uns.bc",
           "$bank\bc-selftest.bc=t:\st.bc") `
    -Send @('run t:\cc.axe -riscv t:\selftest.c h:\fromboard.axe',
            '=cc: wrote',
            'dir h:\',
            # THE SAME BYTES the RISC-V guest ran, on a machine with a
            # different instruction set and a different word order in its
            # registers.  Nothing about these files changed between the two.
            'run t:\agvm.axe t:\uns.bc',
            'run t:\agvm.axe t:\st.bc') | Out-Null
Stop-Emulators

$s3 = Transcript 'build\rvtest-s3.log'

# Said out loud by the compiler, because an image for another machine that
# looks like any other is the mistake this line exists to prevent.
Check $s3 'riscv32' 'rv-board-said-which-machine'

# And the third rung on the other machine.
Check $s3 'AGVM: xtensa' 'bc-interpreter-is-xtensa-here'
foreach ($k in $sevenRx.Keys) {
    Check $s3 $k ('bc-s3-' + $sevenRx[$k])
}
Check $s3 'cc selftest' 'bc-s3-selftest-ran'
Note ([regex]::IsMatch($s3, 'failures:[\s\S]{0,40}?\b0\b')) `
    'bc-s3-selftest-no-failures'

$made = Join-Path $share 'fromboard.axe'
if (-not (Test-Path $made)) {
    Note $false 'rv-board-wrote-an-image' 'nothing on the shared folder'
} else {
    $a = [System.IO.File]::ReadAllBytes($made)
    $b = [System.IO.File]::ReadAllBytes('build\rv-selftest.axe')
    $same = ($a.Length -eq $b.Length)
    if ($same) {
        for ($i = 0; $i -lt $a.Length; $i++) {
            if ($a[$i] -ne $b[$i]) { $same = $false; break }
        }
    }
    # Byte for byte against the host build of the same compiler.  Anything less
    # - a size, a name - would pass for an image that was subtly different, and
    # the whole point is that the two builds of cc_compile.c are one compiler.
    Note $same 'rv-board-image-matches-the-host-build' `
        ("{0} bytes on the board, {1} here" -f $a.Length, $b.Length)
}

# ------------------------------------------------------------------ result ---

$failed = @($results | Where-Object { $_ -like 'FAIL *' })
Write-Host ''
Write-Host ("rvtest: {0} checks, {1} failed" -f $results.Count, $failed.Count)
Write-Host 'target left at esp32s3.' -ForegroundColor DarkGray
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
