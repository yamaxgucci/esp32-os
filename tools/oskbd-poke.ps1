# ArgonOS - type a line on the desktop's own keyboard, with the mouse.
#
# The one thing in the shell that no keyboard pass can prove: the keys drawn
# on the glass.  On the board they are the ONLY way to type - there is no
# keyboard on that desk - so "it works" has meant "Maxim tried it" until now.
#
# Where the keys are is asked of the program, not worked out here.  The
# desktop prints its geometry at startup and again when a prompt window
# opens, and this reads those lines out of the transcript.  A test that
# computes a layout for itself is a second copy of that layout, and the copy
# is wrong the day the real one changes: apps/desktop/check.ps1 has already
# paid for that, with a step that counted arrow keys into a menu which had
# grown an item, and it cost three runs and a false accusation of the kernel.
#
# The grid inside the key rectangle IS repeated here - ten columns, five
# rows - because there is no sensible way for a program to publish sixty
# rectangles.  It is the smallest thing that can drift, and it cannot drift
# quietly: a click that lands on the neighbouring key types the wrong
# character, and the directory this makes is checked by name.
#
# Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Log,
    [Parameter(Mandatory)][string]$Python,
    [string]$Line = 'md c:\bykeys'
)

$ErrorActionPreference = 'Stop'

# The layout, as dsk_kbd.c spells it.  Row four's space bar is four cells
# wide and is written as four spaces there, so any of them will do here.
$rows = @(
    '1234567890',
    'qwertyuiop',
    'asdfghjkl.',
    'zxcvbnm-_:',
    "^\    /*?#"
)

function Get-LastMatch {
    param([string]$text, [string]$pattern)
    $all = [regex]::Matches($text, $pattern)
    if ($all.Count -eq 0) { return $null }
    return $all[$all.Count - 1]
}

# The transcript arrives full of escape codes and line breaks; the numbers
# survive both, so they are what is looked for.
$text = (Get-Content -Raw $Log) -replace "`e\[[0-9;?]*[A-Za-z]", '' -replace "`r?`n", ''

$keys = Get-LastMatch $text 'keyboard keys (-?\d+),(-?\d+) (\d+)x(\d+) row (-?\d+),(-?\d+) (\d+)x(\d+)'
if ($null -eq $keys) { throw 'oskbd-poke: the desktop never said where its keyboard is' }
$win = Get-LastMatch $text 'prompt window at (-?\d+),(-?\d+) (\d+)x(\d+)'
if ($null -eq $win) { throw 'oskbd-poke: no prompt window to type into' }

$kx = [int]$keys.Groups[1].Value; $ky = [int]$keys.Groups[2].Value
$kw = [int]$keys.Groups[3].Value; $kh = [int]$keys.Groups[4].Value
$rx = [int]$keys.Groups[5].Value; $ry = [int]$keys.Groups[6].Value
$rw = [int]$keys.Groups[7].Value; $rh = [int]$keys.Groups[8].Value

$wx = [int]$win.Groups[1].Value; $wy = [int]$win.Groups[2].Value
$ww = [int]$win.Groups[3].Value; $wh = [int]$win.Groups[4].Value

# The middle of a cell, given the grid dsk_kbd.c draws: ten across, five
# down, one pixel of margin at the top, the height floored.
$cols = 10
$rowsN = 5
$cellW = [int][math]::Floor($kw / $cols)
$cellH = [int][math]::Floor(($kh - 2) / $rowsN)

function Cell-Point {
    param([int]$row, [int]$col)
    return @(($kx + $col * $cellW + [int]($cellW / 2)),
             ($ky + 1 + $row * $cellH + [int]($cellH / 2)))
}

$ops = @()

# A TAP on the text, which is what raises the keyboard - not a press, and
# not the frame.  Near the top of the client area, well above where the
# keys themselves are about to appear.
$tapX = $wx + [int]($ww / 2)
$tapY = $wy + 8
$ops += "move $tapX,$tapY", 'wait 200', 'press', 'wait 120', 'release', 'wait 600'

foreach ($ch in $Line.ToCharArray()) {
    $found = $false
    for ($r = 0; $r -lt $rowsN -and -not $found; $r++) {
        $c = $rows[$r].IndexOf($ch)
        if ($c -ge 0) {
            $p = Cell-Point $r $c
            $ops += "move $($p[0]),$($p[1])", 'wait 80', 'press', 'wait 80',
                    'release', 'wait 140'
            $found = $true
        }
    }
    if (-not $found) { throw "oskbd-poke: '$ch' is not on this keyboard" }
}

# Enter is the first of the three wide keys, Hide the last.
$enterX = $rx + [int]($rw / 6)
$hideX = $rx + [int](5 * $rw / 6)
$rowY = $ry + [int]($rh / 2)
$ops += "move $enterX,$rowY", 'wait 150', 'press', 'wait 100', 'release',
        'wait 1200'
$ops += "move $hideX,$rowY", 'wait 150', 'press', 'wait 100', 'release',
        'wait 600'

Write-Host "oskbd-poke: keys at $kx,$ky ${kw}x${kh}; typing '$Line' with the mouse"
$quoted = ($ops | ForEach-Object { '"' + $_ + '"' }) -join ' '
$cmd = "& '$Python' 'tools\inputplay.py' --wait 20 $quoted"
Invoke-Expression $cmd
if ($LASTEXITCODE -ne 0) { throw "oskbd-poke: inputplay failed ($LASTEXITCODE)" }
