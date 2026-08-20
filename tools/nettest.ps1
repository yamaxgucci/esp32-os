# The network, end to end, in the emulator.
#
#   .\argon.cmd nettest
#
# Three passes in QEMU against servers this PC runs (tools\netfixture.py), and
# one pass where the guest is the server and this PC is the client.  Nothing
# here touches the internet, so it answers the same way on a machine that has
# none - except for one case, which is the point of it: fetching by *name*
# needs a resolver, and a resolver needs somebody to ask.  That case is
# reported separately and does not fail the run.
#
# What is actually being checked, and why each one is here:
#
#   a length, and no length          the two ways a body ends
#   chunked                          the third way, which is framed
#   a redirect, absolute and relative
#   404                              a failure that must not leave a file
#   https                            refused, because there is no TLS here
#   ftp                              two connections at once, and PASV
#   httpd                            32 KB back out of the guest, byte for byte
#   /../ and /%2e%2e/                refused twice, decoded and not
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 60,
    # Leave the fixture servers running afterwards, for poking at by hand.
    [switch]$KeepFixture
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$fixtureRoot = 'build\netfix'
$probeOut = 'build\httpd_probe.txt'
$httpPort = 8000
$ftpPort = 2121
$guestPort = 5558   # QEMU forwards this one; see Get-QemuNetArgs

$python = if ($env:ARGON_PYTHON) { $env:ARGON_PYTHON } else {
    (Get-Command python -ErrorAction Stop).Source
}

$results = New-Object System.Collections.Generic.List[string]
function Note {
    param([bool]$Ok, [string]$Name, [string]$Detail = '')
    $line = ($(if ($Ok) { 'PASS ' } else { 'FAIL ' })) + $Name +
            $(if ($Detail) { " - $Detail" } else { '' })
    $results.Add($line)
    if ($Ok) { Write-Host $line -ForegroundColor Green }
    else { Write-Host $line -ForegroundColor Red }
}

# The transcript is a rendered screen: the same text can appear several times
# because scrolling redraws it.  Presence is what matters here, never a count.
function Transcript {
    param([string]$Path)
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    return ($raw -replace "`e\[[0-9;]*[A-Za-z]", "`n")
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    Note ($Text.Contains($Needle)) $Name $(if (-not $Text.Contains($Needle)) { "no '$Needle'" })
}

# ---------------------------------------------------------------- fixture ---

Get-Process python -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*netfixture*' } |
    Stop-Process -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path 'build' | Out-Null
$fixtureLog = 'build\netfix.log'
$fixture = Start-Process -FilePath $python -WindowStyle Hidden -PassThru `
    -ArgumentList "tools\netfixture.py serve --root $fixtureRoot --http-port $httpPort --ftp-port $ftpPort" `
    -RedirectStandardOutput $fixtureLog -RedirectStandardError 'build\netfix.err'
Start-Sleep -Milliseconds 800
if ($fixture.HasExited) {
    throw "netfixture exited at once (exit $($fixture.ExitCode)); see $fixtureLog"
}
Write-Host "fixture: http :$httpPort, ftp :$ftpPort, files in $fixtureRoot"

try {
    # ------------------------------------------------------------ clients ---

    Write-Host "`n== pass 1: fetching ==" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
        -QuietMs 2000 -LogPath 'build\nettest-fetch.log' -Send @(
        'net wait'
        "wget http://10.0.2.2:$httpPort/hello.txt t:\h.txt"
        'type t:\h.txt'
        "wget http://10.0.2.2:$httpPort/chunked.txt t:\c.txt"
        "wget http://10.0.2.2:$httpPort/slow.txt t:\s.txt"
        "wget http://10.0.2.2:$httpPort/moved.txt t:\m.txt"
        "wget http://10.0.2.2:$httpPort/relative t:\r.txt"
        "wget http://10.0.2.2:$httpPort/missing.txt t:\bad.txt"
        'wget https://example.com/ t:\no.htm'
        "wget ftp://10.0.2.2:$ftpPort/hello.txt t:\f.txt"
        'type t:\f.txt'
        'dir t:\'
    ) | Out-Null

    $t = Transcript 'build\nettest-fetch.log'
    Check $t 'address 10.0.2.15' 'dhcp-address'
    Check $t 'saved T:\h.txt' 'http-length'
    Check $t 'hello from the host' 'http-content'
    Check $t 'saved T:\c.txt' 'http-chunked'
    Check $t 'saved T:\s.txt' 'http-no-length'
    Check $t '302 Found' 'http-redirect-seen'
    Check $t 'saved T:\m.txt' 'http-redirect-absolute'
    Check $t 'saved T:\r.txt' 'http-redirect-relative'
    Check $t '404 Not Found' 'http-404'
    Note (-not $t.Contains('saved T:\bad.txt')) 'http-404-leaves-no-file'
    Check $t 'this system speaks http and ftp only' 'https-refused'
    Check $t '226 sent' 'ftp-transfer-complete'
    Check $t 'saved T:\f.txt' 'ftp-fetch'
    Check $t 'using the control address' 'ftp-ignores-pasv-address'
    # 400 bytes is HELLO twenty times: the chunk framing is not in the file.
    Check $t 'c.txt                                 400' 'chunked-decoded-length'

    # -------------------------------------------------------------- server ---

    Write-Host "`n== pass 2: serving ==" -ForegroundColor Cyan
    Remove-Item -Force $probeOut -ErrorAction SilentlyContinue
    $probe = Start-Process -FilePath $python -WindowStyle Hidden -PassThru `
        -ArgumentList ("tools\netfixture.py probe --port $guestPort " +
                       "--compare $fixtureRoot\data.bin --out $probeOut --wait 240") `
        -RedirectStandardOutput 'build\probe.log' -RedirectStandardError 'build\probe.err'

    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
        -QuietMs 2000 -LogPath 'build\nettest-serve.log' -Send @(
        'net wait'
        "wget http://10.0.2.2:$httpPort/data.bin t:\data.bin"
        "httpd $guestPort t:\ /w"
        '=- /up.bin'    # the prober's last question; then we may stop the server
        '~\x03'
        'dir t:\'
    ) | Out-Null

    if (-not $probe.HasExited) {
        $probe.WaitForExit(10000) | Out-Null
        if (-not $probe.HasExited) { $probe.Kill() }
    }

    $t = Transcript 'build\nettest-serve.log'
    Check $t 'saved T:\data.bin' 'http-32k-fetch'
    Check $t 'data.bin -> 200, 32768 bytes' 'httpd-served-32k'
    Check $t '+ up.bin, 9000 bytes' 'httpd-logged-the-upload'
    Check $t 'stopped' 'httpd-stops-on-ctrl-c'

    if (Test-Path $probeOut) {
        foreach ($line in Get-Content $probeOut) {
            if ($line -match '^(PASS|FAIL) (\S+)(.*)$') {
                Note ($Matches[1] -eq 'PASS') $Matches[2] $Matches[3].Trim(' -')
            }
        }
    } else {
        Note $false 'httpd-probe' 'the prober wrote nothing'
    }

    # --------------------------------------------------------- ftp session ---

    Write-Host "`n== pass 3: an ftp session ==" -ForegroundColor Cyan
    Remove-Item -Force "$fixtureRoot\up.txt" -ErrorAction SilentlyContinue
    Remove-Item -Force -Recurse "$fixtureRoot\newdir" -ErrorAction SilentlyContinue

    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
        -QuietMs 2000 -LogPath 'build\nettest-ftp.log' -Send @(
        'net wait'
        'cd t:\'
        "~ftp ftp://10.0.2.2:$ftpPort/`r"
        '=to leave'
        "~ls`r"
        '=226'
        "~get hello.txt`r"
        '=226'
        "~put hello.txt up.txt`r"
        '=226'
        "~md newdir`r"
        '=257'
        "~del up.txt`r"
        '=250'
        "~bye`r"
        '=221'
        'dir t:\'
    ) | Out-Null

    $t = Transcript 'build\nettest-ftp.log'
    Check $t '226 that was the listing' 'ftp-list'
    Check $t 'hello.txt                              20' 'ftp-get-into-ramdisk'
    Check $t '226 stored' 'ftp-put'
    Check $t '221 bye' 'ftp-quit'
    Note (Test-Path "$fixtureRoot\newdir") 'ftp-mkdir-on-server'
    Note (-not (Test-Path "$fixtureRoot\up.txt")) 'ftp-delete-on-server'

    # --------------------------------------------------------------- names ---

    Write-Host "`n== pass 4: by name (needs the internet) ==" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
        -QuietMs 2000 -LogPath 'build\nettest-dns.log' -Send @(
        'net wait'
        'net resolve example.com'
        'net resolve no-such-host.invalid'
        'wget http://example.com/ t:\ex.htm'
    ) | Out-Null

    $t = Transcript 'build\nettest-dns.log'
    $resolved = $t -match 'example\.com is \d+\.\d+\.\d+\.\d+'
    if ($resolved) {
        Check $t 'saved T:\ex.htm' 'dns-fetch'
        Check $t 'no-such-host.invalid: not found' 'dns-negative'
    } else {
        Write-Host 'SKIP dns - no resolver answered; not counted' -ForegroundColor Yellow
    }
} finally {
    if (-not $KeepFixture -and -not $fixture.HasExited) {
        Stop-Process -Id $fixture.Id -Force -ErrorAction SilentlyContinue
    }
}

$failed = @($results | Where-Object { $_.StartsWith('FAIL') })
Write-Host ("`n{0} checks, {1} failed" -f $results.Count, $failed.Count)
if ($failed.Count -gt 0) {
    $failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host 'network: all green' -ForegroundColor Green
exit 0
