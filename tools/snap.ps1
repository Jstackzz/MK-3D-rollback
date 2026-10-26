<#
.SYNOPSIS
  Screenshot a running game window (its own pixels, even when covered) to build\shots\<name>.png.

.EXAMPLE
  powershell -File tools\snap.ps1 -Name mkd-health -Process MKD
#>
param(
    [string]$Name = 'snap',
    [string]$Process = 'MKD'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$shotDir = Join-Path $root 'build\shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class SnapWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[SnapWin]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p) { throw "no window for process $Process" }
$h = $p.MainWindowHandle
$r = New-Object SnapWin+RECT
[SnapWin]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
try { [SnapWin]::PrintWindow($h, $hdc, 2) | Out-Null } finally { $g.ReleaseHdc($hdc) }
$file = Join-Path $shotDir "$Name.png"
$bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
"shot $file (${w}x${hgt})"
