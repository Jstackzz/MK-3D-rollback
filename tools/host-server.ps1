<#
.SYNOPSIS
  Hosts the MK3D Online server on this PC until you close the window: makes its certificate the first time, opens
  TCP 7777 and UDP 7778 on the router (UPnP; on a router without it, forward them to this PC by hand), and shows
  what players need: the address the server prints once the router answers, and the certificate fingerprint. Both go
  into the player package (tools\package.ps1). Players run only the launcher. Needs Python 3.11 or newer here.

.EXAMPLE
  powershell -File tools\host-server.ps1
  powershell -File tools\host-server.ps1 -NoUpnp
#>
param(
    [string]$Data = (Join-Path $env:LOCALAPPDATA 'MK3D\server'),  # database, certificate, private key and log
    [int]$Port = 7777,  # TLS for the launchers; the UDP meeting point and relay is Port + 1
    [switch]$NoUpnp
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue }
if (-not $py) { throw 'Hosting needs Python 3.11 or newer: https://www.python.org/downloads/' }
New-Item -ItemType Directory -Force -Path $Data | Out-Null
$cert = Join-Path $Data 'server-cert.pem'
Push-Location $root
try {
    if (-not (Test-Path $cert)) { & $py.Source -m server.makecert --out-dir $Data | Out-Host }
    $fingerprint = (& $py.Source -m server.makecert --out-dir $Data --show) -join ''
    ''
    "  Certificate fingerprint: $fingerprint"
    '  The server prints the address players connect to once the router has opened the ports. Then make the'
    '  player package for it, in another window:'
    "    powershell -File tools\package.ps1 -Server <that address>:$Port -Cert `"$cert`""
    '  Allow Python through Windows Firewall when it asks. Close this window to stop the server.'
    ''
    $serverArgs = @('-m', 'server', '--port', "$Port", '--udp-port', "$($Port + 1)",
                    '--db', (Join-Path $Data 'server.sqlite'), '--log-file', (Join-Path $Data 'server.log'))
    if (-not $NoUpnp) { $serverArgs += '--upnp' }
    & $py.Source @serverArgs
} finally {
    Pop-Location
}
