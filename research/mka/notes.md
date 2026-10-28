# mka (MK Armageddon, USA, 4D570034) running notes

Every address here is for XBE sha1 24074b8f58a0c10f2dee6862be3ebd267ac1e164 (6,033,408 bytes, XDK 1.0.5849,
internal name mk7, `c:\games\mk7xbox\mk7\Code\mk7xbox_Release\mk7.exe`, entry 0x2AAC10, kernel thunk 0x4D77E0).
The bring-up follows Deception's (`research/mkd/notes.md`); the engine is the same lineage, at other addresses.

## 2026-10-25: project setup

- `recomp/mka/` copied from `recomp/mkd/` (CMakeLists, presets, build/release.bat, .gitignore, .gitattributes),
  target renamed MKA, `sdk` junction to `external/XboxXBERecomp`. `game_info.cpp`: "Mortal Kombat: Armageddon",
  short title and exe MKA.
- Disc: the whole XISO extracted into `recomp/mka/assets/` (`tools/py/xiso.py --extract-all`, 981 files, 4.6 GB:
  `default.xbe`, `art/`, `moviexbox/` (+ `krypt/`), `sndsxbox/`, `update.xbe`, `dashupdate.xbe`).
  `assets/default.xbe` sha1 checked.
- Configure offline: the four FetchContent sources (zydis, toml++, imgui, SDL3) copied from Deception's build tree
  into `recomp/mka/out/deps-src/` and passed as `FETCHCONTENT_SOURCE_DIR_*` to `cmake --preset debug`.
- `xbe_symbols assets/default.xbe sdk/xdk --library MKA.dll --out config/game.toml`: 231 of 243 functions and 12 of
  12 data (Armageddon has an `.XTLID` section, which Deception lacks, so 20 DirectSound names come from it).
  Misses: the Bink group (no Bink here), `D3DDevice_EnableOverlay`, `crt_log`, and five ambiguous or missing names
  filled from `symbols-xbsdb.txt` (marked `# from XbSymbolDatabase`):
  `CDirectSoundStream_AddRef` 0x4A9519 / `_Release` 0x4A9560 (the stream vtable 0x51E268 confirms both),
  `D3DDevice_BeginPush` 0x412410 (`BeginPush_4`), `D3DDevice_EndPush` 0x412430, `D3DDevice_FlushVertexCache`
  0x40CE50, `D3DDevice_SetPalette` 0x40CB00. Names `xbe_symbols` does not know but the runtime implements (patches
  0004/0005): `D3DDevice_SetDepthClipPlanes` 0x408710, `IDirectSoundBuffer_SetNotificationPositions` 0x4AA867.
  `[data] D3D_FvfShader = 0x42A1D0`: the device init stores it at CDevice+0x794 at 0x417CAB
  (`mov dword ptr [ebx+0x794], 0x42A1D0`; Deception 0x2EFBEB). `XPP_GamepadType` 0x4CE334 was found directly and
  agrees with XbSymbolDatabase's `g_DeviceType_Gamepad`.
- The socket names (`socket` 0x48FEE5 etc.) differ from XbSymbolDatabase's (0x492F9F etc.): the toolkit binds the
  thin public wrappers, as in Deception; kept.
- `code_sections` as written, PSFD00/_I/_B/_P included (Deception's PSGSFD* are CRI Sofdec code).
- First lift: 30,836 functions, 1,456,595 instructions, 4,669,291 of 5,003,232 code bytes, 551 jump tables;
  the `unimplemented` list is data decoded as code by the gap fill, as in Deception. Full debug build ~3 min.

## Engine map (Deception address -> Armageddon address)

| what | Deception | Armageddon |
|---|---|---|
| `_setjmp3` | 0x2993E8 | 0x3995EC |
| main's `_setjmp(buf)` call / continuation | 0xDFFBA / 0xDFFBF (buf 0x46CCC0) | 0x19C85E / 0x19C863 (buf 0x5B8848) |
| task exit | 0x13C0B0 | 0x1C5010 |
| class C Run / Yield (int3) / SetFunction | 0x13C130 / 0x13C1E0 / 0x13C430 | 0x1C50A0 / 0x1C5150 / 0x1C5180 |
| class A Run / Yield / SetFunction | 0x13C1F0 / 0x13C360 / 0x13C430 | 0x1C51C0 / 0x1C5340 / 0x1C5430 |
| class B Run / Yield / SetFunction | 0x13C470 / 0x13C5A0 / 0x13C650 | 0x1C5470 / 0x1C55A0 / 0x1C5650 |
| B stack hops | 0x13C610 / 0x13C630 | 0x1C5610 / 0x1C5630 |
| vtables C / A / B | 0x3F5E48 / 0x3F5E70 / 0x3F5E98 | 0x55DB5C / 0x55DB84 / 0x55DBAC + 0x55DBD4 |
| destroy slot | vtable+0x10 | vtable+0x0C (one slot fewer in front; run +0x10, yield +0x14, set function +0x20) |
| current task / wait time / destroy flag | 0x5FF3D4 / 0x5FF3D0 / 0x5FF3C8 | 0x7350C0 / 0x7350C4 / 0x7350B4 |
| scheduler ret / esp / ebp / ss | 0x46DDAC / BC / CC / D8 | 0x5B8B40 / 50 / 60 / 6C |
| last yield ret / hop esp / SetFunction time | 0x46DDA8 / C0 / A4 | 0x5B8B3C / 54 / 38 |
| task system init | 0x13B6D0 | 0x1C43C0 |
| pad read, per-port block | 0x1C52F0, 0x62109C + port * 0x1C | 0x26A9C0, 0x831C0C + port * 0x20 (same fields: flags +0, current +0x10, new +0x14) |
| left stick to directions (in the pad read, before it reads the sticks +0x18 afresh: last frame's stick; online that was the left player's on port 0, see research/mkd/notes.md, so the netplay restores port 0's sticks first) | sub_0006AC90 (flag 4) | sub_0007B9F0 (flag 8) |
| game options block (rounds to win, round time, blood level, death traps; 0.1.7) | 0x5FC980, see research/mkd/notes.md | 0x723BA0 (0x64 bytes): 0x723BC0, 0x723BC4 (90 s), 0x723BC8 (3), 0x723BCC (1); defaults sub_00236320, one item sub_00236150 (0x332F rounds, 0x3331 time, 0x3332 traps, 0x3333 blood), block copy sub_00235AF0(dst, src), a profile's made the game's by sub_00235B60 (0x736560 + profile * 0x1AFC0) |
| death traps work (0 when the blood level is 0; the arenas' scripts call it through their natives table) | sub_001BF070 (blood sub_001BF010, table 0x6597E8) | sub_0025D2D0 (blood sub_0025D280, table 0x965270) |
| CRI SVM_Lock / SVM_Unlock | 0x2427D0 / 0x242A90 | 0x323C50 / 0x324190 |
| CRI "locked by me" (priority 15) / force unlock loop | 0x24BB10 / 0x24BB20 | 0x331B90 / 0x331BA0 |

Armageddon's run and exit code also checks the current task pointer for NULL before the destroy call.

The CRI build differs: Armageddon's ADX init (0x320AA0) registers lock callbacks 0x3203B0 / 0x3203D0 that are
plain `RtlEnterCriticalSection` / `RtlLeaveCriticalSection` on 0x944D20, so the SVM lock is already a real mutex
(Deception's raised the thread to priority 15). Its three threads (made at 0x3209C0 with `_beginthreadex` 0x39BCD1):
vsync server 0x3208E0, file system 0x320960, and a lowest-priority idle/server thread 0x3204D0 that suspends itself
each pass (`SuspendThread`) and is resumed by the vsync thread, instead of Deception's spinning idle thread.
So Deception's `cri.cpp` is not ported (nothing to fix so far).

## Bring-up loop

### Fault 1: the pixel shader template scan (0.9 s)

Symptom: access violation reading guest 0xB0102000 in `sub_002E2720` (`repe cmpsd` at 0x2E2772), main thread,
from the shader setup `sub_002725F0` <- `sub_002725C0` <- `sub_0007D460` <- `sub_0019C680`.

Cause: `sub_002E2720` creates a pixel shader from the template definition 0x50C4D8 and then scans the object
`D3DDevice_CreatePixelShader` returned for the copy of the 240-byte definition (offset kept in 0x5C7BF8), then for
the pointer to it (0x5C7BFC); object size + 4 goes to 0x5C7C00, and it builds its own pixel shader objects from that
template (pool at 0x5C7BF4). The XDK's CreatePixelShader (0x410980) allocates 0xFC bytes: refcount 1, flags 1,
+8 the definition pointer, +0xC the definition. The runtime's host CreatePixelShader returns a 16-byte record, so
the scan ran off the end of the page.

Fix (config only): `D3DDevice_CreatePixelShader = 0` and `D3DDevice_DeletePixelShader = 0`, so the library's own
code makes (with `XMemAlloc`) and frees the objects. The runtime's `SetPixelShader` already handles objects it did
not make through the 5849 layout key `PixelShader_Definition = 0x8` (definition pointer at +8).

### Fault 2: a thread notify routine the lift missed (1.0 s)

Symptom: `indirect call to 003A44A2 has no recompiled function (return address 002AA5B5)` on a new thread.

Cause: 0x3A44A2 is the CRT's thread notify routine (create: call [0x5AF7E0]; delete: call [0x5AF7E4] and free the
per-thread data), stored by 0x3A039F into 0x709770 and called by the XAPI thread startup (0x2AA5B3). It directly
follows a noreturn `ExitThread` call in `sub_003A4478`, so the gap fill decoded it as part of that function.

Fix: `[functions] 0x3A44A2 = "crt_thread_notify"`.

### Task system on fibers

`src/game/task_fibers.cpp` is Deception's with the addresses above, the destroy through vtable+0x0C and the NULL
check of the current task before it (`Destroy`), and the class C SetFunction 0x1C5180 added. With it the Midway
logo plays, then a task longjmps to main's continuation 0x19C863 at 12.5 s (`tasks: 00F6E8D0 longjmps to 0019C863`,
the same pattern as Deception) and the opening movie `d:\moviexbox\OPENINGW.SFD` plays (`build/shots/mka-r2-25s.png`).

Missing kernel imports at boot (no caller reached so far; the XAPI routines that use them have no direct callers):
191 NtCreateIoCompletion, 227 NtSetIoCompletion, 223 NtRemoveIoCompletion, 76 IoQueryVolumeInformation,
243 ObOpenObjectByName, 343 XcPKGetKeyLen. 232 NtUserIoApcDispatcher comes from `src/game/kernel_apc.cpp` (copied).

### Fault 3: task functions the lift never decoded (50 s into the first fight)

Symptom (115 s, the first fight Scorpion vs Ashrah running since 63 s, Ashrah nearly beaten):
`indirect call to 00081E50 has no recompiled function (return address FFFFFFF0, esp 00EA61EC)` on the main thread;
ecx = 0x55DBAC (class B vtable), esp on a task stack: the task fiber's `CallGuest` of the task function [task+0x30].

Cause: 0x81E50 (`push ebp; mov ebp, esp` after int3 padding) is pushed as a task function at 22 sites but is never a
direct call target, and pointers inside code sections are no seeds. The gap fill tried it but its walk failed
(`XBE_RECOMP_TRACE_WALK=0x81E50`: `walk 00081E50 fails at 0008205A (line 247)`, a conflict): an earlier bogus decode
(`sub_0008205D` = the int3 byte inside `push 0x3DCCCCCD` at 0x8205A, `sub_0008205E` behind it) covers its bytes,
and an untrusted walk gives up on any conflict.

Search for the same pattern (`push`/`mov` immediates in decoded code pointing at a `55 8B EC` after padding that
is no function, read from the "// ADDRESS insn" comments of `generated/game`) found three more, all walks failing
on bogus decodes the same way: 0x1300B0 and 0x14FA80 (task functions passed to the task creator 0x1C2770) and
0x1FF90 (stored at [obj+0x74]).
No data-section pointer to a missing prologue exists (pointer seeds cover those), and the 19 other `55 8B EC` entries
after padding that are no function are all decoded already as the tail of a function that jumps to them.

Fix: `[functions]` entries for 0x81E50, 0x1300B0, 0x14FA80 and 0x1FF90 (config entries are trusted walks, which keep
both decodes where they overlap). The direct pass then found 1,774 functions instead of 1,316; 30,887 in all.

## Results (2026-10-25)

Scripted path (times from the first pad read, `MKA_PADS=2`, pads hidden for the first 1.5 s as in Deception):
START 13 s (skips the opening movie; the title movie TITLEW.SFD plays), START 19 s, START 25 s (profile screen
"These profiles are loaded"), A 31 s (main menu: Arcade / Versus / Practice), A 37 s (Arcade, character select at
~40 s), A 45 s (Scorpion, ladder), A / START to the fight at ~63 s; from 66 s P1 presses an attack every 1.5 s.

- Midway logo (LOGOW.SFD) at 7 s, opening movie (OPENINGW.SFD) at 12 s, title movie, profile screen, main menu,
  character select, ladder, fight: `build/shots/mka-r3-*.png`, `mka-r4-*.png`.
- r5 (240 s, debug build): fight 1 Scorpion vs Dairou (Armory) from 63 s to 199 s, won (the ladder shows
  "TIME: 02:18"), fight 2 vs Hsu Hao (Prison) loaded at 203 s and ran to the end; no fault, no trap
  (`build/shots/mka-r5-140s.png`, `mka-r5-200s.png`, `mka-r5-238s.png`).
- r6 (390 s planned): fight 1 vs Kabal (Battle Arena) 63-170 s won, fight 2 vs Nitara (Hell) 183-375 s, three rounds;
  then the process ended at 376.9 s with `Exit (0)` from the window thread, i.e. its window got WM_CLOSE (the only
  path to that log line besides `HalReturnToFirmware`, which logs first). No fault was logged; Deception runs by the
  other agent were starting and closing at the same time. `src/game/diag.cpp` now logs close requests
  (`window: WM_CLOSE ...` / `window: SC_CLOSE ...`) to tell an external close from anything else.
- r7 (420 s, same script, close logging in): fight 1 vs Kung Lao (Prison) 63-204 s won, fight 2 vs Reptile
  212-404 s won, fight 3 vs Shang Tsung (Subway) loaded at 408 s ("WINS: 2", `build/shots/mka-r7-418s.png`); no
  fault, no trap; the only close request was the run script's own at 420 s
  (`window: WM_CLOSE (foreground window's process ..., ours ...)`). So r6's early close came from outside.
  CPU 289 s over 420 s (about 0.7 of a core), 231 MB working set.
- vs1 (menu experiment): with two pads (the physical one on port 0, `MKA_PADS=2` forcing port 1) and P2 pressing
  START on the main menu, VERSUS stays greyed and DOWN skips it to PRACTICE; Practice (character select for both
  sides, Scorpion vs Sektor on Meteor Storm, no round timer) works (`build/shots/mka-vs1-58s.png`). Port 1's game
  flags go from 0xA to 0xB at the practice select. Why VERSUS is disabled is open (the menu is data in
  `art/screen_engine.ssf`; "Signing In" shows at the top right of the menu).

Log noise that is not a fault: `dsound: exports running their original code` (61 `.XTLID` DirectSound exports the
runtime does not replace; the game calls 0x4A821E, which returns 0, and `DirectSoundOverrideSpeakerConfig`),
`vertex: converted on the CPU: v0 type 32` at character select, `d3d: SetDepthClipPlanes 0 0 (1), not applied`,
`xnet: bind 100 to 0.0.0.0:1000 failed (10048)` while another title holds the port, `privileged instruction wbinvd`.
`D3DDevice_InsertCallback` (0x4123A0) is only used by 0x293E90 when the profiling flag 0x5C3948 is set; the runtime's
push buffer walk does not run callbacks.

## Main loop (for the netplay frame boundary)

`main` 0x19C680 (`build/ghidra-mka`, `research/mka/q-main.txt`): init, then `_setjmp(0x5B8848)` at 0x19C85E and an
endless loop at its continuation 0x19C863: frame counters 0x709E6C (always) and 0x829304 (when bit 13 of 0x831A40
and 0x7350B0 are clear, i.e. not paused), then sub_001B6750, sub_0026A9C0 (pads), sub_000A2760, sub_0034BD50,
sub_0025C1E0, sub_0007BE10 (and 0x19CBB0/0x19CC00, 0x34A440/0x34A460 when it returns 0), sub_000A0DC0 (when not
paused), sub_001C2330, sub_001C4D70 (task scheduler: clears the current task 0x7350C0, Deception sub_0013BE30),
sub_0019C950 (calls 0x380800, 0x378740, the render 0x7D810, the no-op D3D 0x409D50), sub_001C6DE0.
Deception's counterpart is the loop at 0xDFFBF (counters 0x61C3F8 / 0x61C418).

Ghidra: `build/ghidra-mka` (imported with `tools/ghidra-import.ps1 -Game mka`: 30,887 functions, 863 renamed from
XbSymbolDatabase and the config, 194 s of analysis).

## Game layer for netplay (2026-10-25)

The layer Deception's netplay sits on, ported with Armageddon's addresses (`src/game`, same APIs as Deception's
files of the same names, namespace `mka`):

- `frame.h/.cpp`: `OnFrameStart` / `OnPadsRead` listeners, `Frame()`, `MainContext()`. The main loop's first call,
  sub_001B6750 at 0x19C8AC (returns to 0x19C8B1; a list of delayed calls, also called from 0x7D33F and itself), and
  the pad update sub_0026A9C0 at 0x19C8B1 (returns to 0x19C8B6; also called by movie and loading loops) are hooked and
  told apart by their return addresses.
- `events.h/.cpp`: the game mode setter, the fight setup (`BeforeFightSetup`, `FightSetups`), the main menu task
  (`MainMenus`), the state switch (`StateSwitches`), menu confirmations (`MenuActions`) with the `MKA_ACTION` redirect,
  the character select screen's block (`SelectBlock`), the start of a Motor Kombat race's load (`MatchStarts`, also
  runs the `BeforeFightSetup` hooks; it was the race setup 0xE9F60 at first, see "Motor Kombat online") and, with
  `MKA_EVENT_LOG=1`, a log line for each of these plus srand calls, mode changes
  made without the setter, every change of health / round wins / round / winner during a fight, and Motor Kombat's
  finish order.
- `autopilot.h/.cpp`: `MKA_AUTOPILOT=versus|arcade|practice` (and `motor`, as far as Motor Kombat's own select
  screen), `MKA_P1` / `MKA_P2` = the game's name ("SUB-ZERO"), the launcher's roster id (`launcher/data/mka.json`:
  "SubZero", "JohnnyCage", "NoobSaibot"; letters and digits compared, any case; all 62 fighters and the 10 Motor
  Kombat ids resolve) or the index; giving either unlocks every character. START until the main menu task starts,
  then START and A in turn until a menu item is confirmed (the profile screen, then ARCADE, which the redirect turns
  into the wanted mode), nothing until the select screen is up, then per port: a press decided once per 0.25 s cycle
  from the cursor read back (a breadth-first search over the grid's move table, so wrap-arounds are used), A on the
  character. Practice: port 0 picks P1, then the dummy. Nothing is pressed after the picks (the fight follows by
  itself). Same launcher entry points as Deception's (`WaitForRequest`, `Begin`, `Rematch`, `Restart`, `ExpectMenu`).
- `pads.cpp`: the autopilot and `InjectPad` / `ParseButtons` as in Deception.

VERSUS is greyed by the menu script (`art/screen_engine.ssf`); what it tests was not traced. The confirmation
sub_001D9530(player, action) marks who takes part; its own test for two players is both records in state 2 (joined,
set by the script's event 0x10D8E in sub_0020AF20). As in Deception, `MKA_ACTION=6` turns the next ARCADE / VERSUS /
PRACTICE confirmation into versus and marks P1 and P2 as taking part (sub_001F8C00(record, 1)); the main menu task
calls the confirmation again with the stored action before it switches, so the hook marks both players there too.

### Verification (debug build unless noted)

- ap1 / ap2 (`MKA_AUTOPILOT=versus;MKA_P1=Scorpion;MKA_P2=Sub-Zero;MKA_PADS=2;MKA_EVENT_LOG=1`, ap2 with
  `MKA_INPUT` attacks on both ports from 38 s, generated: P1 walks right and presses X/Y/X/B/A/X/Y every 5 s, P2 walks
  left and presses X, Y every 5 s): main menu #2 at 12 s, ARCADE confirmed at 14.2 s and redirected to versus, mode
  12 -> 1, select screen at 15.8 s, P2's cursor went 9 -> 0 -> 1 (RIGHT wraps), fight setup #1 at 23.7 s
  ("mode 1, characters 48 vs 59"), hits on both sides (P2 0.912, 0.848 ...; P1 0.928, 0.856, 0.784), P1 won 2-0 (round
  wins 1-0 at frame 3110, round 2 with health back to 1.0, winner 1 at frame 4610), one fight setup for the match,
  then the state switch back to the select screen (0x2057E0); 45 s after the last scripted press the idle select
  screen went back to the main menu by itself (2700 frames). Screenshots: `build/shots/mka-ap1-30s.png`,
  `mka-ap2-45s.png` (both health bars down), `mka-ap2-55s.png` ("SCORPION WINS!").
- vs2 (`MKA_P1=JohnnyCage;MKA_P2=NoobSaibot`, the launcher's ids, same attacks): "characters 4 vs 41", the select
  screen with both picks and every character unlocked (`build/shots/mka-vs2-19s.png`), both health bars down in the
  fight (`mka-vs2-50s.png`), 2-0 at 90 s, back to the select screen at 97 s.
- pr1 (`practice`, Raiden / Kitana): mode 12 -> 4, port 0 picked both, "mode 4, characters 43 vs 28"
  (`build/shots/mka-pr1-45s.png`). ar1 (`arcade`, `MKA_P1=Liu Kang`): not redirected, mode 0, the ladder task
  0x198420, then "mode 0, characters 32 vs 31".
- rel1 (release preset, the same versus autopilot): boots to the main menu (`build/shots/mka-rel1-13s.png`) and
  reaches the fight ("FIGHT!", `mka-rel1-30s.png`); 9 s of CPU in 50 s. rel2 (the final code, release, `MKA_P1=LiuKang;
  MKA_P2=ShangTsung`): the main menu at 13 s, Liu Kang vs Shang Tsung at 35 s (`build/shots/mka-rel2-13s.png`,
  `mka-rel2-35s.png`). fin (the final code, debug, `MKA_P2=SubZero` and the attacks): the same as ap2
  (`build/shots/mka-fin-48s.png`).
- A temporary counter (removed again) during vs2: the C runtime's rand was never called and srand never ran
  (boot, menus, select, a whole match); the engine's Mersenne Twister was drawn about 340 times a second at the select
  screen and up to 5,700 times a second in the fight, in bursts at hits (the float wrapper 0x259280 and below-n
  0x2591D0), far more than Deception's 22 to 33 a frame.

### Engine map for netplay

| what | Armageddon | Deception |
|---|---|---|
| main loop frame counters (always / unpaused) | 0x709E6C / 0x829304 (unpaused: bit 13 of 0x831A40 and 0x7350B0 clear; 0x7350B0 is a second pause the Xbox Live session code sets, 0x34F7xx; while either is set the task scheduler sub_001C4D70 runs only tasks flagged +0x20 bit 4) | 0x61C3F8 / 0x61C418 (flags 0x620F20 bit 13) |
| frame hook: first call / pad update | sub_001B6750 at 0x19C8AC (ret 0x19C8B1) / sub_0026A9C0 at 0x19C8B1 (ret 0x19C8B6) | sub_00130390 (ret 0xE000C) / sub_001C52F0 (ret 0xE0011) |
| per-port block | 0x831C0C + port * 0x20: flags +0 (bit 1 open), button table +4 (logical bits of each pad button, 0xC apart), player record +8 (0 none; a press on a free open port binds one, 0x7B820 / 0x7AD20), previous +0xC, current +0x10, pressed +0x14 ((cur ^ prev) & cur), sticks +0x18 (centred 0x807F807F) | 0x62109C + port * 0x1C, same fields without the record |
| logical button tests | held sub_000A0FD0(port, button), pressed sub_000A1060(port, button) | |
| game mode / setter / getter | 0x8292F8 / sub_00258FF0 / sub_00259000 (also written directly at 0x8A9CC, 0x113EA3, 0x182AB3, 0x182BD8, 0x182C5E, 0x182CCC, 0x18323C) | 0x61C414 / sub_001BC170 |
| mode values | 0xC front end, 0 arcade, 1 versus, 4 practice (health refills), 0xF Motor Kombat select, 7 Motor Kombat race, 8 menu action 1 (probably Konquest: every port drives P1), 3 profile screens, 0xA Kreate-A-Fighter, 0xE Motor Kombat online session, 6 / 9 / 0x10 / 0x11 not identified | 13, 0, 1, 4, 6 chess, 7 konquest, 9 puzzle |
| fight setup | sub_000983A0, once a fight, from the fight load task sub_00096C60 after the load: round wins and winner reset, round 1, the round timer setup sub_00098A40 (seconds 0x831CE0, start frame 0x831CF4 = 0x709E6C, a second counted every 60 frames), the HUD; `BeforeFightSetup` hooks run before it | sub_00083570 |
| fight task / rounds | sub_00096FC0 runs again for every round: round setup sub_00098530 (health = start health), the round loop on sub_00098060 (round over: sub_0009C230 returns 1 P1, 2 P2, 3 draw), round end sub_00098B50; a match won goes back to the select task (versus) or the ladder (arcade) | |
| player records | P1 0x831AEC, P2 0x831B34, 0x48 apart (P3 0x831B7C, P4 0x831BC4 for Motor Kombat) | 0x620FC4 / 0x621030, 0x6C apart |
| record fields | +0 port (-1 none), +4 player index (5 unassigned), +8 state, +0xC health (float, 1.0 full), +0x10 start health, +0x14 costume bits, +0x1C round wins, +0x20 match wins in a row, +0x2C character (0x40 none), +0x30 fighter object, +0x34 its control object | +0xC health, +0x40 round wins, +0x54 character, +0x58 fighter |
| match state | winner 0x830934 (1 P1, 2 P2, -1), round winner 0x5B7D44, round 0x831CCC (from 1), round timer 0x831CE0 (seconds; options: time 0x723BC4, rounds to win 0x723BC0), arena 0x831A54 | winner from round wins |
| menu confirm / action | sub_001D9530(player, action) -> 0x5B8960 (0x17 none), confirming player 0x5B895C; called by the menu script (0x1D8D2E in sub_001D8020) and again by the main menu task | sub_0014EF20 -> 0x46CDB8 (0x18 none) |
| main menu actions (sub_0019F3D0) | 5 arcade, 6 versus, 9 practice (all to the select task), 7 Motor Kombat, 1 mode 8 (probably Konquest), 0xC controller config, 0xD options, 0x10 / 0x11 / 0x12 create / view / delete profile, 0x13 Xbox Live, 0x15 credits, 0x16 Kreate-A-Fighter, 0xE / 0xF sub_00192570, 3 / 4 the main menu again | 6 arcade, 7 versus, 0xB practice ... |
| taking part | sub_001F8C00(record, state) writes record +8: 0 out, 1 taking part / choosing, 2 picked (in the menus: joined), 3 practice's dummy picked; the confirmation marks both for versus, the confirming player for arcade / practice / 0x13 (both if both had joined) | sub_0016FBC0(record, 1) |
| character select | task sub_002057E0 (arcade, versus, practice); its screen block: the screen builder sub_001DDDA0(name 0x5025B0 "common/p_select/p_select", 0x100031, block, 0); the current screen's block is also [0x5BB3FC] while its serial matches [0x5BB400]; cursor slots +0x10 (P1) / +0x44 (P2), characters under them +0x1C / +0x50 (copied to record +0x2C); idle for 2700 frames it goes back to the main menu (action 4). X opens a panel over it (the task object's +0x34 moves 8 on, B moves it back): while it is open A picks nothing, B closes it, and B without it leaves for the main menu (probed with the monitor, alternate costumes locked) | task 0x179A60, cursors 0x471B1C / 0x471B18 |
| grid | 64 slots, 0x561B08 + slot * 0x10: +0 character, +4 an id sub_002077E0 gets on a pick, +8 / +0xC head icon names (normal, locked); rows of 10 (slots 0-39) then rows of 6 (40-63); moves 0x561F08 + slot * 0x10: LEFT, RIGHT, UP, DOWN targets (wrapping); a move (sub_00207480(player, 0 up / 1 down / 2 left / 3 right), menu script event 69000) goes on past characters that cannot be picked; pick sub_00207930(record) | 8 x 3, 0x3F9210 + slot * 0x28 |
| characters | names 0x560FC0 + index * 0x14 (name pointer first: 0 ASHRAH ... 48 SCORPION ... 61 TAVEN, 62 CAP the created fighter, 63 RANDOM); slots 30 and 39 hold the created fighter (needs a profile) | names 0x3F89E0 + id * 16 |
| unlock masks | 64-bit, bit = character: 0x7363C8 (sub_00208D90 with its second argument 0; written by 0x1FC5xx / 0x1FC6xx) or the defaults 0x561968; alternates 0x7363D0 / 0x5BD988. Locked by default: Blaze, Daegon, Meat, Taven | 0x601E60 / 0x601E68, defaults 0x3F8FF0 |
| state switch | sub_0019CA30(priority, task): task function to 0x5B8844 (the current state task), priority 0x5535B0, the new task object 0x5B8840, then longjmp (sub_0019C980) to main's 0x5B8848; back to the main menu from anywhere: sub_0019CA30(3, 0x19F3D0) | sub_000E0190, 0x46CCB8 |
| main menu task | sub_0019F3D0 (its first start switches to the profile screen sub_001FA0C0, which comes back to it); the title / attract task is sub_00027BE0 | sub_000E3770 |
| random generator | Mersenne Twister: seed sub_001D6070 (once, from main at 0x19C80E: rdtsc shifted left 16, or'd with the low 16 bits of a timer: different every boot), draw sub_001D6280 (regenerates with sub_001D6160 every 624), state 0x5BA708 (624 dwords), index 0x5BB0C8, draws since the regeneration 0x5BB0CC; wrappers 0x2591C0 raw, 0x2591D0 below n (16 bit, 703 call sites), 0x259260 signed (no direct callers), 0x259280 float 0..x (181), 0x2592E0 float -x..x (130), 0x259300 chance (9) | seed 0x14BDB0, draw 0x14BFB0, wrappers 0x1BC190 / 1A0 / 260 / 2D0 |
| C runtime rand / srand | 0x39C6B8 / 0x39C6AB (seed at _getptd() + 0x14, _getptd 0x3A0199); srand(0) only from 0x3CD986 (an object's init in the library code around 0x3C1000), rand only from 0x37EB30 (a random pick without repeats, through a pointer); neither ran in a whole versus session | 0x29B8AC / 0x29B89F |
| engine heap allocator | sub_0029D060(heap, size, alignment, name, ...), the same shape as Deception's; e.g. heap [0x5C3C14] "get_mkhdr" (0x1C6810), the default heap [0x5C3BD0] through 0x1D6310(size, alignment) | sub_001DD3B0 |
| async I/O manager | the manager [0x709450] (made by 0x38F660 from 0x3806D0); worker thread entry 0x385E10 (thread id 0x5AF77C, priority 1, made by 0x3885D0) -> loop 0x38D2F0: when a scheduler has work (vtable +0x10) it runs the busy schedulers' passes, sub_0038CB20(ecx = manager: vtable +0x14 busy, +8 the pass; Deception's 0x2911A0), else waits alertably in 0x386090; the main thread's drain 0x38D7C0 (main loop -> 0x19C950 -> 0x380800, ecx = manager): wakes the worker if a scheduler is busy (0x38C4C0), runs deferred calls (0x384C00), pops the completion ring at manager + 0x3C (0x38D3E0) and runs each request's handler (0x38B240: vtable +0x20, +0x24); reads through ReadFileEx 0x2A4E58 (e.g. 0x389400 in chunks of 128 KB, completion 0x389500) | [0x5F0E90], scheduler pass 0x2911A0, drain 0x2917B0 |
| fight state for dumps | 0x830900..0x831D00: HUD state, flags 0x831A40 (bit 2 a round being fought, bit 13 paused) / 0x831A44 (bit 0 Xbox Live, bit 2 probably the attract demo fight), arena, the four records, the port blocks, round state 0x831CC0..0x831CF4 | 0x620000..0x622000 |

### Motor Kombat (map only)

- Menu action 7 (not in the main menu shown here; `MKA_ACTION=7` reaches it): mode 0xF, `sub_0019CA30(10,
  sub_001052B0)`. The select task sub_001052B0 builds "common/kart/kart_main" (0x4DDA50): first SELECT TRACK, then a
  quadrant per player (P2 to P4 "PRESS START"; `build/shots/mka-mk1-20s.png`, `mka-mk2-21s.png`); its loop
  sub_00105590 confirms action 2 once nobody is still choosing (state 1), action 4 goes back (also after 2700 idle
  frames). In this mode port n drives record n (sub_0007AD20). Per player the kart choice is at 0x82ECA0 + player *
  0xC, the karts taken in the mask 0x82ECD4; the menu script's events 0x10D8E (pick / unpick: state 2 / 1) and
  0x10D8F (join: state 1, a free kart) are handled by sub_0020AF20. Karts: 0x560DA0 + kart * 0x34 (+0 a sound,
  +4 "motload_<name>.sec", +8 "M_LIL_<NAME>"), 0 Baraka, 1 Bo' Rai Cho, 2 Cage, 3 Cyrax, 4 Jax, 5 Kitana, 6 Mileena,
  7 Raiden, 8 Scorpion, 9 Sub-Zero: the launcher's motor roster in the same order.
- The race: action 2 sets mode 7 and switches to the load task sub_000E47B0 (priority 10: the track in 0x831A54, the
  race object [0x5B8334], 0x107C bytes, made by sub_000E4B70, karts' art); then the race setup sub_000E9F60 (karts for
  the players in state 2 and the computer's, the round setup sub_00098530 with its timer start) starts the race.
  The load task's own mode setter call (returning to 0xE47E8) is where netplay meets (`MatchStarts`,
  `BeforeFightSetup` hooks): the load draws random values for the karts. The countdown sub_000EFF00 stores the start
  (race +0x2C = 0x829304), the race runs in sub_000F0940. RT accelerates (A does not); four laps on Lin Kuei
  Raceway (select 43, arena 48), eight on select 44 (arena 49).
- The end: the lap check sub_000FA3A0(kart) counts laps (kart +0xBC; the race's laps at race +0x24, four here; lap
  times from the unpaused counter: kart +0xCC lap start, +0xD0 best lap); a kart completing them there marks itself
  finished (kart +8 bit 1; its number and frame to 0x5B816C / 0x5B8170) and, offline and if the race is not over
  yet, calls sub_000FE040(&number): race over (the race object's bit 17) and, the first time, that kart's handle at
  race +0x28C. sub_000F0940 then moves to the results task sub_000F1330: it sorts the finish order into race +0x29C
  (8 kart slots, -1 empty; by laps and time through sub_000EFB90; karts' handles at race +0x24C + slot * 8, a kart's
  +0x20 its slot = player number - 1, +0x1C its kart, +0x1BC the port driving it, -1 for the computer), shows the
  race +0x28C kart's "PLAYER %d" (or the gamertag at race +0x140 + slot * 4) with "WINS", counts a win for the first
  place, sets mode 0xC (the call returning to 0xF2340, where `MKA_EVENT_LOG` logs the order) and goes back to the kart
  select (sub_000F50A0(1)).
- mk4 (RT held from 30 s, the human kart driving blind into walls): the computer's karts finished long before without
  ending the race; the race ended when the human's kart finished, 5:20 in; logged order: 1 Raiden (slot 4, computer),
  2 Scorpion, 3 Bo' Rai Cho, 4 Sub-Zero, 5 Kitana, 6 Jax, 7 Mileena, 8 Baraka (slot 0, port 0, the human); then the
  kart select again, and 45 idle seconds later the main menu. For a result between people, the order at race +0x29C
  (first place = race +0x29C[0]) is what to read, not race +0x28C (the kart whose finish ended the race).

## Netplay on the shared core (2026-10-25)

Armageddon plugs into `recomp/common/src/mk3d` like Deception (its `src/game` files of the same names are the
reference):

- `CMakeLists.txt`: the `MK3D_COMMON` sources and include directory. `game.cpp`: `EnvPrefix()` "MKA" and the fixed
  volatile bytes: the D3D device's vertical blank fields (device 0x427AF0, +0x1DB8..+0x1E04, the same XDK 5849
  layout as Deception's) and the texture header 0x71F700 (0x14 bytes, below). `frame.cpp` implements `mk3d::Frame`,
  `MainContext`, `OnFrameStart`, `OnPadsRead`; `diag.cpp` `mk3d::DescribeCode`; `pads.cpp` the `mk3d::ParseButtons` /
  `InjectPad` forwarders and keeps the player's pad off the game while the launcher's autopilot runs
  (`link::SuppressPhysicalPads`). Forwarding headers `rollback.h`, `gclock.h`, `task_fibers.h`, `netplay.h`,
  `launcher_link.h` give the shared names Armageddon's namespace.
- `task_fibers.cpp` on `mk3d/sfiber.h` with `mk3d::tasks::Save/Load/Alive` (Deception's version, with Armageddon's
  destroy slot +0x0C, the NULL check before a destroy and class C's SetFunction 0x1C5180).
- `rng.cpp`: in a match in sync the Mersenne Twister's draw 0x1D6280 on the main thread is a hash of the agreed seed,
  the frame, the call site (the outermost of the six wrappers 0x2591C0 / 1D0 / 260 / 280 / 2E0 / 300) and the site's
  count in the frame, kept in a fixed 2048-entry table (no allocation per draw); the C runtime's rand 0x39C6B8 counts
  into `mk3d::net::CountRand`. `heap.cpp`: blocks from sub_0029D060 on the main thread come back zeroed, and the
  XAPI heap keeps its free pages committed: RtlFreeHeap 0x2A93AE and its slow path 0x2A95A2 (heap at [esp+4])
  decommit the whole pages of a large free block (RtlpDeCommitFreeBlock 0x2A7CEB, NtFreeVirtualMemory through the
  thunk 0x4D78DC) once the block and the free total pass the heap's thresholds (+0x28 DeCommitFreeBlockThreshold,
  +0x2C DeCommitTotalFreeThreshold, free total +0x30); both thresholds are set to 0xFFFFFFFF before every free (see
  "Motor Kombat online" below for why).
- `aio.cpp`: the loader on the main thread while recording. The I/O manager [0x709450] (0x7093B8) has two
  schedulers: the request scheduler 0x709460 (vtable 0x51C4C8, pass 0x38F1F0, busy 0x38C4F0: a request in its list
  +0x68..+0x6C with work left, or +0x60 waiting) and the device scheduler 0x709538 (vtable 0x51C4B0, pass 0x38FA90,
  busy 0x38CC40: its queue +0x30, or the first of three running requests +0x24..+0x2C with work left). The worker's
  pass over the busy ones, sub_0038CB20 (ecx = the manager), is skipped on the worker while recording; the main
  thread's drain sub_0038D7C0 runs it and the read completions (APCs) first. Idle: nothing busy, no completions
  (manager +0x78), no deferred calls (0x7094E8 +0x3C); Settle pumps 0x380800. `MKA_AIO_LOG=1` logs the schedulers.
- `netgame.cpp` (`mk3d/netgame.h`): ReadLocal strips START and BACK (the pad update's inputs 0x59C828: digital 0x10
  and 0x20; default table 0x54CCA0: logical 0x800 and 0x100, so 0x900 as in Deception). The meeting point is the
  fight setup sub_000983A0 and, for Motor Kombat, the race load task's mode setter call (sub_00258FF0(7) returning to
  0xE47E8, before anything of the race exists; see below). READY words: mode, both characters, both frame counters,
  the arena (Motor Kombat: the select's track 0x831A58, since 0x831A54 still holds the last fight's arena there), the
  four kart choices; Agree takes the larger counters, checks the arena or track and the karts and sets every port's
  button table to the default one; while a match is in sync sub_0007B650 (a
  player's table) is held to the default too, so a logical bit means the same in both games. Reseed: the seed
  0x1D6070, the draws since the last regeneration (0x5BB0CC, which the seed leaves) to 0, then the per-site streams.
  MatchOver: the mode left the match's (Motor Kombat's results set 0xC) or the front end is up again (a versus match
  returns to its select screen). StateHash: health, round wins, characters, mode, both fighter objects (0xC80 bytes
  from record +0x30; the objects reach +0xC80, their size is not known), the race object (0x107C bytes) and its eight
  karts (handles at race +0x24C + slot * 8; a kart is the 0x4E8 bytes of task data sub_000C2C30 makes). The per-frame
  log adds the eight kart hashes during a race. Settled: round wins, mode, the match winner 0x830934, the race's over
  bit, both humans' places. CheckBlocks: the fighters, 0x830900..0x831D00, the race and its karts; DataRange
  0x547000..0x946000. With netplay from the environment both games start the
  arena list at its first entry (its place 0x831A64 is drawn at boot from the time-seeded generator).
- `linkgame.cpp` (`mk3d/linkgame.h`): a versus arena from the launcher's seed (the list 0x548F88 of 15 arenas, -1 at
  the end; its place 0x831A64 = seed % 15 or the named stage; every arena unlocked: masks 0x7363D8); the rounds to win
  0x723BC0 from `wins_needed`; Motor Kombat's track from the seed (43 + seed % 5 in the select's numbering, or the
  stage); Park is sub_0019CA30(3, 0x19F3D0); Outcome: round wins for fights; for "motor" the better placed of the two
  people in the finish order wins, 1-0.
- The autopilot drives Motor Kombat's select (`MKA_AUTOPILOT=motor`): the track (0x831A58: 43 for the first, RIGHT
  adds one; A), then P2 joins with START on port 1 (record state 0 -> 1), each port presses RIGHT until its kart
  (0x82ECA0 + player * 0xC: 0 Baraka ... 9 Sub-Zero, by the "motload_<name>.sec" names) is the one asked and A
  (state 2); P1 readies only after P2 has joined (the race starts once nobody is still choosing). The track's number
  becomes the race's arena + 5 (43 -> 48 Lin Kuei Raceway, 45 -> 50).

What made runs differ, in the order found:
1. The sync test never started: the loader never looked idle, a scheduler of a kind not handled yet (the request
   scheduler) was taken as busy. Both kinds' busy fields are read now.
2. Every frame differed on the run again in one dword, 0x71F704: the data address of the texture header 0x71F700 that
   sub_0027C910 builds over the current back buffer every frame (XGSetTextureHeader); the back buffer alternates with
   each present, which a rollback does not put back. A fixed volatile range now (it is rewritten before every use).
3. The arenas would have differed (see above); not seen in a run, fixed before the first netplay test.

Verification:
- Sync test (debug, `MKA_SYNCTEST=1;MKA_SYNCTEST_FRAMES=900`, the versus autopilot and attacks on both ports, hits on
  both sides inside the tested frames): `synctest: done after 900 frames: 0 mismatches`, close 1.1 ms and restore
  0.5 ms a frame, 22,982 pages (89.8 MB) tracked.
- `tools\netplay-local.ps1 -Game mka -Config release -Port 7301 -Tag mka1` (Scorpion vs SubZero, 70 ms + up to
  30 ms, 3% lost, attacks on both): `4093 frames in both logs, the last runs differ in 0`, a whole match (2-0, round
  wins at frames 1519 and 3090, back to the select screen at 4092), 58 and 159 rollbacks (deepest 6 frames, about
  7 ms each); `build/shots/mka1-a-60s.png` / `-b-60s.png`, `mka1-a-90s.png` / `-b-90s.png` ("SCORPION WINS!" on both).
- `tools\online-e2e.ps1 -Game mka -Port 7977 -Exe recomp/mka/out/build/release/MKA.exe` (a path with forward
  slashes; backslashes passed through bash lost the separators): both games `result winner=1 p1_rounds=2
  p2_rounds=0`, the server `RESULT match=27d8818b3a3f ... 2-0`, both parked back to `ready`. With `-Queue lobby`: both
  `result winner=2 p1_rounds=0 p2_rounds=2`, `RESULT match=bafddb2c4f7c ... 0-2`.
- Motor Kombat, `netplay-local -Mode motor -Tag mkm1` (karts Scorpion and Sub-Zero, RT held on both from 28 s, the
  same network): `18662 frames in both logs, the last runs differ in 0`, a whole race. Both drove blind into walls;
  the computer's karts finished minutes earlier without ending it. The race ended at frame 18,009 (5 minutes in)
  when P1's kart finished, 7th: P2's was still racing and the results placed it 8th (the race ends with the first
  person to finish; the other's place comes from the results' sort, so only one has to finish). Both games went back
  to the kart select at frame 18,661. `build/shots/mkm1-a-300s.png` / `-b-300s.png` (the same moment on both: 7th and
  8th, lap 4/4, "PLAYER 2 SMASHED INTO A WALL").

### Motor Kombat online (2026-10-25)

The first launcher run of a race (`online-e2e -Mode motor`, seed 539585391: track 44, karts Sub-Zero and Scorpion)
reported `desync frame=3000` on both games. `rbcmp` showed only the race hash differing, from frame 2993; the same
track and karts in `netplay-local` (`MKA_TRACK=44`) diverged at frame 1424. The desync dump (`# dump` lines in the net
log; `MKA_NET_DUMP=<frame>` writes the same any time) narrowed it down: in the race object, the message timer +0x1BC
and count +0x1C0 (sub_000DC910 shows a message at most every so often) and one of the eight kart positions at +0x1EC
+ slot * 0xC, slot 5, a computer's kart. Hashing the karts too showed every kart different from the first frame on:
+0x298 differed, a driving factor that the kart setup sub_000C44D0 draws from the random generator
(base * sub_00259280(1.0)). The karts are made by the race load task 0xE47B0 (sub_000E4B70 the race object, then
sub_000C27A0 the karts), before the race setup 0xE9F60 where the games met, so those draws came from the generator
in its menu state, different on each game: the computer's karts drove slightly differently and drifted apart about
20 s into the race. The meeting point moved to the start of the load (the mode setter call returning to 0xE47E8,
events.cpp `RaceLoadStarts`): the reseed comes first, the load runs in sync in lockstep (the core's loading rule) and
the karts' draws come from the per-site streams.

Results (release, karts Sub-Zero and Scorpion, track 44 unless noted):
- `netplay-local -Mode motor -Tag mkm4` (RT held on both, no network faults): `6352 frames in both logs, the last runs
  differ in 0` (it had diverged at frame 1424 before); `build/shots/mkm4-a-120s.png` / `-b-120s.png`.
- `-Tag mkm6` (70 ms + up to 30 ms, 3% lost, RT held and random steering and item buttons on both): `15429 frames
  in both logs, the last runs differ in 0`, a whole race and back to the kart select, 366 and 373 rollbacks (deepest
  6 frames, about 8 ms each); P2 finished 6th, P1 8th. `build/shots/mkm6-a-200s.png` / `-b-200s.png` ("PLAYER 2
  BURNED IN LAVA", lap 6/8, on both).
- `online-e2e -Game mka -Mode motor -Port 7977 -Exe recomp/mka/out/build/release/MKA.exe -Seconds 480 -CharA
  Scorpion -CharB SubZero -InputA "20:RT:700" -InputB "20:RT:700"`: both games `result winner=2 p1_rounds=0
  p2_rounds=1`, the server `RESULT match=6797bab040c0 e2eA131110 beat e2eB131110 0-1 (casual)` (B was player 1 there),
  both parked to `ready`; the two net logs `15460 frames ... differ in 0`. An earlier run with the same build minus
  the heap change: `result winner=1 p1_rounds=1 p2_rounds=0`, `RESULT match=d3065346e2f9 ... 1-0`, 15974 frames equal.
- Sync test of a whole race (release, `MKA_AUTOPILOT=motor;MKA_SYNCTEST=1`, both ports on RT with some steering):
  `done after 15664 frames: 3 mismatches`, all three pages the heap committed inside the tested frame (frame 4 and
  frame 100 while the race loads, frame 1442 in the race: "changed only the first time", see the core notes below);
  every other frame of the race, the load included, came out the same on the run again. The versus sync test with
  the heap change: 900 frames, 0 mismatches; `netplay-local -Tag mka3` (versus, the faulty network): 3513 frames
  equal, 2-0; `online-e2e` versus: `result winner=1 p1_rounds=2 p2_rounds=0`, 3059 frames equal.

The heap change: the first race sync tests faulted about 22 s into the race, in the core's `CloseFrame` / `HashPages`
reading guest 0x967000; the debug log had `memory: virtual memory freed while recording for a rollback (00967000)`
just before. The XAPI heap had grown by 8 pages while the race loaded (0x966000..0x96E000) and decommitted them
when a block there was freed. The core skips pages that are no longer committed when it restores, so a rollback
across such a free would leave the still-allocated block's pages gone in the frames run again (and its hashes read
them). With the decommit thresholds at the maximum (heap.cpp) the free pages stay committed; the runtime does not
count commits against the console's 64 MB.

What the shared core and runtime could do (not changed here, outside recomp/mka):
- `recomp/common/src/mk3d/rollback.cpp` `OnCommit`: a commit over pages already committed and tracked
  (`VirtualAlloc(MEM_COMMIT, PAGE_READWRITE)` in the runtime's `mem::Commit`) makes them writable again without a
  fault, so writes to them stop being logged until something protects them again. The heap does that when a frame
  that grew it runs again after a rollback (the restore puts its bookkeeping back, the pages stay committed): the
  sync test's "changed only the first time" pages above. Fix: in `OnCommit`, for `committed && st == kClean`,
  `Protect(p, true)` again (a dirty page can stay writable).
- `HashPages` and the detail copy in `CloseFrame` read every page of the frame's list; a page decommitted during the
  frame (`OnCommit(..., false)` sets it untracked) faults there. Skip pages whose state is `kUntracked` (or
  `!mem::IsCommitted`).
- `kernel/memory.cpp` `FreeVirtual` while recording only logs; like the pool frees (`AllocRecord`), a decommit or
  release on the recording thread could be held back until its frame is confirmed (`host_rollback::Confirm`), so a
  rollback across it finds the pages still there.

## Frame data in practice (2026-10-28)

Practice shows player 1's last attack (`recomp/common/src/mk3d/framedata.cpp`, this game's side
`src/game/framedata.cpp`). Armageddon has Deception's fields and rules at other offsets (research/mkd/notes.md,
"Frame data in practice"), found with the debug monitor stepping a versus fight and both players pressing a new button
every frame for the ground truth:

- Player records P1 0x831AEC, P2 0x831B34: +8 select state (3 for practice's dummy), +0xC health, +0x30 fighter
  (static: P1 0x5BBF88, P2 0x5BCC58), +0x3C the fighter's task (+0x30 its function). Game frame counter 0x829304,
  the fighter whose task runs 0x734FFC.
- Fighter fields (Deception's in brackets): +0x304 strike contact -1/1/2/3 [0x1BC], +0x31C stance wait until [0x1D4],
  +0x320 no new move until [0x1D8], +0x380 attack phase 1/2/3 [0x20C], +0x388 state [0x214], +0x390 controls: the
  port its actions are read from, -1 for practice's dummy [0x21C], +0x3BC attack command start [0x244], +0x40C CPU
  flag [0x294], +0xC6C block-hold entries [0x6AC].
- Move gate sub_001D1A90 (refused: state & 0x200 except 0x420D, & 0x800, state 0x6000, frame < +0x320, health 0,
  select state not 2 or 3); attack command sub_001D34B0 (phase 1, startup, phase 2, strike sub_00084730, phase 3);
  hit test sub_000843D0; block held sub_00089B50 -> sub_000A0FD0(port, action) (action 1 block, 0xE down); block hold
  sub_001CCF60, crouching hold sub_001CD680 (state 0x900); block reactions 0x2125A0 / 0x211F80 (the latter sets
  +0x320 = frame + 6); neutral 0x823D0 -> stance wait 0xB46F0 -> stance 0xB49B0.
- Practice: no port drives the dummy and the menus have no dummy settings (art/screen_engine.ssf names only
  arcade_submenu_PRACTICE).

Scorpion against Scorpion (move frames, the press is frame 1; "mashed": checked in versus against both players
pressing every frame, the rest from the practice panel):

| move | startup | active | player 1 can act | on hit | on block | crouch block |
|---|---|---|---|---|---|---|
| X | 12 | 6 | 37 (mashed) | +28 hitting on frame 14 | -17 (mashed: five release timings) | whiffs over it |
| Y | 16 | 2 | 30 | +50 | -2 | |
| B | 14 | 7 | 55 | -8 | -33 | |
| crouching A (a low) | 19 | 2 | 50 | -7 | | -19 (mashed) |
| A, 2 hits | 12, again 25 | 6 (7) 1 | 51 (mashed) | +2 to +4 (second hit on frame 25 to 27; +4 mashed) | -20 | |

Open: Scorpion's X at point-blank range (contact on its first active frame) once put both fighters into the
script-run routine 0x2B8F0 for 200+ frames (P1 frozen in phase 2); the panel drops a move not over in 300 frames.

## Environment and tools

`src/game/pads.cpp` takes `MKA_PADS`, `MKA_INPUT`, `MKA_PAD_DELAY`, `MKA_PAD_LOG` (Deception's `MKD_*` renamed);
`MKA_TASK_TRACE=1` traces the task fibers, `MKA_HANG_DUMP=<s>,...` dumps all threads. Run:
`powershell -File tools\run-game.ps1 -Game mka -Exe MKA.exe -Config debug -Seconds 240 -Env "MKA_PADS=2;MKA_INPUT=..."`.
The game layer adds `MKA_AUTOPILOT=versus|arcade|practice|motor`, `MKA_P1`, `MKA_P2`, `MKA_TRACK=<43..>` (Motor
Kombat's track as its select numbers them), `MKA_UNLOCK_ALL=1`,
`MKA_AUTOPILOT_HOLD=1` (stop at character select), `MKA_ACTION=<menu action>` and `MKA_EVENT_LOG=1`. A versus fight
with both chosen fighters: `-Env "MKA_AUTOPILOT=versus;MKA_P1=Scorpion;MKA_P2=SubZero;MKA_PADS=2"` (fight setup at
about 24 s).

No runtime patch was needed: everything above is config (`recomp/mka/config/game.toml`) or game-local code
(`recomp/mka/src/game`).

## Next

- VERSUS greyed with two pads (see vs1): worked around with the menu action redirect (`MKA_ACTION=6`, what the
  autopilot uses), as in Deception; how the menu script decides it is not traced.
- Netplay works on the shared core (versus and Motor Kombat, section above). Not tried yet: rematches through the
  launcher (versus returns to its select screen, Motor Kombat to its kart select; the autopilot's Rematch takes
  both), a person's own button configuration (held to the default table during a match). Motor Kombat's track comes
  from the launcher's seed (43 + seed % 5 assumes five tracks in the select's list; seed 539585391 gave track 44).
  Motor Kombat's sync test still reports the heap's commits in a frame run again (core note above).
- Not mapped: the HUD's animated health copies, the screens between the picks and the fight (nothing is pressed
  there), the size of a fighter object, modes 6 / 9 / 0x10 / 0x11.
- The six missing kernel imports (191, 227, 223, 76, 243, 343) have not been reached; I/O completion ports would be
  the first to implement if an XAPI path starts using them.
- `tools/py/lib_audit.py recomp/mka research/mka/symbols-xbsdb.txt` lists 178 library functions called from game
  code that still run console code (72 XONLINE, 39 D3D, 30 XGRPH, 12 DSOUND, 11 XNET, 8 XPP, 5 D3DX); none faulted
  in the runs above. The D3D getters it marks DEV read device fields the runtime keeps (patch 0005 layout keys).
- Release preset: configured offline like the debug one (`cmake --preset release` with the four
  `FETCHCONTENT_SOURCE_DIR_*` of `out/deps-src`), built in about 2 minutes; boots to the main menu and plays (rel1).
