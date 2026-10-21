# The Wi-Fi path, end to end: a board pairs itself with the shop, a person
# claims it in a browser and orders a program, and the board fetches it.
#
#   .\tools\pairtest.ps1 -Port 8091
#
# Two boots of the emulator, because the console is one conversation at a time:
# the first pairs, the second runs the agent.  Between them a browser (driven by
# store\e2e.py) does what a person does.  The flash image is reused, so the
# board keeps its token - which is the point of a token.
[CmdletBinding()]
param(
    [int]$StorePort = 8091,
    [int]$Port = 5563,
    [int]$TimeoutSec = 180,
    [string]$Dir = 'build\pair-demo'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$logDir = Join-Path $root 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logPath = Join-Path $logDir ('pairtest-' + (Get-Date).ToString('yyyyMMdd-HHmmss') + '.txt')
# Not $python: qemu-common.ps1 assigns that name for the IDF's interpreter.
$shopPython = (Resolve-Path 'store\.venv\Scripts\python.exe').Path
$storeProc = $null
$results = @()

function Check([string]$text, [string]$needle, [string]$name) {
    if ($text -like "*$needle*") {
        Write-Host "PASS $name" -ForegroundColor Green
        $script:results += "PASS $name"
    } else {
        Write-Host "FAIL $name - no '$needle'" -ForegroundColor Red
        $script:results += "FAIL $name - no '$needle'"
    }
}

try {
    Write-Host "== a shop with one published program ==" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot '..\store\make-demo.ps1') -Dir $Dir `
        -Images @('HELLO.AXE') -Admin 'admin@example.com' `
        -Password 'shop password' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the demo shop could not be made' }

    # The board trusts the root compiled into it, so this shop's root goes in
    # and STORE.AXE is rebuilt with it.
    $rootPub = Join-Path $Dir 'data\keys\root.pub'
    $headerCode = @"
import importlib.util as u, pathlib
s = u.spec_from_file_location('a', 'tools/argonsign.py')
m = u.module_from_spec(s); s.loader.exec_module(m)
m.write_public_header(pathlib.Path('tools/store-root.pub'),
                      m.load_key_file(pathlib.Path(r'$rootPub'))['public_bytes'],
                      True)
"@
    $headerFile = 'build\pair-root-header.py'
    [System.IO.File]::WriteAllText((Join-Path $root $headerFile), $headerCode,
                                   (New-Object System.Text.UTF8Encoding($false)))
    & $shopPython $headerFile
    if ($LASTEXITCODE -ne 0) { throw 'the root header could not be written' }
    & .\argon.cmd apps --only STORE.AXE | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'STORE.AXE did not build with the test root' }

    Write-Host "== the shop on :$StorePort ==" -ForegroundColor Cyan
    $env:ARGON_STORE_DATA = Join-Path $Dir 'data'
    $storeProc = Start-Process -FilePath $shopPython -WindowStyle Hidden -PassThru `
        -ArgumentList @('-m', 'uvicorn', 'store.server.app:app', '--host',
                        '127.0.0.1', '--port', "$StorePort",
                        '--log-level', 'warning') `
        -RedirectStandardOutput 'build\pair-store.log' `
        -RedirectStandardError 'build\pair-store.err'
    Start-Sleep -Milliseconds 1500
    if ($storeProc.HasExited) { throw "the shop did not start (see build\pair-store.err)" }
    $base = "http://127.0.0.1:$StorePort"

    Write-Host "== a C: with STORE.AXE on it ==" -ForegroundColor Cyan
    & python (Join-Path $PSScriptRoot 'mksysfs.py') --board none `
        --add 'build\apps\STORE.AXE' --out 'build\pair-sysfs.bin' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the C: image could not be made' }

    . (Join-Path $PSScriptRoot 'idf-env.ps1')
    . (Join-Path $PSScriptRoot 'qemu-common.ps1')
    & .\argon.cmd build 2>&1 | Select-Object -Last 1
    Update-FlashImage -SysFs 'build\pair-sysfs.bin'

    # ---- boot 1: pair ------------------------------------------------------
    Write-Host "== boot 1: the board pairs ==" -ForegroundColor Cyan
    $pass1 = 'build\pair-pass1.log'
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port $Port `
        -SysFs 'build\pair-sysfs.bin' `
        -TimeoutSec $TimeoutSec -QuietMs 2000 -BudgetSec 900 `
        -LogPath $pass1 -Send @(
        'net wait'
        ("run c:\store.axe source {0}/catalog" -f "http://10.0.2.2:$StorePort")
        'run c:\store.axe pair'
        '=pair: code'
    ) | Out-Null
    $t1 = Get-Content $pass1 -Raw
    Check $t1 'pair: code' 'pair-printed-a-code'
    $match = [regex]::Match($t1, 'pair: code ([2-9A-HJ-NP-Z]{6})')
    if (-not $match.Success) { throw 'no code in the transcript' }
    $code = $match.Groups[1].Value
    Write-Host "  the code is $code"

    # ---- the person ------------------------------------------------------
    Write-Host "== a browser claims it and orders HELLO ==" -ForegroundColor Cyan
    # A native command that writes to stderr is a terminating error under
    # 'Stop', and a Python traceback would then end the run before anything was
    # printed - which is exactly what happened, twice, and read as the claim
    # silently failing.
    $ErrorActionPreference = 'Continue'
    & $shopPython (Join-Path $PSScriptRoot '..\store\e2e.py') claim `
        --base $base --code $code --program HELLO *>&1 |
        Tee-Object -FilePath 'build\pair-claim.log' |
        ForEach-Object { Write-Host "  $_" }
    $claimExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $claimOut = Get-Content 'build\pair-claim.log' -Raw
    if ($claimExit -ne 0) {
        Write-Host $claimOut
        throw 'the claim or the order failed'
    }
    Check ($claimOut | Out-String) "ORDERED HELLO" 'claimed-and-ordered'

    # ---- boot 2: the agent fetches it --------------------------------------
    Write-Host "== boot 2: the board fetches what it was ordered ==" -ForegroundColor Cyan
    $pass2 = 'build\pair-pass2.log'
    # The same flash file the first boot left behind: the token lives in C:,
    # and a board that is re-flashed between the two halves is a board that has
    # forgotten its pairing.  -FlashAsIs is exactly "use this image as it is".
    Copy-Item 'build\qemu_flash.bin' 'build\pair-flash.bin' -Force
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port $Port `
        -Flash 'build\pair-flash.bin' -FlashAsIs `
        -TimeoutSec $TimeoutSec -QuietMs 2000 -BudgetSec 900 `
        -LogPath $pass2 -Send @(
        'net wait'
        'run c:\store.axe agent 90'
        '=STORE (pid'
        'type c:\store\installed.txt'
    ) | Out-Null
    $t2 = Get-Content $pass2 -Raw
    Check $t2 'order 1' 'agent-saw-the-order'
    Check $t2 'installed HELLO' 'agent-installed-it'
    Check $t2 'version = 1.0.0' 'registry-says-so'

    # ---- and the shop knows -----------------------------------------------
    Write-Host "== what the shop says ==" -ForegroundColor Cyan
    $sql = "import sqlite3, sys; c = sqlite3.connect(sys.argv[1]); " +
           "print(c.execute('SELECT status, note FROM orders').fetchall()); " +
           "print(c.execute('SELECT status FROM releases').fetchall())"
    $dbPath = Join-Path $Dir 'data\store.db'
    if (-not (Test-Path $dbPath)) { $dbPath = Join-Path $Dir 'data\store\store.db' }
    $said = & $shopPython -c $sql $dbPath 2>&1
    $said | ForEach-Object { Write-Host "  $_" }
    Check ($said | Out-String) "installed" 'shop-marks-it-installed'
} finally {
    if ($storeProc -and -not $storeProc.HasExited) {
        Stop-Process -Id $storeProc.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item Env:\ARGON_STORE_DATA -ErrorAction SilentlyContinue
    git checkout -- tools/store-root.pub
    & .\argon.cmd apps --only STORE.AXE | Out-Null
    $failed = @($results | Where-Object { $_.StartsWith('FAIL') })
    $header = @(
        "pairtest $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))"
        "shop: $base, data in $Dir"
        "transcripts: build\pair-pass1.log, build\pair-pass2.log"
        ''
    )
    ($header + $results) | Set-Content $logPath -Encoding UTF8
    Write-Host ("`n{0} checks, {1} failed" -f $results.Count, $failed.Count)
    Write-Host "log: $logPath"
}

$failed = @($results | Where-Object { $_.StartsWith('FAIL') })
if ($failed.Count -gt 0) { exit 1 }
Write-Host 'pair, claim, order, install: all green' -ForegroundColor Green
exit 0
