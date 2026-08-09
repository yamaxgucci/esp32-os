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
param([string]$Title = 'QEMU', [string]$Out = 'build\qemu_window.png')

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
}
'@

$p = Get-Process | Where-Object { $_.MainWindowTitle -like "*$Title*" } | Select-Object -First 1
if (-not $p) { throw "no window matching '$Title'" }
$h = $p.MainWindowHandle
[Win+R]$r = New-Object Win+R
[void][Win]::GetClientRect($h, [ref]$r)
$w = $r.Rr - $r.L
$ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 1 = PW_CLIENTONLY: no frame, so the pixels are the panel and nothing else.
[void][Win]::PrintWindow($h, $dc, 1)
$g.ReleaseHdc($dc)
$bmp.Save((Resolve-Path -LiteralPath '.').Path + '\' + $Out,
          [System.Drawing.Imaging.ImageFormat]::Png)
"$Out : ${w}x${ht} from '$($p.MainWindowTitle)'"
