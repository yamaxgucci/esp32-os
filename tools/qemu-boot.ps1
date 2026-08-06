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
# Set when the guest sends XOFF, cleared on XON: see Read-Available.
$script:paused = $false

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

            # recv says "N bytes written" when it is done, and that is what is
            # waited for: the prompt cannot be used here, because a screen full
            # of scrolling hex redraws the prompts already on it and the count
            # grows on its own.  That looked exactly like a transfer that worked
            # and a file that was half there.
            $written = Count-Of 'bytes written'
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
            Send-Line 'END'
            # Generous, and generous again for flash: a fresh FAT on flash is
            # slower than a used one, so the first big transfer after a firmware
            # rebuild - which reformats C: - takes minutes rather than seconds.
            if (-not (Wait-Text -Needle 'bytes written' -Was $written `
                                -Seconds (120 + $bytes.Length / 64))) {
                throw "$guest did not arrive: recv never reported it written"
            }
            $seen = Read-Available

            # And the count it reported has to be the count that was sent.  A
            # transfer over a console with no checksum can only be trusted as far
            # as this, but a wrong length is the failure that actually happens.
            $tail = $text.ToString()
            $tail = $tail.Substring([Math]::Max(0, $tail.Length - 4096))
            if ($tail -match '(\d+) bytes written') {
                if ([int]$Matches[1] -ne $bytes.Length) {
                    throw ("$guest arrived as $($Matches[1]) bytes, not " +
                           "$($bytes.Length): the transfer lost data")
                }
            }
        }
    }

    if ($found -and $Send.Count -gt 0) {
        foreach ($cmd in $Send) {
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
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Flush()
                Start-Sleep -Milliseconds 300
                $seen = Read-Available
                continue
            }
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

    # Written here, not after the try: a run that failed is the run whose
    # transcript is worth having, and a throw would have skipped it.
    #
    # Byte for byte, through Latin-1: every byte of the line was appended as the
    # character of the same number, and Latin-1 is the encoding that turns it back
    # into that byte.  WriteAllText's UTF-8 would encode anything above 0x7f as
    # two bytes, which was invisible while the guest only ever said ASCII and
    # turned every Cyrillic character into mojibake the moment it did not.
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) |
        Out-Null
    [System.IO.File]::WriteAllText((Join-Path (Get-Location) $LogPath),
                                   $text.ToString(),
                                   [System.Text.Encoding]::GetEncoding(28591))
}

if ($found) {
    Write-Host "--- marker '$Marker' seen; transcript in $LogPath ---"
    exit 0
}
Write-Host "--- marker '$Marker' NOT seen within ${TimeoutSec}s; transcript in $LogPath ---"
exit 1
