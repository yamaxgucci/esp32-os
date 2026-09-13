# ArgonOS task runner.  Invoked through argon.cmd, which is what gets around
# the default PowerShell execution policy without changing any system setting.
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Command = 'help',

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest = @()
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Show-Help {
    Write-Host @'
ArgonOS

  argon build              build the firmware
  argon run                run in QEMU, console attached to this window
                           (Ctrl+A then X quits the emulator)
  argon run -Gfx           same + SDL window with live RGB (gfx / SMS)
  argon run -Virt          also start pcmplay + kbdvirt + mousevirt (reconnect);
                           click the QEMU window to capture keys; Right-Ctrl pauses
  argon virt               same helpers in this window (QEMU already running)
  argon run -HostFs DIR    live Windows folder as guest H: (UART1 helper);
                           pushes SMS pad (~60 Hz) when sms.cfg is present
  argon run -tcp           run in QEMU, console on 127.0.0.1:5556
                           (connect with PuTTY in Raw mode)
  argon run                (default) also OpenEth hostfwd 127.0.0.1:5558
                           (5559 midivirt, 5560 mousevirt, 5561 kbdvirt)
  argon run -NoNet         skip the virtual NIC
  python tools/pcmplay.py  play guest PCM streamed to :5558 (MD `net`);
                           or use -Virt / argon virt with kbd+mouse
  argon run -Share DIR     pack DIR into build\sdcard.img and boot with A:
  argon sync DIR           rebuild build\sdcard.img from a Windows folder
                           (FAT16 snapshot; then: argon run -Sd)
  argon get GUEST [OUT]    copy a file out of build\sdcard.img to the host
                           (e.g. argon get shot.ppm)
  argon test [-Send ...]   automated boot test; prints the resulting screen
  argon test -cp 866 ...   the same, when the screen is in another code page
  argon tests              host unit tests (needs a host C compiler)
  argon match CAPTURE.nam  fit a model's voicing to a capture of the real
                           amplifier and measure both; -Model bogner|slo|jcm800
  argon nettest            wget / httpd / ftp in QEMU against
                           tools\netfixture.py (37 checks)
  argon boardnet -port COM3   the same on the real board, over Wi-Fi;
                           add -big for a speed number (14 checks)
  python tools/netfixture.py serve   the same servers, to try things by hand
  argon check              local CI: host tests, firmware, apps, relocations
  argon target             which chip the firmware is built for
  argon target esp32c6     the RISC-V board (docs\12-esp32-c6-lcd.md)
  argon target esp32       switch to the board on the desk (docs\09-esp32-cyd.md);
                           esp32-dsp = no radios, big arena; esp32s3 switches
                           back.  esp32s3-cam = esp32s3-board with the camera
                           built into the image.  esp32s3-board = the S3 on the desk, without
                           the two lines that are for QEMU only.  Any: rebuild
  argon flash -port COM5   flash a real board and open the monitor
                           -NoMonitor: flash and return, for a script
  argon monitor -port COM5 open the serial monitor on a real board
  argon clean              remove the firmware build directory
  argon env                open a shell with the build environment loaded
  argon vt                 check whether this console can show the screen

Machine specific paths live in tools\local-env.ps1, which is not committed.
'@
}

function Resolve-HostPython {
    # Prefer a real interpreter over the Windows Store stub (exit 9009).
    foreach ($name in @('python', 'python3', 'py')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        if ($name -eq 'py') {
            $null = & $cmd.Source -3 -c "import sys" 2>$null
            if ($LASTEXITCODE -eq 0) { return @{ Exe = $cmd.Source; Prefix = @('-3') } }
            continue
        }
        $null = & $cmd.Source -c "import sys" 2>$null
        if ($LASTEXITCODE -eq 0) { return @{ Exe = $cmd.Source; Prefix = @() } }
    }
    if ($env:IDF_PYTHON -and (Test-Path -LiteralPath $env:IDF_PYTHON)) {
        return @{ Exe = $env:IDF_PYTHON; Prefix = @() }
    }
    throw 'Python not found on PATH (needed for argon sync).'
}

function Invoke-ShareSync {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [string]$Out = 'build\sdcard.img',
        [int]$SizeMB = 64
    )
    if ($SizeMB -lt 16) {
        Write-Host "Note: bumping SizeMB from $SizeMB to 16 (FAT16 needs enough clusters)."
        $SizeMB = 16
    }
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "Not a directory: $Directory"
    }
    $py = Resolve-HostPython
    $script = Join-Path $PSScriptRoot 'mkfatimg.py'
    $dirAbs = (Resolve-Path -LiteralPath $Directory).Path
    & $py.Exe @($py.Prefix) $script -o $Out -s $SizeMB $dirAbs
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        throw "mkfatimg failed (exit $LASTEXITCODE)."
    }
}

function Initialize-Environment {
    . (Join-Path $PSScriptRoot 'idf-env.ps1')
    if (-not $env:IDF_PATH) {
        throw 'ESP-IDF not found. See tools\local-env.ps1.'
    }
}

# The host tools are built with CMake so the source list lives in exactly one
# place, host-tests\CMakeLists.txt.
function Build-HostTools {
    Initialize-Environment

    $cc = Get-Command gcc -ErrorAction SilentlyContinue
    if (-not $cc) {
        throw 'No host C compiler on PATH. Set ARGON_HOST_CC_BIN in tools\local-env.ps1.'
    }

    # Quiet when it works, but print what the compiler said when it does not:
    # swallowing this turned every host build error into the bare words
    # 'Host build failed.'
    $log = cmake -S host-tests -B build-host -G Ninja 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | Write-Host
        throw 'CMake configure failed.'
    }
    $log = cmake --build build-host 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | Write-Host
        throw 'Host build failed.'
    }
}

# A defaults file newer than the generated sdkconfig, which does not mean what
# anybody expects it to mean.
#
# ESP-IDF reads sdkconfig.defaults only for symbols sdkconfig does not already
# carry.  Once sdkconfig exists, editing a defaults file changes nothing at all:
# `argon build` happily rebuilds with the old value and says nothing.  This has
# cost this project two separate afternoons - once on CONFIG_ETH_USE_OPENETH,
# where a `=n` was ignored and a QEMU-only Ethernet MAC went on being compiled
# for a chip that has no such registers, and once on
# CONFIG_PARTITION_TABLE_CUSTOM_FILENAME, where the image kept the old layout
# and a 1 MB filesystem was then written over a 512 KB partition, taking the
# start of appfs with it.  Neither failure looked anything like its cause.
#
# `argon target <chip>` removes sdkconfig first, which is why switching targets
# has always worked.  So this does not fix anything; it says which command to
# run.
function Warn-StaleSdkconfig {
    if (-not (Test-Path 'sdkconfig')) { return }
    $cfg = (Get-Item 'sdkconfig').LastWriteTimeUtc
    # sdkconfig.<chip>.<variant> fragments are defaults too, by another name, so
    # both patterns are collected.  @() because a single match is not an array
    # and += on a bare FileInfo is a method-not-found at run time.
    $newer = @(Get-ChildItem -File -ErrorAction SilentlyContinue |
               Where-Object {
                   ($_.Name -like 'sdkconfig.defaults*' -or
                    $_.Name -match '^sdkconfig\.[a-z0-9]+\.[a-z0-9]+$') -and
                   $_.LastWriteTimeUtc -gt $cfg
               })
    if (-not $newer) { return }

    $target = '<chip>'
    $line = Select-String -Path 'sdkconfig' -Pattern '^CONFIG_IDF_TARGET="(.+)"$' |
            Select-Object -First 1
    if ($line) { $target = $line.Matches[0].Groups[1].Value }

    Write-Host ''
    Write-Host 'WARNING: these are newer than sdkconfig, and this build will ignore them:' -ForegroundColor Yellow
    foreach ($f in ($newer | Select-Object -Unique)) {
        Write-Host ("  {0}" -f $f.Name) -ForegroundColor Yellow
    }
    Write-Host ('Defaults only apply to symbols sdkconfig does not already have.' +
                " Run ``argon target $target`` first to start from them again.") -ForegroundColor Yellow
    Write-Host ''
}

switch ($Command.ToLowerInvariant()) {

    'build' {
        Initialize-Environment
        Warn-StaleSdkconfig
        & idf.py build @Rest
        exit $LASTEXITCODE
    }

    'target' {
        # Which chip the firmware is for.  Two exist: esp32s3 is the primary
        # platform and the only one QEMU runs, esp32 is the board on the desk
        # (docs\09-esp32-cyd.md).  Each target picks up sdkconfig.defaults plus
        # sdkconfig.defaults.<chip>, and set-target discards the generated
        # sdkconfig, so switching is a full rebuild either way.
        Initialize-Environment
        $chip = if ($Rest.Count -ge 1) { $Rest[0] } else { '' }
        if ($chip -eq '') {
            $cur = '(none - run argon target <chip>)'
            if (Test-Path 'sdkconfig') {
                $line = Select-String -Path 'sdkconfig' -Pattern '^CONFIG_IDF_TARGET="(.+)"$' |
                        Select-Object -First 1
                if ($line) { $cur = $line.Matches[0].Groups[1].Value }
                $ble = Select-String -Path 'sdkconfig' -Pattern '^CONFIG_ARGON_ENABLE_BLE=y$' |
                       Select-Object -First 1
                if (-not $ble) { $cur = "$cur (no radios)" }
            }
            Write-Host "current target: $cur"
            Write-Host 'known targets:  esp32s3 (primary, QEMU)'
            Write-Host '                esp32s3-board  the S3 on the desk: no OpenEth, no HostFS'
            Write-Host '                esp32s3-cam    esp32s3-board + the camera built into the image'
            Write-Host '                esp32s3-zero   ESP32-S3-Zero: 4 MB flash, USB-JTAG console, I2S DAC, no display/SD'
            Write-Host '                esp32       hardware: Wi-Fi and Bluetooth'
            Write-Host '                esp32-dsp   hardware: neither, 48 KB arena'
            Write-Host '                esp32c6     hardware: RISC-V, USB console, the C6-LCD-1.47'
            exit 0
        }

        # Variants of one chip are extra defaults layered on its own, rather
        # than switches inside one file: what differs between them is not a
        # feature but how the 128 KB of instruction RAM is divided.  IDF keeps
        # SDKCONFIG_DEFAULTS in the build cache, so `argon build` afterwards
        # uses the same list.
        $defaults = ''
        switch ($chip) {
            'esp32'     { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32' }
            'esp32-dsp' { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32;sdkconfig.esp32.dsp'
                          $chip = 'esp32' }
            'esp32-probe' { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32;sdkconfig.esp32.probe'
                            $chip = 'esp32' }
            'esp32s3'   { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3' }
            'esp32s3-board' { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.esp32s3.board'
                              $chip = 'esp32s3' }
            'esp32s3-cam' { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.esp32s3.board;sdkconfig.esp32s3.cam'
                            $chip = 'esp32s3' }
            'esp32s3-zero' { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.esp32s3.zero'
                             $chip = 'esp32s3' }
            'esp32c6'   { $defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32c6' }
            default {
                Write-Host "argon target: no defaults for '$chip'."
                Write-Host 'Add sdkconfig.defaults.<chip> before building for it.'
                exit 1
            }
        }
        Remove-Item 'sdkconfig' -ErrorAction SilentlyContinue

        # set-target implies fullclean, and fullclean is shutil.rmtree over
        # everything in build\ - so one read-only file anywhere under it stops
        # the switch with WinError 5.  Not everything under build\ is IDF's: a
        # git clone left there is enough, because a pack file is read-only by
        # design and lives inside a hidden .git, which is why `attrib -R` on
        # its own does not reach it.  IDF's own guard only looks for a .git
        # directly in build\, not for one a level down.
        #
        # The second failure is worse than the first.  A half-cleaned build
        # directory has no CMakeCache.txt left, and idf.py then refuses to touch
        # it at all - "doesn't seem to be a CMake build directory" - so the next
        # attempt fails differently and the only way out is deleting the
        # directory by hand.  Do that here instead: fullclean empties it
        # completely anyway (staged .AXE images and sd_card included), so
        # removing it outright destroys nothing the switch was going to keep.
        if (Test-Path 'build') {
            $readonly = [IO.FileAttributes]::ReadOnly
            # One walk, two jobs: drop the read-only bit so rmtree can finish,
            # and name any clone on the way past.  A clone is the one thing
            # under here that costs more than a rebuild to get back, and it is
            # going either way - fullclean does not ask.
            Get-ChildItem -Recurse -Force 'build' -ErrorAction SilentlyContinue |
                ForEach-Object {
                    if ($_.PSIsContainer -and $_.Name -eq '.git') {
                        Write-Host "argon target: a git clone in the build directory is about to go - $($_.Parent.FullName)"
                    }
                    if ($_.Attributes -band $readonly) {
                        $_.Attributes = $_.Attributes -band -bnot $readonly
                    }
                }
            if (-not (Test-Path 'build\CMakeCache.txt')) {
                Remove-Item -Recurse -Force 'build' -ErrorAction SilentlyContinue
            }
        }

        & idf.py -D "SDKCONFIG_DEFAULTS=$defaults" set-target $chip
        exit $LASTEXITCODE
    }

    'get' {
        Initialize-Environment
        if ($Rest.Count -lt 1) {
            Write-Host 'Usage: argon get <guest-file> [host-out]'
            Write-Host '  example: argon get shot.ppm'
            Write-Host '           argon get a:\shot.ppm build\shot.ppm'
            exit 1
        }
        $guest = $Rest[0]
        $norm = $guest.Replace('/', '\')
        $leaf = [System.IO.Path]::GetFileName($norm.TrimStart('\'))
        if ($leaf.Length -ge 2 -and $leaf[1] -eq ':') {
            $leaf = $leaf.Substring(2)
        }
        $out = if ($Rest.Count -ge 2) { $Rest[1] } else { Join-Path 'build' $leaf }
        $img = 'build\sdcard.img'
        if (-not (Test-Path -LiteralPath $img)) {
            throw "No $img - run argon sync first, then gfxdump on the guest."
        }
        $py = Resolve-HostPython
        & $py.Exe @($py.Prefix) (Join-Path $PSScriptRoot 'fatget.py') $img $guest -o $out
        if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
            throw "fatget failed (exit $LASTEXITCODE)."
        }
        Write-Host "Saved $out - open it with any image viewer (PPM)."
        exit 0
    }

    'sync' {
        # idf-env often puts a working Python on PATH (avoids the Store stub).
        Initialize-Environment
        if ($Rest.Count -lt 1) {
            Write-Host 'Usage: argon sync <host-folder> [-SizeMB 64] [-Out build\sdcard.img]'
            exit 1
        }
        $dir = $Rest[0]
        $sizeMb = 64
        $out = 'build\sdcard.img'
        for ($i = 1; $i -lt $Rest.Count; $i++) {
            if ($Rest[$i] -match '^(?i)-SizeMB$' -and ($i + 1) -lt $Rest.Count) {
                $sizeMb = [int]$Rest[$i + 1]; $i++
            } elseif ($Rest[$i] -match '^(?i)-Out$' -and ($i + 1) -lt $Rest.Count) {
                $out = $Rest[$i + 1]; $i++
            } elseif ($Rest[$i] -like '-*') {
                throw "Unknown sync option: $($Rest[$i])"
            }
        }
        Invoke-ShareSync -Directory $dir -Out $out -SizeMB $sizeMb
        Write-Host "Synced. Boot with: argon run -Sd   (files appear on A:)"
        exit 0
    }

    'run' {
        Initialize-Environment
        # Hashtable splat so -Sd binds as a switch, not as a positional -Port value
        # (array splat '@("-Sd")' would feed the string into [int]$Port).
        $runOpts = @{}
        for ($i = 0; $i -lt $Rest.Count; $i++) {
            $a = $Rest[$i]
            if ($a -match '^(?i)-Share$' -and ($i + 1) -lt $Rest.Count) {
                Invoke-ShareSync -Directory $Rest[$i + 1]
                $runOpts['Sd'] = $true
                $i++
            } elseif ($a -match '^(?i)-Sd$') {
                $runOpts['Sd'] = $true
            } elseif ($a -match '^(?i)-Tcp$') {
                $runOpts['Tcp'] = $true
            } elseif ($a -match '^(?i)-(Gfx|Graphics)$') {
                $runOpts['Gfx'] = $true
            } elseif ($a -match '^(?i)-Virt$') {
                $runOpts['Virt'] = $true
            } elseif ($a -match '^(?i)-HostFs$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['HostFs'] = $Rest[$i + 1]
                $i++
            } elseif ($a -match '^(?i)-HostFsPort$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['HostFsPort'] = [int]$Rest[$i + 1]
                $i++
            } elseif ($a -match '^(?i)-NoBuild$') {
                $runOpts['NoBuild'] = $true
            } elseif ($a -match '^(?i)-NoNet$') {
                $runOpts['NoNet'] = $true
            } elseif ($a -match '^(?i)-Lan$') {
                $runOpts['Lan'] = $true
            } elseif ($a -match '^(?i)-NetPort$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['NetPort'] = [int]$Rest[$i + 1]
                $i++
            } elseif ($a -match '^(?i)-Port$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['Port'] = [int]$Rest[$i + 1]
                $i++
            } elseif ($a -match '^(?i)-SdImage$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['SdImage'] = $Rest[$i + 1]
                $i++
            } else {
                throw "Unknown run option: $a"
            }
        }
        & (Join-Path $PSScriptRoot 'qemu-run.ps1') @runOpts
        exit $LASTEXITCODE
    }

    'virt' {
        $py = Resolve-HostPython
        $script = Join-Path $PSScriptRoot 'virt.py'
        & $py.Exe @($py.Prefix) $script @Rest
        exit $LASTEXITCODE
    }

    'test' {
        Initialize-Environment
        & idf.py build | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

        if (-not (Test-Path 'build-host\vtdump.exe')) {
            Build-HostTools
        }

        # Bare words after "test" are commands to type at the prompt, so
        # `argon test ver mem` works from cmd without array syntax.  Options
        # that take a value have to be named: -Sd is a switch, and treating the
        # word after it as its value would swallow the first command.
        #
        # Options go into a hashtable, not an array: splatting an array passes
        # its elements positionally, so "-Sd" would arrive as the value of the
        # first positional parameter instead of as a switch.
        #
        $valued = @('marker', 'timeoutsec', 'port', 'sdimage', 'quietms',
                    'logpath', 'put', 'hostfs', 'hostfsport', 'radioport')
        $send = @()
        $opts = @{}
        # -cp says which code page the screen bytes are in, for the dump only:
        # the transcript is UTF-8 and vtdump has to convert it back to the bytes
        # the guest's cells held, or a Cyrillic screen comes out as questions.
        $codepage = 437
        for ($i = 0; $i -lt $Rest.Count; $i++) {
            if ($Rest[$i] -like '-*') {
                $name = $Rest[$i].TrimStart('-')
                if ($name.ToLowerInvariant() -eq 'cp' -and
                    ($i + 1) -lt $Rest.Count) {
                    $codepage = $Rest[$i + 1]
                    $i++
                } elseif ($valued -contains $name.ToLowerInvariant() -and
                    ($i + 1) -lt $Rest.Count) {
                    # Repeating an option collects its values, so that more than
                    # one file can be sent in a single run.
                    if ($opts.ContainsKey($name)) {
                        $opts[$name] = @($opts[$name]) + $Rest[$i + 1]
                    } else {
                        $opts[$name] = $Rest[$i + 1]
                    }
                    $i++
                } else {
                    $opts[$name] = $true
                }
            } else {
                $send += $Rest[$i]
            }
        }

        & (Join-Path $PSScriptRoot 'qemu-boot.ps1') @opts -Send $send
        $bootStatus = $LASTEXITCODE

        Write-Host ''
        Write-Host '--- screen ---'
        # vtdump reads the transcript on stdin; cmd does the redirection.  UTF-8
        # out of this console as well, or the dump is mangled on the last step
        # after being right on every earlier one.
        $wasOut = [Console]::OutputEncoding
        [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        & cmd /c "build-host\vtdump.exe 80 25 $codepage < build\qemu-boot.log"
        [Console]::OutputEncoding = $wasOut
        exit $bootStatus
    }

    'boardnet' {
        # The network on the real board: the half `nettest` cannot do, because
        # it needs a radio and a router.  Wants the board on a serial port.
        Initialize-Environment
        $rest = @()
        $port = ''
        for ($i = 0; $i -lt $Rest.Count; $i++) {
            if ($Rest[$i] -match '^-{1,2}(port|p)$') {
                $port = $Rest[$i + 1]
                $i++
            } elseif ($Rest[$i] -match '^-{1,2}big$') {
                $rest += '--big'
            } else {
                $rest += $Rest[$i]
            }
        }
        if (-not $port) {
            Write-Host 'usage: argon boardnet -port COM3 [-big]'
            exit 1
        }
        & python (Join-Path $PSScriptRoot 'boardnet.py') '-p' $port '--quiet' @rest
        exit $LASTEXITCODE
    }

    'nettest' {
        # The network in the emulator, against servers this PC runs.  Not part
        # of `check`: it wants two ports and a Python that can bind them, and a
        # firewall prompt is not a test failure.
        & (Join-Path $PSScriptRoot 'nettest.ps1') @Rest
        exit $LASTEXITCODE
    }

    'rlinktest' {
        # The same network, but over an external radio: EXTRADIO.SYS bound with
        # `net use extradio`, talking RLINK to a fake coprocessor (rlinkd.py) on
        # a UART.  Proves the external-radio socket path with no hardware.
        & (Join-Path $PSScriptRoot 'rlinktest.ps1') @Rest
        exit $LASTEXITCODE
    }

    'attest' {
        # The AT variant of rlinktest: ATRADIO.SYS bound with `net use atradio`,
        # talking the stock ESP-01 AT command set to a fake modem (atmodemd.py)
        # on a UART.  Proves the same socket path over a radio's own firmware.
        & (Join-Path $PSScriptRoot 'attest.ps1') @Rest
        exit $LASTEXITCODE
    }

    'tests' {
        Build-HostTools
        & ctest --test-dir build-host --output-on-failure
        exit $LASTEXITCODE
    }

    'match' {
        # The tone match: a capture of a real amplifier in, a fitted voicing out.
        #
        # A subcommand rather than a line in a notebook because it needs the host
        # tools built and the model named, and naming the wrong model quietly
        # fits one amplifier's voicing to another's capture - which measures fine
        # and is wrong.  Everything else the tool decides for itself, including
        # whether the capture has a loudspeaker in it.
        Build-HostTools
        $model = 'jcm800'
        $di = 'build/listen/tube_di_22050.wav'
        $cap = $null
        # Not $rest: PowerShell variable names are case insensitive, so that would
        # empty the $Rest this loop is reading.
        $extra = @()
        for ($i = 0; $i -lt $Rest.Count; $i++) {
            $a = $Rest[$i]
            if ($a -ieq '-Model') { $i++; $model = $Rest[$i] }
            elseif ($a -ieq '-Di') { $i++; $di = $Rest[$i] }
            elseif ($a -ieq '-Cab') { $i++; $env:AG_CAB_IR = $Rest[$i] }
            elseif ($null -eq $cap) { $cap = $a }
            else { $extra += $a }
        }
        if ($null -eq $cap) {
            Write-Host 'usage: argon match <capture.nam> [-Model bogner] [-Di take.wav] [-Cab ir.wav] [iterations [drive [q]]]'
            exit 2
        }
        if (-not (Test-Path $di)) {
            Write-Host ("argon match: no take at {0}." -f $di)
            Write-Host '  build-host\wavrate.exe build\listen\tube_di_22k.wav build\listen\tube_di_22050.wav 22050'
            exit 1
        }
        $env:AG_MODEL = $model
        & .\build-host\tube_render.exe match $cap $di @extra
        exit $LASTEXITCODE
    }

    'check' {
        # Local CI without a cloud runner: same gate as the no-board queue item.
        Write-Host '== host tests =='
        Build-HostTools
        & ctest --test-dir build-host --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            Write-Host 'check: host tests failed.'
            exit $LASTEXITCODE
        }

        Write-Host ''
        Write-Host '== firmware build =='
        Initialize-Environment
        & idf.py build @Rest
        if ($LASTEXITCODE -ne 0) {
            Write-Host 'check: firmware build failed.'
            exit $LASTEXITCODE
        }

        # Applications are a separate link, so the firmware building says
        # nothing about them: a change under apps\common can break half a
        # dozen .AXE images and leave this gate green.  Build them too.
        Write-Host ''
        Write-Host '== applications =='
        & python (Join-Path $PSScriptRoot 'build_apps.py') --warnings
        if ($LASTEXITCODE -ne 0) {
            Write-Host 'check: application build failed.'
            exit $LASTEXITCODE
        }

        # Does the relocation table actually move an image?
        #
        # Not "does it look right": the same source is linked twice at
        # different pairs of bases, the first image is relocated onto the
        # second's addresses using nothing but its own table, and the bytes
        # must come out equal.  Both architectures, because they relocate
        # differently - xtensa fixes absolute words in a literal pool, RISC-V
        # re-encodes the immediates of instruction pairs - and the second kind
        # had no test at all until a board was the only thing that could say.
        Write-Host ''
        Write-Host '== relocations =='
        $relargs = @('--include', 'apps/hello', '--include', 'sdk/include',
                     'apps/hello/hello.c')
        foreach ($pair in @(@('xtensa', 'xtensa-esp32s3-elf-gcc'),
                            @('riscv32', 'riscv32-esp-elf-gcc'))) {
            & python (Join-Path $PSScriptRoot 'check_axe_relocs.py') `
                --arch $pair[0] --gcc $pair[1] @relargs
            if ($LASTEXITCODE -ne 0) {
                Write-Host 'check: an image does not survive being relocated.'
                exit $LASTEXITCODE
            }
        }

        Write-Host ''
        Write-Host 'check: OK (host tests + firmware + apps + relocations)'
        exit 0
    }

    'apps' {
        # Build the .AXE / .SYS images listed in tools\apps.json.
        Initialize-Environment
        $argv = @()
        $all = $false
        foreach ($a in $Rest) {
            if ($a -ieq '-All') { $all = $true } else { $argv += $a }
        }
        if ($all) { $argv = @('--group', 'all') + $argv }
        & python (Join-Path $PSScriptRoot 'build_apps.py') @argv
        exit $LASTEXITCODE
    }

    #
    # Two things these had to learn, both found by trying to flash a board
    # from a script rather than by hand.
    #
    # `-port COM5` is what `argon boardnet` takes and what this file's own help
    # and half the documentation showed for flashing too - but flash and
    # monitor hand their arguments straight to idf.py, whose option is `-p`.
    # idf.py then reported `command "COM4" is not known to idf.py` and
    # `ninja: error: unknown target 'COM4'`, which names neither the mistake
    # nor the fix.  Rather than pick a winner between two spellings already in
    # use, translate: both work here now.
    #
    # And `flash` always ran the monitor after it, which is right at a desk and
    # impossible in a script - the monitor never returns, so an automated
    # caller hangs until its timeout with the flash long since finished.
    # -NoMonitor is that caller's form.  The monitor stays the default,
    # because someone who typed `argon flash` by hand wants to see the board
    # come up.
    #
    # Without argon at all, from the build directory, with the IDF python:
    #
    #   python -m esptool --chip <chip> -p <PORT> -b 460800 \
    #       --before default_reset --after hard_reset write_flash "@flash_args"
    #
    # The accumulator below is NOT called $rest, and that is not a style
    # choice: PowerShell variable names are case-insensitive, so `$rest = @()`
    # empties the `$Rest` parameter it was meant to read, and the loop then
    # walks nothing.  The symptom is the arguments silently vanishing - the
    # port was still passed (idf.py has a default) and the monitor still
    # started, so the only visible effect was that -NoMonitor did nothing.
    'flash' {
        Initialize-Environment
        $fwd = @()
        $noMonitor = $false
        foreach ($a in $Rest) {
            if ($a -eq '-NoMonitor') { $noMonitor = $true }
            elseif ($a -eq '-port' -or $a -eq '--port') { $fwd += '-p' }
            else { $fwd += $a }
        }
        if ($noMonitor) {
            & idf.py @fwd flash
        } else {
            & idf.py @fwd flash monitor
        }
        exit $LASTEXITCODE
    }

    'monitor' {
        Initialize-Environment
        $fwd = @()
        foreach ($a in $Rest) {
            if ($a -eq '-port' -or $a -eq '--port') { $fwd += '-p' }
            else { $fwd += $a }
        }
        & idf.py @fwd monitor
        exit $LASTEXITCODE
    }

    'vt' {
        & (Join-Path $PSScriptRoot 'vt-probe.ps1')
        exit $LASTEXITCODE
    }

    'clean' {
        Initialize-Environment
        & idf.py fullclean
        exit $LASTEXITCODE
    }

    'env' {
        Initialize-Environment
        Write-Host 'Build environment loaded. Type exit to leave.'
        # A child process inherits the environment; -NoExit keeps it open.
        & powershell -NoLogo -NoProfile -NoExit -Command "Set-Location '$root'"
        exit $LASTEXITCODE
    }

    default {
        Show-Help
        exit 0
    }
}
