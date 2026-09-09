# Capture QEMU's RGB panel window to a PNG.
#
# `gfxdump` shows what the *guest* drew; this shows what QEMU actually put on
# screen, and the two can disagree - a partial update that lies about where its
# pixels come from corrupts the window while the framebuffer stays perfect.
# That is how the bug in qemu_present_rows was found and confirmed fixed.
#
#   .\tools\qemu-run.ps1 -Sd -Gfx -Tcp -NoBuild        # console on TCP 5556
#   # send commands to port 5556, then:
#   .\tools\grab-window.ps1 -Out build\shot.png
#
# Client area only, so the pixels are the panel and nothing else.
#
# Two things this had wrong for as long as it existed, and both made it report
# "no window matching 'QEMU'" while the window was plainly on the screen:
#
#   * `Process.MainWindowTitle` is empty for QEMU's SDL window and
#     `MainWindowHandle` is 0.  Windows only fills those in for a process whose
#     first top-level window it recognises as the main one, which an SDL window
#     created after start-up is not.  So the window has to be found by walking
#     the top-level list with EnumWindows and matching the owning process id.
#
#   * `PrintWindow` with flag 1 alone comes back black for an accelerated
#     surface.  It needs 2 as well (PW_RENDERFULLCONTENT): 1|2 = 3.  Without
#     the 1 the title bar is included and pushes the panel's bottom rows out of
#     the bitmap; without the 2 there is nothing in it at all.
param(
    [string]$Title = 'QEMU',
    [string]$Out = 'build\qemu_window.png',
    # Restrict the search to this process id (any QEMU otherwise).
    [int]$OwnerPid = 0,
    # Put the window up and take no picture.
    #
    # Wanted at the START of a run, not at grab time, and that distinction cost
    # a green check: an emulator window that is minimised when the guest sets
    # its display mode keeps SDL's default size - 800x600 - and the panel is
    # then not what the window shows.  Restoring it later gets a picture of the
    # wrong thing, which compares equal to itself and hides whatever the two
    # photographs were meant to catch.
    [switch]$RestoreOnly
)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class Win {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowText(IntPtr h, StringBuilder s, int max);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }

  public struct Found { public IntPtr H; public string Title; public uint Pid; public int W; public int Ht; }

  // Every visible top-level window whose title contains `needle`, largest
  // client area first: QEMU can have more than one (a monitor window), and the
  // panel is the big one.
  public static List<Found> Windows(string needle, uint wantPid) {
    return Windows(needle, wantPid, false);
  }

  /* anySize: keep windows whose client area is 0x0 - a minimised one. */
  public static List<Found> Windows(string needle, uint wantPid, bool anySize) {
    var hits = new List<Found>();
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      if (!IsWindowVisible(h)) return true;
      int n = GetWindowTextLength(h);
      if (n <= 0) return true;
      var sb = new StringBuilder(n + 1);
      GetWindowText(h, sb, sb.Capacity);
      string t = sb.ToString();
      if (needle.Length > 0 && t.IndexOf(needle, StringComparison.OrdinalIgnoreCase) < 0) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (wantPid != 0 && pid != wantPid) return true;
      R r; if (!GetClientRect(h, out r)) return true;
      int w = r.Rr - r.L, ht = r.B - r.T;
      if (!anySize && (w < 32 || ht < 32)) return true;
      var f = new Found(); f.H = h; f.Title = t; f.Pid = pid; f.W = w; f.Ht = ht;
      hits.Add(f);
      return true;
    }, IntPtr.Zero);
    hits.Sort(delegate(Found a, Found b) { return (b.W * b.Ht).CompareTo(a.W * a.Ht); });
    return hits;
  }
}
'@

if ($RestoreOnly) {
    # Waited for, not sampled once.
    #
    # The window does not exist for the first second or two of an emulator's
    # life, and while it does not exist there is nothing to restore - a single
    # look that early reports "no window" and leaves it minimised for the whole
    # run.  What that costs is not cosmetic: a minimised window's updates do
    # not complete, so an application that hands over its pixels a strip at a
    # time waits for each one and appears to hang.  Measured: the shell's own
    # benchmark printed nothing at all in three minutes, and printed
    # immediately once the window was up.
    #
    # Any size, because a window that has not been shown yet has no client area
    # to speak of - which is exactly the state this exists to end.
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline) {
        $all = [Win]::Windows($Title, [uint32]$OwnerPid, $true)
        if ($all.Count -gt 0) {
            foreach ($w in $all) {
                if ([Win]::IsIconic($w.H)) {
                    [void][Win]::ShowWindow($w.H, 9) # SW_RESTORE
                    "grab-window: restored '$($w.Title)' (it was minimised)"
                } else {
                    "grab-window: '$($w.Title)' is already up"
                }
            }
            exit 0
        }
        Start-Sleep -Milliseconds 250
    }
    "grab-window: no window matching '$Title' appeared within 8s"
    exit 0
}

$hits = [Win]::Windows($Title, [uint32]$OwnerPid)
if ($hits.Count -eq 0) {
    # A minimised window is "visible" to Win32 and has a client area of 0x0,
    # so the size filter above drops it and this used to report no window at
    # all while the window was in the taskbar.  QEMU opens minimised whenever
    # something else owns the foreground - which on a working machine is most
    # of the time - and PrintWindow gives nothing for an iconic window, so the
    # only way to photograph it is to put it back up.
    #
    # Restoring it takes the foreground away from whoever had it.  That is
    # rude, and it is still better than a test that reports a failure it
    # cannot explain: the alternative is no picture, which is the one thing
    # this harness exists to produce.
    $iconic = [Win]::Windows($Title, [uint32]$OwnerPid, $true) |
        Where-Object { [Win]::IsIconic($_.H) }
    if ($iconic) {
        Write-Host ("grab-window: '" + $iconic[0].Title +
                    "' was minimised; restoring it to take the picture")
        [void][Win]::ShowWindow($iconic[0].H, 9) # SW_RESTORE
        Start-Sleep -Milliseconds 400
        $hits = [Win]::Windows($Title, [uint32]$OwnerPid)
    }
}
if ($hits.Count -eq 0) {
    throw "no visible window matching '$Title'$(if ($OwnerPid) { " for pid $OwnerPid" })"
}
$f = $hits[0]

$bmp = New-Object System.Drawing.Bitmap $f.W, $f.Ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 3 = PW_CLIENTONLY | PW_RENDERFULLCONTENT.  See the note at the top.
$ok = [Win]::PrintWindow($f.H, $dc, 3)
$g.ReleaseHdc($dc)
if (-not $ok) {
    $bmp.Dispose()
    throw "PrintWindow refused window 0x$('{0:X}' -f [int64]$f.H) ('$($f.Title)')"
}

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$full = if ([System.IO.Path]::IsPathRooted($Out)) { $Out }
        else { Join-Path (Resolve-Path -LiteralPath '.').Path $Out }
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

# An all-one-colour grab is the failure this script used to hand back silently.
$check = New-Object System.Drawing.Bitmap $full
$first = $check.GetPixel(0, 0)
$varied = $false
for ($y = 0; $y -lt $check.Height -and -not $varied; $y += 7) {
    for ($x = 0; $x -lt $check.Width; $x += 7) {
        if ($check.GetPixel($x, $y) -ne $first) { $varied = $true; break }
    }
}
$check.Dispose()

"$Out : $($f.W)x$($f.Ht) from '$($f.Title)' pid $($f.Pid)$(if (-not $varied) { ' -- WARNING: one flat colour' })"
