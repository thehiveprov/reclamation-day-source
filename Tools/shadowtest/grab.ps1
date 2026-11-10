# Saves what the game's window shows (screen copy of its own rectangle; no input, no focus change). Arg: output png.
param([string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static List<IntPtr> Of(uint pid) { var list = new List<IntPtr>(); EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero); return list; }
}
'@
$p = Get-Process -Name ReclamationDay -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no game"; exit 1 }
$best = $null; $area = 0
foreach ($h in [W]::Of([uint32]$p.Id)) { $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r); $a = ($r.R - $r.L) * ($r.B - $r.T); if ($a -gt $area) { $area = $a; $best = $h; $rect = $r } }
if (-not $best) { "no window"; exit 1 }
$w = $rect.R - $rect.L; $hgt = $rect.B - $rect.T
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.L, $rect.T, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$fg = [W]::GetForegroundWindow() -eq $best
"{0}x{1} at {2},{3} focused={4}" -f $w, $hgt, $rect.L, $rect.T, $fg
