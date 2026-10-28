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
| `--accept-timeout 20` | seconds to accept a found match |
| `--upnp` | open the server's TCP and UDP ports on this network's router (a server on a home PC; `tools/host-server.ps1` passes it) |
| `--no-relay` | cancel a match whose players cannot reach each other instead of relaying it through the server |
| `--relay-only` | development: relay every match (tests the relay on one PC; `tools/online-e2e.ps1 -Relay`) |
| `--log-file server.log`, `--log-level DEBUG` | logging; disputes are logged as `DISPUTE` lines |
| `--plain` | development only: no TLS (launcher address `tcp://host:port`) |

State that survives restarts lives in the SQLite file (accounts, login tokens
as SHA-256, per game/mode ranked Elo and wins/losses, casual wins/losses, the
match log, every result report and the ranked sets). Queues, lobbies, series
(casual rematches, ranked sets in progress) and matches in progress are in
memory and are lost on restart.

### Tests

```
python -m pip install --user pytest pytest-asyncio
python -m pytest server/tests
```

The suite starts a real TLS server on ephemeral ports with a throw-away
certificate and database and covers accounts, pinning, rate limiting, queue
pairing (pure rules and over the wire), accept/decline/timeouts, the UDP
rendezvous and punching on 127.0.0.1, wins on agreement, no change on
disagreement, desync/unconfirmed results, lobbies, casual rematches (the same
fighters, or both picking again) and ranked sets (Elo once per set, the one who
leaves a set loses it).

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
   `recomp/<game>/out/build/release/<MKD|MKA|MKDA>.exe`), input delay (0-4, 1
   unless changed; a match takes the larger of the two players'),
   region, max ping, the game UDP port (default 7001), *Hide my IP address
   from opponents* (cfg `online/hide_ip`: `relay` in `queue_join` and
   `lobby_ready`, PROTOCOL.md 1.7) and the display (window
   mode, resolution, v-sync, *Brightness* (`display/brightness`, 100 % unless
   changed: the runtime's `brightness`, a gamma curve over the game's own ramp,
   blacks and whites kept), *Stretch Deadly Alliance to fill the screen*
   (`display/stretch_4x3`, on unless unticked; the runtime's `stretch_4x3` for a
   game with no 16:9 layout, else 4:3 between bars): borderless at the
   monitor's own resolution unless changed; a game takes them over its
   `settings.toml` when it starts, through `MK3D_DISPLAY`, `MK3D_RESOLUTION`,
   `MK3D_VSYNC`, `MK3D_BRIGHTNESS` and `MK3D_STRETCH_4X3`). A
   `defaults.ini` beside the launcher (a player package's server) supplies
   any setting the settings file does not have.
2. **Sign in** (top right): create an account or sign in; "remember me" keeps
   a 30-day login token in the settings file. A launcher that never signed in
   on this PC opens the window by itself, on *create account*, once a run when
   it first connects (not with `--user`, `--local-test` or `--extract`).
3. **Game selector** (Deadly Alliance / Deception / Armageddon) switches every
   page; modes and rosters come from `launcher/data/<game>.json`.
4. **Ranked / Kasual** (Kasual is the casual queue, `casual` in the protocol and
   the settings): pick the mode and fighter, *Find match*. With *Pick my
   fighter on the game's own select screen* (`[online] pick_in_game`, on by
   default) the launcher sends no fighter and the player picks in the game when
   the match starts (docs/PROTOCOL.md 3.3). The game is
   started in the background and boots to its hold point while you search.
   When a match is found you have 20 s to accept (the MATCH FOUND window stays
   on top, and takes the front back when a game window opens over it: the
   match's game starting the moment a waiting player's search began did); then
   the launcher punches
   the opponent on the game port, measures the ping, writes the `start` line
   into the game's `cmd.txt`, reports the result the game writes to
   `status.txt`, and parks the game again. *Your record* shows the wins and
   losses of that game and mode (and the Elo in Ranked). After the match a
   series card shows the score against that opponent (docs/PROTOCOL.md
   1.9.1): Kasual offers *Rematch* (the same fighters, when both press it),
   *Select* (both pick again on the select screen) and *Leave* for 60 s; the
   game reports the fighters with its result. A ranked set (first
   to 2) asks for its next match by itself once the game is back at its hold
   point, and *Leave set* asks first when leaving would lose the set.
5. **Lobby**: create (optional password, best of 1/3/5, death traps), join from
   the list or by 5-letter code, pick a fighter (or *Pick in the game*), chat,
   *Ready*: both players ready start the match after the server's 3-2-1
   countdown (`starts_in`, shown under the controls; no START). The host
   switches the lobby's death traps between matches (the box under its name,
   `lobby_settings`; both ready again). *Watch selected* (or *Watch* in the
   lobby) makes you a spectator: each lobby match plays in your game from both
   players' feeds (docs/PROTOCOL.md 1.10.1; `FeedLink` passes them between game
   and server: a player's game writes to a TCP port of the launcher's on
   127.0.0.1, a spectator's reads from one). A watched match that ends early
   (`spectate_end` not `agreed`) is dropped from what waits to be watched, and
   one being set up parks its game after 15 s if the game is not in it (the
   players' games may never have met: the watching game would wait for that
   meeting for two minutes); a game reports `starting` once it took a start, so
   the next one is never sent into it. A spectator whose connection drops keeps
   watching: the server puts them back in the lobby at the next login within
   60 s and sends the feed again (the page waits 70 s before it stops the
   watching). *Play* takes a free place again.
6. **Single player**: *PLAY* starts the game plainly (no `MK3D_NET`), so it
   runs its own menus as on the console, its practice mode included;
   *PRACTICE* starts it with `MK3D_TRAINING=1` (below), straight to practice's
   character select with every fighter, costume and arena unlocked (each
   game's autopilot sets the built-in masks, which every unlock check ORs in
   and nothing saves, as it does in every launcher match);
   *Close the game* closes its window as the player would. One game runs at a time,
   but a queue search can run alongside it (standby): no match game boots
   while it is open (one that was waiting is frozen), the match-found prompt
   comes up over it, and accepting freezes it (`winutil::freezeProcess`: its
   windows hidden, then `NtSuspendProcess` on it and any process it started)
   and starts or thaws the match's game. Once nothing more is to be played
   online (the match, its rematches or its set are over, or it was cancelled
   before it started) the match's game is frozen at its hold point and single
   player is thawed and brought to the front, where it stood (runtime patch
   0020 keeps the frozen minutes out of its clock). Lobby matches still wait
   for single player to close. *Practice while you wait* (Ranked and Kasual,
   `cfg::practiceWhileWaiting`, on by default): once a search's match game is
   at its hold point it is frozen there and the game starts again for practice
   (`MK3D_TRAINING=1`: the autopilot takes it to practice's character select,
   only player 1 taking part, and hands it over), so accepting a match thaws a
   game that is ready at once; standby does the rest. In a source build the **Local test** box
   starts two instances on this PC (UDP 7001/7002) with matching start lines,
   no server needed; a player package hides it.
7. **Leaderboard**: top 100 by Elo per game and mode.
8. **Build game**: pick your disc image (`.iso` / `.xiso.iso`), *EXTRACT AND
   BUILD*. The launcher reads the image's Xbox file system itself
   (`src/Xiso.cpp`, the reader `tools/py/xiso.py` also is: an XISO or a whole
   disc dump, no Python), checks the disc's `default.xbe` against the release
   the build is made for (`xbe_sha1` in `launcher/data/<game>.json`) and the
   free space, extracts into `<game
   folder>/assets` on a worker thread with a progress bar, then starts the
   game once so it compiles its code library (*Launch once to compile* does
   that again by hand). Before any game start, a player package's
   `toolchain\` (clang and lld, kept once) is hard-linked into the game's
   compiler folder, and a game without its files is not started. Another
   release (`Game::otherRelease`: the xbe's header time stamp and certificate
   title ID, region and version against `xbe_time` / `xbe_version` /
   `title_id`) is named for the player (an earlier or later print, the PAL or
   Japanese release, another game) and neither extracted nor started: its code
   is laid out differently, so the game would crash. A modified copy of the
   right release (the same header time stamp) asks before extracting and
   starts.
9. **Controls**: which keys press each button of the original Xbox controller
   (two at most; click one and press the new key, Esc cancels, *No key*
   clears it; a key given to another button leaves the one that had it) and
   what each button of a PC controller presses, named by where it sits (the
   bottom face button is Xbox A, PlayStation cross, Switch B). The Xbox
   buttons are named by what they do in the selected game's default layout,
   then the button (`controls::controlText`: "Attack 1 (X)", "Block (Right
   trigger)", Deadly Alliance's "Throw / special (Black button)"), listed
   moves, attacks 1 to 4, block, stance change, throw, the rest. Saved at each
   change to `controls.toml` beside the settings file; the launcher hands its
   path to every game it starts (`XBE_CONTROLS`), and a running game reads it
   again within a second (runtime patch 0016). Without the file the games keep
   the layout of patch 0013.

Files next to the exe (or `%LOCALAPPDATA%\MK3D\MK3D Online` when that folder
is read-only): `mk3d-launcher.ini`, `controls.toml`, `mk3d-launcher.log` and
`sessions\`, one folder per game process with its `cmd.txt`, `status.txt`,
`game.log` and a `net-<match>.log` for each online match (every frame's
inputs); the newest 30 sessions are kept.

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

A controller steers the launcher (`src/GamepadNav.cpp`, SDL3 static at the
games' version and hints, so the same pads work): only while the foreground
window is one of the launcher's, since the games read the controllers in the
background too. The d-pad or left stick moves the focus to the nearest control
that way (a list or table moves its own current item first, to its edge), A
clicks (a combo box opens its list, which then takes up / down / A / B), B is
Esc in a dialog except MATCH FOUND and the rematch prompt
(`GamepadNav::keepFromBack`), LB / RB step the pages, START presses FIND MATCH
on a queue page and Ready on the Lobby page (`LobbyPage::pressReady`); after
every lobby match the main window comes back in front on the Lobby page with
Ready focused (`MainWindow`, on the phase going back to Idle from a match).
Buttons held when a launcher window comes to the front (a
prompt over the game) count only once let go, after 250 ms of quiet. While a
match is connecting, played, reported or watched (`MatchController::inMatch`)
the controller does nothing in the launcher at all (`GamepadNav::paused`): a
launcher left as the foreground window by a game that could not take the focus
used to act on a fight's presses, and a player's buttons once reached the
account menu's Sign out mid-match (Sign out is disabled during a match too). The focus
shows gold (`Style.cpp`). `MK3D_PAD_ANY_WINDOW=1` takes any active launcher
window as in front, for tests on Qt's offscreen platform with an SDL virtual
gamepad.

**Updates** (from 0.1.7, `src/Updater.cpp`). The server names the latest
release (PROTOCOL.md 1.3: `welcome.update`, an `update` push, the
`client_outdated` reply): a player package offers it in a window (UPDATE NOW,
LATER, the notes and a link to the release page; after LATER a footer button
brings it back), every time when the server turns this launcher away. UPDATE
NOW downloads the zip into `update\` beside the launcher, checks its size and
SHA-256 against the server's (whose certificate is pinned; the download host is
not trusted), unpacks it with Windows' `tar` (PowerShell's `Expand-Archive` if
that fails) and checks its `VERSION.txt`. It goes in once nothing uses the
games: no match, search or watched match, no single-player game open (the game
waiting at its hold point is closed). Each file the release has is renamed to
`<name>.mk3d-old` (a running program's file can be renamed, not replaced) and
the new one moved in; a failure moves every file back and says which file
stopped it. Then the new launcher starts with `--updated <old version>
--config <the same settings>` and this one closes; the new one waits for the
settings' instance lock, says what it is now, and deletes the files listed in
`update\old-files.txt` once they are free (at every start, retried for 20 s
after an update). Only the release's files change: settings, saves, sessions,
the disc images and the games' built code stay. At every start the launcher
looks for games whose code no longer matches their executable
(`GameProcess::codeStale`: `compiler\work\<EXE>\built.txt` against the
executable's size and time) and has each build it again, one at a time, with
the game's `--prepare` (its own progress window, no Start page; exit code 0
once ready), so the next match or single player finds it ready; meanwhile that
game does not start (`MatchController::prepareStaleCode`). A game the launcher
starts for online play goes on by itself once its code is built
(`--autostart`). A source build, or a folder the launcher cannot write to, only
links to the release page.

Making a release that installs itself: `tools\package.ps1 -Build`, publish
the zip on GitHub as `MK3D-Online-<version>-windows.zip` under the tag
`v<version>`, then `tools\deploy-server.ps1 ... -Release <the zip> -Notes "..."`
(add `-ReleaseOnly` to leave the server running as it is): it checks that the
link serves that zip and writes the server's `update.json`.

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
| `--rematches N` | with `--queue casual`: choose a rematch after each match, N times (a ranked set plays on by itself) |
| `--rematch-choice rematch,select` | what each of those choices is: `rematch` (the same fighters) or `select` (both pick again); the last one repeats (default `rematch`; `tools/online-e2e.ps1 -RematchChoice`) |
| `--leave-series` | once those are chosen, leave the series at once instead of letting its time run out |
| `--solo` | with `--queue`: start single player first and search alongside it, standby (`tools/online-e2e.ps1 -StandbyA`) |
| `--practice on\|off` | practice while a search waits, saved like the queue pages' box (`tools/online-e2e.ps1 -PracticeA` writes it into launcher A's ini) |
| `--lobby host\|join\|watch --lobby-name NAME --game mkd --mode kombat --character Scorpion` | host a lobby of that name and mark ready (its match starts by itself once both are), join it by name and mark ready, or watch it as a spectator (`tools/online-e2e.ps1 -Watch`) |
| `--death-traps on\|off` | the arenas' death traps this player searches for or hosts, saved like the Kasual page's box (`tools/online-e2e.ps1 -DeathTraps`) |
| `--page ranked\|casual\|lobby\|single\|leaderboard\|build\|controls\|settings` | start page (`practice`, its old name, opens `single`) |
| `--local-test` | start the two-instance loopback test |
| `--extract <image>` | open *Build game* and extract that image |
| `--auto-update` | install an update the server offers at once, without asking (tests) |
| `--updated <version>` | given by an update to the launcher it starts: wait for the old one to close, then delete what it set aside |

A complete local round trip: start a server on 127.0.0.1, run the launcher
with `--user tester1 --register --queue casual --auto-accept 3` and the stand-in
game, and `python -m server.client play --name tester2 --register --udp-port
7002` as the opponent. With real games, `tools\online-e2e.ps1` runs the whole
chain on one PC: the server, two launchers and the two games they start
(`-Game mkd|mka|mkda`, `-Mode kombat|chess|puzzle|motor`, `-Queue
casual|ranked|lobby`; ranked starts the server with `--allow-same-ip` and runs
until the set is over; `-Rematches N` plays a casual series;
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
- Addresses: the launcher never shows or logs another player's address (or its
  own public one); the game's start line in the log says `remote=(hidden)`. A
  direct match still lets the two PCs see each other's address at the network
  level; *Hide my IP address* (or a server run with `--relay-only`) relays the
  match so neither does.
- Results are self-reported: agreement counts, disagreement voids the match,
  a single report counts for nothing, and two colluding accounts can trade
  wins. Closing the game during a match loses a ranked set (PROTOCOL.md 1.9);
  a network failure between the two games voids the match.
- Elo per game and mode (K = 32), no per-character ratings, seasons, rank
  tiers or match history page; the launcher shows the top 100.
- Lobbies hold two players and up to eight spectators; no replays, blocks or
  reports; chat is limited only by length and rate. Queue matches have no
  spectators.
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
- The launcher is Windows only and not code-signed (Windows warns about it at
  first start), updates itself only from 0.1.7 on (Updates, above), and does
  not clean up old `sessions\` folders. Binding the game port on all
  interfaces makes Windows Firewall ask once whether the launcher may accept
  connections; allow it for private (and, if you play over the internet,
  public) networks.
