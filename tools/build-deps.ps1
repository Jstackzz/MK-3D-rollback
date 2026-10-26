<#
.SYNOPSIS
  Configure and build the C++ dependencies with the VS 2026 Build Tools (MSVC + Ninja).

.DESCRIPTION
  Builds each named checkout under external/ into build/<name> using the CMake and Ninja
  bundled with Visual Studio Build Tools. Logs go to build/logs/<name>.log.

.EXAMPLE
  powershell -File tools/build-deps.ps1                 # GekkoNet, ggpo, XbSymbolDatabase
  powershell -File tools/build-deps.ps1 -Targets ggpo   # one target
#>
param(
    [string[]]$Targets = @('GekkoNet', 'ggpo', 'XbSymbolDatabase'),
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Continue'
# 'powershell -File x.ps1 -Targets a,b' arrives as one string; split it.
$Targets = @($Targets | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root   = Split-Path -Parent $PSScriptRoot
$vsRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools"
$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
$cmake  = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

foreach ($p in $vcvars, $cmake, $ninja) {
    if (-not (Test-Path $p)) { throw "missing toolchain component: $p (install VS 2026 Build Tools with the C++ workload)" }
}

# Per-target CMake options. Keep everything static so the emulator fork can link them directly.
$options = @{
    'GekkoNet'  = @('-DBUILD_SHARED_LIBS=OFF')
    'ggpo'      = @('-DBUILD_SHARED_LIBS=OFF', '-DGGPO_BUILD_VECTORWAR=OFF', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5')  # GGPO's CMakeLists predates CMake 3.5
    'XbSymbolDatabase' = @()
}

New-Item -ItemType Directory -Force -Path (Join-Path $root 'build\logs') | Out-Null
$results = @()
foreach ($t in $Targets) {
    $src = Join-Path $root "external\$t"
    $bld = Join-Path $root "build\$t"
    $log = Join-Path $root "build\logs\$t.log"
    if (-not (Test-Path $src)) { $results += "$t : source missing ($src)"; continue }

    $cfgArgs = @('-S', "`"$src`"", '-B', "`"$bld`"", '-G', 'Ninja',
                 "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_MAKE_PROGRAM=`"$ninja`"") + $options[$t]
    $cmd = "call `"$vcvars`" >nul 2>&1 && `"$cmake`" $($cfgArgs -join ' ') && `"$cmake`" --build `"$bld`" --parallel"
    "=== $t  $(Get-Date -Format s)`n$cmd`n" | Out-File $log -Encoding utf8
    $sw = [Diagnostics.Stopwatch]::StartNew()
    cmd /c $cmd 2>&1 | Out-File $log -Append -Encoding utf8
    $code = $LASTEXITCODE
    $sw.Stop()
    $results += "{0,-10} exit={1} time={2:N0}s log={3}" -f $t, $code, $sw.Elapsed.TotalSeconds, $log
}
$results

