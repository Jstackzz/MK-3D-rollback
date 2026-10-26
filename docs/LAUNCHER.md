# MK3D Online: server and launcher

Milestone N6 of [ROADMAP.md](ROADMAP.md): the lobby/matchmaking server, the Qt
launcher, and the launcher <-> game file protocol. The wire formats are
specified in [PROTOCOL.md](PROTOCOL.md); this page is about building, running
and hosting.

```
server/                  Python 3.13 package, asyncio + standard library only
  __main__.py            python -m server ...
  app.py                 sessions, queue, matches, lobbies, UDP rendezvous
  matchmaking.py         pairing rules (pure functions)
  db.py, auth.py, elo.py SQLite storage, scrypt passwords / tokens, Elo
  makecert.py            python -m server.makecert (self-signed certificate + fingerprint)
  client.py              python -m server.client (test client / scripted player / notices)
  tests/                 pytest suite (python -m pytest server/tests)
launcher/                Qt 6 Widgets, C++20, CMake + Ninja + MSVC
  data/{mkda,mkd,mka}.json  rosters and modes (compiled into the exe)
  src/                   MainWindow, pages, ServerClient (TLS), UdpRendezvous, GameProcess, MatchController
  tools/fake_game.py     development stand-in for a game executable (speaks cmd.txt/status.txt)
  tools/aqt_split_layout.py  aqtinstall shim for the Qt 6.11+ repository layout
  build.cmd              configure + build + windeployqt
```

## Server

### Run it locally

Python 3.11 or newer, nothing else for the server itself. The certificate
helper uses the `cryptography` package (`python -m pip install --user
cryptography`) or falls back to the `openssl` command.

```
python -m server.makecert                 # writes server-cert.pem + server-key.pem, prints the fingerprint
python -m server --host 0.0.0.0 --port 7777 --udp-port 7778 --db server.sqlite
```

The server looks for `server-cert.pem` / `server-key.pem` next to the
database unless `--cert/--key` say otherwise, and prints the SHA-256
fingerprint at start-up. Useful options:

| option | meaning |
|---|---|
| `--motd TEXT` | message of the day shown in every launcher |
| `--admins a,b` | accounts allowed to broadcast notices (`python -m server.client notice ...`) |
| `--min-client 0.1.0` | refuse older launchers |
| `--allow-same-ip` | let ranked pair two players behind one public IP (LAN events, testing) |
| `--accept-timeout 15` | seconds to accept a found match |
| `--upnp` | open the server's TCP and UDP ports on this network's router (a server on a home PC; `tools/host-server.ps1` passes it) |
| `--no-relay` | cancel a match whose players cannot reach each other instead of relaying it through the server |
| `--relay-only` | development: relay every match (tests the relay on one PC; `tools/online-e2e.ps1 -Relay`) |
| `--log-file server.log`, `--log-level DEBUG` | logging; disputes are logged as `DISPUTE` lines |
| `--plain` | development only: no TLS (launcher address `tcp://host:port`) |

State that survives restarts lives in the SQLite file (accounts, login tokens
as SHA-256, per game/mode ranked Elo and wins/losses, casual wins/losses, the
match log and every result report). Queues, lobbies and matches in progress
are in memory and are lost on restart.

### Tests

```
python -m pip install --user pytest pytest-asyncio
python -m pytest server/tests
```

The suite starts a real TLS server on ephemeral ports with a throw-away
certificate and database and covers accounts, pinning, rate limiting, queue
pairing (pure rules and over the wire), accept/decline/timeouts, the UDP
rendezvous and punching on 127.0.0.1, Elo on agreement, no change on
disagreement, desync/unconfirmed results, and lobbies.

### Scripted players

```
python -m server.client demo --fingerprint <fp>             # two throw-away accounts play one match
set MK3D_PASSWORD=...                                        # never on the command line
python -m server.client play --fingerprint <fp> --name tester2 --register --game mkd --mode kombat --udp-port 7002
```

`play` behaves like a launcher with a simulated game: it queues, accepts,
punches, answers the stand-in game's heartbeats and reports a result derived
from the match seed, so it agrees with a launcher running
`launcher/tools/fake_game.py`.

## Launcher

### Build

Qt 6 is installed into `external/qt` with aqtinstall:

```
python -m pip install --user aqtinstall
python -m aqt list-qt windows desktop
```

Up to Qt 6.10 the plain `python -m aqt install-qt windows desktop <version>
win64_msvc2022_64 -O external/qt` works. From Qt 6.11 the online repository
keeps desktop packages in per-toolchain folders
(`qt6_6120/qt6_6120_msvc2022_64/`), which aqtinstall 3.3.0 does not find
("Failed to locate XML data for Qt version"). For 6.11 and newer, run aqt
through the shim, which only maps the architecture to the new folder name:

```
python launcher/tools/aqt_split_layout.py install-qt windows desktop 6.12.0 win64_msvc2022_64 -O external/qt
```

This repository uses Qt **6.12.0** (`external/qt/6.12.0/msvc2022_64`); MSVC
2022 binaries are ABI compatible with the VS 2026 Build Tools (MSVC 14.50).
Then:

```
launcher\build.cmd
```

or by hand:

```
tools\vsenv.cmd cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%CD%/external/qt/6.12.0/msvc2022_64
tools\vsenv.cmd cmake --build build/launcher
external\qt\6.12.0\msvc2022_64\bin\windeployqt.exe --release --no-translations --no-system-d3d-compiler --no-opengl-sw build\launcher\MK3DLauncher.exe
```

`build\launcher` is then a self-contained folder (TLS goes through the
Windows Schannel backend; no OpenSSL DLLs are needed).

### Use it

1. **Settings**: server address (`host:7777`) and the fingerprint the server
   operator published, then *Connect*. With an empty fingerprint the launcher
   shows the presented one and asks whether to trust it. Set each game's
   folder and executable (defaults: `games\<game>` and the executable in it
   beside the launcher of a player package, else `recomp/<game>` and
   `recomp/<game>/out/build/release/<MKD|MKA|MKDA>.exe`), input delay (0-4),
   region, max ping, the game UDP port (default 7001) and the display (window
   mode, resolution, v-sync: borderless at the monitor's own resolution unless
   changed; a game takes them over its `settings.toml` when it starts). A
   `defaults.ini` beside the launcher (a player package's server) supplies
   any setting the settings file does not have.
2. **Sign in** (top right): create an account or sign in; "remember me" keeps
   a 30-day login token in the settings file. A launcher that never signed in
   on this PC opens the window by itself, on *create account*, once a run when
   it first connects (not with `--user`, `--local-test` or `--extract`).
3. **Game selector** (Deadly Alliance / Deception / Armageddon) switches every
   page; modes and rosters come from `launcher/data/<game>.json`.
4. **Ranked / Casual**: pick the mode and fighter, *Find match*. With *Pick my
   fighter on the game's own select screen* (`[online] pick_in_game`, on by
   default) the launcher sends no fighter and the player picks in the game when
   the match starts (docs/PROTOCOL.md 3.3). The game is
   started in the background and boots to its hold point while you search.
   When a match is found you have 15 s to accept; then the launcher punches
   the opponent on the game port, measures the ping, writes the `start` line
   into the game's `cmd.txt`, reports the result the game writes to
   `status.txt`, and parks the game again.
5. **Lobby**: create (optional password, best of 1/3/5), join from the list or
   by 5-letter code, pick a fighter (or *Pick in the game*), chat, *Ready*; the
   host starts.
6. **Single player**: *PLAY* starts the game plainly (no `MK3D_NET`), so it
   runs its own menus as on the console, its practice mode included; *Close
   the game* closes its window as the player would. While it is open the match
   controller refuses online play ("Close your single-player game first"), so
   one game at a time writes the save. In a source build the **Local test** box
   starts two instances on this PC (UDP 7001/7002) with matching start lines,
   no server needed; a player package hides it.
7. **Leaderboard**: top 100 by Elo per game and mode.
8. **Build game**: pick your disc image (`.iso` / `.xiso.iso`), *EXTRACT AND
   BUILD*. The launcher reads the image's Xbox file system itself
   (`src/Xiso.cpp`, the reader `tools/py/xiso.py` also is: an XISO or a whole
   disc dump, no Python), checks the disc's `default.xbe` against the release
   the build is made for (`xbe_sha1` in `launcher/data/<game>.json`; another
   release asks first) and the free space, extracts into `<game
   folder>/assets` on a worker thread with a progress bar, then starts the
   game once so it compiles its code library (*Launch once to compile* does
   that again by hand). Before any game start, a player package's
   `toolchain\` (clang and lld, kept once) is hard-linked into the game's
   compiler folder, and a game without its files is not started.
9. **Controls**: which keys press each button of the original Xbox controller
   (two at most; click one and press the new key, Esc cancels, *No key*
   clears it; a key given to another button leaves the one that had it) and
   what each button of a PC controller presses, named by where it sits (the
   bottom face button is Xbox A, PlayStation cross, Switch B). Saved at each
   change to `controls.toml` beside the settings file; the launcher hands its
   path to every game it starts (`XBE_CONTROLS`), and a running game reads it
   again within a second (runtime patch 0016). Without the file the games keep
   the layout of patch 0013.

Files next to the exe (or `%LOCALAPPDATA%\MK3D\MK3D Online` when that folder
is read-only): `mk3d-launcher.ini`, `controls.toml`, `mk3d-launcher.log` and
`sessions\`, one folder per game process with its `cmd.txt`, `status.txt` and
`game.log`.

Matches bind the game UDP port on every interface, so a player across the
internet reaches this one even when the server runs on this PC; `[dev]
udp_loopback=true` keeps one-PC tests on loopback (`tools\online-e2e.ps1`
sets it). A peer the server sees at a loopback or private address plays on the
server's own network: from outside, the launcher reaches it at the server's
address (its own port), and a punch that arrives from a port the server did
not see becomes one more address to punch.

The player package for friends (launcher, Qt, the three games, no game data,
no Python): `tools\package.ps1`, see [HOSTING.md](HOSTING.md); what a player
does with it: [PLAYING.md](PLAYING.md).

### Without the game

Until the game side of PROTOCOL.md section 3 lands in `recomp/`, point a
game's executable at `launcher/tools/fake_game.py` (Settings, *Game folders*).
The launcher runs `.py` files with the configured Python. The stand-in goes
`booting` -> `ready`, binds the start line's `local_port`, exchanges
heartbeats with `remote`, writes `in_match`, and after `MK3D_FAKE_SECONDS`
(default 6) writes a result computed from the seed, so both sides agree.

Command-line options for development and automation:

| option | effect |
|---|---|
| `--config <ini>` | use another settings file (two launchers on one PC need two) |
| `--server host:port`, `--fingerprint <hex>` | override and store the server settings |
| `--user NAME [--register]` | sign in (or create) with the password from `MK3D_DEV_PASSWORD` (`--password-env` picks another variable) |
| `--queue casual\|ranked --game mkd --mode kombat --character Scorpion` | search right after signing in |
| `--auto-accept N` | accept a found match after N seconds |
| `--lobby host\|join --lobby-name NAME --game mkd --mode kombat --character Scorpion` | host a lobby of that name and start its match once both are ready, or join it by name and mark ready |
| `--page ranked\|casual\|lobby\|single\|leaderboard\|build\|controls\|settings` | start page (`practice`, its old name, opens `single`) |
| `--local-test` | start the two-instance loopback test |
| `--extract <image>` | open *Build game* and extract that image |

A complete local round trip: start a server on 127.0.0.1, run the launcher
with `--user tester1 --register --queue casual --auto-accept 3` and the stand-in
game, and `python -m server.client play --name tester2 --register --udp-port
7002` as the opponent. With real games, `tools\online-e2e.ps1` runs the whole
chain on one PC: the server, two launchers and the two games they start
(`-Game mkd|mka|mkda`, `-Mode kombat|chess|puzzle|motor`, `-Queue
casual|ranked|lobby`; ranked starts the server with `--allow-same-ip`;
`-Launcher <folder>\MK3DLauncher.exe` runs an unpacked player package with
its own games).

## Hosting a public server

**Machine.** Any always-on PC or a small VPS (1 vCPU and 512 MB are plenty;
the server is one process with a SQLite file). Install Python 3.11+, copy the
`server/` folder, create a working folder for the database and certificate.

**Certificate and fingerprint.**

```
python -m server.makecert --out-dir /srv/mk3d --host lobby.example.org --host 203.0.113.7
```

Keep `server-key.pem` private (it is written with mode 0600 on Linux) and
back up both files: a new certificate has a new fingerprint and every player
would have to update theirs. Publish the printed fingerprint where players
get the server address (the project page, the Discord); players paste it
into Settings or compare it with the trust-on-first-use prompt.

**Ports.** Open inbound **TCP 7777** (lobby, TLS) and **UDP 7778**
(rendezvous). Players need nothing inbound unless their NAT is strict; then
they forward the game port (UDP 7001) to their PC.

- Home server: give the PC a fixed LAN address, forward TCP 7777 and UDP 7778
  on the router to it, and use a dynamic-DNS name if the public IP changes.
- Windows firewall (elevated PowerShell on the server):
  `New-NetFirewallRule -DisplayName "MK3D lobby" -Direction Inbound -Protocol TCP -LocalPort 7777 -Action Allow`
  and the same with `-Protocol UDP -LocalPort 7778`.
- Linux: `ufw allow 7777/tcp` and `ufw allow 7778/udp` (or the cloud
  provider's security group).

**Run as a service** (Linux, systemd):

```
[Unit]
Description=MK3D Online server
After=network-online.target

[Service]
WorkingDirectory=/srv/mk3d
ExecStart=/usr/bin/python3 -m server --host 0.0.0.0 --port 7777 --udp-port 7778 --db /srv/mk3d/server.sqlite --log-file /srv/mk3d/server.log
Environment=PYTHONPATH=/opt/mk3d
Restart=on-failure
User=mk3d

[Install]
WantedBy=multi-user.target
```

On Windows, a Task Scheduler task "at startup" running the same command, or
NSSM, does the same job.

**Backups.** `sqlite3 server.sqlite ".backup server-backup.sqlite"` works
while the server runs (WAL mode).

**Notices.** Start the server with `--admins yourname`, then
`python -m server.client notice --host lobby.example.org --fingerprint <fp> --name yourname --text "Restart in 10 minutes" --level warn`
(password in `MK3D_PASSWORD`).

## Known limitations (v1)

- The three games speak the game side of the protocol (hold point,
  `start`/`park`/`rematch`, status lines, rollback netplay on `local_port`:
  `recomp/common/src/mk3d`); `launcher/tools/fake_game.py` still stands in for
  a game in launcher-only tests.
- NAT traversal: UPnP on each player's router, UDP hole punching, then the
  server's relay (PROTOCOL.md 2.4, 2.5). No NAT-PMP / PCP; IPv4 only. A
  relayed match costs the detour through the server in delay.
- Results are self-reported: agreement counts, disagreement voids the match,
  a single report counts for nothing. There is no disconnect or rage-quit
  penalty, and two colluding accounts can trade wins.
- Elo per game and mode (K = 32), no per-character ratings, seasons, rank
  tiers or match history page; the launcher shows the top 100.
- Lobbies hold two players, no spectators, replays, blocks or reports; chat
  is limited only by length and rate.
- No rematch negotiation for queue matches (the `rematch` command exists for
  the game; lobbies rematch by starting again).
- One server process with in-memory queues and lobbies; a restart drops
  searches and matches in progress. No password change or account recovery.
- Region is self-declared, and the ping estimate used for ordering is the sum
  of both players' round trips to the server; the real ping is measured by
  the punch and cached per pair for 10 minutes.
- *Build game* extracts the disc and starts the game once; it does not check
  the disc's XBE identity and does not run the recompiler/compiler pipeline
  (the executable must come from the `recomp/` build).
- The launcher is Windows only, not code-signed, has no auto-update, and does
  not clean up old `sessions\` folders. Binding the game port on all
  interfaces makes Windows Firewall ask once whether the launcher may accept
  connections; allow it for private (and, if you play over the internet,
  public) networks.
