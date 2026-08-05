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

function Resolve-Qemu {
    if (-not $env:IDF_TOOLS_PATH) {
        throw 'Run tools\idf-env.ps1 first.'
    }
    $exe = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'tools\qemu-xtensa') `
        -Filter 'qemu-system-xtensa.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $exe) {
        throw "QEMU not installed. Run: idf_tools.py install qemu-xtensa"
    }
    return $exe.FullName
}

# Rebuilds the flash image when the application binary is newer than it.
function Update-FlashImage {
    $app = 'build\argonos.bin'
    $flash = 'build\qemu_flash.bin'
    if (-not (Test-Path $app)) {
        throw "$app not found. Run idf.py build first."
    }
    if ((Test-Path $flash) -and
        (Get-Item $flash).LastWriteTime -ge (Get-Item $app).LastWriteTime) {
        return
    }

    $python = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
        -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $python) { throw 'IDF python environment not found.' }

    Write-Host 'Generating build\qemu_flash.bin'
    & $python.FullName -m esptool --chip=esp32s3 merge_bin `
        --output=$flash --fill-flash-size=8MB `
        --flash_mode dio --flash_freq 80m --flash_size 8MB `
        0x0 build\bootloader\bootloader.bin `
        0x8000 build\partition_table\partition-table.bin `
        0x10000 $app | Out-Null
    if (-not $?) { throw 'esptool merge_bin failed.' }
}

$qemu = Resolve-Qemu
Update-FlashImage

$efuse = 'build\qemu_efuse.bin'
if (-not (Test-Path $efuse)) {
    # Blank efuses: everything default, nothing burned.
    [System.IO.File]::WriteAllBytes((Join-Path (Get-Location) $efuse),
                                    (New-Object byte[] 1024))
}

$qemuArgs = @(
    '-M', 'esp32s3'
    '-m', '32M'
    '-drive', "file=build\qemu_flash.bin,if=mtd,format=raw"
    '-drive', "file=$efuse,if=none,format=raw,id=efuse"
    '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse'
    '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true'
    '-global', 'driver=ssi_psram,property=is_octal,value=true'
    '-display', 'none'
    '-monitor', 'none'
    '-serial', "tcp:127.0.0.1:$Port,server=on,wait=off"
)

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
