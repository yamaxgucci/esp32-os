# Boots the firmware in QEMU, optionally types commands at the console, and
# reports what came back.
#
#   . .\tools\idf-env.ps1
#   .\tools\qemu-boot.ps1
#   .\tools\qemu-boot.ps1 -Send @('ver','mem') -Marker 'A:\>'
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
    [string]$Marker = 'A:\>',
    [int]$TimeoutSec = 60,
    [string[]]$Send = @(),
    [int]$Port = 5556,
    [string]$LogPath = 'build\qemu-boot.log',
    [int]$QuietMs = 1200
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$qemu = Resolve-Qemu
Update-FlashImage
$efuse = Initialize-EfuseFile

# wait=on is essential: a TCP serial port with no peer throws its output away,
# and the whole boot is over in a quarter of a second.  Without it the test
# races the emulator and loses often enough to be useless.
$qemuArgs = (Get-QemuMachineArgs -EfusePath $efuse) + @(
    '-display', 'none'
    '-monitor', 'none'
    '-serial', "tcp:127.0.0.1:$Port,server=on,wait=on"
)

# A leftover emulator would still hold the port and answer with silence.
Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -NoNewWindow -PassThru

$text = New-Object System.Text.StringBuilder
$client = $null
$stream = $null
$found = $false

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
    function Read-Available {
        while ($stream.DataAvailable) {
            $n = $stream.Read($buffer, 0, $buffer.Length)
            if ($n -le 0) { break }
            [void]$text.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $n))
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

    if ($found -and $Send.Count -gt 0) {
        foreach ($cmd in $Send) {
            # A terminal sends CR for Enter; CRLF would look like two keys.
            $bytes = [System.Text.Encoding]::ASCII.GetBytes($cmd + "`r")
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()

            # The command is done when the prompt comes back.
            $want = $seen + 1
            $cmdDeadline = (Get-Date).AddSeconds(10)
            while ((Get-Date) -lt $cmdDeadline) {
                $seen = Read-Available
                if ($seen -ge $want) { break }
                Start-Sleep -Milliseconds 100
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
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) | Out-Null
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $LogPath), $text.ToString())

if ($found) {
    Write-Host "--- marker '$Marker' seen; transcript in $LogPath ---"
    exit 0
}
Write-Host "--- marker '$Marker' NOT seen within ${TimeoutSec}s; transcript in $LogPath ---"
exit 1
