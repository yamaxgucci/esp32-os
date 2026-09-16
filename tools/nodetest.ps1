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
    # Generous, and it has to be: the remote-drive scenario is a round trip per
    # directory entry and per block of a program image, and the placement one
    # now opens a device on the other board and pours audio periods at it.
    # Too short does not report "slow" - the emulator is killed mid-list and the
    # commands after it simply never ran, which reads as a feature that does not
    # work.  It has read that way twice; raise this before believing a failure
    # whose transcript simply stops.
    [int]$TimeoutSec = 420,
    # Baud is nominal: there is no line here, only a socket, so this only has to
    # be a number both ends configure their port to.  Kept at a real rate so the
    # transcript reads like the board's.
    [int]$Baud = 921600,
    [int]$Kb = 64,
    # Long enough to cover A's boot, its file transfer and the wait below.
    [int]$RecvSecs = 60,
    [int]$SendDelaySec = 25,
    # 'migrate' is one scenario on its own, and it earns its place: the whole
    # 'link' list is forty minutes, and a handover defect is diagnosed by
    # repetition.  Everything else is unchanged by it.
    [ValidateSet('raw', 'link', 'both', 'migrate')]
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
        # Files only THIS node gets.  The mirror image, and it proves the harder
        # claim: the program moved and the file did not, so the far node read it
        # over the wire while being told to run it.
        [string[]]$PutA = @(),
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
            -Put (@($PutBoth) + @($PutA)) -Send $SendA | Out-Null
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

# ONE list, and it used to be two.
#
# There were two of these lines, the first appending to a variable that did not
# exist yet and the second assigning over it - so IDLE.AXE and NEEDGFX.AXE were
# named, looked built, and were not: the scenarios below got whatever a previous
# full build had left in build\apps.  That is the worst kind of harness fault,
# because it passes until somebody runs it on a clean tree.
$only = @('LINK.AXE')
if ($Scenario -ne 'raw') {
    $only += @('NODELINK.SYS', 'HELLO.AXE', 'NEEDSND.AXE', 'PCMVIRT.SYS',
               'DEVPOKE.AXE', 'IDLE.AXE', 'NEEDGFX.AXE', 'GRIND.AXE',
               'MIGRANT.AXE')
}
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
        #
        # BLOCKS, not bytes, and this took three runs and two wrong theories to
        # settle.  The sender's byte count is what its driver ACCEPTED; there is
        # no drain in the ABI, so the final block is still in the transmit ring
        # when the sender stops, and the receiver is left holding part of it.
        # Comparing byte counts compares two quantities that only coincide if
        # the sender drained - so it failed by a few dozen bytes, at random,
        # for ever, and read as a lossy wire.  It is not: every block that
        # completed was perfect, in order, with nothing lost between them.
        #
        # So: every block but the one in flight must have arrived whole.  A dead
        # wire still fails this - zero is not 252.
        $s = [regex]::Match($a, 'sent (\d+) bytes in (\d+) blocks')
        if (-not $s.Success) {
            Note $false 'raw-sender-summary-parsed' 'no "sent N bytes" line'
        } else {
            $sentBlocks = [int]$s.Groups[2].Value
            $short = $sentBlocks - $blocks
            Note ($short -ge 0 -and $short -le 1) 'raw-every-block-arrived' `
                ("sent {0} blocks, assembled {1}" -f $sentBlocks, $blocks)
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
                 # The window has to COVER THE WORK, so it is derived from the
                 # work rather than left at a number that used to be enough.
                 #
                 # It was $RecvSecs (60), and it fitted only because the second
                 # ping was being cut short by a harness fault: the echo check
                 # retyped a command into the running first ping and stopped it
                 # at seven messages of twenty-four.  With that fixed the first
                 # ping runs to the end, the second starts later, and 60
                 # seconds no longer reaches it - so the stream ping got no
                 # answers and the check said the stream class was broken.
                 #
                 # A number that only worked because something else was broken
                 # is worth replacing with the arithmetic: the wait before A
                 # speaks, two pings' worth at a second each, and a margin.
                 "link echo $($SendDelaySec + $Pings * 2 + 40)",
                 '=answered ') `
        -SendA @('drv load t:\nodelink.sys',
                 'link use node0',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 "link ping $Pings",
                 # WAIT FOR ITS SUMMARY before typing anything else.
                 #
                 # Without this the next command is typed as soon as a prompt
                 # appears - and a prompt appears from a redraw while the ping
                 # is still running.  The characters then land in the ping,
                 # which stops on any key, and it reports a fraction of what it
                 # sent.  Blamed on the wire twice.
                 "=of $Pings came back",
                 # And the same round trip on the class sound will ride.  It
                 # had never carried a byte between two nodes before this line.
                 "link ping $Pings stream",
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

    # The LAST one, not the first.
    #
    # These counters are printed by every `link` status, and the transcript is a
    # rendered screen that scrolls and redraws - so the same line appears many
    # times and the first occurrence is the status from BEFORE the pings, which
    # describes nothing that happened here.  Taking the last is not a tie-break:
    # these are running totals, so the last one is the only one that covers the
    # whole scenario.
    $all = [regex]::Matches($a,
        'frames\s+(\d+) sent \((\d+) again\), (\d+) received')
    $st = if ($all.Count -gt 0) { $all[$all.Count - 1] } else { $null }
    if ($null -eq $st -or -not $st.Success) {
        Note $false 'link-stats-parsed' 'no "frames N sent" line'
    } else {
        $sent = [int]$st.Groups[1].Value
        $again = [int]$st.Groups[2].Value
        $recv = [int]$st.Groups[3].Value
        Write-Host "stack: $sent frames sent ($again again), $recv received"
        Note ($recv -ge $Pings) 'link-frames-received' "$recv received"
        # Few, not none.
        #
        # None was the check at first, and it was wrong in a way worth keeping:
        # the socket loses nothing, but the round trip between two emulators
        # sharing this PC is not a fixed quantity, and it sometimes exceeds the
        # retransmit timer.  A retransmission then is correct behaviour by a
        # timer that is too short for the moment, not a fault - and an assertion
        # that fails on a busy afternoon is one nobody believes by Friday.
        #
        # Now that the timer is MEASURED, this number says something sharper
        # than it used to, and the bound had to be re-derived rather than kept.
        #
        # The round trip here measures 3 ms and the timer sits on its 30 ms
        # floor - the floor being the granularity of the far end's ANSWER, not a
        # guess about the wire.  So a retransmission now means the far node took
        # more than thirty milliseconds to acknowledge: its scheduler, on a PC
        # running two emulators, which the floor exists to tolerate.  It is not
        # a wire fault, and the sharp assertions for that are the two below -
        # zero gaps and zero bad frames.
        #
        # Still bounded, because a quarter would mean the floor itself is wrong.
        Note ($again * 4 -le $sent) 'link-retransmits-stay-rare' `
            "$again of $sent"
    }

    # The stream class, which until now had never carried anything between two
    # nodes.  Everything built so far is a question with an answer, so all of it
    # went on control or bulk; the class that SOUND is meant to ride had only
    # ever been exercised by the host tests.  A path nothing has used is a path
    # nobody knows the state of.
    #
    # Judged as an unreliable class should be: losing one is what it is allowed
    # to do, losing most of them means the path is broken.
    $sp = [regex]::Match($a, 'ping: \d+ messages on stream(.|\n)*?(\d+) of (\d+) came back')
    if (-not $sp.Success) {
        Note $false 'stream-class-carries-messages' 'no stream ping summary'
    } else {
        $sgot = [int]$sp.Groups[2].Value
        $ssent = [int]$sp.Groups[3].Value
        # A THIRD used to be the bound here, and the comment that justified it
        # named measurements of 22, 23, 14, 11 of 24 as an unexplained property
        # of the stream path.  They were not: this harness could not see the
        # echo of the next command while the previous ping was painting, typed
        # it again, and the retyped characters stopped the ping - which then
        # reported a fraction of what it had sent.  The spread was the spread of
        # WHEN the retype landed.
        #
        # With that gone it is 24 of 24, three full runs running.  So the bound
        # says something again.  Not equality: the class is allowed to lose, and
        # a check that forbids what the design permits will one day be silenced
        # by somebody who is right to silence it.  Nine in ten is the line
        # between "lost one" and "this path is broken".
        Note ($sgot * 10 -ge $ssent * 9) 'stream-class-carries-messages' `
            "$sgot of $ssent"
    }

    # The last one, for the same reason as the frame counters above: running
    # totals printed by every status, on a screen that redraws.
    $lball = [regex]::Matches($b,
        'losses\s+(\d+) gaps, (\d+) duplicates, (\d+) bad')
    $lb = if ($lball.Count -gt 0) { $lball[$lball.Count - 1] } else { $null }
    if ($null -ne $lb -and $lb.Success) {
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
                 # B must still be there when A asks, and A now asks for a
                 # whole directory and a whole program image a block at a time.
                 "link poll $(($SendDelaySec + 90) * 1000)",
                 '=link on node0') `
        -SendA @('drv load t:\nodelink.sys',
                 'link use node0',
                 # Proof, before the fact, that this node has no such file.
                 'dir t:\',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 # One `dev` for the whole system: this node's devices, then the
                 # other node's, asked for over the wire.
                 'dev',
                 # Neither node was named, so both carry the default.  The
                 # status has to say so on demand: the warning at the moment
                 # they met has long since scrolled off a 25-line console.
                 'link',
                 '=both are called node',
                 'link run t:\hello.axe one two',
                 '=from the other node',
                 # And the same thing without being told where.  `run` finds no
                 # such file here, sees a neighbour, and asks it.
                 'run t:\hello.axe three',
                 '=argv[1] = three',
                 # And the bulk class, for the first time on a wire: fetch the
                 # image off the other node and then run it HERE.  A copy that
                 # executes is a byte-for-byte copy.
                 'link get t:\hello.axe t:\copy.axe',
                 '=bytes from the other node',
                 'run t:\copy.axe four',
                 '=argv[1] = four',
                 # And the whole card, as a letter.  Nothing below knows there
                 # is a wire: `dir` and `run` are the ordinary commands, and the
                 # loader reads the image over the link a block at a time.
                 # N: is the far node's WHOLE tree, not one of its drives, so
                 # its T: is n:\tmp - which is the honest mapping: a node has
                 # one filesystem and the letters are this node's view of it.
                 'link mount',
                 "=the other node's card",
                 'dir n:\tmp',
                 '=hello.axe',
                 'run n:\tmp\hello.axe five',
                 '=argv[1] = five')

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

    # One `dev` for the whole system.  node0 is on BOTH nodes, so seeing it in
    # the remote section is not proof by itself; the far node has the link
    # driver of its own and says so, and the section header only appears when
    # the far node answered.
    # Placement, in its simplest and most honest form: the image is the first
    # datum a program needs, so a file that is on the other node IS the
    # statement that the program belongs there.  No manifest, no policy, no
    # names - and the operator did not say where.
    Check $a 'not here; running it on node0' 'place-run-went-to-the-other-node'
    Check $a 'argv[1] = three' 'place-run-really-ran-there'

    # The bulk class, on a wire, for the first time.  The window, the go-back-N
    # and the retransmit timer had only ever met an injected loss model on a
    # development machine.
    Check $a 'bytes from the other node' 'get-file-came-across'
    # A copy that executes is a copy that is right: a single wrong byte in an
    # image is a relocation into nothing, not a warning.
    Check $a 'argv[1] = four' 'get-copy-runs-here'

    # Neither node was named here, so both carry the default and the header uses
    # it.  That is worth leaving as it is rather than naming them: it exercises
    # the case that WILL happen to two boards out of the box, and the collision
    # it causes is meant to be said out loud rather than worked around.
    Check $a 'on node, over node0' 'dev-far-section-appeared'
    Check $a 'both are called node' 'dev-same-name-was-reported'
    # A window after the header, not a split: the console repaints, so the
    # transcript contains the whole screen several times over and anything
    # "after the first occurrence" is most of the run.
    $at = $a.IndexOf('on node, over node0')
    $window = if ($at -ge 0) {
        $a.Substring($at, [Math]::Min(700, $a.Length - $at))
    } else { '' }
    $rows = @('NODELINK', 'pcmmix', 'flash0') |
            Where-Object { $window.Contains($_) }
    Note ($rows.Count -eq 3) 'dev-far-devices-listed' `
        $(if ($rows.Count -eq 3) { '' }
          else { "only $($rows.Count) of 3 expected rows" })

    # The other node's card as a drive letter.  The claim is not that a new
    # command works - it is that NO command had to change: `dir` and `run` are
    # the ones that were already there, and they do not know there is a wire.
    Check $a "the other node's card is N:" 'mount-drive-appeared'

    # A directory read over the link: opendir/readdir/closedir, each its own
    # round trip, and the name has to survive being taken apart and put back.
    # Case-insensitive on purpose: the shell prints the ON-DISK spelling, which
    # is the far node's, not a letter this test gets to choose.
    $nlist = [regex]::Match($a, '(?i)Directory of N:\\tmp(.|\n)*?dir\(s\)')
    $nHas = $nlist.Success -and $nlist.Value.Contains('hello.axe')
    Note $nHas 'mount-far-directory-listed' `
        $(if ($nHas) { '' }
          elseif (-not $nlist.Success) { 'no N: listing in the transcript' }
          else { 'N: listed but hello.axe is not in it' })

    # And the one that matters: the LOADER read an image over the wire, a block
    # at a time, through open/read/seek/close it does not know are remote - and
    # the program ran.  This is what the manifest half of placement was waiting
    # for: to decide by what a program declares, a node must be able to read it.
    Check $a 'argv[1] = five' 'mount-ran-a-program-off-the-far-card'
}

# ------------------------------------------ placement by what a program needs ---
#
# The half of placement that a directory cannot decide.  The file IS here; this
# node simply cannot do what the program says it needs, and the neighbour can.
#
# WHAT MAKES THIS A REAL TEST rather than a rehearsal: the two nodes genuinely
# differ, and nothing was invented to make them.  QEMU registers no pcm0 - the
# board has no converter, so `pcmnull stands alone` - and PCMVIRT.SYS registers
# a sink.  Loading it on B and not on A leaves one node that can make a noise
# and one that cannot, which is the whole question.
#
# And the hard part: NEEDSND.AXE is on A ONLY.  So the program moves and the
# file does not - B has to read the image off A's card, over the same link it is
# being told what to run on.  That is what made the far run need a task of its
# own: served on the link's reader task, the reply carrying those bytes would be
# delivered by the task blocked waiting for the program to finish, and both
# nodes would stop dead with nothing in either log.

if ($Scenario -eq 'link' -or $Scenario -eq 'both') {
    Write-Host "`n== placed by what it needs ==" -ForegroundColor Cyan

    Invoke-Pair -Tag 'place' `
        -PutBoth @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -PutA @('build\apps\NEEDSND.AXE=t:\needsnd.axe',
                'build\apps\NEEDGFX.AXE=t:\needgfx.axe',
                'build\apps\IDLE.AXE=t:\idle.axe') `
        -PutB @('build\apps\PCMVIRT.SYS=t:\pcmvirt.sys') `
        -SendB @('drv load t:\pcmvirt.sys',
                 'drv load t:\nodelink.sys',
                 # Named BEFORE the link is bound, so the first hello already
                 # carries the right name.  Naming afterwards works too - the
                 # rename says hello again - but then the test would be proving
                 # the repair rather than the ordinary case.
                 'link name sound',
                 'link use node0',
                 # Long enough to outlast what the other node does, and no
                 # longer: this node sits here doing nothing for the whole
                 # window, and two scenarios of it was most of the run.
                 "link poll $(($SendDelaySec + 90) * 1000)",
                 '=link on node0',
                 # And then say what it was asked to do while nobody could see.
                 # The asking node holds its own screen by then and its console
                 # has stopped drawing; this node's is free, and its journal
                 # kept the loader's line.
                 'log -n 80') `
        -SendA @('drv load t:\nodelink.sys',
                 'link name quiet',
                 'link use node0',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 # Names, before anything that has to be addressed by one.
                 'link',
                 '=this node is quiet',
                 # Proof that THIS node cannot: the same refusal the loader
                 # gives, from the node that is about to hand the job over.
                 'dev',
                 # Waited for on a word that BOTH outcomes print.  Waiting on a
                 # line only the SUCCESS path prints was a trap: a failed run
                 # exits before printing it, the harness sits out its whole
                 # timeout, and a plain failure became four minutes of apparent
                 # hang in the middle of the transcript.
                 'run t:\needsnd.axe',
                 # Followed by something that prints WHATEVER the run did, and
                 # waited on that.  Waiting on the run's own success line was a
                 # trap: a failed run never prints it, so the harness sat out
                 # its whole timeout and a plain failure became four minutes of
                 # apparent hang in the middle of the transcript.
                 'link',
                 '=frames',
                 # And the harder half: a screen this board HAS but has already
                 # given away.  Nothing is missing here - the loader would start
                 # it quite happily by taking the glass from what is on it, and
                 # that is the DOS answer.  With two boards the right answer is
                 # the free screen next door.
                 #
                 # FROM HERE THIS NODE IS BLIND, and that is the point of it:
                 # holding the framebuffer suspends the text console, so nothing
                 # more appears in this transcript.  The test had to make the
                 # screen busy, and making it busy is exactly what stops it being
                 # watched.  So the command goes out raw - no prompt to wait for,
                 # because no prompt will be drawn - and the evidence is read on
                 # the OTHER node, whose screen is free and whose journal
                 # remembers what it was asked to load.
                 # NOT TESTED HERE, and the reason is worth the space.
                 #
                 # The other half of placement - a screen this board HAS and has
                 # already given away - is built and reasoned but cannot be
                 # exercised from this harness: making the screen busy is what
                 # stops the screen being watched.  Text output is suppressed on
                 # EVERY sink while an application holds the panel, the serial
                 # console included, so a node with a busy screen cannot report
                 # anything about it - not even the echo of the next command.
                 #
                 # Driving it blind on a clock and reading the answer afterwards
                 # was tried: the console does not come back when the holder is
                 # killed either.  Three attempts, no evidence.
                 #
                 # The serial console going silent because an application took
                 # the PANEL looks wrong on its own merits - on a headless board
                 # it is the only console there is - and fixing that would make
                 # this testable as a side effect.  Written up in the plan.
                 'link',
                 '=frames')

    $a = Transcript 'build\nodetest-place-a.log'
    $b = Transcript 'build\nodetest-place-b.log'

    Check $b 'PCMVIRT' 'place-far-node-has-a-sink'

    # Names.  Each node knows its own and learns the other's from a hello on the
    # link's own channel - and a name that survives a reboot is the only kind
    # that can be written into a config file and still mean the same board.
    Check $a 'this node is quiet' 'name-this-node-knows-itself'
    Check $a 'the other is sound' 'name-learned-the-neighbour'
    # And it is used where it matters: the listing of what is over there.
    Check $a 'on sound, over node0' 'name-used-in-the-device-listing'

    # The decision, said out loud.  A shell that moves work silently is a shell
    # nobody can debug.
    Check $a 'needs audio; running it on node0' 'place-needs-decided-here'

    # And it really ran there - the line can have come from nowhere else, since
    # this node has no sound and would have refused to start it.
    Check $a 'ran on a node with sound' 'place-ran-on-the-node-that-can'

    # The far kernel's own loader line, for an image that is not on its card.
    #
    # In A's transcript, not B's, and that is not a mistake to tidy up: while a
    # node runs a guest program its console is redirected into the wire, so the
    # far KERNEL's log arrives here too.  That makes it the sharpest evidence
    # available - this node has no NEEDSND to load and no loader line of its own
    # to confuse it with, so the line can only have been written over there.
    Check $a 'loader: NEEDSND' 'place-far-kernel-loaded-it-over-the-link'

    # The harder half of placement: a screen this board HAS, and has given away.
    #
    # Nothing is unmet here - the hardware is present and the loader would start
    # the program by taking the glass off whatever is on it.  That is the DOS
    # answer and it is wrong for two boards: a new application should open on a
    # screen nobody is using, which is the second-monitor model asked for.
    # There is no check here for the busy-screen half of placement, and that is
    # a gap rather than an omission - see the note in this scenario's command
    # list.  NEEDGFX.AXE and `idle gfx` are built and staged for whoever makes
    # it testable.
}

# ------------------------------------------ one registry, one ps, one system ---
#
# A SECOND pair, and splitting it out of the one above was not tidying.
#
# That scenario had grown to nine things in one guest's lifetime - two drivers,
# naming, binding, placement, attaching devices, opening one, forty audio
# periods, a listing and a kill - and a scenario that long tells you very little
# when its ninth step fails: the guest runs out of time somewhere in the middle
# and every check after that point fails at once, pointing at nothing.  Twice
# the failure looked like the wire.
#
# The line between them is what each is about.  Above: WHERE a program runs.
# Here: the far board's devices and processes appearing as this board's.

if ($Scenario -eq 'link' -or $Scenario -eq 'both') {
    Write-Host "`n== one registry, one ps ==" -ForegroundColor Cyan

    Invoke-Pair -Tag 'share' `
        -PutBoth @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -PutA @('build\apps\DEVPOKE.AXE=t:\devpoke.axe') `
        -PutB @('build\apps\PCMVIRT.SYS=t:\pcmvirt.sys',
                'build\apps\IDLE.AXE=t:\idle.axe') `
        -SendB @('drv load t:\pcmvirt.sys',
                 'drv load t:\nodelink.sys',
                 'link name sound',
                 'link use node0',
                 # Something for the other node's `ps` to find.
                 #
                 # IDLE and not SPIN, and the difference cost nine checks.
                 # spin.axe is the SUPERVISOR's test case: it burns a core and
                 # prints a dot every few milliseconds, so as a stand-in for
                 # "something is running over there" it drowned this board's
                 # console and left it too busy to answer - which failed checks
                 # that had nothing to do with processes and pointed at the wire.
                 'run /b t:\idle.axe',
                 # WAIT FOR THE SOUND rather than poll for a fixed time, and
                 # this is not a tidying.
                 #
                 # `link poll <ms>` was here to keep the guest alive while the
                 # other node worked, and it held this console for two minutes
                 # doing it.  Commands typed into a console that is busy
                 # printing lose characters, so the harness saw no echo and
                 # typed them AGAIN - two polls of 115 seconds, and the journal
                 # dump after them never ran at all.  The check then failed
                 # saying the sound had not arrived, which was a statement
                 # about this list and not about the wire.
                 #
                 # A `=` step types nothing, so nothing is lost to a busy
                 # console.  What it waits FOR had to be chosen twice: the
                 # kernel's summary of what was poured goes to the JOURNAL, not
                 # to this screen, so waiting for it here waited for something
                 # that was never going to appear - the same "the screen is not
                 # a log" mistake this file warns about, made by me, in the
                 # check written to prove the thing the note is about.
                 #
                 # `link poll` prints its status when it ends, and that IS on
                 # this screen.  Long enough to cover the other node's work.
                 "link poll $(($SendDelaySec + 60) * 1000)",
                 '=link on node0',
                 # And from the journal as well, because the screen scrolls and
                 # the check wants a line it can still find.
                 'log -n 80',
                 '=log') `
        -SendA @('drv load t:\nodelink.sys',
                 'link name quiet',
                 'link use node0',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 'link devs',
                 '=are in this registry now',
                 # No `dev` listing here any more.  Nothing checked its output -
                 # the count that is checked comes from `link devs` above - and
                 # a dozen lines of it arriving while the next command is typed
                 # is exactly the state in which this console drops characters.
                 # A listing printed for nobody is not free.
                 'run t:\devpoke.axe sound.pcmvirt 64',
                 '=devpoke:',
                 # The class default: this node's mixer has no real output of
                 # its own, so the sound should leave through the neighbour's
                 # rather than into pcmnull.  Enough periods to see it SETTLE,
                 # not just to see it try: opening a sink on the other board is
                 # a round trip and can arrive while that node is busy, which is
                 # -AG_EBUSY, correct, and retried on the next period.
                 'run t:\devpoke.axe pcmmix 192 40',
                 '=devpoke:',
                 # One `ps` for the whole system.
                 'ps',
                 '=IDLE',
                 # And the name it printed is the name that can be typed back.
                 'kill sound.1',
                 '=sound.1',
                 # From the JOURNAL, not the screen.  The mixer chooses its
                 # output inside a write, which runs on the APPLICATION's task -
                 # and while an application owns the console the kernel log does
                 # not reach it.  The line was always written; looking for it on
                 # the screen was looking in the wrong place.
                 # The journal is a four-kilobyte RING and it wraps: ask for
                 # plenty, and keep what runs before this quiet enough that the
                 # line being looked for is still in it.
                 'log -n 80',
                 '=log')

    $a = Transcript 'build\nodetest-share-a.log'
    $b = Transcript 'build\nodetest-share-b.log'

    # The far node's device, in this node's registry, opened by name.
    # `sound.pcmvirt` can only be the far node's: PCMVIRT.SYS is loaded on B and
    # nowhere else, so this node has no device of that name to open by mistake.
    Check $a 'are in this registry now' 'rdev-proxies-registered'
    Check $a 'sound.pcmvirt is open' 'rdev-opened-a-device-that-is-not-here'

    # The write was ACCEPTED here - and that is all this line can mean now.
    #
    # It used to be called rdev-wrote-through-the-wire, and under the old
    # request-and-answer path it earned the name: the count came back from the
    # far node.  Sound is poured now - stream class, no reply, no
    # retransmission - so the sending side returns the length without evidence
    # and this would say exactly the same with the wire unplugged.  Kept,
    # because "the proxy was called and did not refuse" is still worth knowing;
    # renamed, because it is not proof of arrival.  The proof is below, counted
    # on the node the bytes landed on.
    Check $a '64 of 64 bytes taken' 'rdev-write-was-accepted-here'

    # The class default, asking the WHOLE SYSTEM - which is the line that was
    # retreated from for two days and is now back.
    #
    # This node has no converter of its own; the neighbour has PCMVIRT.  So the
    # mixer's sink must be the far one, reached by its qualified name, and the
    # sound leaves this board over the wire.
    #
    # It used to freeze node A doing exactly this, in about half of runs -
    # console stops answering and does not come back - because the registry
    # held one mutex across the whole of a driver's call and the mixer's reach
    # across the wire happens inside one.  The cure is in device.c: a call
    # resolves what it will call under the registry lock, then lets that lock go
    # and takes the device's own instead, `open` and `close` included.
    #
    # WHICH IS WHY THE CHECKS AFTER THIS ONE MATTER AS MUCH AS THIS ONE.  A
    # board that froze would not answer `ps`, would not kill anything and would
    # not print its journal; the freeze cannot hide behind a passing assertion
    # here.
    Check $a 'sink /dev/sound.pcmvirt' 'default-sound-goes-to-the-node-that-has-one'

    # AND THE SOUND ACTUALLY ARRIVED - counted where it landed.
    #
    # This is the check the streaming path needed and the old one could not be.
    # A poured period is sent on the unreliable class with no reply, so nothing
    # on the sending node can know it got there; the far node counts what it
    # wrote into its converter and says so once the sound stops.  Unplug the
    # wire and this line is simply absent.
    #
    # The LAST occurrence: the journal dump is a screen that scrolls and
    # redraws, and these are running totals.
    $pours = [regex]::Matches($b, 'poured (\d+) periods, (\d+) bytes, (\d+) lost')
    if ($pours.Count -eq 0) {
        Note $false 'pour-arrived-at-the-far-node' 'no "poured N periods" line'
    } else {
        $pr = $pours[$pours.Count - 1]
        $pp = [int]$pr.Groups[1].Value
        $pb = [int]$pr.Groups[2].Value
        $pl = [int]$pr.Groups[3].Value
        Write-Host "poured: $pp periods, $pb bytes, $pl lost"
        Note ($pp -gt 0 -and $pb -gt 0) 'pour-arrived-at-the-far-node' `
            "$pp periods, $pb bytes"
        # Losing some is what an unreliable class is allowed to do; losing most
        # of it means the path is broken rather than lossy.  The bound is where
        # it says something, not where it passes.
        Note ($pl -le $pp) 'pour-loses-less-than-it-delivers' `
            "$pl lost against $pp delivered"
    }

    # One `ps` for the system.  IDLE runs on the other board and has never been
    # on this one, so a row for it here crossed the wire.
    Check $a 'IDLE' 'ps-shows-the-other-node-process'
    # Under the node's name, which is the point: a pid on its own would be
    # ambiguous the moment there are two boards, and this is the spelling an
    # operator can type back.
    Check $a 'sound.1' 'ps-names-it-by-node'
    # And typed back it works: the far process is gone.
    Check $a 'sound.1 stopped' 'kill-reached-the-other-node'
    Check $b 'killed from the other node' 'kill-far-node-said-who'
}

# ------------------------------ one program, every core of both boards ---
#
# The claim under test, in one sentence: ONE program used the cores of TWO
# boards and got the same answer it gets alone.
#
# Everything before this scenario moved whole programs between nodes.  This is
# the first thing that gives one program the other board's cores - threads of
# one job, with no memory in common, passing work and results as messages.
#
# GRIND.AXE IS ON NODE A ONLY, which is what makes it more than a demonstration.
# B has never seen the file, so the copy of it that runs there was read off A's
# card over the same wire that then carried the work - and the two halves of the
# job are two copies of one image, not a program and a matching helper.
#
# WHAT WOULD MAKE THIS PASS WITHOUT MEANING ANYTHING, and how each is closed:
#   - everything running locally: `link nodes` must say two, and grind must
#     report threads on the far node by NAME.
#   - the far half never starting: B's own loader line for GRIND is checked on
#     B, where the only way it can appear is if B loaded it.
#   - the work not actually crossing: the sums must agree.  The serial answer is
#     computed here, on this core, and the spread-out one is assembled from
#     messages; a wire that dropped or mangled anything gives a different
#     number.
#
# THERE IS NO SPEED ASSERTION, deliberately.  Two emulators sharing one PC do
# not divide work the way two boards do - they compete for the same host cores -
# so a speed-up here would be measuring this machine's load.  The speed number
# is printed for a person to read; what is ASSERTED is that the answer is right
# and that both boards did some of it.

if ($Scenario -eq 'link' -or $Scenario -eq 'both') {
    Write-Host "`n== one program, the cores of both boards ==" -ForegroundColor Cyan

    Invoke-Pair -Tag 'job' `
        -PutBoth @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -PutA @('build\apps\GRIND.AXE=t:\grind.axe') `
        -SendB @('drv load t:\nodelink.sys',
                 'link name n2',
                 'link use node0',
                 # B does nothing of its own: the whole of its part is started
                 # by A, on B's node-job channel.  The poll keeps its shell out
                 # of the way and its wire pumped while that happens.
                 "link poll $(($SendDelaySec + 120) * 1000)",
                 '=link on node0',
                 # From the journal, not the screen: the worker process may own
                 # B's console while it runs, and a kernel line written then
                 # reaches nobody.  The journal is a 4 KB ring, so ask early and
                 # ask for plenty.
                 'log -n 120',
                 '=log') `
        -SendA @('drv load t:\nodelink.sys',
                 'link name n1',
                 'link use node0',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 'link nodes',
                 '=cores in all',
                 # Blocks big enough to matter, and the size was MEASURED
                 # rather than guessed.
                 #
                 # At 120 000 iterations the whole serial run took 3 ms here
                 # against 600 ms of starting the far half, so the ratio was
                 # arithmetic about start-up and said nothing about how two
                 # boards divide work - the emulator runs a register-only loop
                 # at something like two billion instructions a second, which
                 # no board does.  At this size the work dominates, which is
                 # the regime the whole mechanism is for, and the threads live
                 # seconds rather than milliseconds - so the messages, the
                 # credits and the queue are exercised over time instead of all
                 # at once.
                 'run t:\grind.axe 8 150000000',
                 # A marker printed on EVERY path out of grind, not only the
                 # good one.  Waiting for a line only success prints does not
                 # fail when something breaks - it hangs, and then reports a
                 # timeout somewhere unrelated.  Twice.
                 '=grind: ')

    $a = Transcript 'build\nodetest-job-a.log'
    $b = Transcript 'build\nodetest-job-b.log'

    # There are two boards and the system knows it.
    Check $a '2 nodes' 'job-two-nodes-visible'
    # And what a byte over the wire costs, which is the number that decides
    # whether any of this is worth doing for a given piece of work.
    Check $a 'cycles here' 'job-wire-cost-reported'

    # The far node by name, in THE PROGRAM's own listing of where it can run -
    # not the shell's.  Both print the name, so a plain search for "n2" would
    # pass on `link nodes` alone and prove nothing about what grind was told.
    #
    # Told apart by what FOLLOWS: the shell's row carries a thread count after
    # the cores and the program's does not.  The obvious test - the program
    # indents its rows and the shell does not, anchored at the start of a line -
    # was tried and failed, and why it failed is worth keeping: an application's
    # output goes to the JOURNAL until the session adopts it, so every line of
    # it arrives as "app/GRIND:1: ..." and nothing of the program's is ever at
    # the start of a line.  The same adoption is behind the harness defect
    # written up in docs/plans/cluster.md.
    #
    # So the test is an ORDER instead: "all cores:" is a sentence only grind
    # prints, and the far node's row with a thread count on it follows within a
    # few lines.  Two attempts before this one were weaker and worth recording,
    # because both LOOKED specific:
    #   - anchoring the program's two-space indent to the start of a line: never
    #     matches, because the journal prefixes every line of it;
    #   - "n2 ... cores" with a negative lookahead for a thread count: matches
    #     the shell's row anyway, since `cores?` will match "core" and leave the
    #     "s" for the lookahead to fail on.  It passed, and proved nothing.
    Note ([regex]::IsMatch($a, "all cores:[\s\S]{0,300}n2\s+\d+ thread")) `
        'job-far-node-named-to-the-program'

    # B loaded a program it has never had a copy of.  The image came off A's
    # card, over the wire, because A asked B for a thread of it.
    Check $b 'loader: GRIND' 'job-far-kernel-loaded-our-image'
    # And B says so in its own words as well.
    Check $b 'serving a job' 'job-far-node-served'

    # Threads really were placed on both boards.
    $th = [regex]::Match($a, '(\d+) threads on (\d+) nodes')
    if (-not $th.Success) {
        Note $false 'job-threads-spread' 'no "N threads on M nodes" line'
    } else {
        Note ([int]$th.Groups[1].Value -ge 2 -and [int]$th.Groups[2].Value -ge 2) `
            'job-threads-spread' `
            ("{0} threads on {1} nodes" -f $th.Groups[1].Value, $th.Groups[2].Value)
    }

    # THE claim: the work was cut up, sent away, done elsewhere, brought back,
    # and the answer is the one this core computes by itself.
    Check $a 'answers agree' 'job-same-answer-as-one-core'

    # Printed for a person, asserted about by nobody - see the note above.
    $sp = [regex]::Match($a, 'speedup (\d+\.\d)x on (\d+) cores')
    if ($sp.Success) {
        # The concatenation is parenthesised because -f binds tighter than +:
        # without the inner brackets it formats only the second half, which has
        # no placeholders, and the line prints "{0}x on {1} cores" literally.
        Write-Host ((("job: {0}x on {1} cores (two emulators on one PC - " +
                      "not a board measurement)") -f $sp.Groups[1].Value,
                     $sp.Groups[2].Value)) -ForegroundColor DarkGray
    }
}

# ------------------------- a running program moved to the other board ---
#
# Migration level 1, and the claim is exact: a program that was COUNTING here
# goes on counting THERE, from the number it had reached.
#
# MIGRANT.AXE IS ON NODE A ONLY, so the copy that runs on B was read off A's
# card over the wire - the same proof of travel the placement and job scenarios
# use.  What is new is that it did not start: it CONTINUED.
#
# WHAT WOULD MAKE THIS PASS WITHOUT MEANING ANYTHING, and how each is closed:
#   - the program merely restarting on the far board: the far node must print a
#     number GREATER than zero and say how many hops it has made, and "starting
#     fresh at 0" must not be what it says;
#   - the state never leaving: the number B continues from is compared against
#     the last number A printed, and B's must not be smaller;
#   - the whole thing being a local restart: B says it carried on, and B is
#     where the transcript is read.
#
# The number is the evidence.  Nothing else in this scenario is.

if ($Scenario -eq 'link' -or $Scenario -eq 'both' -or $Scenario -eq 'migrate') {
    Write-Host "`n== a running program moved to the other board ==" -ForegroundColor Cyan

    Invoke-Pair -Tag 'migrate' `
        -PutBoth @('build\apps\NODELINK.SYS=t:\nodelink.sys') `
        -PutA @('build\apps\MIGRANT.AXE=t:\migrant.axe') `
        -SendB @('drv load t:\nodelink.sys',
                 'link name there',
                 'link use node0',
                 # Nothing of its own: everything that happens here is started
                 # by A.  And NOT a wait for the program's own words - a remote
                 # run's output is relayed to whoever ASKED for it, so every
                 # line the migrated program prints appears on node A's screen
                 # and none of it on this one.  Waiting here for words that
                 # travel the other way cost a full run.
                 #
                 # What IS here is the far kernel's own journal, which is where
                 # its loader says what it loaded.
                 # Timed against A's clock, which starts within a second of
                 # this one: A asks for the handover at about 44 s.  Dumped
                 # from the JOURNAL because the console here is redirected into
                 # the wire for the length of a remote run - which is why the
                 # far kernel's loader line is not on this screen and never
                 # will be.
                 '!powershell -NoProfile -Command Start-Sleep -Seconds 40',
                 'log -n 60',
                 '=log') `
        -SendA @('drv load t:\nodelink.sys',
                 'link name here',
                 'link use node0',
                 "!powershell -NoProfile -Command Start-Sleep -Seconds $SendDelaySec",
                 'run /b t:\migrant.axe',
                 # Let it count a few times, so the number it carries is
                 # plainly not zero and plainly not one.
                 '!powershell -NoProfile -Command Start-Sleep -Seconds 4',
                 # No '=' step after it: `migrate` BLOCKS until it has an
                 # answer, so the prompt coming back is the completion
                 # signal, and every outcome it can print is a different
                 # sentence - there is no one word to wait for.
                 'migrate 1',
                 # Stay alive while B dumps its journal.  Without this A
                 # finishes the moment the handover is reported and the harness
                 # stops B mid-sleep, so the far node's own evidence is lost -
                 # which reads as "the far kernel never loaded it" while the
                 # program is demonstrably running there.
                 '!powershell -NoProfile -Command Start-Sleep -Seconds 25')

    $a = Transcript 'build\nodetest-migrate-a.log'
    $b = Transcript 'build\nodetest-migrate-b.log'

    # It agreed, at a point of its own choosing, and said where it stopped.
    $h = [regex]::Match($a, 'migrant: handing over at (\d+)')
    if (-not $h.Success) {
        Note $false 'migrate-agreed-and-saved' 'no "handing over at N" line'
    } else {
        Note $true 'migrate-agreed-and-saved' ("at {0}" -f $h.Groups[1].Value)
    }
    Check $a 'is now on' 'migrate-reported-success-here'

    # The far node loaded an image it has never had a copy of - ASKED OF THE FAR
    # NODE, which is where that fact belongs.
    #
    # This check spent a while reading A's relayed stream by order instead,
    # because B could not be asked: a guest program's output sink used to be
    # hung on B's whole console, so while a migrated program lived - for ever -
    # B's own shell answered nobody, `log -n 60` included.  The sink belongs to
    # the guest, not to the board, and it does now.
    #
    # So this is the strong form again: B's own journal, naming the image B
    # loaded, on a board that never had a copy of it.
    Check $b 'loader: MIGRANT' 'migrate-far-kernel-loaded-our-image'
    # AND THE BOARD IS STILL ITS OWN: its shell ran a command and printed the
    # answer WHILE hosting a program that has not finished.  "lines held" is
    # the journal dump's own last word, so its presence is the shell speaking,
    # not the guest.
    #
    # Two facts, not one said twice: the line above says the guest is here, and
    # this one says the board still belongs to whoever is sitting at it.  That
    # second half is what was broken - a guest took this board's keyboard to
    # earn the right to be heard on the wire, and a migrated guest never gives
    # it back.
    #
    # NOT checked by looking for the guest's own words in B's journal, which is
    # where the first attempt looked: they land there only when they FAIL to
    # reach the node that asked for them.  That check would have gone green on
    # the bug and red on the fix, which is exactly backwards.
    Check $b 'lines held' 'migrate-far-node-still-answers-its-own-user'

    # AND IT CONTINUED RATHER THAN STARTED.  This is the whole scenario.
    #
    # Read on A, which is where the far program's output is relayed to.  That
    # is not a weaker proof than reading it on B: A never ran a second copy -
    # its own ended, by name, before any of this - and B's journal above says
    # whose image it loaded.
    $c = [regex]::Match($a, 'migrant: carried on from (\d+) after (\d+) hop')
    if (-not $c.Success) {
        Note $false 'migrate-continued-not-restarted' 'no "carried on from" line'
    } else {
        $from = [int]$c.Groups[1].Value
        $hops = [int]$c.Groups[2].Value
        Write-Host "migrated: carried on from $from after $hops hop(s)"
        Note ($from -gt 0 -and $hops -ge 1) 'migrate-continued-not-restarted' `
            "from $from after $hops hop(s)"
        # And it is the number A actually reached, not some number.
        if ($h.Success) {
            $saved = [int]$h.Groups[1].Value
            Note ($from -ge $saved) 'migrate-carried-the-real-number' `
                "saved $saved, resumed at $from"
        }
    }
    # ONE COPY, NOT TWO - and this is deliberately NOT a check that the far
    # copy avoided starting fresh.
    #
    # That check was here and cannot be made sound: "starting fresh" is printed
    # at the beginning and STAYS ON THE SCREEN, so every redraw after the
    # handover replays it into any window drawn around the later text.
    # Counting it was worse (15 hits for one line).  And it proved nothing that
    # migrate-continued-not-restarted does not already prove: the program says
    # exactly one of those two sentences, and that check matched the other.
    #
    # What is NOT proved elsewhere is that the program is not now running in
    # two places.  The kernel here names its exit, and presence is safe to read
    # on a rendered screen - a redraw can repeat a line, never invent one.
    $ended = $a -match 'MIGRANT \(pid \d+\) returned 0'
    Note $ended 'migrate-only-one-copy' `
        $(if ($ended) { 'the copy here ended' } else { 'the copy here never ended' })
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
