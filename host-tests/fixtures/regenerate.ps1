# Rebuilds the loader test fixtures.
#
#   . .\tools\idf-env.ps1
#   .\host-tests\fixtures\regenerate.ps1
#
# Four images from one source.  Each pair is the same application linked at
# different addresses: the test loads one at the other's addresses and requires
# the result to match byte for byte, which is what proves the relocation right.
# Both members of a pair have to come from the same compiler run, and all four
# are committed so the test needs no cross-compiler.
#
# The split pair moves its two parts by *different* distances - code by
# 0x123000, data by 0x456000 - because a loader that used one bias for both
# would pass a test where the two distances happened to match.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$python = if ($env:ARGON_PYTHON) { $env:ARGON_PYTHON } else { 'python' }

$variants = @(
    @{ Out = 'sample-nominal.axe';         Code = '0x42000000'; Data = '0x3C000000'; Contiguous = $false },
    @{ Out = 'sample-relinked.axe';        Code = '0x42123000'; Data = '0x3C456000'; Contiguous = $false },
    @{ Out = 'sample-contig.axe';          Code = '0x42000000'; Data = '0x3C000000'; Contiguous = $true },
    @{ Out = 'sample-contig-relinked.axe'; Code = '0x42123000'; Data = '0x3C456000'; Contiguous = $true }
)

foreach ($v in $variants) {
    # An explicit argument list, not a hashtable splat: splatting an array in
    # PowerShell passes the values positionally and they arrive as the wrong
    # options.
    $argv = @(
        'tools\mkaxe.py',
        '--arch', 'xtensa',
        '--gcc', 'xtensa-esp32s3-elf-gcc',
        '--include', 'sdk/include',
        '--code-base', $v.Code,
        '--data-base', $v.Data,
        '-o', (Join-Path 'host-tests\fixtures' $v.Out),
        'host-tests\fixtures\sample.c'
    )
    if ($v.Contiguous) { $argv += '--contiguous' }

    & $python @argv
    if (-not $?) { throw "mkaxe failed for $($v.Out)" }
}
