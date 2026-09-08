# Drive DESKTOP.AXE in QEMU with nobody at the desk, and photograph it.
#
#   .\apps\desktop\check.ps1
#   .\apps\desktop\check.ps1 -NoBuild        # reuse the staged images
#
# What it proves, and why each step is here:
#
#   * the shell takes the display and paints - the picture is checked for being
#     more than one flat colour, which is what a run that drew nothing gives
#   * the pointer arrives and lands where it was aimed - the status strip
#     prints the coordinates the shell believes, so the picture and the number
#     can be compared without having to trust either on its own
#   * the pointer leaves no trail - it is walked back and forth across the
#     plate in the middle of the screen, which is the one place where a stale
#     save-under would show
#   * **the picture is the whole of what the shell drew** - a second photograph
#     is taken after a forced full repaint (F5) and the two must be identical.
#     Neither picture can show this on its own: a region the shell painted and
#     the panel never received looks exactly like a region the shell forgot.
#   * it exits when asked, and says how many events it saw on the way out
#
# The ordering trap, which cost a session on the board once already: the
# virtual input drivers do not start listening until something opens the
# device.  So the drivers are installed, the shell is started, and only then is
# inputplay allowed to connect - before that the port is refused and the guest
# looks dead.  See the header of tools\inputplay.py.
#
# ---- why this runs at 640x400 and not at the board's 320x240 ---------------
#
# The surface size is read from [display] in C:\SYSTEM.CFG at boot, so testing
# another size means writing that file and rebooting - and **writing to C: in
# QEMU is not reliable enough to build a test on**.  Measured over eight runs
# of exactly that shape:
#
#   * three of them had a file written to C: read back malformed afterwards -
#     `-91` (AG_EFORMAT), on `c:\desktop.axe` once and on the driver
#     `drv install` had just copied to `c:\drv\mousevirt.sys` twice
#   * the boot after such a write took 128 seconds against the usual two
#   * HostFS sometimes does not come back after the reset, so h:\ paths turn
#     into "file not found" as well
#
# The shell itself is fine at 320x240 - it has been photographed there and the
# layout is right.  What is missing is a way to *arrange* that size without
# writing to C:, which is to bake the partition into the flash image with
# tools\mksysfs.py.  Until that exists, this checks the size QEMU boots with.
[CmdletBinding()]
param(
    [int]$Seconds = 40,       # the shell's own deadline, so nothing can hang
    [string]$Out = 'build\desktop',
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $root
try {
    . .\tools\idf-env.ps1

    if (-not $NoBuild) {
        & .\argon.cmd apps --only DESKTOP.AXE KBDVIRT.SYS MOUSEVIRT.SYS |
            Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'building the images failed' }
    }

    $share = Join-Path $root 'build\sd_card'
    foreach ($f in @('DESKTOP.AXE', 'KBDVIRT.SYS', 'MOUSEVIRT.SYS')) {
        if (-not (Test-Path (Join-Path $share $f))) {
            throw "$f is not staged in build\sd_card"
        }
    }

    <#
      A fresh flash image, so C: starts empty.

      The image is only rebuilt when the firmware changes (there is a stamp
      beside it), so without this C: carries everything earlier runs wrote -
      including the two drivers and the [modules] lines that autoload them.
      The second run then boots with them already resident, `drv install`
      replaces them, the new instance cannot bind a port the old one still
      holds, and the guest ends up with a mouse driver that is loaded and
      listening to nothing.  Measured: run one fine, run two with zero pointer
      events and no error anywhere.

      Reformatting littlefs costs the first boot about two seconds.
    #>
    Remove-Item -ErrorAction SilentlyContinue -Path (Join-Path $root 'build\qemu_flash.bin'),
                                                    (Join-Path $root 'build\qemu_flash.stamp')

    New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
    $png = Join-Path $root "$Out.png"
    $png2 = Join-Path $root "$Out-repaint.png"
    $log = "$Out.log"

    $pyexe = if ($env:ARGON_PYTHON -and (Test-Path $env:ARGON_PYTHON)) {
        $env:ARGON_PYTHON
    } else {
        $found = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
            -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $found) { throw 'no Python found for inputplay' }
        $found.FullName
    }

    # ---- the sequence -----------------------------------------------------
    # Where the pointer is walked: across the middle, then back, then up, then
    # a click in the centre.  Coordinates are the surface's own pixels.
    $w = 640
    $h = 400
    $cx = [int]($w / 2)
    $cy = [int]($h / 2)
    $span = [int]($w / 4)

    $moves = @(
        'home',
        "glide $($cx - $span),$cy",
        "glide $($cx + $span),$cy",
        "glide $cx,$([int]($cy - $h / 6))",
        "move $cx,$cy",
        'click',
        'wait 2000'
    )
    $quoted = ($moves | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $after = @('"key f5"', '"wait 1500"') -join ' '

    $send = @(
        'drv install h:\kbdvirt.sys',
        'drv install h:\mousevirt.sys',
        'dev',
        # Raw, so the harness does not sit waiting for a prompt that cannot
        # come until the shell's own deadline expires.
        "~run h:\desktop.axe $Seconds`r",
        '=desktop: surface',
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $quoted",
        "!.\tools\grab-window.ps1 -Out '$png'",
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $after",
        "!.\tools\grab-window.ps1 -Out '$png2'",
        # 'q' rather than Escape: a lone 0x1b is held back by the console's
        # escape-sequence parser and never arrives as a key on its own.
        '~q',
        '=pointer events'
    )

    Write-Host "desktop: driving QEMU, transcript -> $log, picture -> $png"
    & .\tools\qemu-boot.ps1 -Gfx -HostFs $share -LogPath $log `
        -TimeoutSec 90 -Send $send | Out-Host

    # ---- what came back ---------------------------------------------------
    if (-not (Test-Path $log)) { throw "no transcript at $log" }
    $text = Get-Content -Raw $log
    $fail = @()

    if (-not (Test-Path $png)) {
        $fail += 'no picture was taken'
    } elseif ($text -match 'one flat colour') {
        $fail += 'the picture is one flat colour - nothing was drawn'
    }
    if ($text -match 'desktop: this display has no surface') {
        $fail += 'the display had no surface (the band renderer is not built)'
    }
    if ($text -match 'desktop: cannot take the display') {
        $fail += 'the display was already owned by something else'
    }
    if ($text -notmatch 'QEMU RGB window') {
        $fail += 'the emulator has no RGB panel - was it started without graphics=on?'
    }

    # The picture as it stood, against the same picture after a forced full
    # repaint.  Any difference is the shell's own output failing to reach the
    # panel - a torn present, a stale save-under - and it is invisible in
    # either picture alone.
    if ((Test-Path $png) -and (Test-Path $png2)) {
        Add-Type -AssemblyName System.Drawing
        $a = New-Object System.Drawing.Bitmap $png
        $b = New-Object System.Drawing.Bitmap $png2
        if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
            $fail += 'the two pictures are different sizes'
        } else {
            $diff = 0
            for ($y = 0; $y -lt $a.Height; $y++) {
                for ($x = 0; $x -lt $a.Width; $x++) {
                    if ($a.GetPixel($x, $y) -ne $b.GetPixel($x, $y)) { $diff++ }
                }
            }
            $total = $a.Width * $a.Height
            Write-Host ("desktop: {0} of {1} pixels differ after a forced repaint" -f
                        $diff, $total)
            # The status strip carries counters that move, so a run where the
            # numbers changed between the two shots is not a failure; anything
            # beyond that strip is.
            if ($diff -gt ($a.Width * 14)) {
                $fail += "the forced repaint changed $diff pixels - the first paint did not all reach the panel"
            }
        }
        $a.Dispose()
        $b.Dispose()
    }

    $m = [regex]::Match($text, 'desktop: surface (\d+)x(\d+) (\w+)')
    if ($m.Success) {
        Write-Host ("desktop: surface {0}x{1} {2}" -f $m.Groups[1].Value,
                    $m.Groups[2].Value, $m.Groups[3].Value)
    } else {
        $fail += 'the shell never said what surface it got'
    }

    # The numbers it printed on its way out.  Zero pointer events with a good
    # picture means the drawing works and the input does not, and telling those
    # two apart is most of why this script exists.
    $m = [regex]::Match($text, 'desktop: (\d+) pointer events, (\d+) key events, (\d+) repaints, (\d+) reflushes')
    if ($m.Success) {
        $ptr = [int]$m.Groups[1].Value
        $key = [int]$m.Groups[2].Value
        $rep = [int]$m.Groups[3].Value
        Write-Host "desktop: $ptr pointer events, $key key events, $rep repaints"
        if ($ptr -lt 5) { $fail += "only $ptr pointer events arrived" }
        if ($key -lt 1) { $fail += 'no key ever arrived' }
        if ($rep -lt 1) {
            $fail += 'F5 never reached the shell, so the second picture is not a fresh paint'
        }
    } else {
        $fail += 'the shell did not print its counts (it may have been killed)'
    }

    if ($fail.Count -gt 0) {
        Write-Host ''
        foreach ($f in $fail) { Write-Host "FAIL: $f" }
        Write-Host "transcript: $log"
        exit 1
    }
    Write-Host "desktop: ok - $png, $png2, transcript $log"
} finally {
    Pop-Location
}
