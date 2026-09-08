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
    [int]$OwnerPid = 0
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
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }

  public struct Found { public IntPtr H; public string Title; public uint Pid; public int W; public int Ht; }

  // Every visible top-level window whose title contains `needle`, largest
  // client area first: QEMU can have more than one (a monitor window), and the
  // panel is the big one.
  public static List<Found> Windows(string needle, uint wantPid) {
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
      if (w < 32 || ht < 32) return true;
      var f = new Found(); f.H = h; f.Title = t; f.Pid = pid; f.W = w; f.Ht = ht;
      hits.Add(f);
      return true;
    }, IntPtr.Zero);
    hits.Sort(delegate(Found a, Found b) { return (b.W * b.Ht).CompareTo(a.W * a.Ht); });
    return hits;
  }
}
'@

$hits = [Win]::Windows($Title, [uint32]$OwnerPid)
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
