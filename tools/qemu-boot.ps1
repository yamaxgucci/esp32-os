# Boots the firmware in QEMU, optionally types commands at the console, and
# reports what came back.
#
#   . .\tools\idf-env.ps1
#   .\tools\qemu-boot.ps1
#   .\tools\qemu-boot.ps1 -Send @('ver','mem') -Marker 'A:\>'
#   .\tools\qemu-boot.ps1 -Put 'HELLO.AXE=t:\hello.axe' -Send @('run t:\hello.axe')
#   .\tools\qemu-boot.ps1 -Gfx -Send @('~run h:\desktop.axe 30\x0d',
#                                      '!.\tools\grab-window.ps1 -Out build\a.png')
#
# A -Send item may be prefixed:
#
#   ~   raw bytes, no Enter, no waiting for a prompt (\xNN is decoded)
#   =   send nothing, wait for this text to appear
#   !   run this on the host, not in the guest, and record what it printed
#
# QEMU's serial port is exposed over TCP rather than stdio.  Piping stdio is
# what the obvious version of this script does, and it does not work: a
# redirected stdin reaches EOF immediately and QEMU exits before the firmware
# has printed anything.
#
# ArgonOS never exits on its own, so the run is bounded from the outside.
# Exit code 0 means the marker was seen.
[CmdletBinding()]
param(
    # Matches any drive prompt: A:\> on a card, C:\> on flash, T:\> on the
    # RAM disk.  Which one appears depends on what managed to mount.
    [string]$Marker = '\>',
    [int]$TimeoutSec = 60,
    # A wall-clock ceiling for the WHOLE run, as against TimeoutSec which is
    # per step - and per step, under icount, is ten times what it says.  One
    # marker that never arrives could therefore sit for forty minutes without
    # anybody being told, and did: a run was left waiting while its window
    # showed a static screen and three separate "is it stuck?" from the far
    # side of the desk went answered with "no, it is slow".  Zero means the
    # old behaviour, which is no ceiling at all.
    [int]$BudgetSec = 900,
    [string[]]$Send = @(),
    # Files to copy into the guest before the commands run, as
    # 'localpath=guestpath'.  There is no other way in: the emulator has no card
    # reader, and the shell's recv command is what the console is for.
    [string[]]$Put = @(),
    [int]$Port = 5556,
    [string]$LogPath = 'build\qemu-boot.log',
    [int]$QuietMs = 1200,
    # Attach a card image. Off by default so a test that does not care about
    # removable media is not slowed down by probing for it.
    [switch]$Sd,
    [string]$SdImage = 'build\sdcard.img',
    # Live host folder as guest H: (UART1 ↔ hostfsd). Same helper as qemu-run.
    [string]$HostFs = '',
    [int]$HostFsPort = 5557,
    # Fake external radio coprocessor on UART2 (↔ tools/rlinkd.py). Lets a guest
    # driven with EXTRADIO.SYS reach the network with no built-in radio.
    # 5562 is deliberately outside the OpenEth hostfwd range (5558-5561): those
    # ports are already forwarded to the guest, and QEMU refuses to start if one
    # is also taken by rlinkd on the host.
    [switch]$Radio,
    [int]$RadioPort = 5562,
    # Which fake coprocessor answers on the radio UART: rlinkd.py (RLINK, our own
    # firmware) or atmodemd.py (stock AT firmware).  A file name under tools\.
    [string]$RadioScript = 'rlinkd.py',
    [switch]$NoNet,
    [int]$NetPort = 5558,
    # PSRAM the emulated part carries: 8 (default, the smaller module) or 16.
    [int]$Psram = 8,
    # Open QEMU's virtual RGB panel, so a graphical application can be
    # photographed with tools\grab-window.ps1 while it runs.  Without this the
    # emulator has no window at all and the soft framebuffer goes nowhere a
    # camera can see it.
    #
    # The console still comes back over TCP, which is the point: this is how a
    # graphical .AXE gets driven and looked at by a script with nobody at the
    # desk.  See apps\desktop\check.ps1.
    [switch]$Gfx,
    # A pre-built C: partition, merged into the flash image (tools\mksysfs.py).
    #
    # Everything C: normally has to be given at runtime - the surface size in
    # [display], the modules to autoload, an application to run - arrives
    # before the first boot instead.  That matters because writing C: on a
    # running guest and rebooting is not reliable here: three runs in eight had
    # a file written that way read back as -91 (AG_EFORMAT), and the boot after
    # such a write took 128 seconds against the usual two.
    [string]$SysFs = '',
    # Tie virtual time to instructions retired instead of to host time.
    #
    # Without this, neither clock the guest can read means anything about the
    # chip: esp_timer follows host wall time, and CCOUNT is that same virtual
    # clock rescaled by the core frequency QEMU models (40 MHz, not 240), so
    # "cycles" is just microseconds times forty.  Measured on a DSP inner loop
    # that reports 0.1 cycles per iteration - a number no processor can produce.
    #
    # With -icount shift=0 one guest instruction advances virtual time by one
    # nanosecond, so ag_micros() * 1000 is the instruction count, exactly and
    # repeatably, whatever else the host is doing.  That is not cycles either -
    # it ignores stalls, the cache and PSRAM latency - but it is a floor that
    # belongs to the guest rather than to this PC.
    [switch]$Icount,

    # ---- a second guest, for the cluster ------------------------------------
    #
    # Two emulators on this PC wired UART1 to UART1 are two whole ArgonOS
    # kernels on one wire, with nothing pretending to be anything.  It is the
    # only way to develop a cluster without a second board, and QEMU already
    # does everything it needs: a serial port here is a TCP socket, and this
    # script already opens one as a server (the console) and others as clients
    # (HostFS, the radio).  The two guests are then just the two ends.
    #
    # -LinkPort puts UART1 on that port; -LinkListen makes this guest the one
    # that listens.  Exactly one of the pair listens; the other connects.
    [int]$LinkPort = 0,
    [switch]$LinkListen,
    # A file this guest creates once its -Put deliveries are in, so the OTHER
    # guest of a pair can wait for it before it starts talking (nodetest.ps1).
    [string]$ReadyFile = '',

    # Which flash image, and whether to leave other emulators alone.
    #
    # Both exist for the same reason and neither is optional for a pair.  The
    # flash drive is writable: two guests on one file corrupt each other's C:,
    # and the damage shows up later as a file that reads back -91.  And this
    # script stops the previous emulator of the same image before starting -
    # only one this tree registered (qemu-common.ps1, Stop-Emulators), never
    # another tree's - which for a pair sharing one image would still be the
    # second shooting the first.
    [string]$Flash = '',
    # The image in -Flash is FINISHED: do not rebuild build\qemu_flash.bin and
    # do not copy over it.  For a pair of guests that are two different
    # machines - only one of them can be the target that was built last, so the
    # other's image was made earlier and kept aside.
    [switch]$FlashAsIs,
    # Which machine this guest is, when it is not the one sdkconfig names.
    # Goes with -FlashAsIs and means the same thing: this half of the pair was
    # built before the target moved on.
    [string]$Chip = '',
    [switch]$KeepOtherEmulators,
    # Suffix for this guest's own emulator log, so a pair does not write one
    # file from two processes.
    [string]$Tag = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$qemu = Resolve-Qemu -Chip $Chip
if (-not $FlashAsIs) {
    Update-FlashImage -SysFs $SysFs
}
$efuse = Initialize-EfuseFile

# A guest of its own gets a flash image of its own, copied from the canonical
# one so it runs exactly the same firmware.  Copied every time rather than kept:
# the point of a second node is a repeatable pair, and a C: left over from the
# previous run is the staleness the stamp above exists to prevent, arriving by
# the back door.
$flashPath = 'build\qemu_flash.bin'
if ($Flash) {
    if (-not $FlashAsIs) {
        Copy-Item -LiteralPath $flashPath -Destination $Flash -Force
    } elseif (-not (Test-Path -LiteralPath $Flash)) {
        throw "no prepared flash image at $Flash"
    }
    $flashPath = $Flash
    # Its own efuse too: blank, and written by whoever boots.
    $efuse2 = [IO.Path]::ChangeExtension($Flash, $null) + '_efuse.bin'
    if (-not (Test-Path $efuse2)) {
        [IO.File]::WriteAllBytes((Join-Path (Get-Location) $efuse2),
                                 (New-Object byte[] 1024))
    }
    $efuse = $efuse2
}

if ($LinkPort -gt 0 -and ($HostFs -or $Radio)) {
    # UART1 is the only spare serial QEMU's esp32s3 has - UART2 exists in the
    # header and passes no data - and HostFS, the radio and the node link all
    # want it.  Saying so here beats a run where one of the three silently is
    # not there.
    throw 'a node link cannot share UART1 with -HostFs or -Radio.'
}

$hostfsProc = $null
if ($HostFs) {
    if (-not (Test-Path -LiteralPath $HostFs)) {
        New-Item -ItemType Directory -Force -Path $HostFs | Out-Null
    } elseif (-not (Test-Path -LiteralPath $HostFs -PathType Container)) {
        throw "HostFs path is not a directory: $HostFs"
    }
    $python = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
        -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $py = if ($python) { $python.FullName } else {
        $g = Get-Command python -ErrorAction SilentlyContinue
        if (-not $g) { throw 'Python not found for hostfsd.' }
        $g.Source
    }
    $hostfsScript = (Resolve-Path (Join-Path $PSScriptRoot 'hostfsd.py')).Path
    $rootAbs = (Resolve-Path -LiteralPath $HostFs).Path
    $padCfg = Join-Path $rootAbs 'sms.cfg'
    $defaultPadCfg = Join-Path $PSScriptRoot '..\apps\sms\sms.cfg'
    if (-not (Test-Path -LiteralPath $padCfg) -and
        (Test-Path -LiteralPath $defaultPadCfg)) {
        Copy-Item -LiteralPath $defaultPadCfg -Destination $padCfg -Force
    }
    if (-not (Test-Path -LiteralPath $padCfg)) {
        $padCfg = (Resolve-Path -LiteralPath $defaultPadCfg).Path
    }
    try {
        Get-NetTCPConnection -LocalPort $HostFsPort -State Listen `
            -ErrorAction SilentlyContinue |
            ForEach-Object {
                Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue
            }
        Start-Sleep -Milliseconds 200
    } catch {}
    $hostfsOut = Join-Path (Get-Location) 'build\hostfsd.out.log'
    $hostfsErr = Join-Path (Get-Location) 'build\hostfsd.err.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $hostfsOut) | Out-Null
    $hostfsArgs = '"{0}" --root "{1}" --port {2} --pad-cfg "{3}"' -f `
        $hostfsScript, $rootAbs, $HostFsPort, $padCfg
    $hostfsProc = Start-Process -FilePath $py -ArgumentList $hostfsArgs `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $hostfsOut -RedirectStandardError $hostfsErr
    Start-Sleep -Milliseconds 500
    if ($hostfsProc.HasExited) {
        throw "hostfsd exited immediately (exit $($hostfsProc.ExitCode))"
    }
    Write-Host "HostFS: $rootAbs -> guest H: (TCP $HostFsPort / UART1)"
}

$radioProc = $null
if ($Radio) {
    $rpython = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
        -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $rpy = if ($rpython) { $rpython.FullName } else {
        $g = Get-Command python -ErrorAction SilentlyContinue
        if (-not $g) { throw 'Python not found for rlinkd.' }
        $g.Source
    }
    $rlinkScript = (Resolve-Path (Join-Path $PSScriptRoot $RadioScript)).Path
    try {
        Get-NetTCPConnection -LocalPort $RadioPort -State Listen `
            -ErrorAction SilentlyContinue |
            ForEach-Object {
                Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue
            }
        Start-Sleep -Milliseconds 200
    } catch {}
    $radioOut = Join-Path (Get-Location) 'build\rlinkd.out.log'
    $radioErr = Join-Path (Get-Location) 'build\rlinkd.err.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $radioOut) | Out-Null
    $radioArgs = '"{0}" --port {1}' -f $rlinkScript, $RadioPort
    $radioProc = Start-Process -FilePath $rpy -ArgumentList $radioArgs `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $radioOut -RedirectStandardError $radioErr
    Start-Sleep -Milliseconds 500
    if ($radioProc.HasExited) {
        throw "rlinkd exited immediately (exit $($radioProc.ExitCode))"
    }
    Write-Host "Radio: fake coprocessor on TCP $RadioPort / UART2"
}

# wait=on is essential: a TCP serial port with no peer throws its output away,
# and the whole boot is over in a quarter of a second.  Without it the test
# races the emulator and loses often enough to be useless.
$qemuArgs = (Get-QemuMachineArgs -EfusePath $efuse -PsramMb $Psram -Graphics:$Gfx `
                                 -FlashPath $flashPath -Chip $Chip) + @(
    '-display', $(if ($Gfx) { 'sdl' } else { 'none' })
    '-monitor', 'none'
    '-serial', "tcp:127.0.0.1:$Port,server=on,wait=on"
)

if ($Icount) {
    # Deliberately without sleep=off.  It looks like the right switch - stop
    # QEMU pacing itself to the wall clock and let the benchmark run flat out -
    # and it makes things about ten times slower, because with sleep=off an
    # idle guest is no longer free: every millisecond a FreeRTOS task spends in
    # vTaskDelay becomes a million emulated instructions of the idle loop, and
    # the console and HostFS tasks delay constantly.  Measured on the same
    # workload: one minute with the default, still running after eleven without.
    $qemuArgs += @('-icount', 'shift=0')
    # And the waits have to be scaled with it, or the run fails for the one
    # reason that has nothing to do with what is being measured.
    #
    # -icount shift=0 makes the emulator about an order of magnitude slower, and
    # every deadline in this script came from the same fixed 60 s: the boot, the
    # prompt, each command.  A boot that takes five seconds normally takes fifty
    # under icount, which fits inside 60 on an idle machine and does not on a busy
    # one - so the same command passed in the morning and stopped at
    # "marker NOT seen within 60s" in the evening, with a transcript that ends in
    # the middle of the IDF banner and looks exactly like a firmware hang.  It is
    # not one: the same firmware boots fine without -Icount.
    $TimeoutSec = $TimeoutSec * 10
    Write-Host "icount: waits scaled to ${TimeoutSec}s (the emulator is ~10x slower)"
}

if (-not $NoNet) {
    $qemuArgs += Get-QemuNetArgs -HostPort $NetPort -GuestPort $NetPort
}

if ($HostFs) {
    # Second -serial is UART1 (HostFS). Console stays on the first.
    $qemuArgs += @('-serial', "tcp:127.0.0.1:$HostFsPort,reconnect=1")
}

if ($LinkPort -gt 0) {
    # UART1: the wire to the other node.  One end listens, the other connects,
    # and which is which is decided by the caller rather than by the firmware -
    # the guests are identical images and must stay that way.
    #
    # wait=off on the listener, unlike the console: the console uses wait=on
    # because a TCP serial with no peer throws its output away and the boot is
    # over before a test can attach.  Here the peer is another emulator that
    # has not started yet, and waiting for it would deadlock the pair.
    $qemuArgs += @('-serial', $(if ($LinkListen) {
        "tcp:127.0.0.1:$LinkPort,server=on,wait=off"
    } else {
        "tcp:127.0.0.1:$LinkPort,reconnect=1"
    }))
}

if ($Radio) {
    # The radio is the next -serial after the console (UART1) - or after HostFS
    # (UART2) when that is attached too.  QEMU's esp32s3 emulates only UART0 and
    # UART1, so the working combination is -Radio without -HostFs, which lands
    # the radio on UART1; the driver defaults to UART1 on the "generic" (QEMU)
    # board to match.  On real hardware the radio is a free UART (UART2 by
    # default), set with radio.uart in BOARD.CFG.
    $qemuArgs += @('-serial', "tcp:127.0.0.1:$RadioPort,reconnect=1")
}

if ($Sd) {
    $qemuArgs += Get-QemuSdArgs -Path $SdImage
}

# A leftover emulator would still hold the port and answer with silence.
#
# -KeepOtherEmulators: not even that, when this guest is half of a pair
# booted from a copy of the same image (tools\xclutest.ps1 keeps each
# node's image apart, so that is belt and braces).
if (-not $KeepOtherEmulators) {
    # The leftover, and not every emulator on the machine.  This used to stop every
    # process called qemu-system-xtensa, which is right while one person runs one
    # board and wrong the moment anything else is running: a second worktree
    # emulating a second node, or a driver being tried by hand in another window,
    # died silently whenever a test started here.  It cost an afternoon to see,
    # because nothing says "somebody killed it" - the port simply stops answering.
    #
    # What identifies a leftover is the image it is running: two emulators of the
    # same flash file are the same board twice, and the older one is the one that
    # has to go.
    #
    # And the same FILE, not the same file NAME.  This compared the name alone -
    # `qemu_flash.bin` - which nearly every run in every worktree shares, so a run
    # here still killed a run in another tree whenever both used the default
    # image.  The image is now taken from this tree's own record of what it
    # started (qemu-common.ps1, Register-Emulator), with its full path: an
    # emulator another tree started is not in that record at all.
    Stop-Emulators -Flash $flashPath
}

# Somebody else's emulator on our console port is not ours to stop - but it is
# ours to name.  Without this the run waits a minute for a prompt and then says
# "could not connect", which reads as a firmware that did not boot.
$holder = Get-PortHolder -Port $Port
if ($holder) {
    throw ("port $Port is already held by $($holder.ProcessName) (pid $($holder.Id)), " +
           'which this tree did not start.  Pick another -Port, or stop that ' +
           'process if it is a leftover of yours from before 3 Oct 2026.')
}

# QEMU's own chatter goes to a file: it is not part of what the board said,
# and PowerShell renders anything on stderr as an error.
$qemuOut = $(if ($Tag) { "build\qemu-emulator-$Tag.log" } else { 'build\qemu-emulator.log' })
# An escape hatch for debugging, and the only way to attach one: ARGON_QEMU_EXTRA
# is split on spaces and appended, so `-s` puts a gdb stub on :1234 and `-S`
# holds the machine until the debugger says go.
if ($env:ARGON_QEMU_EXTRA) {
    $qemuArgs += ($env:ARGON_QEMU_EXTRA -split ' ' | Where-Object { $_ -ne '' })
}

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -NoNewWindow `
    -PassThru -RedirectStandardOutput $qemuOut -RedirectStandardError "$qemuOut.err"
Register-Emulator -Process $proc -Flash $flashPath

# A forward that could not be set up is said once, quietly, to that file.
#
# The emulator does not fail when a hostfwd port is taken - it warns and runs
# on without it, and the port it is usually missing is the one the virtual
# mouse is reached through.  What that looks like from here is a driver that
# never came up: sixty seconds of waiting, a gesture played into nothing and
# half a dozen unrelated failures downstream.  It was read as somebody else's
# emulator twice and as a shell bug once before the line below was written.
#
# A leftover emulator is the usual reason, and this file kills those a few
# lines above - but killing is not releasing, and the ports come free a
# moment later than the process goes away.
Start-Sleep -Milliseconds 700
if (Test-Path "$qemuOut.err") {
    $fwd = Select-String -Path "$qemuOut.err" -Pattern 'host forwarding' `
        -SimpleMatch -ErrorAction SilentlyContinue
    if ($fwd) {
        throw ("the emulator could not set up a forwarded port, so the " +
               "run would look like a dead driver: " +
               $fwd[0].Line.Trim())
    }
}

# With graphics, the window has to be up before the guest chooses its mode.
#
# SDL creates it at 800x600 and resizes when the RGB panel appears; a window
# that is minimised at that moment stays 800x600, and every photograph of the
# run is then of a window that is not the panel.  Windows minimises it whenever
# something else owns the foreground, which on a machine somebody is using is
# most of the time.  So it is put up once, here, rather than at grab time -
# which is too late by then.
$winJob = $null
if ($Gfx) {
    # In the background, because waiting for it here would stop reading the
    # console - and the guest talks while the window is still coming up.  Once
    # rather than repeatedly: restoring a window takes the foreground, and
    # doing that on a timer to somebody who is working is not acceptable.
    $winJob = Start-Job -ScriptBlock {
        param($script, $ownerPid)
        & $script -RestoreOnly -OwnerPid $ownerPid
    } -ArgumentList (Join-Path $PSScriptRoot 'grab-window.ps1'), $proc.Id
}

$text = New-Object System.Text.StringBuilder
$nextFlush = [datetime]::MinValue

# Byte for byte, through Latin-1: every byte of the line was appended as the
# character of the same number, and Latin-1 is the encoding that turns it back
# into that byte.  WriteAllText's UTF-8 would encode anything above 0x7f as two
# bytes, which was invisible while the guest only ever said ASCII and turned
# every Cyrillic character into mojibake the moment it did not.
#
# One handle, opened for sharing, rather than WriteAllText each time.  The
# transcript is now rewritten once a second and something is usually watching
# it; WriteAllText opens the file exclusively, so a reader that happened to
# have it open killed the run - which is a poor way to be told that somebody
# was reading the log.
$script:logStream = $null

function Write-Transcript {
    if ($null -eq $script:logStream) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) |
            Out-Null
        $script:logStream = New-Object System.IO.FileStream(
            (Join-Path (Get-Location) $LogPath),
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::ReadWrite)
    }
    $bytes = [System.Text.Encoding]::GetEncoding(28591).GetBytes($text.ToString())
    $script:logStream.Position = 0
    $script:logStream.Write($bytes, 0, $bytes.Length)
    $script:logStream.SetLength($bytes.Length)
    $script:logStream.Flush()
}

# Emptied before the run, not only written after it.  Until the guest says
# anything the file still held the *previous* run's transcript, and every check
# made against it in the meantime - by a person or by a script waiting for a
# marker - was answered about a run that had already finished.
Write-Transcript

$client = $null
$stream = $null
$found = $false
# Set when the guest sends XOFF, cleared on XON: see Read-Available.
$script:paused = $false
# How much had arrived when the last thing was sent.  '=' items search only
# after this: a reply that came back before the wait started is still that
# send's reply, and counting occurrences instead missed exactly those.
$script:sendMark = 0

try {
    # QEMU listens before it starts executing, but not before it starts.
    $connectDeadline = (Get-Date).AddSeconds(10)
    while ($null -eq $client -and (Get-Date) -lt $connectDeadline) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if ($null -eq $client) { throw "Could not connect to QEMU serial on port $Port." }

    $stream = $client.GetStream()
    $buffer = New-Object byte[] 4096

    # Pumps whatever has arrived into $text and returns how many marker
    # occurrences are now present.
    #
    # XOFF and XON are taken out of the stream and acted on rather than recorded:
    # they are the guest saying "stop sending" and "go on", not something it put
    # on its screen, and leaving them in the transcript would put a stray
    # character into the screen vtdump reconstructs.
    function Read-Available {
        while ($stream.DataAvailable) {
            $n = $stream.Read($buffer, 0, $buffer.Length)
            if ($n -le 0) { break }
            $keep = New-Object System.Text.StringBuilder
            for ($i = 0; $i -lt $n; $i++) {
                $b = $buffer[$i]
                if ($b -eq 0x13) { $script:paused = $true; continue }
                if ($b -eq 0x11) { $script:paused = $false; continue }
                [void]$keep.Append([char]$b)
            }
            [void]$text.Append($keep.ToString())
        }
        # Write what we have, at most once a second.
        #
        # The transcript used to appear only when the run ended, which is fine
        # for a boot test that takes twenty seconds and useless for a game that
        # takes fifteen minutes: for all of them there was no way to see what
        # the guest was doing except to wait for it to stop.  A second's
        # granularity costs nothing and makes `tail` work.
        if ((Get-Date) -gt $script:nextFlush) {
            $script:nextFlush = (Get-Date).AddSeconds(1)
            Write-Transcript
        }
        $s = $text.ToString()
        $count = 0
        $idx = 0
        while (($idx = $s.IndexOf($Marker, $idx)) -ge 0) { $count++; $idx += $Marker.Length }
        return $count
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $seen = 0
    while ((Get-Date) -lt $deadline) {
        $seen = Read-Available
        if ($seen -gt 0) { $found = $true; break }
        Start-Sleep -Milliseconds 100
    }

    # A terminal sends CR for Enter; CRLF would look like two keys.
    # The parameter is $Line and NOT $Text, and that is not style.
    #
    # PowerShell variable names are case-insensitive and its scoping is dynamic,
    # so a parameter called $Text IS the script's $text - the StringBuilder the
    # whole transcript is appended to - inside this function and everything it
    # calls.  It was called $Text, and the line below therefore recorded the
    # length of the COMMAND (twelve characters, say) where it meant the length
    # of the transcript so far (hundreds of thousands).
    #
    # That mark is where a `=needle` wait starts looking, so every one of them
    # searched from the top of the run: a marker printed long before the command
    # satisfied the wait for it instantly, and the scenario ran on.  Scenarios
    # that appeared to skip ahead, and checks that passed on stale text, were
    # this.
    function Send-Line {
        param([string]$Line)
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`r")
        $script:sendMark = $text.Length
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
    }

    # Types a command and CHECKS THAT IT ARRIVED.
    #
    # The console drops input while it is busy printing, and a whole line
    # arrives from a socket faster than a UART would ever deliver it.  What that
    # looks like is not a dropped command - it is a DIFFERENT command: `link
    # ping 24 stream` lost its first character and ran as `ink`, and the failure
    # reported was "Bad command or file name", several checks away from the
    # cause.
    #
    # So: look for the echo, and if the line is not there, clear whatever half
    # of it is and type it again.  Once - a second failure is a real problem and
    # should be seen, not retried into silence.
    # NOT named $Text.  PowerShell variables are case-insensitive and its
    # scoping is dynamic, so a parameter called $Text IS the script's $text -
    # the StringBuilder every function here appends the transcript to - for this
    # function and everything it calls.  Read-Available then tried to Append to
    # a String and the whole guest died with a message about the wrong type,
    # nowhere near the line that caused it.
    function Send-Command {
        param([string]$Line)
        Send-Line $Line
        # WAITED FOR, not sampled once, and the difference is whole runs.
        #
        # This looked once after 250 ms.  A console that is still printing the
        # previous command's output echoes late - not never - so the check
        # declared the line lost and typed it AGAIN, which is the one thing it
        # must not do: `link devs` twice is not `link devs`, and two `run`s of
        # the same program fight over the same device.  Three seconds of
        # patience costs nothing when the echo is prompt, which is almost
        # always, and a genuinely dropped line is still retyped.
        # Searched from WHERE THE LINE WAS SENT, not in the last few thousand
        # characters, and that pairing matters as much as the waiting.
        #
        # It used to look once, in a 6000-character tail.  Waiting longer
        # without changing that made it WORSE, not better: a command whose
        # output starts at once - `link ping 24` prints a line per message -
        # pushes its own echo out of that tail while the check is still being
        # patient, so the check concludes the line was lost and types it again.
        # The retyped characters then stop the running ping, which reports
        # seven of twenty-four and looks exactly like a bad wire.  Measured:
        # two runs in a row, both blamed on the link.
        #
        # Send-Line records the transcript length before it writes, so
        # everything from that mark on is what arrived because of this command.
        # Nothing scrolls out of that.
        $mark = $script:sendMark
        for ($echoTry = 0; $echoTry -lt 12; $echoTry++) {
            Start-Sleep -Milliseconds 250
            [void](Read-Available)
            $since = $text.ToString()
            if ($mark -lt $since.Length) {
                $since = $since.Substring($mark)
            }
            # Escapes stripped before looking.
            #
            # What arrives is a RENDERED SCREEN, not a log: a redraw lands
            # cursor moves and erase-to-end sequences in the middle of the line
            # being echoed, so a plain search for the command text fails while
            # the command is sitting there perfectly.  That false alarm is not
            # harmless - see above.
            $since = $since -replace "$([char]27)\[[0-9;]*[A-Za-z]", ''
            if ($since.Contains($Line)) { return }
        }

        # NOT TYPED AGAIN, and that is the fix rather than the giving up it
        # looks like.  The check above cannot be made reliable while a program
        # is painting: what arrives is a rendered screen, so a redraw lands in
        # the MIDDLE of the echo ("migr" + the program's line + "ate 1") and no
        # amount of escape-stripping puts the two halves back together.  The
        # command is sitting there, entered, the whole time.
        #
        # Retyping it then does real damage, and did, twice, for months:
        #   * `link ping 24` stops on any key, so the retyped characters cut it
        #     short and it reported 23 of 24 - read as a lossy wire;
        #   * `migrate 1` became `migrate 1migrate 1`, and the migration that
        #     never happened was read as a broken feature.
        # Both times the wrong half of the system was blamed.
        #
        # So: say so, and let the '=' wait that follows decide.  A command that
        # really never arrived fails there, with its own name on it.
        Write-Host "warning: no echo seen for '$Line' (a painting program hides it); carrying on" `
            -ForegroundColor Yellow
    }

    # How many times some text appears in what has arrived so far.
    #
    # Note what this is counting: a rendered screen, not a log.  When the screen
    # scrolls, rows that are still on it are drawn again, so the same text can be
    # counted more than once.  For waiting on a command that is fine - one more
    # prompt than before means it finished, whether the count grew by one or
    # three.  For anything where "it happened" must be exact, wait for text that
    # is printed once and does not sit on the screen scrolling.
    function Count-Of {
        param([string]$Needle)
        $s = $text.ToString()
        $count = 0
        $idx = 0
        while (($idx = $s.IndexOf($Needle, $idx)) -ge 0) {
            $count++
            $idx += $Needle.Length
        }
        return $count
    }

    # Waits for text to appear more often than it already has.
    function Wait-Text {
        param([string]$Needle, [int]$Was, [int]$Seconds)
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            [void](Read-Available)
            if ((Count-Of $Needle) -gt $Was) { return $true }
            Start-Sleep -Milliseconds 100
        }
        return $false
    }

    # Waits for one more prompt than has been seen so far, which is how a
    # command says it has finished.
    function Wait-Prompt {
        param([int]$Was, [int]$Seconds = 10)
        $deadline = (Get-Date).AddSeconds($Seconds)
        # The run's own ceiling wins: waiting for a prompt is the commonest
        # thing this script does and the commonest place for it to sit.
        if ($script:runDeadline -and $deadline -gt $script:runDeadline) {
            $deadline = $script:runDeadline
        }
        while ((Get-Date) -lt $deadline) {
            $now = Read-Available
            if ($now -gt $Was) { return $now }
            Start-Sleep -Milliseconds 100
        }
        return Read-Available
    }

    if ($found -and $Put.Count -gt 0) {
        foreach ($spec in $Put) {
            $split = $spec.IndexOf('=')
            if ($split -lt 1) { throw "Bad -Put spec '$spec'; want local=guest" }
            $local = $spec.Substring(0, $split)
            $guest = $spec.Substring($split + 1)
            if (-not (Test-Path $local)) { throw "No such file: $local" }

            $bytes = [System.IO.File]::ReadAllBytes(
                (Join-Path (Get-Location) $local))
            Write-Host "Sending $local ($($bytes.Length) bytes) to $guest"

            # TRIED MORE THAN ONCE, and that is a judgement about what is being
            # tested rather than laziness.
            #
            # This link has no flow control.  When the PC is busy - a build, a
            # second run, anything - the guest's input queue fills, the port
            # drops bytes, and recv sits waiting for the rest of a line that
            # will never come.  The whole forty-minute run then dies on the
            # scaffolding, having tested nothing; three runs died that way in
            # one afternoon.
            #
            # A transfer is a MEANS here, not the thing under test: every
            # scenario below is about the wire between two guests, not about
            # getting a file into one.  So a stalled transfer is tried again,
            # and says so loudly - a run whose transfers all needed three
            # attempts is a machine too busy to be measuring anything, and that
            # is worth seeing rather than hiding.
            #
            # recv rewrites the file from the beginning, so a retry cannot
            # leave behind the truncation it is there to repair.
            $sent = $false
            $began = Get-Date
            foreach ($attempt in 1..3) {
                if ($attempt -gt 1) {
                    Write-Host ("  attempt $attempt of 3 - the guest stopped " +
                                "keeping up; the machine is busy") `
                        -ForegroundColor Yellow
                    # Whatever half-typed line is sitting there is not hex: a
                    # bare return makes the guest reject it and give the prompt
                    # back, so the retry starts from a known place.
                    Send-Line ''
                    Start-Sleep -Milliseconds 500
                    [void](Read-Available)
                }

            # recv says "N bytes written" when it is done, and that is what is
            # waited for: the prompt cannot be used here, because a screen full
            # of scrolling hex redraws the prompts already on it and the count
            # grows on its own.  That looked exactly like a transfer that worked
            # and a file that was half there.
            Send-Line "recv $guest"

            # 64 bytes a line - 128 hex characters, which is what the line
            # editor's buffer holds with room to spare.
            #
            # Paced by the guest's own echo rather than by a delay.  There is no
            # flow control on this link: when the far end is slow - and writing to
            # FAT on flash is slow, a sector rewrite every line - the input queue
            # fills, the port drops bytes, and recv sees a line that is not hex
            # and gives up.  Waiting for each line to come back means the sender
            # can never get ahead of the receiver, whatever it is writing to.
            for ($at = 0; $at -lt $bytes.Length; $at += 64) {
                $take = [Math]::Min(64, $bytes.Length - $at)
                $hex = -join (0..($take - 1) | ForEach-Object {
                    $bytes[$at + $_].ToString('x2') })
                Send-Line $hex

                # One line at a time, and never a second one while the guest has
                # said stop.  That makes the transfer as fast as whatever is on
                # the other end can write - instant to the RAM disk, a sector
                # rewrite per line to flash - without ever getting ahead of it.
                [void](Read-Available)
                $waited = 0
                while ($script:paused -and $waited -lt 10000) {
                    Start-Sleep -Milliseconds 5
                    $waited += 5
                    [void](Read-Available)
                }
                if ($script:paused) {
                    throw "$guest stalled at byte $at : the guest never resumed"
                }
            }

            # Generous, and scaled: writing to the RAM disk is instant, but the
            # same file onto FAT on flash is a sector rewrite every 64 bytes and
            # takes seconds per kilobyte.
            # Wait for this transfer's own length, not a bare "bytes written":
            # a previous Put's report can still be on the scrolling screen and
            # would look like a new one if only the words were counted.
            #
            # Counted before `END` goes, so nothing that answers it can be in
            # the count.  A transfer that is sent again costs this wait - 183 s
            # for NODELINK.SYS - and on 26 Sep 2026 that was four times in one
            # nodetest, each long enough for node B to finish its script and
            # leave.  Why the report was not seen in time is not known yet: the
            # lines below say how long each part took, to find out.
            $done = "$($bytes.Length) bytes written"
            [void](Read-Available)
            $written = Count-Of $done
            $lines_done = Get-Date
            Send-Line 'END'
            if (Wait-Text -Needle $done -Was $written `
                          -Seconds (120 + $bytes.Length / 64)) {
                    $sent = $true
                    # When: a transfer that takes minutes is what decides
                    # whether the other guest of a pair is still there.
                    Write-Host ("  {0}: lines in {1:N1} s, written {2:N1} s after END" -f
                                $guest, ($lines_done - $began).TotalSeconds,
                                ((Get-Date) - $lines_done).TotalSeconds)
                    break
                }
                Write-Host ("  {0}: no '{1}' {2:N0} s after END (seen {3} before it)" -f
                            $guest, $done, ((Get-Date) - $lines_done).TotalSeconds,
                            $written) -ForegroundColor Yellow
            }
            if (-not $sent) {
                throw ("$guest did not arrive after three attempts: recv " +
                       "never reported it written.  The machine is too busy " +
                       "to run this - nothing else should be building or " +
                       "emulating while it does.")
            }
            $seen = Read-Available
        }
        # SAID OUT LOUD, so a second guest can wait for it.
        #
        # A pair starts B first and A half a second later, and each delivers
        # its own files - but a transfer that had to be retried costs about two
        # minutes, and then A is sending into a node whose receiver has not
        # started.  Two checks reported a dead wire that way today and once
        # before.  Invoke-Pair waits for this line rather than for a number of
        # seconds; see tools\nodetest.ps1.
        Write-Host '--- deliveries done ---'

    }

    # Ready, deliveries or none: the other guest of a pair waits for this.
    if ($found -and $ReadyFile) {
        Set-Content -Path $ReadyFile -Value 'ready' -Encoding ascii
    }

    if ($found -and $Send.Count -gt 0) {
        # The whole run's clock starts here, once, and every wait below is
        # cut short by it.  What matters as much as stopping is saying where
        # it stopped: a budget that kills the run silently is a worse version
        # of the hang it replaces.
        $runDeadline = if ($BudgetSec -gt 0) {
            (Get-Date).AddSeconds($BudgetSec)
        } else {
            [datetime]::MaxValue
        }
        $script:runDeadline = $runDeadline
        $overBudget = $false

        foreach ($cmd in $Send) {
            if ((Get-Date) -ge $runDeadline) {
                $overBudget = $true
                Write-Host ("qemu-boot: out of time after ${BudgetSec}s; " +
                            "stopped at: $cmd")
                [void]$text.Append("`r`n[host] out of time at: $cmd`r`n")
                break
            }
            # A leading ~ means raw bytes, sent without an Enter and without
            # waiting for a prompt: that is how a key that interrupts a running
            # application is delivered, since there is no prompt to wait for
            # while it runs.  \xNN escapes are decoded, so ~\x1c is Ctrl+\.
            if ($cmd.StartsWith('~')) {
                # Escapes are decoded by hand: -replace with a scriptblock is a
                # PowerShell 6 feature, and on 5.1 it quietly substitutes the
                # text of the scriptblock instead of calling it.
                $raw = $cmd.Substring(1)
                $out = New-Object System.Collections.Generic.List[byte]
                for ($i = 0; $i -lt $raw.Length; $i++) {
                    if ($raw[$i] -eq '\' -and ($i + 4) -le $raw.Length -and
                        $raw[$i + 1] -eq 'x') {
                        $out.Add([byte][Convert]::ToInt32(
                            $raw.Substring($i + 2, 2), 16))
                        $i += 3
                    } else {
                        $out.Add([byte][char]$raw[$i])
                    }
                }
                $bytes = $out.ToArray()
                $script:sendMark = $text.Length
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Flush()
                Start-Sleep -Milliseconds 300
                $seen = Read-Available
                continue
            }
            # A leading ! runs a command on *this* machine and sends the guest
            # nothing.  It is here for the same reason the ~ items are: some of
            # what a test has to do happens while an application is running and
            # there is no prompt to hang the next step off.  A graphical
            # program is driven by tools\inputplay.py over a socket and
            # photographed with tools\grab-window.ps1, and both of those are
            # host commands that have to land between the launch and the quit.
            #
            # Whatever it prints goes into the transcript, so a run that failed
            # for a host-side reason says so in the same file as the guest's
            # own output.
            if ($cmd.StartsWith('!')) {
                $host_cmd = $cmd.Substring(1)
                Write-Host "host: $host_cmd"
                [void]$text.Append("`r`n[host] $host_cmd`r`n")
                # What has arrived so far, on disk, BEFORE the command runs.
                #
                # A host step may need to read what the guest has said: the
                # desktop's test asks the program where it drew its keyboard
                # and then clicks there.  The transcript used to be written
                # once, at the end, so such a step read the PREVIOUS run's
                # file - and said, with the line plainly in the log
                # afterwards, that the program had never printed it.
                try {
                    New-Item -ItemType Directory -Force `
                             -Path (Split-Path -Parent $LogPath) | Out-Null
                    [System.IO.File]::WriteAllText(
                        (Join-Path (Get-Location) $LogPath), $text.ToString(),
                        [System.Text.Encoding]::GetEncoding(28591))
                } catch {
                    # A flush that fails is not a reason to fail the run: the
                    # step may not need the file at all.
                }
                try {
                    $out = Invoke-Expression $host_cmd 2>&1 | Out-String
                    if ($out) {
                        Write-Host $out.TrimEnd()
                        [void]$text.Append($out)
                    }
                } catch {
                    $msg = "[host] FAILED: $($_.Exception.Message)"
                    Write-Host $msg
                    [void]$text.Append("$msg`r`n")
                }
                $seen = Read-Available
                continue
            }
            # Wait for text rather than sending anything: '=' items exist for
            # a command that finishes when something else is done - a server in
            # the guest answering the last request a host-side prober makes.
            if ($cmd.StartsWith('=')) {
                $needle = $cmd.Substring(1)
                $mark = $script:sendMark
                $deadline = (Get-Date).AddSeconds($TimeoutSec)
                if ($deadline -gt $runDeadline) { $deadline = $runDeadline }
                $ok = $false
                while ((Get-Date) -lt $deadline) {
                    [void](Read-Available)
                    if ($text.ToString().IndexOf($needle, $mark) -ge 0) {
                        $ok = $true
                        break
                    }
                    Start-Sleep -Milliseconds 100
                }
                if (-not $ok) { Write-Host "warning: '$needle' never appeared" }
                $seen = Read-Available
                continue
            }
            # Prefer stable completion markers: screen redraws bump `\>` counts
            # and would cut a long `copy` / `run` short.
            if ($cmd -match '(?i)^copy\s') {
                $wasCopy = Count-Of 'file(s) copied'
                $wasErr = Count-Of ': no space left'
                Send-Command $cmd
                $deadline = (Get-Date).AddSeconds($TimeoutSec)
                if ($deadline -gt $runDeadline) { $deadline = $runDeadline }
                while ((Get-Date) -lt $deadline) {
                    [void](Read-Available)
                    if ((Count-Of 'file(s) copied') -gt $wasCopy) { break }
                    if ((Count-Of ': no space left') -gt $wasErr) { break }
                    Start-Sleep -Milliseconds 200
                }
                $seen = Read-Available
            } elseif ($cmd -match '(?i)^wget\s') {
                # A transfer prints one line when it is done and a different
                # one when it is not; the prompt cannot be used, because the
                # progress line scrolls the screen and redraws the prompts
                # already on it - which looked exactly like a finished fetch
                # and cut the next command into the running one.
                # The ways it can end, each printed once.  Not ': ' or the
                # prompt: log lines and redraws both contain those.
                $endings = @('saved ', 'no answer', 'Not Found', 'cannot be',
                             'no reply within', 'not http', 'too many redirects',
                             'this system speaks', 'holds the')
                $was = @{}
                foreach ($e in $endings) { $was[$e] = Count-Of $e }
                Send-Command $cmd
                $deadline = (Get-Date).AddSeconds($TimeoutSec)
                if ($deadline -gt $runDeadline) { $deadline = $runDeadline }
                while ((Get-Date) -lt $deadline) {
                    [void](Read-Available)
                    $done = $false
                    foreach ($e in $endings) {
                        if ((Count-Of $e) -gt $was[$e]) { $done = $true; break }
                    }
                    if ($done) { break }
                    Start-Sleep -Milliseconds 200
                }
                $seen = Read-Available
            } elseif ($cmd -match '(?i)^httpd\s') {
                # Serves until Ctrl+C, so there is no prompt to wait for and
                # nothing to wait for it with: what follows in the list is
                # what stops it.
                $wasUp = Count-Of 'Ctrl+C to stop'
                Send-Command $cmd
                [void](Wait-Text -Needle 'Ctrl+C to stop' -Was $wasUp `
                                 -Seconds $TimeoutSec)
                $seen = Read-Available
            } elseif ($cmd -match '(?i)^run\s+/b\b') {
                # A BACKGROUND run never reports an exit - that is what /b
                # means - so waiting for "returned" is waiting for something
                # that cannot happen.  It cost the whole command timeout and
                # then gave up, and the damage was not the wait: it pushed the
                # commands AFTER it past the run's budget, so a journal dump
                # that a check depended on never happened at all.  Twenty
                # minutes of a run, and the failure pointed at the feature.
                #
                # What the shell does print, exactly once, when a background
                # process starts.
                $wasStart = Count-Of 'started pid '
                Send-Command $cmd
                if (-not (Wait-Text -Needle 'started pid ' -Was $wasStart `
                                    -Seconds 60)) {
                    Write-Host "warning: background run did not report a pid"
                }
                $seen = Read-Available
            } elseif ($cmd -match '(?i)^run\s') {
                # EVERY WAY A RUN CAN END, not only the happy one.
                #
                # This waited for "returned " alone, and a run that crossed the
                # wire did not print it: the shell reported a remote run's exit
                # in silence while `link run` reported it in words.  So the
                # wait ran to the full command timeout - SEVEN MINUTES in
                # xclutest - and the node on the other end, whose own script is
                # a fixed number of seconds long, reached the end of it and
                # exited while this one waited.  Everything asked after that
                # went to a machine that was gone: retransmitted to the ceiling
                # and answered -60, with the far node's counters standing still
                # because there was no far node.  That is the whole of the
                # "a node serves exactly one remote run per session" fault that
                # was written up here and in docs\plans\cluster.md.
                #
                # The shell now says "returned N from <node>", which is the
                # real fix; this list is the belt to that pair of braces, so
                # that the next answer nobody thought of costs a warning rather
                # than a timeout.
                $endings = @('returned ', 'did not run there',
                             'the other node:')
                $was = @{}
                foreach ($e in $endings) { $was[$e] = Count-Of $e }
                Send-Command $cmd
                $deadline = (Get-Date).AddSeconds($TimeoutSec)
                if ($deadline -gt $runDeadline) { $deadline = $runDeadline }
                $done = $false
                while (-not $done -and (Get-Date) -lt $deadline) {
                    [void](Read-Available)
                    foreach ($e in $endings) {
                        if ((Count-Of $e) -gt $was[$e]) { $done = $true; break }
                    }
                    if (-not $done) { Start-Sleep -Milliseconds 200 }
                }
                if (-not $done) {
                    Write-Host "warning: run did not report process exit"
                }
                $seen = Read-Available
            } else {
                Send-Command $cmd
                $seen = Wait-Prompt -Was $seen -Seconds $TimeoutSec
            }
        }
    }

    # Let any trailing output land.
    $quietDeadline = (Get-Date).AddMilliseconds($QuietMs)
    while ((Get-Date) -lt $quietDeadline) {
        [void](Read-Available)
        Start-Sleep -Milliseconds 100
    }
} finally {
    if ($stream) { $stream.Dispose() }
    if ($client) { $client.Dispose() }
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    Unregister-Emulator -Process $proc
    if ($hostfsProc -and -not $hostfsProc.HasExited) {
        Stop-Process -Id $hostfsProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($winJob) {
        # Whatever it said about the window is worth seeing: "no window
        # appeared" explains a black photograph, and nothing else does.
        Receive-Job $winJob -ErrorAction SilentlyContinue | Out-Host
        Remove-Job $winJob -Force -ErrorAction SilentlyContinue
    }
    if ($radioProc -and -not $radioProc.HasExited) {
        Stop-Process -Id $radioProc.Id -Force -ErrorAction SilentlyContinue
    }

    # Written here, not after the try: a run that failed is the run whose
    # transcript is worth having, and a throw would have skipped it.
    Write-Transcript
    if ($null -ne $script:logStream) {
        $script:logStream.Dispose()
        $script:logStream = $null
    }
}

if ($found) {
    Write-Host "--- marker '$Marker' seen; transcript in $LogPath ---"
    exit 0
}
Write-Host "--- marker '$Marker' NOT seen within ${TimeoutSec}s; transcript in $LogPath ---"
exit 1
