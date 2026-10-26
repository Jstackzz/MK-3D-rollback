# Hosting online play and making the player package

## How players connect

It works like Tekken 6 Native Online: players run only the launcher, nothing
else. Every launcher signs in to one server (accounts, queues, lobbies, ranks),
which introduces the two players of a match; the fight then goes straight
between their two PCs. To get through home routers, each launcher:

1. opens the game's UDP port on its router by itself (UPnP), on routers that
   allow it (most do);
2. punches through routers that do not, using the addresses the server saw;
3. when the two PCs still cannot reach each other (some mobile or carrier-grade
   NAT connections), plays the match through the server's relay instead: the
   server passes the game packets on, with a little more delay.

Nobody forwards ports or installs a VPN. The one thing that is not automatic
is the server itself: someone has to run it on a machine that is on while
people play. That is you.

## 1. Run the server

The server is `server/` (Python 3.11 or newer, standard library only).

**On your PC** (playing with friends; your PC has to be on while they play):

```
powershell -File tools\host-server.ps1
```

It makes the server's certificate the first time (in
`%LOCALAPPDATA%\MK3D\server`, with the database and the log), opens TCP 7777
and UDP 7778 on your router (UPnP; your router answered UPnP when this was
written, and your connection has a public address), prints the certificate
fingerprint, and once the router answers, the address players connect to.
Allow Python through Windows Firewall when it asks. On a router without UPnP,
forward those two ports to your PC by hand (`-NoUpnp` skips the attempt).

Most home connections get a new public address now and then; a free dynamic
DNS name (No-IP, Duck DNS, or the router's own) keeps one name pointing at it,
so packages made for that name keep working.

To play on the same PC, point your own launcher at `127.0.0.1:7777` (Settings,
Server): your friends' launchers reach you at the server's address.

**On an always-on machine** (a public release, strangers playing any time):
any Linux machine with a public IPv4 address, such as Oracle Cloud's Always
Free VM (steps below) or a small paid VPS. From this PC:

```
powershell -File tools\deploy-server.ps1 -Address <its public IP> -Key <its SSH private key>
```

It copies `server/` over SSH and runs `server/deploy/install.sh` there, which
installs the server as a service that starts at boot and restarts if it stops
(its log: `journalctl -u mk3d-server -f`), opens TCP 7777 and UDP 7778 in the
machine's own firewall (ufw, firewalld, or the iptables rules of Oracle's Ubuntu
images), and prints the certificate fingerprint. Open the same two ports in the
provider's network firewall too (a security list or security group). Running it
again updates the server and keeps the accounts and the certificate. On the
machine itself, `sudo sh server/deploy/install.sh <name>` from a clone of this
repository does the same.

### Oracle Cloud Always Free, step by step

1. **Account.** <https://www.oracle.com/cloud/free/>, *Start for free*: email,
   name, a phone number and a card (identity only; Always Free resources are
   not charged). The **home region** chosen here is permanent and is where the
   free machine lives: take the one nearest most players (for North America,
   US East (Ashburn) or US Midwest (Chicago)). The account can take from a few
   minutes to a few hours to be ready.
2. **Keep it from being stopped (recommended).** Oracle stops free machines it
   judges idle, and a game lobby looks idle. Upgrading the account to *Pay As
   You Go* (Billing & Cost Management, Upgrade and Manage Payment) exempts it
   while Always Free resources stay free; then add a $1 budget with an email
   alert (Billing & Cost Management, Budgets) so anything that would cost money
   is caught at once.
3. **The machine.** Menu, Compute, Instances, *Create instance*:
   - name `mk3d-server`;
   - image: *Change image*, Ubuntu, **Canonical Ubuntu 24.04**;
   - shape: *Change shape*, Virtual machine, Specialty and previous generation,
     **VM.Standard.E2.1.Micro** (marked *Always Free-eligible*; the Ampere
     VM.Standard.A1.Flex works too where the region has room);
   - networking: keep the new virtual cloud network and public subnet, with
     *Assign a public IPv4 address* on;
   - SSH keys: *Generate a key pair for me*, **Save private key**, and keep the
     file: it is the key to the machine;
   - *Create*, wait for *Running*, and copy the **Public IP address** from the
     instance's page.
4. **Open the two ports in Oracle's network.** On the instance's page, its
   subnet, the subnet's *Default Security List* (or *Security* tab), *Add
   Ingress Rules*: source CIDR `0.0.0.0/0`, IP protocol TCP, destination port
   `7777`; then a second rule: `0.0.0.0/0`, UDP, `7778`.
5. **Install the server** from this PC with `tools\deploy-server.ps1` (above),
   giving it the public IP and the saved key file.
6. **Make the player package** with that address and the printed fingerprint
   (section 2).

A free name for the server (Duck DNS: sign in, add a subdomain, enter the
public IP) can go into the package instead of the IP, so the address can follow
the server if it ever moves.

## 2. The player package

```
powershell -File tools\package.ps1
```

makes it for the official server, `129.80.13.245:7777` (the Oracle Cloud machine
set up on 2026-10-25, certificate fingerprint
`C9:D3:E6:40:B9:8D:4E:1C:B6:5C:9A:2A:67:3F:D4:31:51:34:73:69:A2:CC:3E:0E:46:68:B9:93:69:00:D4:C1`).
For another server:

```
powershell -File tools\package.ps1 -Server <address>:7777 -Cert %LOCALAPPDATA%\MK3D\server\server-cert.pem
powershell -File tools\package.ps1 -Server <address>:7777 -Fingerprint <the fingerprint the server printed>
```

It writes `dist\MK3D-Online\` and `dist\MK3D-Online-<version>-<date>.zip` from the
launcher in `build\launcher` and the games' release builds in
`recomp\<game>\out\build\release` (`-Build` builds them first): the launcher and
its Qt runtime, each game's executable and compiler folder, clang and lld once in
`toolchain\`, empty `assets` folders, `defaults.ini` with your server, the guide
(`READ ME FIRST.txt`, from `docs/PLAYING.md`), `VERSION.txt` and `LICENSES\`. It
stops before zipping if it finds anything from the games in the folder: no disc
files, no recompiled code, no assets.

Send the zip; each player unpacks it and builds the games from their own discs
(the Build game page). Play from the same package yourself, or at least from the
same game executables: two different builds of a game fall out of step at once.
Make a new package after any change to the games and send it to everyone.

The math the games' state depends on (sine, arctangent, powers and so on) is built
into the executables (runtime patch 0015), so PCs with different Windows versions
and processors compute the same results, which rollback needs.

## 3. When a match does not connect

- The launcher's Settings page shows the router's state on the Router line
  (port open, no UPnP, or a router behind carrier-grade NAT).
- The server log names every match it relays ("relayed through this server")
  and how many packets went each way. `--no-relay` turns the relay off;
  `--relay-only` relays every match (for testing it).
- A player whose launcher cannot reach the server at all has the address or
  fingerprint wrong (Settings), or the server's ports are not open.
