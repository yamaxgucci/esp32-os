# The external radio on its FACTORY AT firmware, end to end, in the emulator.
#
#   .\argon.cmd attest
#
# The AT twin of rlinktest.  Boots the guest with no built-in radio in the
# socket path: ATRADIO.SYS is loaded, `net use atradio` binds it, and every
# socket then runs as stock AT commands (AT+CIPSTART / AT+CIPSEND / +IPD) on a
# UART to a fake ESP-01 on this PC (tools\atmodemd.py), which makes the real
# connections.  The guest fetches a file from the same netfixture nettest uses,
# so a green run means the whole stack (wget -> httpc -> netprov -> ATRADIO.SYS
# -> AT -> atmodemd -> host socket -> netfixture) works over a radio's own
# firmware, with no hardware and no built-in radio doing the talking.
#
# QEMU's esp32s3 emulates only UART0 (console) and UART1, so the modem rides
# UART1 here; the driver defaults to UART1 on the "generic" (QEMU) board.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 90,
    [switch]$KeepFixture
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$fixtureRoot = 'build\netfix'
$httpPort = 8000
$ftpPort = 2121

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

function Transcript {
    param([string]$Path)
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    return ($raw -replace "`e\[[0-9;]*[A-Za-z]", "`n")
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    Note ($Text.Contains($Needle)) $Name $(if (-not $Text.Contains($Needle)) { "no '$Needle'" })
}

# The .SYS the guest will load has to exist.
& python (Join-Path $PSScriptRoot 'build_apps.py') --only ATRADIO.SYS | Out-Null
if (-not (Test-Path 'build\apps\ATRADIO.SYS')) { throw 'ATRADIO.SYS did not build.' }

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
    Write-Host "`n== external radio (AT firmware): bind and fetch ==" -ForegroundColor Cyan
    # atmodemd.py is spawned by qemu-boot's -Radio switch (-RadioScript points it
    # at the AT modem); the fake ESP-01 dials 127.0.0.1 on this PC, where the
    # fixture is listening.
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
        -QuietMs 2000 -LogPath 'build\attest.log' -Radio -RadioScript 'atmodemd.py' `
        -Put "build\apps\ATRADIO.SYS=t:\atradio.sys" -Send @(
        'drv load t:\atradio.sys'
        'net use atradio'
        'net'
        'net resolve localhost'
        "wget http://127.0.0.1:$httpPort/hello.txt t:\h.txt"
        'type t:\h.txt'
        "wget http://127.0.0.1:$httpPort/chunked.txt t:\c.txt"
        'dir t:\'
    ) | Out-Null

    $t = Transcript 'build\attest.log'
    Check $t 'modem answers AT' 'radio-handshake'
    Check $t '/dev/atradio' 'radio-device-registered'
    Check $t 'network is via atradio' 'radio-bound-as-provider'
    Check $t 'via atradio' 'net-status-shows-provider'
    Check $t 'localhost is 127.0.0.1' 'radio-resolve'
    Check $t 'saved T:\h.txt' 'radio-http-fetch'
    Check $t 'hello from the host' 'radio-http-content'
    Check $t 'saved T:\c.txt' 'radio-http-chunked'
} finally {
    if (-not $KeepFixture) {
        if ($fixture -and -not $fixture.HasExited) {
            Stop-Process -Id $fixture.Id -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "fixture left running (pid $($fixture.Id))"
    }
}

# ------------------------------------------------------------------ result ---

$failed = @($results | Where-Object { $_ -like 'FAIL *' })
Write-Host ''
Write-Host ("attest: {0} checks, {1} failed" -f $results.Count, $failed.Count)
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
