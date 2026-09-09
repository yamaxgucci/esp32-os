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
    # The shell's own deadline, so nothing can hang.  It has to cover the
    # WHOLE scripted run, not one step: the sequence below is around a minute
    # of waits, and a deadline shorter than that ends the shell partway - which
    # showed up as an empty photograph, a stray keystroke arriving at the
    # console prompt, and a dialog counted as a window left open.  None of the
    # three said anything about a deadline.
    [int]$Seconds = 120,
    [string]$Out = 'build\desktop',
    [switch]$NoBuild,
    # Give the shell no surface at all, so it rasterises in bands straight to
    # the panel (apps\desktop\dsk_paint_band.c).  Everything below then tests
    # the OTHER backend, and the two-photograph rule is what makes it worth
    # doing: a strip the shell drew and the panel never received looks exactly
    # like a strip the shell forgot, and only the repaint comparison tells them
    # apart.  It is how the kernel refusing every narrow band was found.
    [switch]$Bands
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
    $png3 = Join-Path $root "$Out-restored.png"
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
                '--display', $(if ($Bands) { 'panel' } else { 'soft' }),
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

    # Where the dragged icon is dropped.  The position that should then be in
    # DESKTOP.INI is checked numerically further down rather than by regex,
    # because "did it move" is a comparison and not a spelling.
    $dropX = 300
    $dropY = $workY + 200

    # The folder window it opens, and the middle of its first row.
    $fwX = 8
    $fwY = $workY + 8
    $rowX = $fwX + $border + 40
    $rowY = $fwY + $border + $titleH + 8

    $moves = @(
        'home',

        # FIRST, while the icons can still be seen: drag the second drive icon
        # off its column, which is the other half of what DESKTOP.INI
        # remembers.  press / glide / release, because a click and a move are
        # not a drag - see tools\inputplay.py.
        #
        # Before the windows, and that is the whole reason it is here: the
        # icons live in a column down the left edge and the first folder window
        # opens on top of them, so a press aimed at an icon after that lands on
        # the window instead.  Which is what happened, and it looked exactly
        # like a drag that was not recorded.
        #
        # The first cell is at (4,4) inside the work area and the second 44
        # pixels below it, so the second icon's middle is around
        # (32, menubar + 4 + 44 + 22).
        "move 32,$($workY + 70)",
        'press', 'wait 200',
        "glide $($dropX),$($dropY)", 'wait 400',
        'release', 'wait 800',

        # The context menu, both ways in: the right button, and a press held
        # still for longer than LONG_PRESS_MS - which is the only way a finger
        # has, there being no second button on glass.  Each is closed by a
        # click far away from it, on bare desk where a click does nothing.
        "move $($dropX + 60),$($workY + 40)", 'rclick', 'wait 500',
        "move 600,380", 'click', 'wait 300',
        "move $($dropX + 60),$($workY + 90)",
        'press', 'wait 800', 'release', 'wait 400',
        "move 600,380", 'click', 'wait 300',

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
    # and needs no menu coordinates.
    #
    # Do NOT open DESKTOP.AXE from here: a desktop starting a desktop works,
    # and a second shell over the first is not what is being measured.

    # ---- which row is which ----------------------------------------------
    #
    # Every step below picks an entry by counting Downs from Home, so every
    # step depends on what C:\ holds and in what order the window sorts it -
    # directories first, then files by name.  Counting them by hand is how a
    # run launched GFXDEMO where it meant to launch HELLO, and then quit the
    # shell with the 'q' that was meant for GFXDEMO, and then typed the rest
    # of the script at the console prompt.  None of which mentioned rows.
    #
    # So the listing is written down once and the indices come out of it.
    # DESKTOP.INI is in it because the icon drag above CREATED it: dragging an
    # icon writes the arrangement, the file lands on C:, and it sorts between
    # DESKTOP.AXE and GFXDEMO.AXE - shifting everything below it by one.
    $rows = @('drv', 'DESKTOP.AXE', 'DESKTOP.INI', 'GFXDEMO.AXE',
              'HELLO.AXE', 'SYSTEM.CFG')

    # Home, then one Down per row: a selection that starts from a known place
    # rather than from wherever the last step left it.
    function Pick($list, $name) {
        $at = [array]::IndexOf($list, $name)
        if ($at -lt 0) { throw "no row called $name in the listing" }
        $keys = @('key home')
        if ($at -gt 0) { $keys += (1..$at | ForEach-Object { 'key down' }) }
        # `,$keys` and not `$keys`: PowerShell unrolls a returned array, and a
        # ONE-element array comes back as a plain string.  The caller then
        # writes `Pick ... + @(...)`, which on a string is text concatenation -
        # so row 0 produced one item reading "key homewait 200 key f8..." and
        # inputplay stopped at "no key called 'homewait 200 ke'".  Everything
        # after that step silently did not happen, and the disk checks passed
        # because they were checking that a copy which never happened was not
        # there.
        return ,$keys
    }

    $runHello = (Pick $rows 'HELLO.AXE') +
                @('wait 300', 'key enter',
                  # Loading off flash, running, printing, and then sitting on
                  # "finished with 0" until a key.
                  'wait 4000',
                  # The key that dismisses the report.  It must not also
                  # arrive at the desktop underneath.
                  'key enter', 'wait 1500')

    $runGfx = (Pick $rows 'GFXDEMO.AXE') +
              @('wait 300', 'key enter', 'wait 4000',
                # 'q' is how gfxdemo exits, and it only gets there if a child
                # of an application can be typed at.
                'say q', 'wait 2500')

    # ---- the file operations ---------------------------------------------
    #
    # On the window that is already open on C:\, with the keyboard: F8 copies,
    # F2 renames, Delete deletes.  The destination is typed as a BARE NAME
    # ("drv2", not "c:\drv2") for two reasons - it is the rule that a name with
    # no directory in it means "beside the original", which is worth
    # exercising, and inputplay cannot type a colon or a backslash: they need
    # shift, and the key table deliberately fakes no modifiers.
    #
    # What is NOT asserted here is stopping one halfway.  There is nothing on
    # this image slow enough to catch: the two files in C:\DRV copy in one
    # tick, and a copy that finished before Escape could be sent would make the
    # test pass whether cancelling worked or not.  Interrupting is pinned down
    # on the host instead, where the tick count is the clock -
    # test_cancelled_copy_keeps_what_it_finished and
    # test_failed_move_keeps_the_source in host-tests\test_fsops.c.
    #
    $clear = 1..12 | ForEach-Object { 'key backspace' }

    # File > Create directory..., from the keyboard.  F10 puts the shell on the
    # bar and opens the first menu; Down steps over the separators by itself,
    # so the seventh stop is Create directory (New window, Run, Copy, Move,
    # Rename, Delete, Create directory) and the eighth is Properties.
    #
    # It runs FIRST of the operations, because it changes what is on which
    # row: `newdir` sorts in as the second directory, so $rowsAfterMkdir is
    # what everything after this counts against.
    $opsMkdir = @('key f10', 'wait 500') +
                (1..7 | ForEach-Object { 'key down' }) +
                @('wait 300', 'key enter', 'wait 800',
                  'say newdir', 'wait 200', 'key enter', 'wait 1500')

    $rowsAfterMkdir = @('drv', 'newdir') +
                      ($rows | Where-Object { $_ -ne 'drv' })

    # `drv` is copied to `drv2`, which then sorts in beside it.
    $opsCopy = (Pick $rowsAfterMkdir 'drv') +
               @('wait 200', 'key f8', 'wait 800') + $clear +
               @('say drv2', 'wait 200', 'key enter', 'wait 3000')

    $rowsAfterCopy = @('drv', 'drv2') +
                     ($rowsAfterMkdir | Where-Object { $_ -ne 'drv' })

    # Enter answers Yes, which is the first button of a yes/no box.
    $opsDelete = (Pick $rowsAfterCopy 'drv2') +
                 @('wait 300', 'key delete', 'wait 800', 'key enter',
                   'wait 3000')

    # And drv2 is gone again, so the rows are the ones after the mkdir.
    $opsRename = (Pick $rowsAfterMkdir 'HELLO.AXE') +
                 @('wait 300', 'key f2', 'wait 800') + $clear +
                 @('say hi.axe', 'wait 200', 'key enter', 'wait 2000')

    # File > Properties, from the keyboard: F10 puts the shell on the bar and
    # opens the first menu, and Down steps over the separators by itself.
    # Seven items in: New window, Run, Copy, Move, Rename, Delete, Create
    # directory, Properties.
    # How long to let the shell catch up before a photograph is taken.
    #
    # One number for both backends, which it took a fix to be able to say.
    # Band mode used to be fifteen times slower under the emulator - a drag
    # step of 339 ms against 22 ms on a real CYD - because every strip waited
    # for the emulated panel to acknowledge the last one.  A wait sized for the
    # surface path then photographed a shell still working through its queue: a
    # dialog missing its button, and a different pixel count every run (8936,
    # 20724, 5800).  The panel is now told once per frame instead of once per
    # strip (ag_gfx_flush in surfaceless mode), which took the same drag step
    # to 8 ms and a full repaint from 1542 ms to 15.
    $settle = 1200
    $opsProps = @('key f10', 'wait 500') +
                (1..8 | ForEach-Object { 'key down' }) +
                @('wait 300', 'key enter', "wait $settle")

    # Window > System console, the last item of that menu (Cascade, Tile,
    # Close, Close all, Arrange icons, System console - the arrows skip
    # separators).  Inside the one keyboard pass rather than a step of its own,
    # and that is not tidiness: every invocation of inputplay opens a fresh
    # connection to KBDVIRT, and this file's own header records what a run with
    # six of them delivered - eleven keystrokes of sixty-nine.  It also puts
    # the console window in BOTH photographs, so the repaint comparison covers
    # a window whose content the shell does not own.
    #
    # Before the properties box rather than after: that box is modal, and while
    # one is up the menu bar is fed nothing at all.  A console step after it
    # photographed the dialog closing and called it a console window.
    $openConsole = @('key f10', 'wait 400', 'key right', 'wait 300') +
                   (1..6 | ForEach-Object { 'key down' }) +
                   @('wait 300', 'key enter', 'wait 1200')

    # Opening a window activates it, so the folder window has to be brought
    # back before anything that acts on a selection: Properties on the console
    # window is Properties on nothing, and the dialog never opens.  The Window
    # menu lists the windows topmost first after its own items, so the eighth
    # entry is the one underneath the console.
    $backToFolder = @('key f10', 'wait 400', 'key right', 'wait 300') +
                    (1..8 | ForEach-Object { 'key down' }) +
                    @('wait 300', 'key enter', 'wait 800')

    $keyboard = $runHello + $runGfx + $opsMkdir + $opsCopy + $opsDelete +
                $opsRename + $openConsole + $backToFolder + $opsProps
    $quoted = ($moves | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $after = @('"key f5"', ('"wait ' + $settle + '"')) -join ' '
    # The properties box is deliberately still up for both photographs - it is
    # a modal window, and whether a full repaint puts it back is exactly the
    # kind of thing the two-photograph criterion is for.  Enter dismisses it
    # afterwards, leaving the folder window and the console window: what each
    # of those is worth is asserted at the bottom.
    $closing = @('"key enter"', '"wait 600"') -join ' '

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
        # ONE keyboard pass for all of it, and that is not tidiness.
        #
        # Every invocation of inputplay opens a fresh socket to KBDVIRT, and
        # the driver has to accept it and install it while an application is
        # already reading from the old one.  Six of those in a row is six
        # chances at that race: a run that did it that way delivered the first
        # six keystrokes and then eleven of sixty-nine, with every connection
        # reporting success.  One connection has no race to lose.
        ("!& '$pyexe' 'tools\inputplay.py' --wait 20 " +
         (($keyboard | ForEach-Object { '"' + $_ + '"' }) -join ' ')),
        "!.\tools\grab-window.ps1 -Out '$png'",
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $after",
        "!.\tools\grab-window.ps1 -Out '$png2'",
        "!& '$pyexe' 'tools\inputplay.py' --wait 20 $closing",
        # 'q' rather than Escape: a lone 0x1b is held back by the console's
        # escape-sequence parser and never arrives as a key on its own.
        '~q',
        '=pointer events',

        # ---- and now the whole point of DESKTOP.INI ----------------------
        #
        # The shell is started a second time and given NOTHING: no clicks, no
        # keys, and a deadline so it leaves on its own.  Whatever is on the
        # screen and in its counters therefore came out of the file, and the
        # counters are what says so - "0 pointer events, 0 key events, 1
        # window" cannot be produced by a shell that forgot.
        #
        # A photograph would show the window too, and would not distinguish a
        # restored window from one this script had opened; the counters do.
        'type c:\desktop.ini',
        '~run c:\desktop.axe 12',
        '~\x0d',
        '=desktop: surface',
        # The surface line is printed before the first paint has reached the
        # panel, so a photograph taken the moment it appears catches the
        # console underneath - which is how the restored desktop came out as a
        # screen full of DESKTOP.INI.  Three seconds is the arrangement being
        # put back; the assertions below do not depend on this picture, it is
        # there to be looked at.
        '!Start-Sleep -Seconds 3',
        "!.\tools\grab-window.ps1 -Out '$png3'",
        '=pointer events',

        # The disk, from the console rather than from the shell that changed
        # it: a window drawing the right thing over a filesystem it did not
        # actually change would pass a test that only asked the window.
        #
        # `type` and not `dir` for the file that should be gone, because it
        # names the file it could not find and `dir` names the directory it
        # searched - and here the interesting thing is the file.
        #
        # Nothing here types a file that might still be there: `type` on an
        # .AXE that survived a failed rename puts a kilobyte of Xtensa onto the
        # screen, and that hides every other answer on it.  The listing says
        # both things at once - the new name present, the old one absent - so
        # it is what is asked for the rename.
        'type c:\drv2\kbdvirt.sys',
        'dir c:\drv',
        'dir c:\'
    )

    Write-Host "desktop: driving QEMU, transcript -> $log, picture -> $png"
    # No -HostFs: everything the guest needs is on the C: image, which is one
    # fewer moving part - and HostFS is one that has been seen not to come back.
    & .\tools\qemu-boot.ps1 -Gfx -SysFs $sysfs -LogPath $log `
        -TimeoutSec 240 -Send $send | Out-Host

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
    if ($Bands) {
        if ($text -notmatch 'desktop: surface \d+x\d+ bands') {
            $fail += 'asked for no surface, but the shell did not go into bands'
        }
        # A refused present is invisible in the picture: it looks like a
        # region nothing was drawn in.  The backend says it once, and this is
        # the only place that would ever read it.
        if ($text -match 'present .* refused') {
            $fail += 'the panel refused a band - see the transcript for the code'
        }
    } elseif ($text -match 'desktop: surface \d+x\d+ bands') {
        $fail += 'the shell went into bands with a surface available'
    }
    if ($text -match 'desktop: cannot take the display') {
        $fail += 'the display was already owned by something else'
    }
    # The soft path says "as fb0 + QEMU RGB window" when it attaches the
    # emulated screen; the surfaceless path says "panel0 takes rectangles"
    # about the same screen, so each mode is asked for its own line.
    if ($Bands) {
        if ($text -notmatch 'panel0 takes rectangles') {
            $fail += 'the emulator gave no rectangle-taking panel - graphics=on?'
        }
    } elseif ($text -notmatch 'QEMU RGB window') {
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

    # ---- the file operations ---------------------------------------------
    #
    # Read off the last console screen, which is what the guest ended up
    # showing after the shell exited.  The transcript itself is a stream of
    # whole-screen repaints and cannot answer an ordering question.
    if (Test-Path 'build-host\vtdump.exe') {
        $screen = (& cmd /c "build-host\vtdump.exe 80 25 437 < $log") -join "`n"
        Write-Host $screen

        # Renamed: the listing has the new name and not the old one.
        if ($screen -notmatch '(?i)HI\.AXE') {
            $fail += 'F2 did not rename HELLO.AXE'
        }
        if ($screen -match '(?i)HELLO\.AXE') {
            $fail += 'the old name is still there after the rename'
        }
        # Copied and then deleted: the copy is gone and the original is not.
        if ($screen -notmatch '(?i)drv2.kbdvirt\.sys: file not found') {
            $fail += 'Delete did not remove the copied tree'
        }
        if ($screen -notmatch '(?i)kbdvirt\.sys') {
            $fail += 'the delete took the original C:\DRV as well as the copy'
        }
        # Created: the listing has it, and as a directory.
        if ($screen -notmatch '(?i)newdir +<DIR>') {
            $fail += 'Create directory did not make it'
        }
    } else {
        Write-Host 'desktop: no vtdump, so the disk itself is not checked'
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
            # Two counts, not one, and the split is the whole point.
            #
            # The status strip along the bottom carries counters that move -
            # the key and pointer totals go up between the two shots because
            # F5 is itself a key - so a difference there is expected and says
            # nothing.  ANYWHERE ELSE a difference means the first paint did
            # not all reach the panel, and the tolerance for it is zero.
            #
            # It used to be one number against a budget of "about a strip's
            # worth of pixels", and that let a real bug through: a window
            # caption left half navy and half grey came to 1330 pixels,
            # comfortably under the budget, and the check said ok.  A budget
            # measured in pixels cannot tell a moving counter from a stale
            # window; a budget measured in ROWS can.
            $stripTop = $a.Height - 20
            $inStrip = 0
            $above = 0
            $firstX = -1
            $firstY = -1
            for ($y = 0; $y -lt $a.Height; $y++) {
                for ($x = 0; $x -lt $a.Width; $x++) {
                    if ($a.GetPixel($x, $y) -ne $b.GetPixel($x, $y)) {
                        if ($y -ge $stripTop) {
                            $inStrip++
                        } else {
                            $above++
                            if ($firstX -lt 0) { $firstX = $x; $firstY = $y }
                        }
                    }
                }
            }
            Write-Host ("desktop: after a forced repaint, {0} pixels differ in the status strip and {1} above it" -f
                        $inStrip, $above)
            if ($above -gt 0) {
                $fail += ("the forced repaint changed $above pixels outside the status strip, from ($firstX,$firstY) - " +
                          'the first paint did not all reach the panel')
            }
        }
        $a.Dispose()
        $b.Dispose()
    }

    # ---- the console window ----------------------------------------------
    #
    # It shows the kernel's console inside a window of the shell's own, which
    # nothing else here does: every other window draws what the shell itself
    # decided.  Two things say it worked.  This line, which the shell prints
    # when the window opens - and prints ON the console, so it lands inside the
    # window it is announcing.  And the two photographs above, which now
    # include the window: cells that reached the panel once and not on a
    # repaint would show up there, and the console's content is static by this
    # point (the last thing to print was a program that has already exited).
    if ($text -notmatch 'desktop: console window opened') {
        $fail += 'the console window never opened'
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
    # The shell runs TWICE in this scenario and prints these on the way out
    # each time, so which match is wanted depends on which run is being asked
    # about: the first is the one that was driven, the last is the one that was
    # given nothing and had to remember.
    $mm = [regex]::Matches($text,
                        'desktop: (\d+) pointer events, (\d+) key events, (\d+) repaints')
    $mm2 = [regex]::Matches($text,
                         'desktop: (\d+) windows, (\d+) reflushes, (\d+) moves coalesced')
    $m = if ($mm.Count -gt 0) { $mm[0] } else { $mm }
    $m2 = if ($mm2.Count -gt 0) { $mm2[0] } else { $mm2 }
    if ($m.Success -and $m2.Success) {
        $ptr = [int]$m.Groups[1].Value
        $key = [int]$m.Groups[2].Value
        $rep = [int]$m.Groups[3].Value
        $win = [int]$m2.Groups[1].Value
        Write-Host ("desktop: {0} pointer events, {1} key events, {2} repaints, {3} windows left" -f
                    $ptr, $key, $rep, $win)
        if ($ptr -lt 5) { $fail += "only $ptr pointer events arrived" }
        if ($key -lt 1) { $fail += 'no key ever arrived' }
        # Both ways of asking for the context menu, counted.  A right button
        # that reached nothing and a long press that was taken for a drag both
        # look like a passing run otherwise.
        $ctx = ([regex]::Matches($text, 'desktop: context menu at')).Count
        Write-Host "desktop: the context menu opened $ctx time(s)"
        if ($ctx -lt 2) {
            $fail += "the context menu opened $ctx time(s), wanted 2 (right button and long press)"
        }
        if ($rep -lt 1) {
            $fail += 'F5 never reached the shell, so the second picture is not a fresh paint'
        }
        # Every keystroke the script sends, counted, against what the shell
        # says it saw.
        #
        # This is the assertion that catches a run which quietly did half the
        # sequence.  Without it, a step that never happened is invisible: the
        # disk checks below then confirm that a copy which was never made is
        # not there, and the whole thing passes.  One run delivered thirty of
        # ninety keys and failed on nothing but the rename.
        $wantKeys = 0
        foreach ($k in $keyboard) {
            if ($k -like 'key *') {
                $wantKeys++
            } elseif ($k -like 'say *') {
                $wantKeys += $k.Substring(4).Length
            }
        }
        if ($key -lt $wantKeys) {
            $fail += "only $key of $wantKeys keystrokes reached the shell, so the sequence did not all run"
        }
        # Four opened from the menu, two closed with Alt+F4.  A different
        # number means a menu that did not open, a click that missed, or a
        # close that did not close - and the picture alone would not say which.
        # Two: the folder window this sequence opened and the console window,
        # which is deliberately left open - what the second run then restores
        # says whether a window with no folder behind it stays out of the
        # arrangement, which is the rule DESKTOP.INI is written by.
        if ($win -ne 2) {
            $fail += "$win windows were left open, not the two the sequence should leave"
        }
    } else {
        $fail += 'the shell did not print its counts (it may have been killed)'
    }

    # ---- what the second run remembered ---------------------------------
    #
    # It was given no input at all and left on its own deadline, so everything
    # it reports came out of C:\DESKTOP.INI.
    if ($mm.Count -lt 2 -or $mm2.Count -lt 2) {
        $fail += 'the shell did not start a second time, so nothing was restored'
    } else {
        $r = $mm[$mm.Count - 1]
        $r2 = $mm2[$mm2.Count - 1]
        $rPtr = [int]$r.Groups[1].Value
        $rKey = [int]$r.Groups[2].Value
        $rWin = [int]$r2.Groups[1].Value
        Write-Host ("desktop: the restored run saw {0} pointer and {1} key events, and had {2} window(s)" -f
                    $rPtr, $rKey, $rWin)
        if ($rPtr -ne 0 -or $rKey -ne 0) {
            $fail += "the second run was not left alone ($rPtr pointer, $rKey key events), so it proves nothing"
        }
        # One, not two: the console window is not part of an arrangement -
        # it shows a thing the machine has rather than a place on a disk, and
        # DESKTOP.INI records places.  A second run with two windows would mean
        # the shell had written down a window it cannot put back.
        if ($rWin -ne 1) {
            $fail += "the second run had $rWin windows open, not the one folder window it should restore"
        }
    }

    # And the icon: DESKTOP.INI was typed to the console, so the position it
    # holds can be compared with where the drag actually dropped it.  A regex
    # for "some big number" would pass on the default position too; this is
    # the comparison itself.
    # No ^ anchor and no multiline: the transcript is a stream of cursor
    # positioning, not of lines, so there is nothing for ^ to anchor to.  The
    # writer emits exactly "T: = x,y", which is specific enough on its own.
    $iniLine = [regex]::Match($text, 'T:\s*=\s*(-?\d+)\s*,\s*(-?\d+)')
    if (-not $iniLine.Success) {
        $fail += 'DESKTOP.INI holds no position for the icon that was dragged'
    } else {
        $ix = [int]$iniLine.Groups[1].Value
        $iy = [int]$iniLine.Groups[2].Value
        Write-Host ("desktop: DESKTOP.INI puts the dragged icon at {0},{1}" -f $ix, $iy)
        # It started at 4,48.  Anything near there means the drag was not
        # recorded; the drop was at 300,menubar+200.
        if ($ix -lt 100 -or $iy -lt 100) {
            $fail += "the dragged icon was written down at $ix,$iy - close to where it started, so the drag was not recorded"
        }
    }
    if (-not (Test-Path $png3)) {
        $fail += 'no picture of the restored desktop'
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
