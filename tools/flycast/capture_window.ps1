# capture_window.ps1 DIR [MAXSEC] - for every NAME.req that appears in DIR,
# copy the Flycast window's client area from the screen to NAME.png and
# rename the request to NAME.done (tools/flycast/oracle_shots.lua holds the
# emulator until then).  Stops when DIR/STOP appears or after MAXSEC.  The
# window is raised topmost for the copy: the screen copy reads what is
# visible, and PrintWindow blocks while the emulator thread is held.
#
# Part of sa1-psp (MIT).
param([string]$dir, [int]$maxsec = 1800)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SaWin {
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
 public struct R { public int L, T, Rr, B; } public struct P { public int X, Y; } }
"@
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $maxsec) {
  if (Test-Path (Join-Path $dir "STOP")) { break }
  foreach ($q in @(Get-ChildItem -Path $dir -Filter "*.req" -ErrorAction SilentlyContinue)) {
    $p = Get-Process flycast -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Start-Sleep -Milliseconds 200; continue }
    $h = $p.MainWindowHandle
    if ([SaWin]::IsIconic($h)) { [SaWin]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 300 }
    [SaWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0043) | Out-Null   # topmost, no size/move, show
    Start-Sleep -Milliseconds 150
    $r = New-Object SaWin+R
    [SaWin]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object SaWin+P
    [SaWin]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = $r.Rr - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { Start-Sleep -Milliseconds 200; continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $w, $hh))
    $name = [System.IO.Path]::GetFileNameWithoutExtension($q.Name)
    $bmp.Save((Join-Path $dir ($name + ".png")), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Rename-Item $q.FullName ($name + ".done") -Force
    Write-Output ("captured " + $name + " " + $w + "x" + $hh)
  }
  Start-Sleep -Milliseconds 30
}
