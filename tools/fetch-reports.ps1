<#
.SYNOPSIS
  Copies the F3 reports players sent (docs/PROTOCOL.md 1.13.2) from the server to this PC: the server keeps them in
  /var/lib/mk3d/reports/<day>/<time>-<player>-<game>-<report>/ (final.jpg and next_00.jpg .. next_19.jpg, the
  frame's draws frame.txt, the textures it used tex_*.png, the end of the game's log game.log, info.txt). They land
  in -Out (default build\reports), the same folders, the ones copied before kept.

.EXAMPLE
  powershell -File tools\fetch-reports.ps1 -Address 129.80.13.245 -Key "$HOME\.ssh\mk3d-server"
  powershell -File tools\fetch-reports.ps1 -Address 129.80.13.245 -Key "$HOME\.ssh\mk3d-server" -Since 2026-11-03
#>
param(
    [Parameter(Mandatory = $true)][string]$Address,  # the server's public IP address or name
    [Parameter(Mandatory = $true)][string]$Key,      # the SSH private key for it
    [string]$User = 'ubuntu',
    [ValidatePattern('^(\d{4}-\d{2}-\d{2})?$')]
    [string]$Since = '',                             # only the days from this one on (YYYY-MM-DD)
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'build\reports' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$sshArgs = @('-i', $Key, '-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=20')
$target = "$User@$Address"
# the service owns the folder: root packs it into a tar in /tmp, copied here (byte for byte, unlike a pipe through
# PowerShell) and unpacked with Windows' own tar
$days = if ($Since) { "`$(ls | awk '`$0 >= `"$Since`"')" } else { '.' }
& ssh @sshArgs $target "sudo sh -c 'cd /var/lib/mk3d/reports && tar -cf /tmp/mk3d-reports.tar $days && chmod 644 /tmp/mk3d-reports.tar'"
if ($LASTEXITCODE) { throw "could not read the reports on $target (none yet, or the address or key is wrong)" }
$tar = Join-Path $Out 'reports.tar'
& scp @sshArgs "${target}:/tmp/mk3d-reports.tar" $tar
if ($LASTEXITCODE) { throw 'could not copy the reports' }
& ssh @sshArgs $target 'sudo rm -f /tmp/mk3d-reports.tar'
& tar -xf $tar -C $Out
if ($LASTEXITCODE) { throw 'could not unpack the reports' }
Remove-Item $tar
$reports = @(Get-ChildItem -Path $Out -Directory | ForEach-Object { Get-ChildItem -Path $_.FullName -Directory })
"{0} reports in {1}" -f $reports.Count, $Out
$reports | Sort-Object Name -Descending | Select-Object -First 20 | ForEach-Object { '  ' + $_.Parent.Name + '\' + $_.Name }
