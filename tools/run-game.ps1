<#
.SYNOPSIS
  Launch a recompiled game build for a while, take window screenshots, close it, and summarise its log.

.EXAMPLE
  powershell -File tools\run-game.ps1 -Game mkd -Config debug -Seconds 40 -Shots 5,15,30 -Env 'MKD_TASK_TRACE=1;MKD_PADS=2'
#>
param(
    [string]$Game = 'mkd',
    [string]$Exe = 'MKD.exe',
    [ValidateSet('debug', 'release')][string]$Config = 'debug',
    [int]$Seconds = 30,
    [string]$Shots = '',  # seconds, comma separated
    [string]$Env = '',  # KEY=VALUE pairs separated by ';' (-File cannot pass a hashtable)
    [string]$Tag = '',
    [string]$Pattern = '\[warn\]|\[error\]|error:|diag:|missing|exception|unimplemented|trap|not implemented|tasks:',
    [int]$MaxLines = 80
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "recomp\$Game\out\build\$Config"
$shotDir = Join-Path $root 'build\shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@
[Win]::SetProcessDPIAware() | Out-Null

function Snap([System.Diagnostics.Process]$p, [string]$name) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    $r = New-Object Win+RECT
    if ($h -eq [IntPtr]::Zero -or -not [Win]::GetWindowRect($h, [ref]$r) -or ($r.R - $r.L) -lt 64) {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $r.L = $b.Left; $r.T = $b.Top; $r.R = $b.Right; $r.B = $b.Bottom
    }
    $w = $r.R - $r.L; $hgt = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # the window's own pixels (PW_RENDERFULLCONTENT), so other windows over it do not matter; the screen if that fails
    $ok = $false
    if ($h -ne [IntPtr]::Zero) {
        $hdc = $g.GetHdc()
        try { $ok = [Win]::PrintWindow($h, $hdc, 2) } finally { $g.ReleaseHdc($hdc) }
    }
    if (-not $ok) { $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $hgt)) }
    $file = Join-Path $shotDir "$name.png"
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    "shot $file (${w}x${hgt})"
}

# -Env takes KEY=VALUE pairs separated by ';' (values may contain commas)
foreach ($kv in ($Env | ForEach-Object { $_ -split ';' })) { if ($kv -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
if (-not $env:SDL_ASSERT) { $env:SDL_ASSERT = 'always_ignore' }
$before = @(Get-ChildItem (Join-Path $dir 'logs') -Filter '*.log' -ErrorAction SilentlyContinue | ForEach-Object FullName)
$p = Start-Process -FilePath (Join-Path $dir $Exe) -WorkingDirectory $dir -PassThru
$t0 = Get-Date
"started $Exe pid $($p.Id)"
$label = if ($Tag) { "$Game-$Tag" } else { "$Game-$($t0.ToString('HHmmss'))" }
$taken = @{}
$shotList = @($Shots -split ',' | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ })
while ($true) {
    Start-Sleep -Milliseconds 500
    $p.Refresh()
    $el = ((Get-Date) - $t0).TotalSeconds
    if ($p.HasExited) { "exited at {0:N1}s code {1} (0x{2:X8})" -f $el, $p.ExitCode, $p.ExitCode; break }
    foreach ($s in $shotList) { if ($el -ge $s -and -not $taken[$s]) { $taken[$s] = $true; Snap $p "$label-${s}s" } }
    if ($el -ge $Seconds) {
        "still running at {0:N0}s: cpu {1:N1}s, {2:N0} MB, {3} threads" -f $el, $p.CPU, ($p.WorkingSet64 / 1MB), $p.Threads.Count
        $null = $p.CloseMainWindow()
        if (-not $p.WaitForExit(8000)) { Stop-Process -Id $p.Id -Force; 'killed' } else { 'closed' }
        break
    }
}
$log = Get-ChildItem (Join-Path $dir 'logs') -Filter '*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) { 'no log'; exit }
$lines = Get-Content $log.FullName
"=== $($log.Name): $($lines.Count) lines"
$lines | Select-String -Pattern $Pattern | Select-Object -First $MaxLines | ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
'--- tail'
$lines | Select-Object -Last 6 | ForEach-Object { $_.Substring(0, [Math]::Min(220, $_.Length)) }
