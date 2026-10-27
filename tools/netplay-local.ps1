<#
.SYNOPSIS
  Netplay test on one PC: two game instances, each autopiloted into the same mode, joined over UDP on 127.0.0.1.
  Instance A is side 0 (P1), B side 1 (P2). Each logs every frame in sync to build\netplay\<tag>-a.log / -b.log;
  tools\py\rbcmp.py compares them. Screenshots of both windows go to build\shots\<tag>-a-<s>s.png and -b-.
  -Game mkd|mka|mkda picks the game (its executable and its MKD_* / MKA_* / MKDA_* variables).
  -Watch adds a spectator, instance C, -WatchDelay seconds later: A and B write their spectator feed to
  tools\py\feedrelay.py, which passes it on to C (build\netplay\<tag>-c.log; compare it with A's using rbcmp.py).

.EXAMPLE
  powershell -File tools\netplay-local.ps1 -Tag np1 -Seconds 120 -InputA "30:RIGHT:1,32:X,33:Y" -InputB "31:LEFT:1,34:A"
#>
param(
    [string]$Game = 'mkd',
    [string]$Exe = '',  # default: the game's own (MKD.exe, MKA.exe, MKDA.exe)
    [string]$Config = 'debug',
    [string]$Mode = 'versus',
    [int]$Seconds = 120,
    [string]$Tag = 'np',
    [int]$Delay = 2,
    [string]$InputA = '',
    [string]$InputB = '',
    [string]$Shots = '40,60,90',
    [string]$Extra = '',  # more KEY=VALUE pairs for both, ';' separated
    [int]$Port = 7101,    # the games' UDP ports: Port and Port + 1 (runs side by side need their own)
    [switch]$Watch,
    [int]$WatchDelay = 0,
    [int]$FeedPort = 7150  # the relay's: the players' feed on FeedPort, the spectator on FeedPort + 1
)
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = "$($Game.ToUpper()).exe" }
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "recomp\$Game\out\build\$Config"
$netDir = Join-Path $root 'build\netplay'
$shotDir = Join-Path $root 'build\shots'
New-Item -ItemType Directory -Force -Path $netDir, $shotDir | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class NpWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[NpWin]::SetProcessDPIAware() | Out-Null

function Snap([System.Diagnostics.Process]$p, [string]$name) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return "no window for $name" }
    $r = New-Object NpWin+RECT
    [NpWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hgt = $r.B - $r.T
    if ($w -lt 64) { return "tiny window for $name" }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    try { [NpWin]::PrintWindow($h, $hdc, 2) | Out-Null } finally { $g.ReleaseHdc($hdc) }
    $file = Join-Path $shotDir "$name.png"
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    "shot $file"
}

function Start-Instance([string]$side, [int]$bind, [int]$peer, [string]$inputs, [string]$log) {
    $x = $Game.ToUpper()  # each game reads its own variables: MKD_*, MKA_*, MKDA_*
    $vars = @{
        "${x}_NET_LOG" = $log; "${x}_AUTOPILOT" = $Mode; "${x}_PADS" = '2'; "${x}_EVENT_LOG" = '1'; 'SDL_ASSERT' = 'always_ignore'
        "${x}_INPUT" = $inputs
    }
    if ($side -eq 'w') {  # the spectator: the relay's feed only
        $vars["${x}_NET_WATCH"] = '1'; $vars["${x}_NET_FEED"] = "127.0.0.1:$($FeedPort + 1)"
    } else {
        $vars["${x}_NET_BIND"] = "$bind"; $vars["${x}_NET_PEER"] = "127.0.0.1:$peer"; $vars["${x}_NET_SIDE"] = $side
        $vars["${x}_NET_DELAY"] = "$Delay"
        if ($Watch) { $vars["${x}_NET_FEED"] = "127.0.0.1:$FeedPort" }
    }
    foreach ($kv in ($Extra -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] } }
    foreach ($k in $vars.Keys) { Set-Item -Path "env:$k" -Value $vars[$k] }
    $p = Start-Process -FilePath (Join-Path $dir $Exe) -WorkingDirectory $dir -PassThru
    foreach ($k in $vars.Keys) { Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue }
    return $p
}

$logA = Join-Path $netDir "$Tag-a.log"; $logB = Join-Path $netDir "$Tag-b.log"; $logC = Join-Path $netDir "$Tag-c.log"
Remove-Item $logA, $logB, $logC -ErrorAction SilentlyContinue
$relay = $null; $c = $null
if ($Watch) {
    $relayLog = Join-Path $netDir "$Tag-relay.txt"
    $relay = Start-Process -FilePath python -ArgumentList @('-I', (Join-Path $root 'tools\py\feedrelay.py'), '--players', $FeedPort,
        '--watchers', ($FeedPort + 1)) -PassThru -WindowStyle Hidden -RedirectStandardOutput $relayLog -RedirectStandardError "$relayLog.err"
    Start-Sleep -Milliseconds 500
}
$a = Start-Instance '0' $Port ($Port + 1) $InputA $logA
Start-Sleep -Milliseconds 300
$b = Start-Instance '1' ($Port + 1) $Port $InputB $logB
"started A pid $($a.Id), B pid $($b.Id)"
$t0 = Get-Date
$shotList = @($Shots -split ',' | Where-Object { $_ } | ForEach-Object { [int]$_ })
$taken = @{}
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not ($a.HasExited -and $b.HasExited)) {
    $t = ((Get-Date) - $t0).TotalSeconds
    if ($Watch -and -not $c -and $t -ge $WatchDelay) {
        $c = Start-Instance 'w' 0 0 '' $logC
        "started C (spectator) pid $($c.Id) at $([int]$t) s"
    }
    foreach ($s in $shotList) {
        if ($t -ge $s -and -not $taken[$s]) {
            $taken[$s] = $true
            if (-not $a.HasExited) { Snap $a "$Tag-a-${s}s" }
            if (-not $b.HasExited) { Snap $b "$Tag-b-${s}s" }
            if ($c -and -not $c.HasExited) { Snap $c "$Tag-c-${s}s" }
        }
    }
    Start-Sleep -Milliseconds 250
}
$all = @($a, $b) + @($c | Where-Object { $_ })
foreach ($p in $all) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($p in $all) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
if ($relay -and -not $relay.HasExited) { Stop-Process -Id $relay.Id -Force }
"A exit: $($a.ExitCode)  B exit: $($b.ExitCode)" + $(if ($c) { "  C exit: $($c.ExitCode)" } else { '' })
"logs: $logA $logB" + $(if ($c) { " $logC" } else { '' })
