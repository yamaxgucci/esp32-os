# Boot QEMU + HostFS, install pcmvirt/midivirt, record each synth to WAV.
# Usage (repo root, after idf-env):
#   .\tools\qemu-pcm-listen.ps1
[CmdletBinding()]
param(
    [int]$TimeoutSec = 180,
    [string]$HostFs = 'build\sd_card',
    [string]$OutDir = 'build\listen'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$py = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
    -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $py) { throw 'IDF Python not found' }
$py = $py.FullName

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$qemu = Resolve-Qemu
Update-FlashImage
$efuse = Initialize-EfuseFile
$Port = 5556
$HostFsPort = 5557

Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
try {
    Get-NetTCPConnection -LocalPort $HostFsPort -State Listen -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
} catch {}

$hostfsScript = (Resolve-Path (Join-Path $PSScriptRoot 'hostfsd.py')).Path
$rootAbs = (Resolve-Path -LiteralPath $HostFs).Path
$padCfg = Join-Path $rootAbs 'sms.cfg'
$hostfsProc = Start-Process -FilePath $py `
    -ArgumentList ('"{0}" --root "{1}" --port {2} --pad-cfg "{3}"' -f `
        $hostfsScript, $rootAbs, $HostFsPort, $padCfg) `
    -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path (Get-Location) 'build\hostfsd.out.log') `
    -RedirectStandardError (Join-Path (Get-Location) 'build\hostfsd.err.log')
Start-Sleep -Milliseconds 400

$qemuArgs = (Get-QemuMachineArgs -EfusePath $efuse) + @(
    '-display', 'none'
    '-monitor', 'none'
    '-serial', "tcp:127.0.0.1:$Port,server=on,wait=on"
) + (Get-QemuNetArgs -HostPort 5558 -GuestPort 5558) + @(
    '-serial', "tcp:127.0.0.1:$HostFsPort,reconnect=1"
)

$qemuOut = 'build\qemu-emulator.log'
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -NoNewWindow `
    -PassThru -RedirectStandardOutput $qemuOut -RedirectStandardError "$qemuOut.err"

$client = $null
$stream = $null
$text = New-Object System.Text.StringBuilder
$script:paused = $false

function Read-Available {
    $buffer = New-Object byte[] 4096
    while ($stream.DataAvailable) {
        $n = $stream.Read($buffer, 0, $buffer.Length)
        if ($n -le 0) { break }
        for ($i = 0; $i -lt $n; $i++) {
            $b = $buffer[$i]
            if ($b -eq 0x13) { $script:paused = $true; continue }
            if ($b -eq 0x11) { $script:paused = $false; continue }
            [void]$text.Append([char]$b)
        }
    }
}

function Send-Line([string]$Line) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`r")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

function Send-Raw([byte[]]$Bytes) {
    $stream.Write($Bytes, 0, $Bytes.Length)
    $stream.Flush()
}

function Wait-Needle([string]$Needle, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        Read-Available
        if ($text.ToString().Contains($Needle)) { return $true }
        Start-Sleep -Milliseconds 150
    }
    return $false
}

try {
    $deadline = (Get-Date).AddSeconds(15)
    while ($null -eq $client -and (Get-Date) -lt $deadline) {
        try { $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port) }
        catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $client) { throw 'no serial' }
    $stream = $client.GetStream()
    if (-not (Wait-Needle '\' 45)) { throw 'no guest prompt' }

    Send-Line 'drv install h:\pcmvirt.sys'
    Start-Sleep -Seconds 2
    Read-Available
    Send-Line 'drv install h:\midivirt.sys'
    Start-Sleep -Seconds 2
    Read-Available

    $apps = @(
        @{ Name = 'dx7';   Guest = 'h:\qemu_dx7.wav';   Exe = 'h:\dx7.axe';   Midi = $true;  Keys = @(); Space = $false }
        @{ Name = 'grain'; Guest = 'h:\qemu_grain.wav'; Exe = 'h:\grain.axe'; Midi = $true;  Keys = @(); Space = $false }
        @{ Name = 'irfx';  Guest = 'h:\qemu_irfx.wav';  Exe = 'h:\irfx.axe';  Midi = $false; Keys = @(); Space = $true }
        @{ Name = 'synth'; Guest = 'h:\qemu_synth.wav'; Exe = 'h:\synth.axe'; Midi = $true;  Keys = @('d','d','d','d','d','d','d','t','i'); Space = $false }
    )

    foreach ($app in $apps) {
        Write-Host "=== $($app.Name) -> $($app.Guest) ==="
        $hostWav = Join-Path $rootAbs ("qemu_{0}.wav" -f $app.Name)
        Remove-Item -Force -ErrorAction SilentlyContinue $hostWav
        Send-Line ("run {0} {1}" -f $app.Exe, $app.Guest)
        $wait = (Get-Date).AddSeconds(50)
        $last = -1
        $stable = 0
        while ((Get-Date) -lt $wait) {
            Read-Available
            if (Test-Path $hostWav) {
                $sz = (Get-Item $hostWav).Length
                if ($sz -gt 1000 -and $sz -eq $last) { $stable++ } else { $stable = 0 }
                $last = $sz
                if ($stable -ge 8) { break }
            }
            Start-Sleep -Milliseconds 250
        }
        Start-Sleep -Seconds 1
        Read-Available
        $dest = Join-Path $OutDir ("qemu_{0}.wav" -f $app.Name)
        if (Test-Path $hostWav) {
            Copy-Item $hostWav $dest -Force
            Write-Host "copied $hostWav -> $dest"
        } else {
            Write-Host "missing $hostWav"
        }
    }
} finally {
    if ($stream) { $stream.Dispose() }
    if ($client) { $client.Dispose() }
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($hostfsProc -and -not $hostfsProc.HasExited) {
        Stop-Process -Id $hostfsProc.Id -Force -ErrorAction SilentlyContinue
    }
    [System.IO.File]::WriteAllText(
        (Join-Path (Get-Location) 'build\qemu-pcm-listen.log'),
        $text.ToString(),
        [System.Text.Encoding]::GetEncoding(28591))
}

Write-Host 'done. logs: build/listen/qemu_*.pcmplay.log'
