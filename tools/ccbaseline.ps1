# Compile every CC example with the host build of the board's own compiler and
# print one hash per file.
#
# This is the safety net under any change to the code generator.  Run it before
# the change, run it after, and diff the two lists: a back end that has been
# taken apart and put back together either produces the same images or it does
# not, and reading the diff of the compiler does not answer that.
#
#   .\tools\ccbaseline.ps1 > build\cc-before.txt
#   ... change the compiler ...
#   .\tools\ccbaseline.ps1 > build\cc-after.txt
#   fc build\cc-before.txt build\cc-after.txt
#
# Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
param(
    [string]$Exe = 'build-host\cchost.exe'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Exe)) {
    throw "$Exe is not built. cmake --build build-host --target cchost"
}

$bad = 0
Get-ChildItem 'apps\cc\examples\*.c' | Sort-Object Name | ForEach-Object {
    # Compiled to nothing: the image is hashed in the tool, and writing a file
    # per example would only make the tree dirty.
    $out = & $Exe $_.FullName '-' 2>&1
    if ($LASTEXITCODE -ne 0) {
        $bad++
        Write-Output ("{0,-16} FAILED {1}" -f $_.Name, ($out -join ' '))
    } else {
        # Only the name and the hash, so the list does not move when the tree
        # does: the tool prints the full path it was given.
        $m = [regex]::Match(($out -join ' '), '(\d+) bytes fnv ([0-9a-f]{8})')
        Write-Output ("{0,-16} {1,8} bytes  {2}" -f $_.Name, $m.Groups[1].Value,
                      $m.Groups[2].Value)
    }
}
if ($bad -gt 0) {
    Write-Output "$bad example(s) did not compile"
    exit 1
}
exit 0
