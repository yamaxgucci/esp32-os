# Drive the built-in `fm` in QEMU over a real filesystem.
#
#   .\apps\fm\check.ps1
#
# The file operations themselves are host-tested in host-tests\test_fsops.c
# against a tree that only exists in memory, which is where the walk is pinned
# down: the order things happen in, what is left behind when it stops halfway,
# a directory that errors partway not being mistaken for one that has ended.
#
# What that cannot show is a real filesystem, and this is here for exactly the
# parts a fake gets to decide for itself:
#
#   * littlefs and the RAM disk refuse to remove a directory with anything in
#     it, so children-before-parent is a real requirement here rather than a
#     convention the fake enforces
#   * `rename` across two mounts fails with a real error, and the fall back to
#     copy-and-delete is what the user is offered
#   * the manager is the BUILT-IN one, linked into the kernel with AG_BUILTIN -
#     the same sources as FM.AXE, but a different build, and only one of the
#     two is exercised by building the other
#
# fm is a console program, so it is driven with escape sequences rather than
# with the virtual input drivers: F5 is CSI 15 ~, F8 is CSI 19 ~, F10 is CSI 21
# ~ (components\argon_kernel\src\console\vtin.c).  Esc is not used to quit -
# a lone 0x1b is held back by the console's own escape parser and never
# arrives as a key on its own.
#
# ---- why the FINAL SCREEN and not the transcript --------------------------
#
# The transcript is not a log, it is a stream of whole-screen repaints: every
# line that has been on the screen is in it many times over, and once the
# newlines are stripped so that wrapped lines can be matched, "X and then Y"
# matches any X and any Y in the whole session - the Y coming from a repaint
# after the X rather than from after the event.  That cost a run here: "the
# directory is gone" passed and failed depending on nothing.
#
# So the state at the end is read from the screen the guest actually ended up
# showing, rendered by tools\vtdump with the kernel's own screen module, and
# only progress words ("copied", "deleted") are looked for in the stream -
# those scroll away and are not on the last screen at all.
[CmdletBinding()]
param(
    [string]$Out = 'build\fm.log',
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $root
try {
    . .\tools\idf-env.ps1

    if (-not $NoBuild) {
        & .\argon.cmd build | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'building the firmware failed' }
    }

    # T: is the RAM disk: nothing is written to flash, so a failed run leaves
    # the image it was given exactly as it found it.
    $tree = @(
        'md t:\src',
        'md t:\src\sub',
        'echo hello > t:\src\a.txt',
        'echo world > t:\src\sub\b.txt',
        'md t:\dst'
    )

    $send = $tree + @(
        # Raw, so the harness does not wait for a prompt that will not come
        # until fm exits.  The Enter goes in its own write: sent together with
        # the text it is occasionally lost, one run in three.
        '~fm t:\src t:\dst',
        '~\x0d',
        '=F1 for the keys',

        # Down once, off ".." and onto [sub].  F5 on ".." does nothing at all,
        # which looks exactly like a copy that failed.
        '~\x1b[B',
        '~\x1b[15~',
        '=Copy directory',
        '~\x0d',
        '=copied',

        # And the same tree back off the disk.  F8, then y - the question now
        # says "and everything in it", because it takes the whole tree where
        # it used to be refused unless the directory was empty.
        '~\x1b[19~',
        '=everything in it',
        '~y',
        '=deleted',

        '~\x1b[21~',
        '=\>',

        # What the disk actually holds, from the console rather than from the
        # manager: a manager that draws the right thing over a filesystem it
        # did not change would pass a test that only asked the manager.
        # In this order, because the last screen is what gets asserted and
        # these four lines have to still be on it at the end.
        'dir t:\dst',
        'type t:\dst\sub\b.txt',
        # `type` and not `dir` for the file that should be gone: `dir` on a
        # path that does not exist lists its PARENT and reports no files,
        # which is indistinguishable from an empty directory that does
        # exist.  `type` says "file not found" and names the file.
        'type t:\src\sub\b.txt',
        'type t:\src\a.txt'
    )

    Write-Host "fm: driving QEMU, transcript -> $Out"
    & .\tools\qemu-boot.ps1 -Send $send -LogPath $Out -TimeoutSec 150 |
        Out-Host

    if (-not (Test-Path $Out)) { throw "no transcript at $Out" }

    # What was said on the way: these scroll off, so they are looked for in
    # the stream, with the escapes and the wrapping taken out.
    $said = (Get-Content -Raw $Out) -replace "\x1b\[[0-9;]*[A-Za-z~]", '' `
                                    -replace "[\r\n]", ''

    # Where it ended up: the last screen, rendered rather than grepped.
    if (-not (Test-Path 'build-host\vtdump.exe')) {
        # Built by argon tests / check / test.  This needs it and does not
        # need the rest, so it says so rather than building the world.
        throw 'no build-host\vtdump.exe - run: argon tests'
    }
    $screen = (& cmd /c "build-host\vtdump.exe 80 25 437 < $Out") -join "`n"
    Write-Host $screen

    $fail = @()

    if ($said -notmatch 'F1 for the keys') {
        $fail += 'fm never started'
    }
    if ($said -notmatch 'copied') {
        $fail += 'fm did not report the copy'
    }
    if ($said -notmatch 'deleted') {
        $fail += 'fm did not report the delete'
    }
    if ($said -match '\[fault\]') {
        $fail += 'something faulted'
    }

    # The last four commands put the answer on the screen in this order, and
    # the screen is what is asserted:
    #
    #   dir t:\dst              the copy made the tree
    #   type t:\dst\sub\b.txt    with the bytes that were in it
    #   type t:\src\sub\b.txt    the delete took the directory
    #   type t:\src\a.txt        and left its sibling alone
    if ($screen -notmatch 'sub +<DIR>') {
        $fail += 'the subdirectory was not copied'
    }
    if ($screen -notmatch 'world') {
        $fail += "the copied file's contents did not survive"
    }
    if ($screen -notmatch 'src.sub.b\.txt: file not found') {
        $fail += 'the deleted directory still has its file in it'
    }
    if ($screen -notmatch 'hello') {
        $fail += 'the delete took the sibling file as well as the directory'
    }

    if ($fail.Count -gt 0) {
        Write-Host ''
        foreach ($f in $fail) { Write-Host "FAIL: $f" }
        Write-Host "transcript: $Out"
        exit 1
    }
    Write-Host "fm: ok - copied a tree and deleted it, transcript $Out"
} finally {
    Pop-Location
}
