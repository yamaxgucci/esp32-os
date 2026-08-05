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
  argon run -tcp           run in QEMU, console on 127.0.0.1:5556
                           (connect with PuTTY in Raw mode)
  argon test [-Send ...]   automated boot test; prints the resulting screen
  argon tests              host unit tests (needs a host C compiler)
  argon flash -port COM5   flash a real board and open the monitor
  argon monitor -port COM5 open the serial monitor on a real board
  argon clean              remove the firmware build directory
  argon env                open a shell with the build environment loaded

Machine specific paths live in tools\local-env.ps1, which is not committed.
'@
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

    'run' {
        Initialize-Environment
        & (Join-Path $PSScriptRoot 'qemu-run.ps1') @Rest
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
                    'logpath')
        $send = @()
        $opts = @{}
        for ($i = 0; $i -lt $Rest.Count; $i++) {
            if ($Rest[$i] -like '-*') {
                $name = $Rest[$i].TrimStart('-')
                if ($valued -contains $name.ToLowerInvariant() -and
                    ($i + 1) -lt $Rest.Count) {
                    $opts[$name] = $Rest[$i + 1]
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
        # vtdump reads the transcript on stdin; cmd does the redirection.
        & cmd /c 'build-host\vtdump.exe < build\qemu-boot.log'
        exit $bootStatus
    }

    'tests' {
        Build-HostTools
        & ctest --test-dir build-host --output-on-failure
        exit $LASTEXITCODE
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
