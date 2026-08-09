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
  argon run -HostFs DIR    live Windows folder as guest H: (UART1 helper);
                           pushes SMS pad (~60 Hz) when sms.cfg is present
  argon run -tcp           run in QEMU, console on 127.0.0.1:5556
                           (connect with PuTTY in Raw mode)
  argon run -Share DIR     pack DIR into build\sdcard.img and boot with A:
  argon sync DIR           rebuild build\sdcard.img from a Windows folder
                           (FAT16 snapshot; then: argon run -Sd)
  argon get GUEST [OUT]    copy a file out of build\sdcard.img to the host
                           (e.g. argon get shot.ppm)
  argon test [-Send ...]   automated boot test; prints the resulting screen
  argon test -cp 866 ...   the same, when the screen is in another code page
  argon tests              host unit tests (needs a host C compiler)
  argon check              local CI: host tests, then firmware build
  argon flash -port COM5   flash a real board and open the monitor
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

    cmake -S host-tests -B build-host -G Ninja | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    cmake --build build-host | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Host build failed.' }
}

switch ($Command.ToLowerInvariant()) {

    'build' {
        Initialize-Environment
        & idf.py build @Rest
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
            } elseif ($a -match '^(?i)-HostFs$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['HostFs'] = $Rest[$i + 1]
                $i++
            } elseif ($a -match '^(?i)-HostFsPort$' -and ($i + 1) -lt $Rest.Count) {
                $runOpts['HostFsPort'] = [int]$Rest[$i + 1]
                $i++
            } elseif ($a -match '^(?i)-NoBuild$') {
                $runOpts['NoBuild'] = $true
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
                    'logpath', 'put', 'hostfs', 'hostfsport')
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

    'tests' {
        Build-HostTools
        & ctest --test-dir build-host --output-on-failure
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

        Write-Host ''
        Write-Host 'check: OK (host tests + firmware build)'
        exit 0
    }

    'flash' {
        Initialize-Environment
        & idf.py @Rest flash monitor
        exit $LASTEXITCODE
    }

    'monitor' {
        Initialize-Environment
        & idf.py @Rest monitor
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
