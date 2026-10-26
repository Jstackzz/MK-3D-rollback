# MK3D Rollback

Native PC ports with rollback netplay for the original Xbox versions of the
"3D era" Mortal Kombat fighters:

| Game | Title ID | XDK build | Online in the original |
|---|---|---|---|
| Mortal Kombat: Deadly Alliance (USA, Rev 1) | 4D57000C | 1.0.4721 | none |
| Mortal Kombat: Deception (USA) | 4D570019 | 1.0.5849 | Xbox Live, System Link |
| Mortal Kombat: Armageddon (USA) | 4D570034 | 1.0.5849 | Xbox Live, System Link |

The target is the shape of Ekspym's Tekken 6 Native Online (October 2026):
a native Windows build the player compiles from their own disc, a launcher
with ranked, casual, lobby and practice modes, and rollback netplay. Tekken 6
got there by static recompilation of the Xbox 360 build with ReXGlue. The
original Xbox now has the same kind of toolchain: TGP482's XboxXBERecomp has
shipped native ports of BLACK, Splinter Cell: Double Agent and 007: From
Russia With Love, and sp00nznet's xboxrecomp is a second, MIT-licensed
toolkit. Since the Xbox is an x86 machine with the SDK statically linked in
well-separated sections, the game code lifts to C++ directly and the SDK
(Direct3D 8, DirectSound, XAPI, kernel) is replaced at API level. That is
why the Xbox versions were chosen over the PS2 ones.

- **Native track (primary):** XboxXBERecomp pipeline on Deception first
  (XDK 5849 signatures also cover Armageddon), then rollback via GGPO or
  GekkoNet, then the launcher and builder modelled on Tekken 6 Native Online.
- **Emulator track (secondary):** xemu or Cxbx-Reloaded as the reference
  oracle and, if ever needed, a rollback fork.

Nothing from the games is distributed. You need your own disc images; see
[docs/TARGETS.md](docs/TARGETS.md) for the builds identified from the dumps on
this machine.

## Play online

**Download the player package from the
[Releases page](https://github.com/Jstackzz/MK-3D-rollback/releases)**: unzip it,
start `MK3DLauncher.exe`, build each game from your own disc on the *Build game*
page, and play. It connects to the official server by itself; nothing from the
games is in the download. Instructions: [docs/PLAYING.md](docs/PLAYING.md).

`tools\package.ps1` makes the player package: a zip with the launcher and the
three games that a friend unpacks and plays from, without Python or this
repository; each player builds the games from their own discs on the
launcher's *Build game* page. [docs/HOSTING.md](docs/HOSTING.md) covers running
the server and making the package, [docs/PLAYING.md](docs/PLAYING.md) (the
package's `READ ME FIRST.txt`) what a player does with it.

## Layout

```
docs/            plan, target identities, toolchain setup, prior art
docs/research/   surveys of the toolkits and of the Tekken 6 projects
tools/py/        xiso.py (disc image -> files), xbe_info.py (XBE identity)
tools/           toolchain helpers (vsenv.cmd, build-deps.ps1, setup-windows.ps1)
recomp/          per-game recompiler projects (XboxXBERecomp game repos)   [primary]
emu/             emulator-side work, if any                                 [secondary]
research/        per-build identity, symbol maps, engine notes
orig/            your own extracted game files (git-ignored)
external/        toolkits: XboxXBERecomp, xboxrecomp, XbSymbolDatabase, GekkoNet, ggpo, ghidra (git-ignored)
reference/       study-only clones: Cxbx-Reloaded, xemu, Tekken 6 sources, Halo decomps (git-ignored)
```

## Quick start

```bash
python tools/py/xiso.py "path/to/Mortal Kombat - Deception (USA).xiso.iso" --extract default.xbe --out orig/mkd
python tools/py/xbe_info.py orig/mkd/default.xbe --json research/mkd/xbe_info.json
```

Read [docs/ROADMAP.md](docs/ROADMAP.md) for the milestones and
[docs/SETUP.md](docs/SETUP.md) for the toolchain.

## Controls

Any controller SDL knows (Xbox, PlayStation, Switch pads, most fight sticks)
plays as the original Xbox controller; the first one plugged in is player 1.
The keyboard also plays as player 1 while the game's window is in front. The
defaults:

| keys | Xbox controller |
|---|---|
| arrows or W A S D | d-pad |
| J, I, K, L | X, Y, A, B (the four attacks) |
| Space | right trigger (block) |
| O | left trigger |
| U, P | white, black |
| Enter, Backspace | START, BACK |

LB and RB stand in for the Xbox's white and black buttons. The launcher's
*Controls* page changes both: which keys press each button, and what each
button of a controller presses (saved in `controls.toml`, which a running game
reads again within a second). `XBE_KEYBOARD=0` turns the keyboard off. In
online matches START and BACK are not sent, so one player cannot pause the
other's game.

## License

Project code: MIT, matching XboxXBERecomp and xboxrecomp (GGPO is MIT,
GekkoNet BSD-2-Clause; pieces borrowed from xemu are LGPL-2.1 and stay so).
No game code or assets are, or ever will be, part of this repository.
