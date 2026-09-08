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
# ---- how the surface size is chosen ----------------------------------------
#
# C: is built here and merged into the flash image (tools\mksysfs.py, then
# -SysFs), so the whole run is one boot: the surface size is in SYSTEM.CFG
# before anything has started, the two input drivers are already in C:\DRV and
# named in [modules], and the shell itself is on C:.  Nothing is written to
# flash by the guest and nothing is rebooted.
#
# That is not tidiness.  The obvious version - write C:\SYSTEM.CFG from the
# shell and reboot - was tried and is not reliable enough to build a test on:
# over eight runs of that shape, three had a file written to C: read back
# malformed afterwards (`-91`, AG_EFORMAT: once `c:\desktop.axe`, twice a
# driver `drv install` had just copied), the boot after such a write took 128
# seconds against the usual two, and HostFS did not always come back after the
# reset either.
[CmdletBinding()]
param(
    # The surface to give the shell.  320x240 is what the boards have; the
    # default is what the firmware boots with when [display] says nothing.
    [int]$Width = 0,
    [int]$Height = 0,
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

    foreach ($f in @('DESKTOP.AXE', 'KBDVIRT.SYS', 'MOUSEVIRT.SYS')) {
        if (-not (Test-Path (Join-Path $root "build\apps\$f"))) {
            throw "$f is not built (build\apps\$f)"
        }
    }

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

    # ---- C: ---------------------------------------------------------------
    $work = Join-Path $root 'build\dsk'
    New-Item -ItemType Directory -Force $work | Out-Null
    @('; built by apps\desktop\check.ps1 - the [display] section is appended',
      '; by mksysfs --display-size',
      '[modules]',
      'device = c:\drv\kbdvirt.sys',
      'device = c:\drv\mousevirt.sys') |
        Set-Content -Encoding ascii (Join-Path $work 'SYSTEM.CFG')

    $sysfs = Join-Path $work 'sysfs.bin'
    $mkArgs = @('tools\mksysfs.py', '--board', 'none',
                '--partitions', 'partitions.csv',
                '--display', 'soft',
                '--out', $sysfs,
                '--add', ((Join-Path $work 'SYSTEM.CFG') + '=SYSTEM.CFG'),
                '--add', 'build\apps\KBDVIRT.SYS=drv/kbdvirt.sys',
                '--add', 'build\apps\MOUSEVIRT.SYS=drv/mousevirt.sys',
                '--add', 'build\apps\DESKTOP.AXE=DESKTOP.AXE')
    if ($Width -gt 0 -and $Height -gt 0) {
        $mkArgs += @('--display-size', "${Width}x${Height}")
    }
    # littlefs-python lives in the IDF venv, not necessarily in ARGON_PYTHON.
    $mkpy = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
        -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $mkpy) { throw 'no IDF python for mksysfs (needs littlefs-python)' }
    & $mkpy.FullName @mkArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'mksysfs failed' }

    # ---- the sequence -----------------------------------------------------
    # Where the pointer is walked: across the middle, then back, then up, then
    # a click in the centre.  Coordinates are the surface's own pixels.
    $w = if ($Width -gt 0) { $Width } else { 640 }
    $h = if ($Height -gt 0) { $Height } else { 400 }
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
        # Nothing to install: the drivers came up from C:\DRV with the boot,
        # because SYSTEM.CFG on the baked image names them in [modules].
        'dev',
        # Raw, so the harness does not sit waiting for a prompt that cannot
        # come until the shell's own deadline expires.
        "~run c:\desktop.axe $Seconds`r",
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
    # No -HostFs: everything the guest needs is on the C: image, which is one
    # fewer moving part - and HostFS is one that has been seen not to come back.
    & .\tools\qemu-boot.ps1 -Gfx -SysFs $sysfs -LogPath $log `
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

    $m = [regex]::Match($text,
                        'desktop: surface (\d+)x(\d+) (\w+), focus (\w+)')
    if ($m.Success) {
        Write-Host ("desktop: surface {0}x{1} {2}, focus {3}" -f
                    $m.Groups[1].Value, $m.Groups[2].Value,
                    $m.Groups[3].Value, $m.Groups[4].Value)
        if ($Width -gt 0 -and [int]$m.Groups[1].Value -ne $Width) {
            $fail += ("asked for a {0}x{1} surface and got {2}x{3}" -f $Width,
                      $Height, $m.Groups[1].Value, $m.Groups[2].Value)
        }
        # Painting before the slot has focus is painting into a flush the
        # kernel discards.  The shell waits for it; if it gave up waiting, the
        # picture below is not to be trusted.
        if ($m.Groups[4].Value -ne 'yes') {
            $fail += 'the shell started painting without focus'
        }
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
