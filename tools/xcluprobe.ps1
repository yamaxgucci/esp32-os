# ONE QUESTION: does the SECOND remote run reach node B at all?
#
#   .\argon.cmd xcluprobe
#
# The fault written up at the top of tools\xclutest.ps1 says a node on this
# pair serves exactly one remote run per session.  This is not a check - it
# takes no view of what should happen - it is a measurement, arranged so that
# whatever happens carries its own evidence:
#
#   * node B prints its frame counters BETWEEN every pair of polls, so "bytes
#     in" is a curve rather than one number at the end.  A frame that never
#     arrives and a frame that arrives and is thrown away are the same -60 from
#     the other end and are opposite faults;
#   * node A prints its own counters after each run, so its retransmits are
#     attributable to one ask rather than to the session;
#   * both runs are of a program node B CAN execute, so the cascade is not in
#     the picture: the only difference between them is that one is second.
#
# Everything is reused from the last xclutest - the banked C3 image and the
# built images - because this asks nothing about the build.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 300,
    [int]$SendDelaySec = 25
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$bank = 'build-bank'
$flashB = Join-Path $bank 'flash-esp32c3.bin'
$linkPort = 5590

foreach ($f in @($flashB, 'build\apps\NODELINK.SYS', 'build\apps\NODELNRV.SYS',
                 'build\apps\NODEHIRV.AXE')) {
    if (-not (Test-Path $f)) { throw "$f is missing - run xclutest once first." }
}

Stop-Emulators
Start-Sleep -Milliseconds 500

# Node B's half, in a process of its own.  Six polls of twenty seconds with the
# counters printed between them: a hundred and twenty seconds of listening in
# which every twenty is accounted for.
$runner = 'build\xcluprobe-b.ps1'
$q = { param($a) '@(' + (($a | ForEach-Object { "'$_'" }) -join ',') + ')' }

$sendB = @('drv load t:\nodelnrv.sys', 'link name c3', 'link use node0', 'link')
for ($i = 0; $i -lt 9; $i++) { $sendB += @('link poll 20000', 'link') }
$sendB += @('log -n 60')

$putB = @('build\apps\NODELNRV.SYS=t:\nodelnrv.sys',
          'build\apps\NODEHIRV.AXE=t:\rv.axe',
          'build\apps\NODEHIRV.AXE=c:\nodehirv.axe',
          'build\apps\NODEHI.AXE=t:\xeno.axe')

$lines = @("Set-Location '$root'",
           "& '$(Join-Path $PSScriptRoot 'qemu-boot.ps1')' ``")
$lines += "    -Port 5586 -TimeoutSec $TimeoutSec -NoNet ``"
$lines += "    -LinkPort $linkPort -LinkListen -Chip 'esp32c3' ``"
$lines += "    -Flash '$flashB' -FlashAsIs -KeepOtherEmulators ``"
$lines += "    -Tag 'b-probe' -LogPath 'build\xcluprobe-b.log' ``"
$lines += "    -Put $(& $q $putB) ``"
$lines += "    -Send $(& $q $sendB)"
$lines += 'exit $LASTEXITCODE'
Set-Content -Path $runner -Value $lines -Encoding ascii

$procB = Start-Process -FilePath 'powershell' -PassThru -NoNewWindow `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                    '-File', ('"{0}"' -f (Join-Path (Get-Location) $runner))) `
    -RedirectStandardOutput 'build\xcluprobe-b.out' `
    -RedirectStandardError 'build\xcluprobe-b.err'
Start-Sleep -Milliseconds 500
if ($procB.HasExited) { throw "node B exited at once ($($procB.ExitCode))." }

# A does not start until B has its files - a retried transfer costs two minutes
# and would put this whole measurement at the wrong moment.
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $procB.HasExited) {
    $out = Get-Content 'build\xcluprobe-b.out' -Raw -ErrorAction SilentlyContinue
    if ($out -and $out.Contains('--- deliveries done ---')) { break }
    Start-Sleep -Milliseconds 500
}

try {
    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port 5576 `
        -TimeoutSec $TimeoutSec -LinkPort $linkPort -KeepOtherEmulators `
        -Flash 'build\qemu_flash_xclu_a.bin' `
        -Tag 'a-probe' -LogPath 'build\xcluprobe-a.log' `
        -Put @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -Send @('drv load t:\nodelink.sys',
                'link name s3',
                'link use node0',
                "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                'link nodes',
                'link',
                # FOUR REMOTE RUNS OF THREE KINDS, in one session, with the
                # counters between them.  Two of everything proved nothing when
                # both worked: the first shape of this probe ran the same
                # native program twice and both came back, so "the second one
                # fails" is not the rule.  What differs between the shapes is
                # how much of the system the run goes through, so run all of
                # them in order and let the first failure name its own kind.
                #
                # 1. Direct and native: A names the node, B can execute it.
                'link run t:\rv.axe one',
                'link',
                # 2. By placement: A has no such file, so `run` finds the node
                #    that does.  Two more messages before anything runs.
                'run t:\rv.axe two',
                'link',
                # 3. By placement AND across the instruction sets: B has the
                #    file, cannot execute it, and substitutes its own build.
                #    This is the shape xclutest's first session uses.
                'run t:\xeno.axe',
                'link',
                # A live round trip after it, so a wire that died during the
                # cascade is told apart from a request that was ignored.
                # NOT `link ping`: that one needs the far node to be running
                # `link echo`, which is the other half of it, and a node that
                # is holding itself open with `link poll` answers none of them.
                # "0 of 4 came back" on a wire that runs programs a second
                # later is this, and it is the right answer to the wrong
                # question.  `link devs` fetches the far registry, so it needs
                # nothing of the far node's shell.
                'link devs',
                'link',
                # 4. And the simplest kind again, last.  If this one fails
                #    after passing as number one, whatever broke the wire is
                #    above it and happened in between.
                'link run t:\rv.axe four',
                'link',
                'log -n 40') | Out-Null
} finally {
    if (-not $procB.WaitForExit($TimeoutSec * 2000)) {
        Write-Host 'node B did not finish; stopping it.' -ForegroundColor Yellow
        Stop-Process -Id $procB.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-Emulators
    Start-Sleep -Milliseconds 500
}

Write-Host ''
Write-Host 'node A: build\xcluprobe-a.log' -ForegroundColor Cyan
Write-Host 'node B: build\xcluprobe-b.log' -ForegroundColor Cyan
