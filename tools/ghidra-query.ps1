<#
.SYNOPSIS
  Ask the analysed Ghidra project questions without opening the GUI: decompile functions, list references.

.EXAMPLE
  powershell -File tools\ghidra-query.ps1 -Decompile 0x2406F0,0x240830 -Xrefs 0x4A0AA0 -Out research\mkd\q-movie.txt
#>
param(
    [string]$Game = 'mkd',    # build\ghidra-<game> holds program <game>.exe (tools\ghidra-import.ps1 makes it)
    [string]$Project = '',
    [string]$Program = '',
    [string]$Decompile = '',  # comma separated addresses: the functions containing them
    [string]$Xrefs = '',      # comma separated addresses
    [string]$Out = 'build\ghidra-query.txt'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$G = Join-Path $root 'external\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat'
if (-not $Project) { $Project = "build\ghidra-$Game" }
if (-not $Program) { $Program = "$Game.exe" }
$proj = Join-Path $root $Project
$projName = (Split-Path $Project -Leaf) -replace '^ghidra-', ''
$outPath = Join-Path $root $Out
if (Test-Path $outPath) { Remove-Item $outPath }
$a = @("`"$proj`"", $projName, '-process', $Program, '-noanalysis', '-readOnly', '-scriptPath', "`"$(Join-Path $root 'tools\ghidra')`"")
if ($Decompile) { $a += @('-postScript', 'DecompileFuncs.java', "`"$outPath`"") + ($Decompile -split ',') }
if ($Xrefs) {
    $x = "$outPath.xrefs"
    if (Test-Path $x) { Remove-Item $x }
    $a += @('-postScript', 'XrefsTo.java', "`"$x`"") + ($Xrefs -split ',')
}
$log = Join-Path $root 'build\logs\ghidra-query.log'
$p = Start-Process -FilePath $G -ArgumentList $a -WorkingDirectory $proj -PassThru -WindowStyle Hidden -Wait `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err"
if ($Xrefs -and (Test-Path "$outPath.xrefs")) { Get-Content "$outPath.xrefs" | Add-Content $outPath; Remove-Item "$outPath.xrefs" }
if (Test-Path $outPath) { Get-Content $outPath } else { "no output; see $log" ; Get-Content $log -Tail 20 }
