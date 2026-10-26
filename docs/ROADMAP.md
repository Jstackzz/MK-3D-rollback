# Roadmap (v3, re-planned 2026-10-24 for the original Xbox versions)

## The goal

"MK Deception Online": a native Windows build of Mortal Kombat: Deception
that the player builds from their own disc, launched from a Tekken 6 Native
Online style launcher (ranked, casual, lobby, practice, leaderboard,
settings, "build the game first"), with rollback netplay. Then Armageddon
and Deadly Alliance on the same pipeline.

## Status (2026-10-25)

| milestone | Deception | Armageddon | Deadly Alliance |
|---|---|---|---|
| N2-N4 native build, arcade and versus playable | done | done | done |
| N5 rollback (sync test, two-game netplay) | done | done | done |
| N6 launcher: casual, ranked, lobby | done (versus, Chess, Puzzle Kombat) | done (versus, Motor Kombat) | done (versus) |
| N7 quality | keyboard play, START/BACK kept out of online matches | START/BACK kept out | START/BACK kept out |
| player package (no Python, no repository) | built and played (casual, lobby) from a fresh unpack | built and played (casual) from a fresh unpack | built and played (casual) from a fresh unpack |

Netplay, rollback and the launcher link are shared by the three games
(`recomp/common`); each game adds its own adapter (`recomp/common/README.md`).
Per-game details: `research/mkd/notes.md`, `research/mka/notes.md`,
`research/mkda-rev1/notes.md`. The player package (`tools/package.ps1`,
`docs/HOSTING.md`, `docs/PLAYING.md`) carries the launcher and the three games
without game data; the launcher extracts the player's disc itself, and the
math the games' state depends on is built into the executables (runtime patch
0015) so different PCs compute the same bits. Open beyond the table: a match
between two PCs over the internet, rematches for queue matches, in-game team
picks for Chess Kombat.

## Why Xbox is the easier platform for this goal

- The CPU is x86. The game's code is lifted to C++ by Zydis-based
  recompilers that already produced playable ports (XboxXBERecomp: BLACK,
  Splinter Cell: Double Agent, 007). There is no instruction-set gap to
  close, no vector-unit microcode, and Ghidra decompiles x86 well.
- The SDK is statically linked but sits in its own sections (D3D, D3DX,
  XGRPH, DSOUND, XNET, XONLINE, XPP), identified by per-XDK-build
  signatures, and gets replaced at API level by the recompiler's runtime
  (Direct3D 8 on Direct3D 12, DirectSound on XAudio2, XAPI, kernel).
- 64 MB of guest memory in a flat arena inside a 64-bit host process is a
  small, simple thing to snapshot for rollback.
- Deception and Armageddon ship their own online code (Xbox Live, System
  Link), the same starting position Ekspym had with Tekken 6's RPCN netcode.

The honest counterweight: the NV2A GPU's register combiners and vertex
programs, the MCPX DSP program the MK games carry (`dspimage`, Dolby
encoding), and Midway's engine specifics are real work, and the toolchains
are weeks old. Signature availability for XDK 4721 and 5849 is the first
thing to confirm.

## Native track (primary)

### N0: bootstrap (done 2026-10-24)

- All three games identified from the owner's dumps (`docs/TARGETS.md`):
  Deception 4D570019 and Armageddon 4D570034 on XDK 5849, Deadly Alliance
  Rev 1 4D57000C on XDK 4721. Tools: `tools/py/xiso.py`, `tools/py/xbe_info.py`.
- Toolchain: VS 2026 Build Tools, CMake, Ninja, LLVM (Clang + LLD) for the
  recompiler output, Ghidra 12.1.4 (x86 support is built in).
- Checkouts: XboxXBERecomp, xboxrecomp, XbSymbolDatabase (toolkits);
  Cxbx-Reloaded, xemu, Halo decompilations, Tekken 6 sources (reference).
- Prior art: `docs/research/`.

### N1: symbols and engine map of Deception

- XboxXBERecomp ships XDK layouts and signatures for 4831 and 5849 only, so
  Deception and Armageddon scan directly; Deadly Alliance (4721) needs a set
  authored from its own XBE (copy the 4831 layout, scan, fix misses in
  Ghidra, then `xbe_symbols --sign`). Cross-check matches against
  XbSymbolDatabase, which already named 380 XDK symbols in Deception
  (`research/mkd/symbols-xbsdb.txt`).
- Known traps for these titles: the executable-flagged shader sections
  (`PSGSFD*` in Deception, `PSFD*` in Armageddon) must be dropped from
  `code_sections`; XONLINE/XVOICE have no host side (override on the boot
  path, bypass for netplay); `dspimage` feeds the SDK's effects-image
  decoder, Dolby is stereo-only; `BeginPush/EndPush` have no 5849 signature.
- Export a flat PE (`xbe2pe.py`) for Ghidra, import, name the SDK surface,
  then find the main loop and frame tick, the pad read (XInput through XAPI),
  the fight-state globals, RNG, the Present call, the sound playback entry,
  and the online layer's entry points.
- Record everything in `research/mkd/` keyed by XBE SHA-1.

### N2: recompiler bring-up (decision gate)

Status 2026-10-24: `xbe_symbols` matched 176 of 243 XDK functions and the
frame swap on Deception; `xbe_recomp` lifted 19,271 functions / 1.08 M
instructions in 10 s. The full native build completed the same evening after one SDK patch
(`recomp/patches/`); the native `MKD.exe` boots, brings up D3D12 and runs
the lifted game code for half a second before its first engine-level fault
(a NULL object in startup). The bring-up loop then fixed the assets layout, added the missing APC
dispatcher export, and replaced the engine's stack-copying coroutine
scheduler with host fibers (`recomp/mkd/src/game/`); the game now loads its
startup assets and runs its main loop before the next fault. Running notes:
`research/mkd/notes.md`.

- Create `recomp/mkd` from XboxXBERecomp's game template, lift, build with
  Clang, run. Target: title screen, then a VS fight with the stock renderer.
- Measure: build time, lifted function count, unmatched SDK stubs that trap,
  frame time.
- Gate: if the lift is correct but the runtime lacks something MK needs
  (DSP image, a D3D8 feature, XOnline), fill it in the runtime; if the lift
  itself cannot be made correct, try xboxrecomp's hardware-level path for the
  broken piece or fall back to a matching decompilation of that code.

### N3: runtime completion for this game

Everything the stock runtime traps on: Dolby/DSP (likely stub to stereo),
specific D3D8 usage (render targets, palettes, pixel shader combiners in the
`PSGSFD` sections), DirectSound mix bins and effects, memory card and
profile saves, XOnline and XVoice (stub or bypass), FMV.

### N4: playable native build

Full arcade mode at 60 fps, VS mode, saves. First thing worth showing anyone.

### N5: rollback

The recompiled game owns a flat guest arena; snapshot = arena copy plus
runtime device state (D3D8 state, DSound voices, kernel objects, timers),
then dirty pages. Frame boundary at the main loop head or at Present. Inputs
are the 16-byte XInput-style pad report, exactly what the Tekken 6 fork
uses. GGPO (Ekspym's fork) or GekkoNet; re-simulated frames skip rendering
and sound starts; determinism harness and stress sessions from day one.
Host threads created by guest `CreateThread` are the main risk: the sound
and streaming threads must be either deterministic in lockstep with the
frame or excluded like Tekken's draw/sound threads.

### N6: launcher and builder

Tekken 6 Native Online shell with renames: Qt 6 launcher, `status.txt` /
`cmd.txt` game protocol, hold point at stage select, server message set
(`hello/quick/lobby/start/peer/ping_report/go/result`) over TLS with a
pinned certificate, UDP rendezvous, SQLite. Builder: the player's disc image
in, `xiso.py` extraction, `xbe_symbols`, lift, Clang, link; nothing from the
game in the repo. Casual quick match, private lobby and practice first;
ranked, MMR and leaderboards once the server exists.

### N7: quality

Internal resolution and widescreen, practice-mode frame data, HUD, updater.

### N8: the other two games

Armageddon next (same XDK, same engine lineage), then Deadly Alliance
(XDK 4721 signatures, oldest engine, no online code of its own).

## Emulator track (secondary)

xemu as the reference oracle (savestates for memory analysis, GPU traces,
behaviour comparison) and Cxbx-Reloaded's HLE semantics as documentation of
what each SDK call must do. A rollback fork of either is possible in
principle but is not planned while the native track is moving.

## Cross-cutting

- Determinism tests on every change: replay-driven, checksum-compared.
- No game data in the repo. Players build from their own discs.
- Every address documented with the XBE SHA-1 it applies to.
- Licensing: toolkits MIT, xemu-derived pieces LGPL-2.1, GGPO MIT,
  GekkoNet BSD-2. Keep this project MIT so builder and launcher can ship.
