# Boots the firmware in QEMU, optionally types commands at the console, and
# reports what came back.
#
#   . .\tools\idf-env.ps1
#   .\tools\qemu-boot.ps1
#   .\tools\qemu-boot.ps1 -Send @('ver','mem') -Marker 'A:\>'
#   .\tools\qemu-boot.ps1 -Put 'HELLO.AXE=t:\hello.axe' -Send @('run t:\hello.axe')
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
    [string]$SdImage = 'build\sdcard.img'
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

if ($Sd) {
    $qemuArgs += Get-QemuSdArgs -Path $SdImage
}

# A leftover emulator would still hold the port and answer with silence.
Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

# QEMU's own chatter goes to a file: it is not part of what the board said,
# and PowerShell renders anything on stderr as an error.
$qemuOut = 'build\qemu-emulator.log'
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -NoNewWindow `
    -PassThru -RedirectStandardOutput $qemuOut -RedirectStandardError "$qemuOut.err"

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

    # A terminal sends CR for Enter; CRLF would look like two keys.
    function Send-Line {
        param([string]$Text)
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text + "`r")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
    }

    # Waits for one more prompt than has been seen so far, which is how a
    # command says it has finished.
    function Wait-Prompt {
        param([int]$Was, [int]$Seconds = 10)
        $deadline = (Get-Date).AddSeconds($Seconds)
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

            $was = $seen
            Send-Line "recv $guest"

            # 32 bytes a line, and the line editor echoes every one of them, so
            # the reader is pumped as we go: filling the input queue faster than
            # the guest drains it is how input gets dropped.
            for ($at = 0; $at -lt $bytes.Length; $at += 32) {
                $take = [Math]::Min(32, $bytes.Length - $at)
                $hex = -join (0..($take - 1) | ForEach-Object {
                    $bytes[$at + $_].ToString('x2') })
                Send-Line $hex
                Start-Sleep -Milliseconds 20
                [void](Read-Available)
            }

            Send-Line 'END'
            $seen = Wait-Prompt -Was $was -Seconds 30
        }
    }

    if ($found -and $Send.Count -gt 0) {
        foreach ($cmd in $Send) {
            Send-Line $cmd
            $seen = Wait-Prompt -Was $seen
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
