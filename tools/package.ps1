<#
.SYNOPSIS
  Builds the player package: one zip a friend unzips and plays from, with no Python and no repository. It holds the
  launcher with its Qt runtime, the three games' executables with their compiler folders (clang and lld once, in
  toolchain\), empty assets folders, and defaults.ini with the server's address and certificate fingerprint: the
  official server unless -Server says otherwise.
  Nothing from the games goes in: every player builds each game from their own disc on the launcher's Build game page
  (docs/PLAYING.md); the script stops if it finds game data in the folder it made.

.EXAMPLE
  powershell -File tools\package.ps1
  powershell -File tools\package.ps1 -Server myname.ddns.net:7777 -Cert C:\mk3d-server\server-cert.pem
  powershell -File tools\package.ps1 -Server 203.0.113.7:7777 -Fingerprint 3A:5F:...:C2
  powershell -File tools\package.ps1 -Build -NoZip
#>
param(
    [string]$Server = '',       # host:port players connect to (the server's TLS port); default: the official server
    [string]$Fingerprint = '',  # SHA-256 of the server's certificate, any spelling; default: computed from -Cert
    [string]$Cert = '',         # the server's server-cert.pem
    [switch]$NoServer,          # no server set: players type one into Settings
    [string]$Out = 'dist',      # the package folder and zip go here (relative to the repository)
    [string[]]$Games = @('mkd', 'mka', 'mkda'),
    [switch]$Build,             # build the launcher and the games' release executables first
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$name = 'MK3D-Online'

function Get-CertFingerprint([string]$path) {
    $pem = Get-Content -Raw -Path $path
    $m = [regex]::Match($pem, '-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----', 'Singleline')
    if (-not $m.Success) { throw "no certificate in $path" }
    $der = [Convert]::FromBase64String(($m.Groups[1].Value -replace '\s', ''))
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return (($sha.ComputeHash($der) | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Get-Sha256([string]$path) { return (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLower() }

# --- the server players connect to: the official one (docs/HOSTING.md) unless told otherwise ---
$officialServer = '129.80.13.245:7777'
$officialFingerprint = 'C9:D3:E6:40:B9:8D:4E:1C:B6:5C:9A:2A:67:3F:D4:31:51:34:73:69:A2:CC:3E:0E:46:68:B9:93:69:00:D4:C1'
if ($NoServer) {
    $Server = ''
    $Fingerprint = ''
} elseif (-not $Server) {
    $Server = $officialServer
    if (-not $Fingerprint -and -not $Cert) { $Fingerprint = $officialFingerprint }
}
if ($Cert -and -not $Fingerprint) { $Fingerprint = Get-CertFingerprint $Cert }
$Fingerprint = ($Fingerprint.ToLower() -replace '[^0-9a-f]', '')
if ($Server) {
    if ($Server -notmatch '^[^:\s]+:\d+$') { throw "-Server must be host:port, for example myname.ddns.net:7777" }
    if ($Fingerprint.Length -ne 64) { throw "give the server certificate's fingerprint with -Fingerprint or -Cert" }
} else {
    Write-Warning '-NoServer: players type the server address and fingerprint into Settings themselves'
}

# --- builds ---
$vsenv = Join-Path $root 'tools\vsenv.cmd'
if ($Build) {
    & $vsenv cmake --build "$root\build\launcher"
    if ($LASTEXITCODE) { throw 'launcher build failed' }
    foreach ($g in $Games) {
        & $vsenv cmake --build "$root\recomp\$g\out\build\release"
        if ($LASTEXITCODE) { throw "$g build failed" }
    }
}
$launcherDir = Join-Path $root 'build\launcher'
if (-not (Test-Path "$launcherDir\MK3DLauncher.exe")) { throw "build the launcher first (or pass -Build)" }
foreach ($g in $Games) {
    $exe = Join-Path $root "recomp\$g\out\build\release\$($g.ToUpper()).exe"
    if (-not (Test-Path $exe)) { throw "build $g first: $exe (or pass -Build)" }
}

# --- the Visual C++ runtime, put beside every executable that needs it ---
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory | Where-Object { $_.Name -match '^\d' } |
    Sort-Object Name -Descending | ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' } |
    Select-Object -First 1
if (-not $crt) { throw 'the Visual C++ redistributable files were not found (Visual Studio with the C++ workload)' }
$crt = $crt.FullName

# --- the folder ---
$outDir = Join-Path $root $Out
$stage = Join-Path $outDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage, "$stage\toolchain", "$stage\LICENSES" | Out-Null

# the launcher and the parts of Qt it loads (Widgets draw without Direct3D: no shader compiler)
Copy-Item "$launcherDir\MK3DLauncher.exe", "$launcherDir\Qt6Core.dll", "$launcherDir\Qt6Gui.dll", "$launcherDir\Qt6Network.dll",
    "$launcherDir\Qt6Svg.dll", "$launcherDir\Qt6Widgets.dll" $stage
foreach ($plugins in 'platforms', 'styles', 'tls', 'imageformats', 'iconengines', 'networkinformation') {
    Copy-Item -Recurse "$launcherDir\$plugins" "$stage\$plugins"
}
Get-ChildItem $crt -Filter '*.dll' | Where-Object { $_.Name -match '^(msvcp140(_1|_2)?|vcruntime140(_1)?)\.dll$' } |
    Copy-Item -Destination $stage

# clang and lld once: the launcher links them into a game's compiler folder before its first start
$toolchain = Join-Path $root "recomp\$($Games[0])\out\build\release\compiler"
foreach ($tool in 'clang.exe', 'lld-link.exe') {
    $want = Get-Sha256 "$toolchain\$tool"
    foreach ($g in $Games) {
        $other = Join-Path $root "recomp\$g\out\build\release\compiler\$tool"
        if ((Get-Sha256 $other) -ne $want) { throw "$tool differs between the games' compiler folders: $other" }
    }
    Copy-Item "$toolchain\$tool" "$stage\toolchain\"
}

foreach ($g in $Games) {
    $upper = $g.ToUpper()  # not $G: PowerShell's names ignore case
    $rel = Join-Path $root "recomp\$g\out\build\release"
    $dst = Join-Path $stage "games\$g"
    New-Item -ItemType Directory -Force -Path "$dst\assets", "$dst\compiler" | Out-Null
    Copy-Item "$rel\$upper.exe" $dst
    foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item "$crt\$dll" $dst }
    # the compiler folder without its work folder (game code), its DLLs (copied fresh) and clang and lld (toolchain\)
    Get-ChildItem "$rel\compiler" | Where-Object { $_.Name -notin @('work', 'clang.exe', 'lld-link.exe') -and $_.Extension -ne '.dll' } |
        ForEach-Object { Copy-Item -Recurse $_.FullName "$dst\compiler\" }
    foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll') { Copy-Item "$crt\$dll" "$dst\compiler\" }
}

# what the package sets up, what it is, and the licenses
if ($Server) {
    Set-Content -Encoding ascii -Path "$stage\defaults.ini" -Value @"
[server]
address=$Server
fingerprint=$Fingerprint
auto_connect=true
"@
}
Copy-Item (Join-Path $root 'docs\PLAYING.md') "$stage\READ ME FIRST.txt"
Copy-Item (Join-Path $root 'external\XboxXBERecomp\THIRD_PARTY.txt') "$stage\LICENSES\games-THIRD_PARTY.txt"
Copy-Item "$toolchain\LLVM-LICENSE.txt" "$stage\LICENSES\LLVM-LICENSE.txt"
Copy-Item (Join-Path $root 'launcher\licenses\*') "$stage\LICENSES\"
$deps = Join-Path $root "recomp\$($Games[0])\out\build\release\_deps\zydis-src"
Copy-Item "$deps\LICENSE" "$stage\LICENSES\Zydis-LICENSE.txt"
Copy-Item "$deps\dependencies\zycore\LICENSE" "$stage\LICENSES\Zycore-LICENSE.txt"

$version = ((Get-Content (Join-Path $root 'launcher\CMakeLists.txt') | Select-String 'project\(mk3d_launcher VERSION ([0-9.]+)').Matches[0].Groups[1].Value)
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$lines = @("$name $version, packaged $stamp", '', 'Both players need the same package: compare these with your opponent''s.', '')
foreach ($f in @('MK3DLauncher.exe') + ($Games | ForEach-Object { "games\$_\$($_.ToUpper()).exe" })) {
    $lines += ('{0}  {1}' -f (Get-Sha256 "$stage\$f"), $f)
}
Set-Content -Encoding ascii -Path "$stage\VERSION.txt" -Value $lines

# --- nothing from the games: no disc files, no recompiled code, empty assets folders ---
$problems = @()
$problems += Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.xbe', '.iso', '.xiso', '.xpr', '.xmv', '.xwb', '.sfd', '.bik' } |
    ForEach-Object { "disc file: $($_.FullName)" }
$problems += Get-ChildItem $stage -Recurse -Directory | Where-Object { $_.Name -in 'work', 'hdd', 'logs', 'generated' } |
    ForEach-Object { "build folder: $($_.FullName)" }
foreach ($g in $Games) {
    if (Get-ChildItem "$stage\games\$g\assets" -Force) { $problems += "games\$g\assets is not empty" }
    if (Test-Path "$stage\games\$g\$($g.ToUpper()).dll") { $problems += "games\$g has its game code DLL" }
}
foreach ($f in Get-ChildItem $stage -Recurse -File) {
    $head = New-Object byte[] 4
    $s = [System.IO.File]::OpenRead($f.FullName)
    try { $n = $s.Read($head, 0, 4) } finally { $s.Dispose() }
    if ($n -eq 4 -and [System.Text.Encoding]::ASCII.GetString($head) -eq 'XBEH') { $problems += "Xbox executable: $($f.FullName)" }
}
if ($problems) { $problems | ForEach-Object { Write-Host $_ }; throw 'game data in the package, not zipped' }

$files = @(Get-ChildItem $stage -Recurse -File)
$bytes = ($files | Measure-Object -Sum Length).Sum
"{0}: {1} files, {2:N0} MB" -f $stage, $files.Count, ($bytes / 1MB)
if (-not $NoZip) {
    $zip = Join-Path $outDir "$name-$version-$stamp.zip"
    Remove-Item -Force $zip -ErrorAction SilentlyContinue
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $outDir $name
    if ($LASTEXITCODE) { throw 'zip failed' }
    "{0}: {1:N0} MB, SHA-256 {2}" -f $zip, ((Get-Item $zip).Length / 1MB), (Get-Sha256 $zip)
}
