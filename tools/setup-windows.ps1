<#
.SYNOPSIS
  Fetch or install the large third-party pieces the repo does not vendor.

.PARAMETER Ghidra
  Download Ghidra 12.1.4 (about 543 MB) into external\ghidra (x86 support is built in).
.PARAMETER Llvm
  Download the portable LLVM 23.1.4 release (about 860 MB) into external\llvm; XboxXBERecomp compiles its lifted C++ with clang++ and links with lld.
.PARAMETER Python
  Install the Python packages the toolkits' scripts need (capstone for xboxrecomp).
.PARAMETER All
  Everything above.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1 -Llvm -Python
#>
param(
    [switch]$Ghidra,
    [switch]$Llvm,
    [switch]$Python,
    [switch]$All
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
if ($All) { $Ghidra = $Llvm = $Python = $true }
if (-not ($Ghidra -or $Llvm -or $Python)) { Get-Help $PSCommandPath -Detailed; exit 1 }

function Fetch([string]$Url, [string]$Dest) {
    if (Test-Path $Dest) { "already have $Dest"; return }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dest) | Out-Null
    "downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    "  {0:N1} MB  sha256 {1}" -f ((Get-Item $Dest).Length / 1MB), (Get-FileHash $Dest -Algorithm SHA256).Hash
}

if ($Ghidra) {
    $gdir = Join-Path $root 'external\ghidra'
    $gzip = Join-Path $gdir 'ghidra_12.1.4_PUBLIC_20260921.zip'
    Fetch 'https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip' $gzip
    $inst = Join-Path $gdir 'ghidra_12.1.4_PUBLIC'
    if (-not (Test-Path $inst)) { "unpacking Ghidra"; Expand-Archive -Path $gzip -DestinationPath $gdir }
    "Ghidra at $inst ; run $inst\ghidraRun.bat (needs Java 21+; Java 26 works)"
}

if ($Llvm) {
    $ldir = Join-Path $root (Join-Path 'external' 'llvm')
    $tar  = Join-Path $root (Join-Path 'external' (Join-Path 'llvm-download' 'clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.xz'))
    Fetch 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/clang%2Bllvm-23.1.4-x86_64-pc-windows-msvc.tar.xz' $tar
    New-Item -ItemType Directory -Force -Path $ldir | Out-Null
    $bin = Join-Path $ldir (Join-Path 'clang+llvm-23.1.4-x86_64-pc-windows-msvc' 'bin')
    if (-not (Test-Path (Join-Path $bin 'clang.exe'))) { "unpacking LLVM (Windows tar reads .tar.xz)"; & (Join-Path $env:SystemRoot (Join-Path 'System32' 'tar.exe')) -xf $tar -C $ldir }
    "LLVM at $bin (clang.exe, clang++.exe, lld-link.exe); tools/llvmenv.cmd puts it on PATH"
}

if ($Python) {
    python -m pip install --upgrade capstone
}
