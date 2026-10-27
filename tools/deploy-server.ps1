<#
.SYNOPSIS
  Puts the MK3D Online server on a Linux machine you reach over SSH (an Oracle Cloud VM, a VPS) and installs it as
  an always-on service there: copies server/ over, runs server/deploy/install.sh as root, and prints the certificate
  fingerprint for tools\package.ps1. Run it again after changing the server: it updates the server and keeps the
  accounts and the certificate. After a release every player must take, -MinClient with its version has older
  launchers told to update when they connect; later runs keep it.

  -Release with a release's zip, once its GitHub release is published, announces it: launchers from 0.1.7 on offer
  it as an update and install it themselves (docs/LAUNCHER.md, Updates). The server's update.json gets the zip's
  version, link, size and SHA-256, after a check that the link serves this very zip; connected launchers are told
  within seconds. -ReleaseOnly does only that, without reinstalling (or restarting) the server.

.EXAMPLE
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\Downloads\ssh-key-2026-10-25.key"
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\Downloads\ssh-key-2026-10-25.key" -Name mk3d.duckdns.org
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\Downloads\ssh-key-2026-10-25.key" -MinClient 0.1.4
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\.ssh\mk3d-server" -Release dist\MK3D-Online-0.1.8-20261101-1200.zip -Notes "Faster menus." -ReleaseOnly
#>
param(
    [Parameter(Mandatory = $true)][string]$Address,  # the machine's public IP address or name
    [Parameter(Mandatory = $true)][string]$Key,      # the SSH private key for it (Oracle has you save one)
    [string]$User = 'ubuntu',                        # who to log in as: ubuntu on Ubuntu images, opc on Oracle Linux
    [string]$Name = '',                              # the name players will use, when not the address itself
    [ValidatePattern('^\d+(\.\d+)*$')]
    [string]$MinClient = '',                         # the oldest launcher let in (default: the one set before)
    [string]$Release = '',                           # a published release's zip (dist\MK3D-Online-<version>-...zip)
    [string]$Notes = '',                             # a few lines on what is new, shown in the update's window
    [string]$Repo = 'Jstackzz/MK-3D-rollback',       # the GitHub repository the release is published in
    [switch]$ReleaseOnly                             # announce the release only: the server is not reinstalled
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
foreach ($tool in 'ssh', 'scp') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is missing: Windows Settings, System, Optional features, add OpenSSH Client"
    }
}
if (-not (Test-Path $Key)) { throw "no key at $Key" }

# Windows' OpenSSH refuses a private key other accounts can read: use a copy only this account can read
$sshDir = Join-Path $HOME '.ssh'
New-Item -ItemType Directory -Force -Path $sshDir | Out-Null
$keyCopy = Join-Path $sshDir 'mk3d-server.key'
Copy-Item -Force -Path $Key -Destination $keyCopy
& icacls $keyCopy /inheritance:r /grant:r "${env:USERNAME}:F" | Out-Null
if ($LASTEXITCODE) { throw "could not restrict $keyCopy to this account" }

$target = "$User@$Address"
$sshArgs = @('-i', $keyCopy, '-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=20')
$players = if ($Name) { $Name } else { $Address }

# the release launchers are offered: its version from the zip's VERSION.txt, the link GitHub serves it at
$updateJson = $null
if ($Release) {
    if (-not (Test-Path $Release)) { throw "no zip at $Release" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $Release).Path)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq 'MK3D-Online/VERSION.txt' }
        if (-not $entry) { throw "$Release is not a player package (no MK3D-Online/VERSION.txt)" }
        $reader = New-Object IO.StreamReader($entry.Open())
        $first = $reader.ReadLine()
        $reader.Close()
    } finally { $zip.Dispose() }
    if ($first -notmatch '^MK3D-Online (\d+(\.\d+)*),') { throw "VERSION.txt starts '$first', not 'MK3D-Online <version>,'" }
    $version = $Matches[1]
    $size = (Get-Item $Release).Length
    $sha = (Get-FileHash -Algorithm SHA256 -Path $Release).Hash.ToLower()
    $url = "https://github.com/$Repo/releases/download/v$version/MK3D-Online-$version-windows.zip"
    "Checking that $url serves this zip ($size bytes) ..."
    $served = Join-Path $env:TEMP "mk3d-release-check-$version.zip"
    try {
        Invoke-WebRequest -Uri $url -OutFile $served -UseBasicParsing
    } catch {
        throw "the release is not at $url yet: publish it on GitHub first ($($_.Exception.Message))"
    }
    $servedSha = (Get-FileHash -Algorithm SHA256 -Path $served).Hash.ToLower()
    Remove-Item -Force $served -ErrorAction SilentlyContinue
    if ($servedSha -ne $sha) { throw "$url serves another zip (SHA-256 $servedSha, this one $sha)" }
    $updateJson = Join-Path $env:TEMP 'mk3d-update.json'
    $info = [ordered]@{ version = $version; url = $url; sha256 = $sha; size = $size; notes = $Notes
                        page = "https://github.com/$Repo/releases/tag/v$version" }
    [IO.File]::WriteAllText($updateJson, ($info | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
}
if ($ReleaseOnly -and -not $updateJson) { throw '-ReleaseOnly needs -Release' }

if (-not $ReleaseOnly) {
    $tar = Join-Path $env:TEMP 'mk3d-server.tar'
    & "$env:SystemRoot\System32\tar.exe" -c -f $tar -C $root --exclude __pycache__ --exclude '*.pyc' --exclude tests server
    if ($LASTEXITCODE) { throw 'packing server/ failed' }
    "Copying the server to $target ..."
    & scp @sshArgs $tar "${target}:/tmp/mk3d-server.tar"
    if ($LASTEXITCODE) { throw "could not copy to $target (is the machine running, and the address and key right?)" }
    "Installing it there ..."
    & ssh @sshArgs $target "rm -rf /tmp/mk3d && mkdir -p /tmp/mk3d && tar -x -f /tmp/mk3d-server.tar -C /tmp/mk3d && sudo sh /tmp/mk3d/server/deploy/install.sh $players $MinClient"
    if ($LASTEXITCODE) { throw 'the installer failed: its messages are above' }
    Remove-Item -Force $tar -ErrorAction SilentlyContinue
}
if ($updateJson) {
    "Announcing $version ..."
    & scp @sshArgs $updateJson "${target}:/tmp/mk3d-update.json"
    if ($LASTEXITCODE) { throw "could not copy update.json to $target" }
    # read again by the server within seconds; launchers connected are offered it at once
    & ssh @sshArgs $target 'sudo install -m 644 /tmp/mk3d-update.json /var/lib/mk3d/update.json && rm -f /tmp/mk3d-update.json'
    if ($LASTEXITCODE) { throw 'could not put update.json in place' }
    Remove-Item -Force $updateJson -ErrorAction SilentlyContinue
    "Launchers from 0.1.7 on now offer $version as an update."
}
if (-not $ReleaseOnly) {
    ''
    "Next: powershell -File tools\package.ps1 -Server ${players}:7777 -Fingerprint <the fingerprint above>"
}
