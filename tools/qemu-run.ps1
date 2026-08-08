# Runs ArgonOS in QEMU and attaches your terminal to its console.
#
#   . .\tools\idf-env.ps1
#   .\tools\qemu-run.ps1
#
# You get the A:\> prompt in this window and can type at it.
# To quit the emulator press Ctrl+A then X.
#
# Use -Tcp to expose the console on a TCP port instead, for connecting with
# PuTTY (Connection type: Raw, 127.0.0.1 port 5556) or any terminal program:
#
#   .\tools\qemu-run.ps1 -Tcp
[CmdletBinding()]
param(
    [switch]$Tcp,
    [int]$Port = 5556,
    [switch]$NoBuild,
    [switch]$Sd,
    [string]$SdImage = 'build\sdcard.img',
    # Open Espressif QEMU's virtual RGB panel (SDL window). Soft fb / gfx flush
    # land there. Serial console stays on stdio or -Tcp; keep focus on that
    # terminal for keyboard (the SDL window is video-only).
    [switch]$Gfx,
    # Live host folder as guest H: (UART1 ↔ tools/hostfsd.py on HostFsPort).
    [string]$HostFs = '',
    [int]$HostFsPort = 5557
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

if (-not $NoBuild) {
    & idf.py build | Out-Null
    if (-not $?) { throw 'Build failed.' }
}

$qemu = Resolve-Qemu
Update-FlashImage
$efuse = Initialize-EfuseFile

$qemuArgs = Get-QemuMachineArgs -EfusePath $efuse

if ($Sd) {
    $qemuArgs += Get-QemuSdArgs -Path $SdImage
}

$hostfsProc = $null
$hostfsSerial = $null
if ($HostFs) {
    if (-not (Test-Path -LiteralPath $HostFs)) {
        New-Item -ItemType Directory -Force -Path $HostFs | Out-Null
        Write-Host "HostFS: created $HostFs"
    } elseif (-not (Test-Path -LiteralPath $HostFs -PathType Container)) {
        throw "HostFs path is not a directory: $HostFs"
    }
    $python = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
        -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $python) {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if (-not $python) { throw 'Python not found for hostfsd.' }
        $py = $python.Source
    } else {
        $py = $python.FullName
    }
    $hostfsScript = (Resolve-Path (Join-Path $PSScriptRoot 'hostfsd.py')).Path
    $rootAbs = (Resolve-Path -LiteralPath $HostFs).Path
    $hostfsOut = Join-Path (Get-Location) 'build\hostfsd.out.log'
    $hostfsErr = Join-Path (Get-Location) 'build\hostfsd.err.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $hostfsOut) | Out-Null
    # SMS live pad: same sms.cfg the guest loads from H:\
    $padCfg = Join-Path $rootAbs 'sms.cfg'
    $defaultPadCfg = Join-Path $PSScriptRoot '..\apps\sms\sms.cfg'
    if (-not (Test-Path -LiteralPath $padCfg)) {
        if (Test-Path -LiteralPath $defaultPadCfg) {
            Copy-Item -LiteralPath $defaultPadCfg -Destination $padCfg -Force
        }
    }
    if (-not (Test-Path -LiteralPath $padCfg)) {
        $padCfg = (Resolve-Path -LiteralPath $defaultPadCfg).Path
    }
    Write-Host "HostFS: $rootAbs -> guest H: (TCP $HostFsPort / UART1)"
    Write-Host "SMS pad: live H:\sms.pad from $padCfg"
    # Drop a stale hostfsd still bound to this port (previous argon run).
    try {
        Get-NetTCPConnection -LocalPort $HostFsPort -State Listen -ErrorAction SilentlyContinue |
            ForEach-Object {
                Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue
            }
        Start-Sleep -Milliseconds 200
    } catch {}
    # Start-Process mangles ArgumentList arrays when paths contain spaces
    # ("Unity Projects"); pass one pre-quoted command line instead.
    $hostfsArgs = '"{0}" --root "{1}" --port {2} --pad-cfg "{3}"' -f `
        $hostfsScript, $rootAbs, $HostFsPort, $padCfg
    $hostfsProc = Start-Process -FilePath $py `
        -ArgumentList $hostfsArgs `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $hostfsOut `
        -RedirectStandardError $hostfsErr
    Start-Sleep -Milliseconds 500
    if ($hostfsProc.HasExited) {
        $tail = @()
        foreach ($f in @($hostfsOut, $hostfsErr)) {
            if (Test-Path $f) {
                $tail += Get-Content $f -ErrorAction SilentlyContinue
            }
        }
        throw ("hostfsd exited immediately (exit {0}). {1}" -f `
            $hostfsProc.ExitCode, ($tail -join ' '))
    }
    # Appended AFTER console -serial so this is UART1, not UART0.
    $hostfsSerial = "tcp:127.0.0.1:$HostFsPort"
}

# Decide where the console goes before building the arguments: a window that
# cannot render the screen is worse than no window, so it is not offered.
if (-not $Tcp) {
    $vtOk = Enable-ConsoleVt
    Write-Host "console: $(Get-ConsoleVtReport)"

    if (-not $vtOk) {
        Write-Host ''
        Write-Host 'This window cannot display the ArgonOS screen, so the'
        Write-Host 'console is being put on a TCP port instead of here.'
        $Tcp = $true
    }
}

if ($Tcp) {
    # wait=on: the board holds still until you connect, so you see the boot
    # rather than joining after it. A TCP serial port with no peer discards
    # everything it is given.
    $display = if ($Gfx) { 'sdl' } else { 'none' }
    $qemuArgs += @('-display', $display, '-monitor', 'none',
                   '-serial', "tcp:127.0.0.1:$Port,server=on,wait=on")
    Write-Host ''
    Write-Host "ArgonOS console on 127.0.0.1:$Port (raw TCP)."
    Write-Host 'Connect with PuTTY: Session, Connection type Raw,'
    Write-Host "  Host Name 127.0.0.1, Port $Port, then Open."
    Write-Host 'The board waits for your connection before booting.'
    if ($Gfx) {
        Write-Host 'SDL window: live RGB. With -HostFs, pad keys work with SDL focus.'
    }
    Write-Host 'Press Ctrl+C here to stop the emulator.'
} elseif ($Gfx) {
    # Video in an SDL window; console + keyboard on this terminal.
    $qemuArgs += @('-display', 'sdl', '-serial', 'mon:stdio')
    Write-Host 'ArgonOS: SDL RGB window + console here.'
    Write-Host 'With -HostFs, SMS pad works with SDL focus (host push).'
    Write-Host 'Ctrl+A then X quits the emulator.'
} else {
    $qemuArgs += @('-nographic', '-serial', 'mon:stdio')
    Write-Host 'ArgonOS console attached to this window.  Ctrl+A then X quits.'
}

if ($hostfsSerial) {
    $qemuArgs += @('-serial', $hostfsSerial)
}

#
# Start-Process -NoNewWindow hands this console's own handles to the child.
# The call operator does not: PowerShell can sit in the middle of a native
# command's output, read it as text and print it again through its own
# rendering, which does not act on escape sequences - so the screen arrives as
# a wall of "<-[2J" even though the console mode is set correctly.
#
function Start-Qemu {
    param([string]$Exe, [string[]]$Arguments)
    return Start-Process -FilePath $Exe -ArgumentList $Arguments `
        -NoNewWindow -PassThru
}

try {
    $proc = Start-Qemu -Exe $qemu -Arguments $qemuArgs
    $proc.WaitForExit()
} finally {
    if ($hostfsProc -and -not $hostfsProc.HasExited) {
        Stop-Process -Id $hostfsProc.Id -Force -ErrorAction SilentlyContinue
    }
    # Leaving echo disabled would make the shell look broken after we exit.
    Restore-ConsoleVt
}
