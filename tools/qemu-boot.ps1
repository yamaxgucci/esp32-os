# Boots the firmware in QEMU, waits for a marker in the serial output, then
# stops the emulator and prints what it saw.
#
#   . .\tools\idf-env.ps1
#   .\tools\qemu-boot.ps1
#   .\tools\qemu-boot.ps1 -Marker "A:\>" -TimeoutSec 45
#
# ArgonOS never exits on its own, so a boot test has to be bounded from the
# outside.  Exit code 0 means the marker appeared.
[CmdletBinding()]
param(
    [string]$Marker = 'boot:',
    [int]$TimeoutSec = 60,
    [string]$LogPath = 'build\qemu-boot.log'
)

$ErrorActionPreference = 'Stop'

if (-not $env:IDF_PATH) {
    Write-Error 'Run tools\idf-env.ps1 first.'
    exit 2
}

$idf = Get-Command idf.py -ErrorAction SilentlyContinue
if (-not $idf) {
    Write-Error 'idf.py not on PATH.'
    exit 2
}

$errPath = [System.IO.Path]::ChangeExtension($LogPath, '.err')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) | Out-Null
Remove-Item $LogPath, $errPath -Force -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $idf.Source -ArgumentList 'qemu' -NoNewWindow `
    -PassThru -RedirectStandardOutput $LogPath -RedirectStandardError $errPath

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$found = $false

while ((Get-Date) -lt $deadline) {
    if (Test-Path $LogPath) {
        # -SimpleMatch: markers like "A:\>" are not regular expressions.
        if (Select-String -Path $LogPath -Pattern $Marker -SimpleMatch -Quiet) {
            $found = $true
            # Give the tail of the output a moment to land before killing it.
            Start-Sleep -Milliseconds 700
            break
        }
    }
    if ($proc.HasExited -and -not (Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue)) {
        break
    }
    Start-Sleep -Milliseconds 400
}

Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

if (Test-Path $LogPath) {
    Get-Content $LogPath
}
if ((Test-Path $errPath) -and (Get-Item $errPath).Length -gt 0) {
    Write-Host '--- stderr ---'
    Get-Content $errPath
}

if ($found) {
    Write-Host "--- marker '$Marker' found ---"
    exit 0
}
Write-Host "--- marker '$Marker' NOT found within ${TimeoutSec}s ---"
exit 1
