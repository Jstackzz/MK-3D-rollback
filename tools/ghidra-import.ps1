<#
.SYNOPSIS
  Make the Ghidra project of one game: a flat PE of its XBE (xbe2pe.py), imported and auto-analysed headless, then
  named from XbSymbolDatabase and the game's config. Takes a while (the analysis is the long part).

.EXAMPLE
  powershell -File tools\ghidra-import.ps1 -Game mka
  then: powershell -File tools\ghidra-query.ps1 -Game mka -Decompile 0x12345 -Out research\mka\q-x.txt
#>
param(
    [string]$Game = 'mkd',
    [string]$Research = '',  # folder of symbols-xbsdb.txt, default research\<game> (Deadly Alliance: research\mkda-rev1)
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$G = Join-Path $root 'external\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat'
$cfg = Join-Path $root "recomp\$Game\config\game.toml"
$proj = Join-Path $root "build\ghidra-$Game"
$pe = Join-Path $root "build\$Game-flat.exe"
if (-not $Research) { $Research = "research\$Game" }
$names = @((Join-Path $root "$Research\symbols-xbsdb.txt"), $cfg) | Where-Object { Test-Path $_ }
if (-not (Test-Path $cfg)) { throw "no config at $cfg" }
if ((Test-Path $proj) -and -not $Force) { throw "$proj exists; -Force to replace it" }
if (Test-Path $proj) { Remove-Item -Recurse -Force $proj }
New-Item -ItemType Directory -Force -Path $proj, (Join-Path $root 'build\logs') | Out-Null
Push-Location (Join-Path $root "recomp\$Game")
try { & python (Join-Path $root 'external\XboxXBERecomp\tools\symbols\xbe2pe.py') 'config\game.toml' $pe } finally { Pop-Location }
if (-not (Test-Path $pe)) { throw 'xbe2pe made nothing' }
# the program is named <game>.exe inside the project
$named = Join-Path $proj "$Game.exe"
Copy-Item $pe $named -Force
$log = Join-Path $root "build\logs\ghidra-import-$Game.log"
$a = @("`"$proj`"", $Game, '-import', "`"$named`"", '-scriptPath', "`"$(Join-Path $root 'tools\ghidra')`"",
       '-postScript', 'ApplyNames.java') + ($names | ForEach-Object { "`"$_`"" })
$p = Start-Process -FilePath $G -ArgumentList $a -WorkingDirectory $proj -PassThru -WindowStyle Hidden -Wait `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err"
"ghidra import exit $($p.ExitCode); log $log"
Get-Content $log -Tail 15
