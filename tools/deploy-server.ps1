<#
.SYNOPSIS
  Puts the MK3D Online server on a Linux machine you reach over SSH (an Oracle Cloud VM, a VPS) and installs it as
  an always-on service there: copies server/ over, runs server/deploy/install.sh as root, and prints the certificate
  fingerprint for tools\package.ps1. Run it again after changing the server: it updates the server and keeps the
  accounts and the certificate. After a release every player must take, -MinClient with its version has older
  launchers told to update when they connect; later runs keep it.

.EXAMPLE
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\Downloads\ssh-key-2026-10-25.key"
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\Downloads\ssh-key-2026-10-25.key" -Name mk3d.duckdns.org
  powershell -File tools\deploy-server.ps1 -Address 129.153.10.20 -Key "$HOME\Downloads\ssh-key-2026-10-25.key" -MinClient 0.1.4
#>
param(
    [Parameter(Mandatory = $true)][string]$Address,  # the machine's public IP address or name
    [Parameter(Mandatory = $true)][string]$Key,      # the SSH private key for it (Oracle has you save one)
    [string]$User = 'ubuntu',                        # who to log in as: ubuntu on Ubuntu images, opc on Oracle Linux
    [string]$Name = '',                              # the name players will use, when not the address itself
    [ValidatePattern('^\d+(\.\d+)*$')]
    [string]$MinClient = ''                          # the oldest launcher let in (default: the one set before)
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

$tar = Join-Path $env:TEMP 'mk3d-server.tar'
& "$env:SystemRoot\System32\tar.exe" -c -f $tar -C $root --exclude __pycache__ --exclude '*.pyc' --exclude tests server
if ($LASTEXITCODE) { throw 'packing server/ failed' }

$target = "$User@$Address"
$sshArgs = @('-i', $keyCopy, '-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=20')
"Copying the server to $target ..."
& scp @sshArgs $tar "${target}:/tmp/mk3d-server.tar"
if ($LASTEXITCODE) { throw "could not copy to $target (is the machine running, and the address and key right?)" }
$players = if ($Name) { $Name } else { $Address }
"Installing it there ..."
& ssh @sshArgs $target "rm -rf /tmp/mk3d && mkdir -p /tmp/mk3d && tar -x -f /tmp/mk3d-server.tar -C /tmp/mk3d && sudo sh /tmp/mk3d/server/deploy/install.sh $players $MinClient"
if ($LASTEXITCODE) { throw 'the installer failed: its messages are above' }
Remove-Item -Force $tar -ErrorAction SilentlyContinue
''
"Next: powershell -File tools\package.ps1 -Server ${players}:7777 -Fingerprint <the fingerprint above>"
