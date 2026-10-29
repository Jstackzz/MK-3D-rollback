# recomp/common: what the three games share

The netplay stack built on Mortal Kombat: Deception, made game-neutral: Deadly Alliance and Armageddon plug in by
implementing a handful of adapter functions instead of copying it. Everything here is compiled into each game's
executable (`src/mk3d/*.cpp`, added by the game's `CMakeLists.txt`) and lives in namespace `mk3d`.

| file | what |
|---|---|
| `mk3d/sfiber.h/.cpp` | snapshot-able fibers: cooperative contexts whose parked stacks can be saved and put back |
| `mk3d/gclock.h/.cpp` | the deterministic guest clock (a function of the simulated frame on the main thread) |
| `mk3d/rollback.h/.cpp` | copy-on-write page tracking: put the whole simulation back to the top of an earlier frame |
| `mk3d/netplay.h/.cpp` | two-player netplay over UDP: the meeting point, prediction and rollback, lockstep while loading, time sync, state checks, the per-frame log |
| `mk3d/link.h/.cpp` | the launcher protocol (`docs/PROTOCOL.md` section 3): cmd.txt / status.txt, start, park, rematch, results |
| `mk3d/synctest.cpp` | `<prefix>_SYNCTEST=1`: from a match's first frame, every frame is run, rolled back and run again; both runs must write the same |
| `mk3d/monitor.cpp` | `<prefix>_MONITOR=<port>`: the debug monitor (`tools/py/mkdbg.py`): read and write memory, dump, pause and step, hold pad buttons, call game functions |
| `mk3d/framedata.h/.cpp` | frame data in practice: player 1's last attack (startup, active, recovery, on hit / on block) as an ImGui panel, F7 / `MK3D_FRAMEDATA`; F8 makes the dummy block; never online |
| `mk3d/names.h/.cpp` | the players' account names in an online or a watched fight (the start line's `p1_name` / `p2_name`), inside the health bars beside the timer while the fight's HUD is up, fading with the game's screen fades |
| `mk3d/game.cpp` | `Env()`: environment variables under the game's prefix |

## What a game provides

| header | implemented in the game's | what |
|---|---|---|
| `mk3d/game.h` | `game.cpp`, `frame.cpp`, `diag.cpp`, `pads.cpp` | `EnvPrefix()` ("MKD", "MKA", "MKDA"); the frame loop as hooks (`Frame`, `MainContext`, `OnFrameStart`, `OnPadsRead`); `DescribeCode` for diagnostics; `ParseButtons` / `InjectPad` |
| `mk3d/tasks.h` | `task_fibers.cpp` | the engine's tasks on `sfiber`: `Save`, `Load`, `Alive` (rollback snapshots them every frame) |
| `mk3d/netgame.h` | `netgame.cpp` | the per-port input blocks, the meeting point's words and agreement (frame counters, a side mode's setup), reseeding the engine's random generator, the match's start and end, the state hash, the values the launcher reports, state switches, the loader, the per-site random streams, log fields and diagnostic blocks |
| `mk3d/linkgame.h` | `linkgame.cpp` | the autopilot's front end driving (hold point, start, rematch, park), matches started, and a match's result per mode |
| `mk3d/framedata.h` | `framedata.cpp`, `game_info.cpp` | where the fighters are and the game's own rule for starting a move (an `Adapter`), its hit test calling `ActiveCheck`, its "is this action held" function asking `DummyHolds`; `GameInfo` `.window`, `.ui`, `.message` |
| `mk3d/names.h` | `names.cpp`, `game_info.cpp` | whether the fight's HUD is up, where its bars are and how far a screen fade has gone (an `Adapter`; `PlaceInBars` reads Deception's and Armageddon's bar quads, `ShadeUnder` finds a game's fade box in its object lists); `GameInfo` `.window`, `.ui` |

Fixed byte ranges another thread owns (a graphics device's vertical blank fields) are named once with
`rb::AddFixedVolatile` (Deception: `src/game/game.cpp`). A game's rand hook calls `net::CountRand`.

Deception (`recomp/mkd/src/game`) is the reference: `netgame.cpp`, `linkgame.cpp`, `game.cpp`, and the forwarding
headers `rollback.h`, `gclock.h`, `task_fibers.h`, `netplay.h`, `launcher_link.h` that keep its own names
(`mkd::rb`, `mkd::net`, ...) working.

## Environment

`<prefix>_NET_BIND`, `_PEER`, `_SIDE`, `_DELAY`, `_MODE`, `_LOG`, `_LAG`, `_JITTER`, `_LOSS` configure netplay for
tests (`netplay.h`); `_NET_DUMP`, `_NET_MEMDUMP`, `_NET_SIDEDUMP=<frame>` and `_NET_DATA=1` add diagnostics to the
per-frame log, which `tools/py/rbcmp.py` compares between two games. The launcher sets `MK3D_NET=1`, `MK3D_CMD` and
`MK3D_STATUS` for the link. `<prefix>_TEST_NAMES=Kitana|Sindel` shows those two names over any fight, an offline one
too (screenshots of the names' place).
