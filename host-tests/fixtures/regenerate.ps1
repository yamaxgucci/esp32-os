# Rebuilds the loader test fixtures.
#
#   . .\tools\idf-env.ps1
#   .\host-tests\fixtures\regenerate.ps1
#
# Two images from one source at different link addresses.  The test loads one at
# the other's address and requires the result to match byte for byte, which is
# what proves the relocation right - so both have to come from the same compiler
# run, and both are committed so the test needs no cross-compiler.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$python = if ($env:ARGON_PYTHON) { $env:ARGON_PYTHON } else { 'python' }

foreach ($pair in @(@('0x42000000', 'sample-nominal.axe'),
                    @('0x3C123000', 'sample-relinked.axe'))) {
    & $python tools\mkaxe.py `
        --arch xtensa --gcc xtensa-esp32s3-elf-gcc `
        --include sdk/include --link-base $pair[0] `
        -o (Join-Path 'host-tests\fixtures' $pair[1]) `
        host-tests\fixtures\sample.c
    if (-not $?) { throw "mkaxe failed for $($pair[1])" }
}
