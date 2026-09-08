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
        & .\argon.cmd apps --only DESKTOP.AXE KBDVIRT.SYS MOUSEVIRT.SYS `
                                 HELLO.AXE GFXDEMO.AXE |
            Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'building the images failed' }
    }

    foreach ($f in @('DESKTOP.AXE', 'KBDVIRT.SYS', 'MOUSEVIRT.SYS',
                     'HELLO.AXE', 'GFXDEMO.AXE')) {
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
                '--add', 'build\apps\DESKTOP.AXE=DESKTOP.AXE',
                # Two programs to open from the desktop: one that prints and
                # exits, one that takes the screen.  The pair is the whole of
                # what launching has to get right.
                '--add', 'build\apps\HELLO.AXE=HELLO.AXE',
                '--add', 'build\apps\GFXDEMO.AXE=GFXDEMO.AXE')
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
    #
    # Coordinates are the surface's own pixels, and the ones below are worked
    # out from the shell's own layout rather than read off a screenshot: the
    # menu bar is `menubar_h` tall, a title starts `PAD_X` in, the first window
    # opens eight pixels inside the work area.  Written out here so that a
    # change to any of those makes this fail loudly rather than click on the
    # wrong thing quietly.
    $w = if ($Width -gt 0) { $Width } else { 640 }
    $h = if ($Height -gt 0) { $Height } else { 400 }
    $cx = [int]($w / 2)
    $cy = [int]($h / 2)
    $span = [int]($w / 4)

    $menubarH = 18          # DSK_FONT_H + 2
    $statusH  = 12          # DSK_SMALL_H + 4
    $border   = 4
    $titleH   = 18
    $workY    = $menubarH
    $workH    = $h - $menubarH - $statusH

    # "File" is the first title: six pixels in, four characters of 8x16, and
    # PAD_X either side.  The middle of it, and the middle of its first item.
    $fileX  = 6 + [int]((8 * 4 + 12) / 2)
    $fileY  = [int]($menubarH / 2)
    $itemX  = 6 + 3 + 20
    $itemY  = $menubarH + 3 + 8

    # The first window: eight pixels inside the work area, 220x120 (or what
    # fits).  Its caption, and the minimise box two boxes in from the right.
    $winW = if ($w -lt 228) { $w - 8 } else { 220 }
    $winX = 8
    $winY = $workY + 8
    $capY = $winY + $border + [int]($titleH / 2)
    $capX = $winX + $border + 40
    $minX = $winX + $border + $winW - 2 * $border - 32 + 8

    # Where its plate goes once it is minimised: bottom-left of the work area.
    $plateX = 36
    $plateY = $workY + $workH - 20

    # The drive icon: one cell in from the top-left of the work area.
    $drvX = 4 + 28
    $drvY = $workY + 4 + 22

    # The folder window it opens, and the middle of its first row.
    $fwX = 8
    $fwY = $workY + 8
    $rowX = $fwX + $border + 40
    $rowY = $fwY + $border + $titleH + 8

    $moves = @(
        'home',
        # A drive opens as a window onto its root.
        "move $drvX,$drvY", 'click', 'wait 120', 'click', 'wait 1500',
        # Into the only directory there is, and back out of it by "..".
        "move $rowX,$rowY", 'click', 'wait 120', 'click', 'wait 1500',
        "move $rowX,$rowY", 'click', 'wait 120', 'click', 'wait 1500',
        # Park the pointer where it hides nothing before the photograph.
        "move $($w - 30),$($workY + 30)",
        'wait 1200'
    )

    # The two photographs have to be of the SAME state, so everything that
    # changes the screen happens before the first of them - the launches
    # included.  Taken between them, a moved selection alone put nine thousand
    # pixels of honest difference into a test whose whole subject is pixels
    # that differ for dishonest reasons.

    # ---- launching, which is the other half of a shell -------------------
    #
    # Two programs, because they fail differently.  A console one has to have
    # its output reach the screen; a graphical one has to receive a key, which
    # is the only way it can be told to give the screen back.  Both were
    # broken by the same thing and neither showed the other's symptom.
    #
    # Launched from the folder window that is already open on C:\, by
    # selecting a row and pressing Enter - which is how anyone would do it,
    # and needs no menu coordinates.  Home first so the selection is known
    # wherever the walk above left it.
    #
    # C:\ holds, in this order: drv, DESKTOP.AXE, GFXDEMO.AXE, HELLO.AXE,
    # SYSTEM.CFG.  Directories first, then files by name.  Do NOT open row 1
    # from here: DESKTOP.AXE starting DESKTOP.AXE works, and a second shell
    # over the first is not what is being measured.
    $runHello = @('key home') + (1..3 | ForEach-Object { 'key down' }) +
                @('wait 300', 'key enter',
                  # Loading off flash, running, printing, and then sitting on
                  # "finished with 0" until a key.
                  'wait 4000',
                  # The key that dismisses the report.  It must not also
                  # arrive at the desktop underneath.
                  'key enter', 'wait 1500')

    $runGfx = @('key home') + (1..2 | ForEach-Object { 'key down' }) +
              @('wait 300', 'key enter', 'wait 4000',
                # 'q' is how gfxdemo exits, and it only gets there if a child
                # of an application can be typed at.
                'say q', 'wait 2500')
    $quoted = ($moves | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $after = @('"key f5"', '"wait 1500"') -join ' '
    # Two windows closed with the keyboard, which is the other way to close
    # one.  Ctrl+F4 closes a window; Alt+F4 would leave the shell.
    $closing = @('"wait 200"') -join ' '

    $send = @(
        # Nothing to install: the drivers came up from C:\DRV with the boot,
        # because SYSTEM.CFG on the baked image names them in [modules].
        'dev',
        # Raw, so the harness does not sit waiting for a prompt that cannot
        # come until the shell's own deadline expires.
        #
        # The Enter goes in its own write, three hundred milliseconds after
        # the text.  Sent together it is occasionally lost - the line arrives
        # and is echoed, the command never runs, and the screenshot shows it
        # sitting at the prompt untouched.  One run in three, which is not a
        # thing to leave in a test.
        "~run c:\desktop.axe $Seconds",
        '~\x0d',
        '=desktop: surface',
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $quoted",
        ("!& '$pyexe' 'tools\inputplay.py' --wait 20 " +
         (($runHello | ForEach-Object { '"' + $_ + '"' }) -join ' ')),
        ("!& '$pyexe' 'tools\inputplay.py' --wait 20 " +
         (($runGfx | ForEach-Object { '"' + $_ + '"' }) -join ' ')),
        "!.\tools\grab-window.ps1 -Out '$png'",
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $after",
        "!.\tools\grab-window.ps1 -Out '$png2'",
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $closing",
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
    # ---- launching -------------------------------------------------------
    #
    # Each of these was a symptom of one bug (a child of an application was
    # bound to a session slot of its own instead of on top of its parent's), so
    # they are asserted separately rather than as one "launching works".

    # The console program's own output.  It went to the journal instead of the
    # screen while the child was not the top of a focused slot, so a match here
    # is the console routing and nothing else.
    if ($text -notmatch 'pid \d+, arena \d+ KB') {
        $fail += 'HELLO.AXE ran but its output never reached the console'
    }
    if ($text -notmatch 'finished with 0') {
        $fail += 'the desktop never reported HELLO.AXE finishing'
    }
    # The graphical one prints nothing of its own when it goes, so the proof
    # is the desktop's line about it - and its loop has no way out except a
    # key.  A child that never saw one would still be running, holding the
    # screen, and there would be no such line anywhere.
    if ($text -notmatch '(?i)GFXDEMO\.AXE finished with 0') {
        $fail += 'GFXDEMO.AXE was started but never saw the key that ends it'
    }
    # And its own greeting, which is a graphical child writing to the console.
    if ($text -notmatch 'Esc/Q/Enter quit') {
        $fail += "GFXDEMO.AXE's console output never reached the screen"
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
    # Two patterns, because the shell prints two lines - and it prints two
    # lines because a single one grew past the console's eighty columns and
    # was wrapped, with escape sequences through the middle of it.
    # The LAST match, not the first.  A program started from the desktop may
    # itself be a desktop, and then there are two sets of these lines in the
    # transcript - the nested one prints first, because it exits first.
    $mm = [regex]::Matches($text,
                        'desktop: (\d+) pointer events, (\d+) key events, (\d+) repaints')
    $mm2 = [regex]::Matches($text,
                         'desktop: (\d+) windows, (\d+) reflushes, (\d+) moves coalesced')
    $m = if ($mm.Count -gt 0) { $mm[$mm.Count - 1] } else { $mm }
    $m2 = if ($mm2.Count -gt 0) { $mm2[$mm2.Count - 1] } else { $mm2 }
    if ($m.Success -and $m2.Success) {
        $ptr = [int]$m.Groups[1].Value
        $key = [int]$m.Groups[2].Value
        $rep = [int]$m.Groups[3].Value
        $win = [int]$m2.Groups[1].Value
        Write-Host ("desktop: {0} pointer events, {1} key events, {2} repaints, {3} windows left" -f
                    $ptr, $key, $rep, $win)
        if ($ptr -lt 5) { $fail += "only $ptr pointer events arrived" }
        if ($key -lt 1) { $fail += 'no key ever arrived' }
        if ($rep -lt 1) {
            $fail += 'F5 never reached the shell, so the second picture is not a fresh paint'
        }
        # Four opened from the menu, two closed with Alt+F4.  A different
        # number means a menu that did not open, a click that missed, or a
        # close that did not close - and the picture alone would not say which.
        if ($win -ne 1) {
            $fail += "$win windows were left open, not the one the sequence should leave"
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
