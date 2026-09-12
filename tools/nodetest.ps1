# Two nodes, in the emulator, with a wire between them.
#
#   .\argon.cmd nodetest              both scenarios
#   .\argon.cmd nodetest -Scenario raw    the wire alone
#   .\argon.cmd nodetest -Scenario link   the wire with the whole stack on it
#
# Boots TWO whole ArgonOS kernels on this PC and joins their UART1 ports to each
# other.  Then, twice:
#
#   raw   - measures the wire with apps\link, the same program that measured
#           the real cable between the S3 and the CYD.  Nothing of ours is in
#           the path but a UART driver, so a failure here is the harness.
#   link  - loads NODELINK.SYS on both, binds it, and bounces messages through
#           the whole stack: the shell, the provider indirection, the driver,
#           NLINK's scheduler and window, the UART, and all of it backwards.
#           A round trip here is what a remote process will be built out of.
#
# WHY BOTH ENDS ARE REAL.  Everything else in this tree that needed a second
# party used a Python process pretending to be one (hostfsd, rlinkd, the AT
# modem), which is right when the far end is a coprocessor running somebody
# else's firmware.  It is wrong here: the far end is US, and a fake node would
# be a fake of the thing under test.  The only thing simulated is the copper.
#
# HOW IT WORKS.  A serial port in QEMU is a TCP socket, and qemu-boot already
# opens one as a server (the console) and others as clients (HostFS, the radio).
# A pair is then just the two ends of one socket: node B listens on UART1, node A
# connects to it.  Nothing in the firmware knows or can know the difference.
#
# WHAT HAD TO CHANGE for it, all in qemu-boot.ps1, and each was a way for this
# to fail confusingly rather than cleanly:
#
#   * that script kills every qemu-system-xtensa before it starts, which is
#     right for one guest and fatal for two - the second shoots the first.
#     -KeepOtherEmulators, and this script does the one kill at the top.
#   * the flash drive is writable raw MTD.  Two emulators on one file corrupt
#     each other's C:, and the damage is not seen here - it is seen weeks later
#     as a file that reads back -91 on an unrelated run.  -Flash gives B its own
#     copy of the same image.
#   * QEMU's own chatter went to one log from both processes.  -Tag.
#
# TIMING.  B listens on a long window and A waits before speaking, rather than
# the two racing to be ready.  Without the wait A can finish booting first and
# push into a UART nobody has opened, and what that looks like from the
# transcript is a lossy wire - a measurement, not a mistake, and one that would
# have been believed.
#
# Exit code 0 means every check passed.
[CmdletBinding()]
param(
    [int]$TimeoutSec = 120,
    # Baud is nominal: there is no line here, only a socket, so this only has to
    # be a number both ends configure their port to.  Kept at a real rate so the
    # transcript reads like the board's.
    [int]$Baud = 921600,
    [int]$Kb = 64,
    # Long enough to cover A's boot, its file transfer and the wait below.
    [int]$RecvSecs = 60,
    [int]$SendDelaySec = 25,
    [ValidateSet('raw', 'link', 'both')]
    [string]$Scenario = 'both',
    [int]$Pings = 24
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'idf-env.ps1')
. (Join-Path $PSScriptRoot 'qemu-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$linkPort = 5570
$flashB = 'build\qemu_flash_b.bin'

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
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    return ($raw -replace "`e\[[0-9;]*[A-Za-z]", "`n")
}

function Check {
    param([string]$Text, [string]$Needle, [string]$Name)
    Note ($Text.Contains($Needle)) $Name $(if (-not $Text.Contains($Needle)) { "no '$Needle'" })
}

# Runs the pair: B's command list in the background, A's in the foreground.
#
# A separate PROCESS for B, not a background job.  Start-Job was the obvious way
# and does not work: the job's runspace cannot dot-source tools\idf-env.ps1 - it
# simply never returns, and the symptom is a job stuck in Running with nothing
# on its output, which reads exactly like a guest that failed to boot.  A
# process inherits $env: from this one, so the environment is already there.
#
# B's invocation is WRITTEN TO A FILE and that file is run, rather than being
# passed as arguments.  Not fastidiousness - two separate quoting problems, and
# the second has no clean answer:
#
#   * Start-Process does not quote for you, and this tree lives under "Unity
#     Projects", so an unquoted script path arrives as -File 'D:\Work\Unity' and
#     the child complains that the file has no .ps1 extension.  True, and silent
#     about the real problem.
#   * powershell -File passes every argument as a literal string and never
#     builds an array from one, so a -Send list of three commands binds its
#     first item to -Send and the other two POSITIONALLY - which lands "link
#     echo 60" on -BudgetSec and fails with a message about converting a string
#     to an integer.  Nothing in it mentions -Send.
#
# A generated script has neither problem: the array is written as an array.
function Invoke-Pair {
    param(
        [string]$Tag,
        [string[]]$PutBoth,
        # Files only the FAR node gets.  Used to prove where a program ran: if
        # node A never had the image, output that appears on A crossed the wire.
        [string[]]$PutB = @(),
        [string[]]$SendB,
        [string[]]$SendA
    )
    $putForB = @($PutBoth) + @($PutB)
    $runner = "build\nodetest-$Tag-b.ps1"
    $lines = @("Set-Location '$root'", "& '$(Join-Path $PSScriptRoot 'qemu-boot.ps1')' ``")
    $lines += "    -Port 5566 -NetPort 5578 -TimeoutSec $TimeoutSec ``"
    $lines += "    -LinkPort $linkPort -LinkListen -Flash '$flashB' ``"
    $lines += "    -KeepOtherEmulators -Tag 'b-$Tag' -LogPath 'build\nodetest-$Tag-b.log' ``"
    $lines += "    -Put @($(($putForB | ForEach-Object { "'$_'" }) -join ',')) ``"
    $lines += "    -Send @($(($SendB | ForEach-Object { "'$_'" }) -join ','))"
    $lines += 'exit $LASTEXITCODE'
    Set-Content -Path $runner -Value $lines -Encoding ascii

    $procB = Start-Process -FilePath 'powershell' -PassThru -NoNewWindow `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                        '-File', ('"{0}"' -f (Join-Path (Get-Location) $runner))) `
        -RedirectStandardOutput "build\nodetest-$Tag-b.out" `
        -RedirectStandardError "build\nodetest-$Tag-b.err"
    Start-Sleep -Milliseconds 500
    if ($procB.HasExited) {
        throw ("node B exited at once (code $($procB.ExitCode)); " +
               "see build\nodetest-$Tag-b.err")
    }

    try {
        # A gets its own copy of the flash image too, not the canonical one.
        #
        # It ran on the canonical one at first, and that is wrong in a way that
        # took a run to see: C: is written by whoever boots, so every pair left
        # its leavings in the image every later test starts from - and the next
        # thing to use it failed to receive a file over the console with no
        # explanation.  Both nodes get a fresh copy, because a test that
        # depends on the one before it is not a test.
        & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -Port 5556 `
            -TimeoutSec $TimeoutSec -LinkPort $linkPort -KeepOtherEmulators `
            -Flash 'build\qemu_flash_a.bin' `
            -Tag "a-$Tag" -LogPath "build\nodetest-$Tag-a.log" `
            -Put $PutBoth -Send $SendA | Out-Null
    } finally {
        if (-not $procB.WaitForExit($TimeoutSec * 2000)) {
            Write-Host 'node B did not finish; stopping it.' -ForegroundColor Yellow
            Stop-Process -Id $procB.Id -Force -ErrorAction SilentlyContinue
        }
        foreach ($f in @("build\nodetest-$Tag-b.err")) {
            if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
                Write-Host "--- $f ---" -ForegroundColor DarkGray
                Get-Content $f | Select-Object -Last 12 | Out-Host
            }
        }
        Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }
}

# ------------------------------------------------------------------ build ---

$only = @('LINK.AXE')
if ($Scenario -ne 'raw') { $only += 'NODELINK.SYS'; $only += 'HELLO.AXE' }
& python (Join-Path $PSScriptRoot 'build_apps.py') --only @only | Out-Null
foreach ($img in $only) {
    if (-not (Test-Path "build\apps\$img")) { throw "$img did not build." }
}

# One flash image, built once, before either guest can race the other to it.
Update-FlashImage

# The one kill, here rather than in each guest.
Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ------------------------------------------------------------ raw scenario ---

if ($Scenario -eq 'raw' -or $Scenario -eq 'both') {
    Write-Host "`n== the wire alone: apps\link between two kernels ==" `
        -ForegroundColor Cyan
    Write-Host "node B: console 5566, UART1 listening on $linkPort, flash $flashB"
    Write-Host "node A: console 5556, UART1 connecting to $linkPort"

    Invoke-Pair -Tag 'raw' -PutBoth @('build\apps\LINK.AXE=t:\link.axe') `
        -SendB @("run t:\link.axe recv $Baud $RecvSecs") `
        -SendA @("!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 "run t:\link.axe send $Baud $Kb")

    $a = Transcript 'build\nodetest-raw-a.log'
    $b = Transcript 'build\nodetest-raw-b.log'

    Check $a 'ArgonOS' 'raw-node-a-booted'
    Check $b 'ArgonOS' 'raw-node-b-booted'
    Check $a 'send:' 'raw-node-a-sent'
    Check $b 'recv:' 'raw-node-b-received'

    # `link` reports whole blocks, corrupt ones and lost ones separately, which
    # is the distinction the program exists for: a corrupt block means the line
    # is too fast for the wiring, a lost one means the receiver could not keep
    # up, and they are cured in opposite directions.
    $m = [regex]::Match($b,
        'got (\d+) bytes, (\d+) blocks ok, (\d+) corrupt, (\d+) lost, (\d+) stray')
    if (-not $m.Success) {
        $tail = ($b -split "`n" | Where-Object { $_ -match 'block|byte|B/s|arrived' } |
                 Select-Object -Last 4) -join ' | '
        Note $false 'raw-summary-parsed' "receiver said: $tail"
    } else {
        $bytes  = [int]$m.Groups[1].Value
        $blocks = [int]$m.Groups[2].Value
        $bad    = [int]$m.Groups[3].Value
        $lost   = [int]$m.Groups[4].Value
        $stray  = [int]$m.Groups[5].Value
        Write-Host ("wire: $bytes bytes, $blocks blocks ok, $bad corrupt, " +
                    "$lost lost, $stray stray")
        Note ($blocks -gt 0) 'raw-carried-blocks' "$blocks blocks"
        Note ($bad -eq 0) 'raw-no-corruption' "$bad corrupt"
        Note ($lost -eq 0) 'raw-nothing-lost' "$lost lost"
        # Stray bytes are the ones thrown away hunting for a header.  A few at
        # the start are honest; a steady stream means the framing is wrong
        # rather than the line.
        Note ($stray -lt 512) 'raw-framing-held' "$stray stray bytes"

        # The check that says "everything arrived", as against "what arrived was
        # clean".  A receive window that closed early reports zero corrupt and
        # zero lost about half a transfer, because `lost` counts gaps between
        # sequence numbers and a missing tail is not a gap.  Mutation-tested:
        # point A at a port nobody listens on and the two above pass anyway.
        $s = [regex]::Match($a, 'sent (\d+) bytes in (\d+) ms')
        if (-not $s.Success) {
            Note $false 'raw-sender-summary-parsed' 'no "sent N bytes" line'
        } else {
            Note ([int]$s.Groups[1].Value -eq $bytes) 'raw-byte-for-byte' `
                ("sent {0}, got {1}" -f $s.Groups[1].Value, $bytes)
        }
    }
}

# ----------------------------------------------------------- link scenario ---

if ($Scenario -eq 'link' -or $Scenario -eq 'both') {
    Write-Host "`n== the whole stack: NODELINK.SYS and NLINK over the same wire ==" `
        -ForegroundColor Cyan

    # Each long command is followed by a `=` step: send nothing, wait for the
    # line that says it FINISHED.
    #
    # Not decoration.  qemu-boot decides a command is done when one more prompt
    # has appeared than before, and that is a count over the whole transcript -
    # so anything that puts another `>` on the wire ends the wait early.  Both
    # nodes were cut off mid-ping that way, and the transcripts stopped in the
    # middle of a working run: the driver's own trace showed the clock ticking
    # and the receive loop spinning happily at the moment the harness gave up on
    # it.  Waiting for the sentence the command prints when it is done cannot be
    # fooled by a repaint.
    $put = @('build\apps\NODELINK.SYS=t:\nodelink.sys')
    Invoke-Pair -Tag 'link' -PutBoth $put `
        -SendB @('drv load t:\nodelink.sys',
                 'link use node0',
                 "link echo $RecvSecs",
                 '=answered ') `
        -SendA @('drv load t:\nodelink.sys',
                 'link use node0',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 "link ping $Pings",
                 '=came back')

    $a = Transcript 'build\nodetest-link-a.log'
    $b = Transcript 'build\nodetest-link-b.log'

    Check $a '/dev/node0' 'link-driver-registered'
    Check $a 'link is node0' 'link-bound-on-a'
    Check $b 'link is node0' 'link-bound-on-b'

    # The round trip.  Everything above this line is setup; this is the first
    # statement that two kernels have had a conversation.
    Check $a ("$Pings of $Pings came back") 'link-every-ping-answered'
    Check $b 'answered' 'link-far-end-answered'

    $st = [regex]::Match($a,
        'frames\s+(\d+) sent \((\d+) again\), (\d+) received')
    if (-not $st.Success) {
        Note $false 'link-stats-parsed' 'no "frames N sent" line'
    } else {
        $sent = [int]$st.Groups[1].Value
        $again = [int]$st.Groups[2].Value
        $recv = [int]$st.Groups[3].Value
        Write-Host "stack: $sent frames sent ($again again), $recv received"
        Note ($recv -ge $Pings) 'link-frames-received' "$recv received"
        # A clean socket needs no retransmissions at all.  If this ever goes
        # non-zero here, the fault is in the driver or the window - not in the
        # wire, because there is no wire.
        Note ($again -eq 0) 'link-no-retransmits-on-a-clean-socket' "$again again"
    }

    $lb = [regex]::Match($b, 'losses\s+(\d+) gaps, (\d+) duplicates, (\d+) bad')
    if ($lb.Success) {
        Note ([int]$lb.Groups[3].Value -eq 0) 'link-no-bad-frames' `
            ("{0} bad" -f $lb.Groups[3].Value)
    }

    # The driver says "listening" only when its service task started (ABI 0.50),
    # which is the difference between a link and a node: without it nothing
    # reads the wire unless somebody is inside a `link` command.
    Check $a 'listening' 'link-service-task-started'
}

# ------------------------------------------- a program run on the other node ---
#
# The thing a cluster is FOR, at its smallest: a command typed on node A loads
# and runs on node B, B's output appears on A's console, and B's exit code comes
# back as the command's own.
#
# HELLO.AXE goes to B only.  That is the point rather than an economy: A never
# has the file, so output that appears on A cannot have come from A.

if ($Scenario -eq 'link' -or $Scenario -eq 'both') {
    Write-Host "`n== a program run on the other node ==" -ForegroundColor Cyan

    Invoke-Pair -Tag 'run' `
        -PutBoth @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -PutB @('build\apps\HELLO.AXE=t:\hello.axe') `
        -SendB @('drv load t:\nodelink.sys',
                 'link use node0',
                 # B has to still BE there when A asks.  qemu-boot kills the
                 # emulator once its command list is done, so a node that just
                 # binds and waits is a node that is gone a second later - which
                 # from A looks exactly like a far end that never answers.
                 # `link poll` holds the shell without doing anything; the
                 # serving happens on the link's own reader task regardless.
                 "link poll $(($SendDelaySec + 30) * 1000)",
                 '=link on node0') `
        -SendA @('drv load t:\nodelink.sys',
                 'link use node0',
                 # Proof, before the fact, that this node has no such file.
                 'dir t:\',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 'link run t:\hello.axe one two',
                 '=from the other node')

    $a = Transcript 'build\nodetest-run-a.log'
    $b = Transcript 'build\nodetest-run-b.log'

    Check $b 'serving remote runs' 'run-far-node-is-serving'

    # The claim this scenario makes is "it ran THERE", so the first thing to
    # establish is that it could not have run here.  A's own T: is listed
    # before the run and has no hello.axe on it.
    $listing = [regex]::Match($a, 'Directory of T:\\(.|\n)*?dir\(s\)')
    $noFile = $listing.Success -and -not $listing.Value.Contains('hello.axe')
    # The detail is only for a FAILURE.  Note prints whatever it is given, so a
    # message computed for both outcomes says the opposite of the result half
    # the time - which is a test that will eventually be read wrong.
    Note $noFile 'run-local-node-has-no-such-file' `
        $(if ($noFile) { '' }
          elseif (-not $listing.Success) { 'no T: listing to check' }
          else { 'hello.axe IS on this node' })

    # So everything below crossed the wire.
    Check $a 'hello from ArgonOS' 'run-output-came-back'
    Check $a 'argv[1] = one' 'run-arguments-arrived'
    Check $a 'argv[2] = two' 'run-all-arguments-arrived'
    Check $a 'from the other node' 'run-exit-code-came-back'

    # Including the far KERNEL's own log of loading the image - which is the
    # sharpest evidence there is, because A has no loader line of its own to
    # confuse it with and no file to have loaded.
    Check $a 'loader: HELLO' 'run-far-kernel-loaded-it'
    Check $a 'returned 0' 'run-far-kernel-reported-exit'
}

# ------------------------------------------------- unloading, on one node ---
#
# The dangerous half of a driver that owns a task is not running it, it is
# STOPPING it: the task executes code inside the module's image, so the image
# cannot be freed while it is in there.  And a bound device outlives its driver
# in whoever bound it - which was a real use-after-free, and the kind that does
# not crash: after `drv unload NODELINK` the shell still said "link on node0"
# and called into an image that had been given back, answering with zeros.
#
# One guest is enough for this, and one guest is what it uses.

if ($Scenario -eq 'link' -or $Scenario -eq 'both') {
    Write-Host "`n== a driver that owns a task, unloaded ==" -ForegroundColor Cyan

    & (Join-Path $PSScriptRoot 'qemu-boot.ps1') -TimeoutSec $TimeoutSec `
        -LinkPort 5580 -LinkListen -KeepOtherEmulators -Tag 'unload' `
        -Flash 'build\qemu_flash_unload.bin' -LogPath 'build\nodetest-unload.log' `
        -Put 'build\apps\NODELINK.SYS=t:\nodelink.sys' `
        -Send @('drv load t:\nodelink.sys',
                'link use node0',
                'drv unload NODELINK',
                'dev node0',
                'link',
                'ver') | Out-Null
    Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $u = Transcript 'build\nodetest-unload.log'
    Check $u 'listening' 'unload-task-started'
    Check $u 'unloaded NODELINK' 'unload-succeeded'
    Check $u 'node0: no such device' 'unload-device-gone'
    # The binding let go.  "link on node0" here would be the use-after-free.
    Check $u 'no link bound' 'unload-binding-released'
    # And the system is still alive afterwards, which a freed image being
    # executed would not have allowed.
    Check $u 'ArgonOS 0.1.0' 'unload-system-survived'
}

# Said out loud so nobody quotes it at a board.  There is no line here and no
# baud rate: the wire is a TCP socket between two processes on this PC, and what
# it carries per second is a property of this PC.  The real cable is measured by
# running the same program on two boards.
Write-Host ''
Write-Host ('rate: QEMU only - this wire is a socket, not copper. ' +
            'For real numbers put LINK.AXE on two boards.') -ForegroundColor DarkGray

$failed = @($results | Where-Object { $_ -like 'FAIL *' })
Write-Host ("nodetest: {0} checks, {1} failed" -f $results.Count, $failed.Count)
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
