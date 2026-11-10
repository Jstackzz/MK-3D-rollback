# mkda (MK Deadly Alliance, USA, Rev 1, 4D57000C) running notes

Every address here is for XBE sha1 5aba0b06995b2179673a9ff05bcf5d0e16a9bc57 (2,895,872 bytes, XDK 1.0.4721, internal
name mk5, `C:\mk5\Code\mk5xbox_Release\mk5.exe`, entry 0x15AFBC, kernel thunk 0x2311C0). The bring-up follows
Armageddon's (`research/mka/notes.md`) and Deception's (`research/mkd/notes.md`); same engine lineage, older and
compiled without optimisation in places (the task system reloads every global on each use).

## 2026-10-25: project setup

- `recomp/mkda/` from `recomp/mka/` (CMakeLists, presets, build/release.bat, .gitignore, .gitattributes), target
  MKDA, `sdk` junction to `external/XboxXBERecomp`. `game_info.cpp`: "Mortal Kombat: Deadly Alliance", short title
  and exe MKDA. Environment variables use the prefix `MKDA_` (`MKDA_PADS`, `MKDA_INPUT`, `MKDA_PAD_DELAY`,
  `MKDA_PAD_LOG`, `MKDA_TASK_TRACE`, `MKDA_HANG_DUMP`).
- Disc: the whole XISO extracted into `recomp/mkda/assets/` (`tools/py/xiso.py --extract-all`, 353 files, 1.6 GB:
  `default.xbe`, `artxbox/` (127 `.ssf`), `moviesxbox/` (23 `.xmv`), `sndsxbox/` (202 `.mbx`/`.msx`/`.MMB`)).
  `assets/default.xbe` sha1 checked. Movies are XMV (the XDK's WMV based Xbox Media Video, section `XMV`), not CRI
  Sofdec: this game has no CRI middleware at all, so Deception's `cri.cpp` has no counterpart.
- Configure offline: the four FetchContent sources copied from `recomp/mka/out/deps-src` into
  `recomp/mkda/out/deps-src` and passed as `FETCHCONTENT_SOURCE_DIR_{ZYDIS,TOMLPLUSPLUS,IMGUI,SDL3}`.

## XDK 4721

`xbe_symbols` picks the signature folder by the D3D8 build (4721) and stops when `xdk/4721/signatures.toml` is
missing. Scanned with the 4831 set copied into a scratch `xdk/4721/`: 116 of 248 functions and 11 of 13 data, plus
8 by constants (`D3DDevice_Reset`, `SetTexture` (0.65), `Swap`, `D3D_InitDefaultState`, `XInputGetCapabilities`,
`XInputGetState`, `XInputSetState`, `XLaunchNewImageA`). Every address it found agrees with XbSymbolDatabase where that
has the name. The 5849 set finds 58 only, but among them all eight LIBCMT x87 bodies the 4831 set has no signature
for (`crt_sin` 0x1C64A1, `crt_cos` 0x1C63F1, `crt_tan` 0x1C6CE9, `crt_atan` 0x1C719F, `crt_sqrt` 0x1C6C2D, `crt_log`
0x1C815B, `crt_log10` 0x1C8237, `crt_pow` 0x1C6110): the CRT did not change between 4721 and 5849.

Filled from `symbols-xbsdb.txt` (marked `# from XbSymbolDatabase` in the config): `D3DDevice_Begin`, `EnableOverlay`,
`LightEnable`, `LoadVertexShaderProgram`, `Release`, `SetLight`, `SetMaterial`, `SetPixelShaderConstant`,
`Lock2DSurface`, `CDirectSoundStream_Flush` / `GetStatus` (`__r2`) / `Process`, `DirectSoundUseLightHRTF`,
`IDirectSoundBuffer_GetStatus` / `Lock` / `SetOutputBuffer` / `Unlock` / `SetNotificationPositions`, `XInitDevices`,
`XInputOpen`, `XInputClose`; `[data] XPP_MemoryUnitType = 0x229890` (`g_DeviceType_MU`; the game passes it to
`XGetDeviceChanges` at 0x143C2F, the gamepad record 0x22990C at 0x142D5F).

Not in this image (the D3D section's functions are all listed with their callers in the scratch listing; none of
these exists): `D3DDevice_SetIndices` (4721's `DrawIndexedVertices` takes the index pointer, so `D3D_IndexData`
is not needed either), `BeginPush` / `EndPush`, `BlockOnFence` / `InsertFence` / `IsFencePending`, `CopyRects`,
`FlushVertexCache`, `GetRasterStatus`, `GetTile` / `SetTile`, `GetVertexShaderFunction`, `PersistDisplay`,
`RunPushBuffer`, `SetGammaRamp`, `SetRenderTargetFast`, `SetVertexShaderConstant1Fast`, `SwitchTexture`,
`D3DResource_IsBusy` / `BlockUntilNotBusy`, `SetDepthClipPlanes`; no XNET/XONLINE (no socket or XNet names), no Bink.

`xdk/4721/layout.toml` (new in the toolkit): every key proven in this XBE's D3D8, DSOUND and XAPI code, cited per key.
The 4721 CDevice has the 4831 layout throughout (static device 0x1E10E0, miniport at +0x23C0); two 5849-only mirror
keys have 4721 values: `CDevice_VertexShaderHandle = 0x384` (SetVertexShader 0x1D23FE) and `CDevice_Material =
0xAB0` (SetMaterial 0x1D4F1C). `CDevice_ClipPlanes` stays unknown (no SetDepthClipPlanes in 4721). The render state
and texture stage state numbering is the one the runtime uses (deferred states from 92 at 0x1E0CB0, complex from 136
at 0x1E0D60, `D3D__Stream` 0x1E0DD8 right after state 165; stages of 32 at 0x1E0940).

`xdk/4721/signatures.toml` (new in the toolkit): `xbe_symbols --sign` of the completed config (a scratch copy with the
two render target getters signed too, see fault 1). A scan of this XBE with it reproduces the config exactly (145 of
249 functions, 12 of 13 data) except those two getters, which the config sets to 0 on purpose. Both files are
`recomp/patches/xboxxberecomp-0008-xdk-4721.patch` (new files only; `git apply --check --reverse` passes).
The config's `[xbe] xdk = 4721` makes `xbe_recomp` include `xdk/4721/layout.toml`; the codegen emitted the
`XDK_*` defines from it (`generated/game/image_symbols.h`).

## Engine map (Deception / Armageddon -> Deadly Alliance)

| what | Deception | Armageddon | Deadly Alliance |
|---|---|---|---|
| `_setjmp3` | 0x2993E8 | 0x3995EC | 0x1C6598 |
| main / `_setjmp(buf)` call / continuation | 0xDFE20 / 0xDFFBA / 0xDFFBF (buf 0x46CCC0) | 0x19C680 / 0x19C85E / 0x19C863 | 0xCABF0 / 0xCAD39 / 0xCAD3E (buf 0x2D4C00) |
| task exit | 0x13C0B0 | 0x1C5010 | 0xE0590 |
| class C Run / Yield (int3) / SetFunction | 0x13C130 / 0x13C1E0 / 0x13C430 | 0x1C50A0 / 0x1C5150 / 0x1C5180 | 0xE0610 / 0xE06C0 / 0xE06D0 (A's) |
| class A Run / Yield / SetFunction | 0x13C1F0 / 0x13C360 / 0x13C430 | 0x1C51C0 / 0x1C5340 / 0x1C5430 | 0xE0710 / 0xE0880 / 0xE06D0 |
| class B Run / Yield / SetFunction | 0x13C470 / 0x13C5A0 / 0x13C650 | 0x1C5470 / 0x1C55A0 / 0x1C5650 | 0xE0960 / 0xE0A90 / 0xE0B40 |
| B stack hops | 0x13C610 / 0x13C630 | 0x1C5610 / 0x1C5630 | 0xE0B00 / 0xE0B20 (A and C: `ret` 0xE0950) |
| vtables C / A / B | 0x3F5E48 / 0x3F5E70 / 0x3F5E98 | 0x55DB5C / 0x55DB84 / 0x55DBAC | 0x2704C0 / 0x2704E8 / 0x270510 |
| destroy slot | vtable+0x10 | vtable+0x0C | vtable+0x10 (run +0x14, yield +0x18, set function +0x24) |
| current task / wait time / destroy flag | 0x5FF3D4 / 0x5FF3D0 / 0x5FF3C8 | 0x7350C0 / 0x7350C4 / 0x7350B4 | 0x321428 / 0x321420 / 0x321418 |
| scheduler ret / esp / ebp / ss | 0x46DDAC / BC / CC / D8 | 0x5B8B40 / 50 / 60 / 6C | 0x2D4F54 / 64 / 74 / 80 |
| last yield ret / hop esp / SetFunction time | 0x46DDA8 / C0 / A4 | 0x5B8B3C / 54 / 38 | 0x2D4F50 / 68 / 4C |
| task scheduler (per frame) | 0x13BE30 | 0x1C4D70 | 0xE0340 (list 0x32141C walked by 0xE0390) |
| task walk / after a task's Run (continuation) | 0x13BE80 / 0x13C08B | 0x1C4DD0 / 0x1C4FEE | 0xE0390 / 0xE0575 |

Deadly Alliance's walk calls a task's callback at +0x28 (if any) before its Run (0xE055A, returning to 0xE055D),
then runs it if it is still alive. Projectiles and effects install one there (the fireball of Shang Tsung,
sub_001358A0, set at 0x135658; also 0x5A580, 0xD3B30, 0xDD340, 0xDDAF0, 0xF5140, 0xF6920), and a projectile that
is over ends itself from it through the exit 0xE0590, outside its task's function. The exit then does what it does
anywhere: the hook at +0x2C, the destroy if still alive, and a jump to the scheduler's return point [0x2D4F54] with
the stack [0x2D4F64], which is 0xE0575, the walk's next task. `task_fibers.cpp` reproduces that with a resume to
the continuation 0xE0575 (`game.toml`); returning into the callback instead read guest 0x64 and crashed (0.1.3).
Deception and Armageddon have no such callback, but Armageddon also reaches an exit outside a task's function (seen
at the end of a versus match, as the select screen loads), so both games take the same path to their own walks.

Main loop (continuation 0xCAD3E of `main` 0xCABF0): while [0x2D4C44] is 0: frame counters 0x42A888 (always) and
0x42A8A0 (when [0x321424] is 0), sub_000D7F80, ..., sub_000CB1B0 / sub_000CB2C0, sub_00143030 (controllers),
sub_00132AA0, sub_000CB3F0 / sub_000CB400, sub_000759F0 + sub_00125CD0 (when not paused), sub_000E0340 (task
scheduler), sub_0016B6DB, sub_00022F90, sub_0006D3D0 + sub_0004F870 (when [0x2CE200]), DirectSoundDoWork 0x1E4CAD,
sub_000E0E80.

## Library calls replaced in game code (`src/game/xdk_fixes.cpp`)

From `tools/py/lib_audit.py recomp/mkda research/mkda-rev1/symbols-xbsdb.txt --info research/mkda-rev1/xbe_info.json`:

- `D3DDevice_UpdateOverlay` 0x1DEAE0: the XMV movie loop sub_001C5D20 (PlayVideo: `XMVDecoder_GetNextFrame`
  0x202DB2 into one of two YUY2 textures, `GetOverlayUpdateStatus`, `UpdateOverlay`) shows frames on the video
  overlay through the NV2A's PVIDEO registers at [CDevice + 0x4FC] (0 on the host). Converted into the back buffer
  and presented through the runtime's Swap; +0x2BA8 gets the vertical blank count as the library stores it, so the
  recompiled `GetOverlayUpdateStatus` 0x1DEAC0 keeps pacing the loop.
- `D3DDevice_DrawIndexedVerticesUP` 0x1D4520 (callers 0x19FBF0, 0x1A00F1, 0x1A0159, 0x1A4CA0): its inline vertex
  array (method 0x1818) is not drawn by the runtime's push buffer walk; expanded into `DrawVerticesUP` 0x1D43C0.
- `D3DDevice_GetVertexShaderConstant` 0x1D1F10: reads the library's shadow at CDevice + 0xC58 + hardware register
  * 16 (`SetVertexShaderConstant1` 0x1D1D0A writes it), which the runtime's setters do not keep; sub_001C432B saves
  c-96..c-1 with it. Answered from `d3d::State().vsc`.
- `IDirectSoundBuffer_SetConeOrientation` 0x1E54E4 (caller 0x176D20): the library follows [this + 0x10] of the
  object below the interface, which the runtime's buffer object does not have; a no-op.

Left running console code on purpose: the resource creation and lock functions (memory only), `IDirectSound_GetCaps`
0x1E4B6A (reads DSOUND globals), `D3DDevice_SetRenderState_ZBias` (as in the other games), the XPP device functions
(the runtime keeps the device type records), the D3DX math, and the DSOUND jump thunks 0x1E4AFB (Release), 0x1E51B6
(Play), 0x1E51BB (Stop), 0x1E51C0 (GetStatus), 0x1E5CA5 (SetBufferData), which reach the bound functions.

## Bring-up loop

First lift (`xbe_recomp`, 15 s): 753 functions found directly, 977 pointer seeds, 2,624 by gap filling, 8,456 in all,
594,932 instructions, 1,972,707 of 2,228,668 code bytes, 193 jump tables, 685 overlapping decodes; the
`unimplemented` list is data decoded as code, as in the other two. Full debug build about 3 minutes (64 partitions).
No `[functions]` entries were needed so far (no missed task functions or thread routines reached).

Kernel imports: all resolved at boot (`boot: kernel imports resolved, 0 missing`); `kernel_apc.cpp` (export 232) is
in place as in the other games.

### Run 1: boots, legal screen, both opening movies (no fault)

The legal screen (Midway / Criterion RenderWare copyright, `attract.ssf`) shows at 60 fps for about 18 s, then the
first task longjmp to main's continuation 0xCAD3E (`tasks: 00D31070 longjmps to 000CAD3E`), `logo.xmv` (Midway logo)
and `intro.xmv` play through the `UpdateOverlay` override (`overlay: shown on the frame buffer, 720x480 format 24
onto 640x480 format 1E`; 30 fps while movies play). `build/shots/mkda-r1-5s.png` (legal), `mkda-r1-20s.png` (logo),
`mkda-r1-28s.png` (intro).

### Fault 1: D3DResource_Release recursing forever at the first fight load (66 s)

Symptom: `diag: exception C0000005 in MKDA.dll!__imp__sub_001D4CD0 [guest 001D4CF8 call 0x001D4CD0]`, 40 frames of
the same, then a write to guest D003FFFC: the main thread's guest stack overflowed, right after the arena and
fighter files of the first fight were opened.

Cause: `D3DResource_Release` 0x1D4CD0 releases a surface's parent (+0x14) when the surface's last reference goes. A
probe on it (since removed) caught a surface 0x44ADA0 (common 0x010D0002, data 0x0345F000, a 128x128 A8R8G8B8
level) whose parent was itself, from RenderWare's camera raster code: sub_0018D50C takes level 0 of a camera texture
(sub_0018D62E -> `D3DTexture_GetSurfaceLevel2` 0x1DDDD0, which makes a surface with the texture as parent and adds a
reference to the texture), sets it as render target (sub_0018D673) and releases it. The game also reads the current
targets back (`GetRenderTarget2` at 0x18D1C3 / 0x1C42DF, `GetDepthStencilSurface2` at 0x18D1ED / 0x1C4309) and
releases them. The runtime's host getters add a reference with `W32(s, R32(s) + 1)`, without the library's
`D3DResource_AddRef` 0x1D4C90 rule that a surface's first reference also references its parent texture; the game's
`Release` then dropped a texture reference it never had, the texture was freed while still in use, and a new surface
was allocated at its address (its parent pointer: the old texture, i.e. itself). Once that surface's count reached 1
the release recursed without end.

Fix (config only): `D3DDevice_GetRenderTarget2 = 0` and `D3DDevice_GetDepthStencilSurface2 = 0`, so the library's
getters run (they read CDevice +0x21B4 / +0x21B8, which the runtime keeps, and call `D3DResource_AddRef`). The same
host getters exist for Deception and Armageddon, whose games apparently never read a texture surface back.

## Results (2026-10-25)

Scripted path (`MKDA_PADS=2`, times from the first pad read; pads hidden for the first 1.5 s as in the other games):
START 36 s (skips the intro movie), START 42 s (title -> main menu: Arcade, Versus, Practice, Konquest, The Krypt,
Player Profile, Options, Kontent), A 48 s (Arcade), A 54 s (character select, Shang Tsung), A 60 s (ladder), A 66 s
(fight loads at ~66 s), then P1 presses X, A, Y, B, X, RIGHT+X, A, LEFT+Y every 1.5 s.

- r4 (180 s): fight 1 Shang Tsung vs Sub-Zero (ice arena) 67-176 s, won ("TIME: 01:50" on the ladder at 178 s,
  next Drum Arena). `build/shots/mkda-r4-46s.png` (main menu), `mkda-r4-72s.png`, `mkda-r4-95s.png`,
  `mkda-r4-140s.png` (round 2), `mkda-r4-178s.png` (ladder).
- r5 (335 s): fight 1 66-175 s, fight 2 vs Sonya 185-308 s (2 min 3 s of fighting), fight 3 loaded at 325 s; no
  fault, no trap (`build/shots/mkda-r5-230s.png`, `mkda-r5-290s.png`, `mkda-r5-333s.png`). CPU 120 s over 335 s,
  1.16 GB working set at the end.
- r6 (215 s, final build): fight 1 vs Kenshi 67-167 s, fight 2 vs Sub-Zero from 180 s; no fault
  (`build/shots/mkda-r6-150s.png`, `mkda-r6-212s.png`).

Log noise that is not a fault: `d3d: MakeSpace from 001D15ED` (`D3DVertexBuffer_Lock2` pushes a vertex cache
invalidate per lock until the scratch push buffer rewinds), `d3d12: CPU pixels drawn over target ...` after fights.
The XMV loop polls the pads (its skip callback) as fast as it can between frames, so `MKDA_PAD_LOG` floods during
movies; the console does the same.

## Controllers (for netplay later)

sub_00142D50 (init) and sub_00143030 (per frame, from the main loop): XPP gamepad record 0x22990C, XInput handles at
0x2EFD9C + port * 0x78 (capabilities at 0x2EFD80 + port * 0x78), P1 / P2 ports 0x266B24 / 0x266B28 (-1 none; a pad
that presses a button while unassigned joins through sub_00076B20). Per player logical buttons through the pointer
table 0x2813E8 (4 dwords per player): P1 current 0x2CE774, previous 0x2CE770, sticks 0x2CE7A0, new presses 0x2CE778;
P2 0x2CE780 / 0x2CE77C / 0x2CE7A4 / 0x2CE784; slots 2 and 3 follow, unassigned pads use 0x2EFC08 / 04 / 00 /
0x2EFBFC. Button map: 17 entries {analog, mask} at 0x281438, logical bits at 0x266B50 + i * 12. New presses are
(current ^ previous) & current, as in Deception.

The sticks never become a direction here (2026-10-26): the pad update only packs the four thumbstick shorts into the
sticks word (a byte each, 0x80 / 0x7F centred), and the only reader of a player's word besides it is sub_00075E30
(the camera of sub_00130320). Deception (sub_0006AC90, from its pad update) and Armageddon (sub_0007B9F0, from
sub_0026A9C0) OR the left stick into the d-pad's logical bits past a deadzone byte of 48 (0x3EBB24 / 0x54E5EC) with a
threshold of 0.0, so a controller's stick moved a fighter there and did nothing in this game. pads.cpp
StickPressesDpad now presses the d-pad from the left stick with the same deadzone (48 * 256 of the pad's 16 bits).

## Game layer for netplay (2026-10-25)

Deception's game layer (frame hooks, events, autopilot) for this game, in `recomp/mkda/src/game`:
- `frame.h/.cpp`: `Frame()`, `MainContext()`, `OnFrameStart` (the main loop's call of sub_000D7F80 at 0xCAD95,
  return 0xCAD9A, right after the counters) and `OnPadsRead` (after its pad update sub_00143030 at 0xCADEC, return
  0xCADF1; the movie skip check sub_00132EA0 also calls it); the per-player input block layout as constants.
- `events.h/.cpp`: game mode setter (logged), the main menu confirmation (counted; `MKDA_MODE` / `RequestMode`
  redirect), fight setup (`BeforeFightSetup`, `FightSetups()`), state switch (`StateSwitches()`), main menu task
  (`MainMenus()`), plus log-only hooks (fighters loaded, round start, player state setter, character pick, RNG seed)
  and, with `MKDA_EVENT_LOG=1`, a per-frame watch that logs health, round wins, round, time, winner and the pause flag
  whenever they change.
- `autopilot.h/.cpp`: `MKDA_AUTOPILOT=versus|arcade|practice` (also `kombat`), `MKDA_P1` / `MKDA_P2` (the game's
  name, the launcher's roster id from `launcher/data/mkda.json` such as `SubZero` or `BoRaiCho`, or the index;
  letters and digits compared without case, else one name containing the text or contained in it), unlocks all
  characters when either is given (`MKDA_UNLOCK_ALL=1` otherwise), `MKDA_AUTOPILOT_HOLD=1` stops at character select;
  the launcher API of Deception's (`WaitForRequest`, `Begin`, `Rematch`, `Restart`, `ExpectMenu`). It watches the
  main menu's selection, the select cursors and the players' states, never fixed timings.
- `pads.cpp`: the autopilot and `InjectPad` / `ParseButtons` as in Deception; with the autopilot on, `MKDA_INPUT`
  times count from the first fight setup, so a script can attack once the fight is there.
- `monitor.cpp`: Deception's debug monitor (`MKDA_MONITOR=<port>`, `tools/py/mkdbg.py` is the client; `frame`
  answers with 0x42A888 / 0x42A8A0). Removed with the netplay work: the shared one (`recomp/common/src/mk3d`)
  reads `MKDA_MONITOR` the same way.

How the state was found: the main loop's writes and reads (generated code comments, `q-mode.txt`); the game mode is
the 107-reader global next to the frame counters (as in Deception, 0x61C414 beside 0x61C3F8 / 0x61C418), its only
setter sub_0012FDE0 is called by the main menu confirmation sub_000E3CC0 (`q-menu.txt`, `q-menu2.txt`); the main
menu's item table 0x2371C0 (names, draw and confirm callbacks) leads to the menu system; the main menu dispatch
sub_000E25C0 to character select sub_0010C640 (`q-select*.txt`) and the fight task sub_0006E010 (`q-fight*.txt`);
the RNG by its tempering constants 0x9D2C5680 / 0xEFC60000; the file layer from the kernel thunks of NtCreateFile /
NtReadFile through XAPI's CreateFile / ReadFileEx (`q-aio*.txt`); the heap by its "OVERFLOW_HEAP" message
(`q-heap.txt`). Everything below was then checked in runs with the event log and the monitor.

### Results

All runs debug preset unless noted, `MKDA_EVENT_LOG=1`; the main menu is up at about 11.6 s (the autopilot's START
presses skip the legal screen and both movies), the fight setup at about 24.4 s.
- Versus, two pads (`MKDA_PADS=2`, Scorpion vs Sub-Zero): VERSUS (item 1) confirmed at frame 806, mode 8 -> 1, both
  players state 2; select at 958, P1 picks slot 4 (character 0), P2 joins with its first press and picks slot 9
  (character 19); fight task 1449, fighters loaded 1557, fight setup #1 at 1566 (mode 1, 0 vs 19), round 1 at 1567.
  `build/shots/mkda-ap2-17s.png` (select screen, both picks), `mkda-ap1-30s.png` (the fight, timer 59). Scripted
  attacks (`MKDA_INPUT`, P2 first then P1, `p2@` entries): P1 health 1.000 -> 0.700 from P2's hits, then P2 1.000 ->
  0.550 from P1's (`mkda-ap3-42s.png`, both bars down). A whole match (P1 attacking): P2 KO in round 1 (wins 1 0),
  round 2 at frame 3059, KO at 4131 (wins 2 0, winner 0), back to the versus select screen 8 s later (state switch
  priority 2 to 0x10C640). START mid-round: pause flag 1, the game counter held at 2289/2290 for 120 frames while the
  frame counter ran on.
- Versus, one pad (no `MKDA_PADS`, roster ids `JohnnyCage` / `Mokap`): one controller open, so ARCADE was confirmed and
  redirected to versus (both players in); port 1 plugged in at select, joined as P2 and picked MOKAP (slot 20 with
  DOWN held); fight setup mode 1, 14 vs 23 (`mkda-vs1-33s.png`).
- Practice (`Kano` / `Raiden`): PRACTICE (item 2) -> mode 6; port 0 picks both fighters in turn; fight setup mode 6,
  21 vs 10, timer infinite (`mkda-pr1-33s.png`).
- Arcade (`Blaze`): ARCADE -> mode 0, P1 picks BLAZE (slot 18 with DOWN held), ladder task 0xC7DF0, fight setup mode
  0, 22 vs 10 against the CPU (`mkda-ar1-38s.png`).
- Release preset (configured offline like the debug one, `out/build/release`, borderless window by default): boots to
  the main menu (`mkda-rel2-13s.png`, VERSUS selected by the autopilot) and reaches Kitana vs Reptile
  (`mkda-rel2-34s.png`; `rel1` is the same with an earlier build). The window captures there are 3840x2160 because
  the window is borderless full screen.
- Last check with the final code: `mkda-final-17s.png` (select), `mkda-final-43s.png` (both bars down; P1 0.700, P2
  0.550), and the one-pad redirect with the launcher's ids (`MKDA_AUTOPILOT=kombat`, `BoRaiCho` / `HsuHao`: fight
  setup mode 1, 20 vs 7, `mkda-final1pad-30s.png`).
- The fighters themselves are not drawn (shadows, blood and effects are; also in the earlier runs, e.g.
  `mkda-r5-230s.png`): a rendering problem, not of the game layer (fixed since: "The invisible fighters" below).
- A round that ends with both fighters at full health (time out, nobody touched) sends the game to the title
  (sub_000702B0): an idle check, seen in the first run.

### Engine map for netplay

| what | Deception | Deadly Alliance |
|---|---|---|
| main loop frame hook / pad hook (return address) | sub_00130390 (0xE000C) / sub_001C52F0 (0xE0011) | sub_000D7F80 (0xCAD9A) / sub_00143030 (0xCADF1) |
| frame counters always / unpaused | 0x61C3F8 / 0x61C418 | 0x42A888 / 0x42A8A0 (+1 while 0x321424 is 0) |
| pause | 0x620F20 bit 13 | 0x321424: the pause menus, a removed controller (sub_00076E90), front end overlays |
| pad update | sub_001C52F0, a block per port | sub_00143030, a block per player; ports of P1 / P2 0x266B24 / 0x266B28 (-1 none; next ports 0x266B2C / 0x266B30) |
| input block (netplay writes) | 0x62109C + port * 0x1C: previous +0xC, current +0x10, new +0x14, sticks +0x18 | 0x2CE770 + player * 0xC: previous +0, current +4, new +8; sticks 0x2CE7A0 + player * 4 (a byte per axis, 0x80 / 0x7F centred); pointer table 0x2813E8 (current, previous, sticks, new per slot; slot 4 = scratch 0x2EFBFC for pads on no player) |
| logical bits / player actions | per port button table 0x6210A0 + port * 0x1C | bit i = entry i of the map 0x281438 (A 0x40, B 0x20, X 0x80, Y 0x10, BLACK 0x8, WHITE 0x1, LT 0x4, RT 0x2, BACK 0x100, LS 0x200, RS 0x400, START 0x800, UP 0x1000, RIGHT 0x2000, DOWN 0x4000, LEFT 0x8000); actions read through each player's configuration P1 0x42C640, P2 0x42C560 (12 bytes per action, mask first): new presses sub_00075CA0(player, action), held sub_00075B50; actions 5 B, 6 A, 8 BACK, 0xB START, 0xC UP, 0xD RIGHT, 0xE DOWN, 0xF LEFT |
| game mode / setter | 0x61C414 / sub_001BC170 | 0x42A89C / sub_0012FDE0 (8 also written directly by 0xBEF83, 0xE2035) |
| mode values | 13 front end, 0 arcade, 1 versus, 4 practice, ... | 8 front end and attract, 0 arcade, 1 versus, 2 konquest, 3 the krypt, 4 options, 5 player profile, 6 practice, 7 kontent |
| fight setup (once per match) | sub_00083570 | sub_0006F680, called by the fight task sub_0006E010 9 frames after both fighters are loaded (sub_000719C0), in the task's last frame; zeroes round wins, winner -1, round 1, makes the health bars (sub_0006A140); the round loop sub_0006E290 starts the next frame (round start sub_0006F7F0 refills health) |
| player records | P1 0x620FC4, P2 0x621030 (0x6C apart) | no single record: per-player select descriptors 0x273DC8 / 0x273E00 (0x38 bytes: pointers to state, character, cursor, flags, ...) |
| health (float, 1.0 full) | record + 0xC | P1 0x42C774, P2 0x42C770 (refill = 1.0 * 0x273578 / 0x27357C) |
| round wins / rounds / winner | record + 0x40 | P1 0x42C76C, P2 0x42C768; rounds to win 0x31FD18 (2); round 0x42C778; round time 0x42C77C (seconds, -1 every 60 frames by 0x2CE5E8); round result 0x2CE638 (1 P1, 2 P2); winner 0x42C758 (0 P1, 1 P2, -1 none); match over 0x42C748 |
| character index | record + 0x54 | P1 0x271F00, P2 0x271F04 (0x18 none); names 0x243BDC + index * 0x110 (pointer first): 0 SCORPION ... 8 MOLOCH (not selectable), 22 BLAZE, 23 MOKAP |
| fighter object | record + 0x58 | P1 0x2EA540, P2 0x2EA544 (+0x144 character, +0x148 player), made by sub_001015C0(0x1001 / 0x1002) |
| menu confirm / action codes | sub_0014EF20(port, action) -> 0x46CDB8 | menu stack 0x2D4C98 (depth 0x2D4CFC, input while 0x2CE63C), selection 0x268D68 + menu * 0x1C (main menu 1: 0x268D84), items 0x268D58 + menu * 0x1C; main menu confirm callbacks sub_000CD8A0, sub_000CD980 (VERSUS), sub_000CD9C0 (needs a profile) -> sub_000E3CC0: item 0 -> mode 0, 1 -> 1 (8 with fewer than two gamepads, sub_00132E10), 2 -> 6, 3 -> 2, 4 -> 3, 5 -> 5, 6 -> 4, 7 -> 7, then 0x321404 = 1 for the page loop; confirming player 0x2D4CF8 |
| taking part | sub_0016FBC0(record, 1) | sub_0012FDF0(player, state) on 0x2EA550 / 0x2EA554: 0 out, 2 in (choosing), 3 picked, 4 demo (recorded input 0x42AAE4 / 0x42AAE8); VERSUS's callback sets both to 2 |
| character select | task 0x179A60, cursors 0x471B1C / 0x471B18 | task sub_0010C640 (versus, arcade, practice; set up by sub_0010CB00), cursors P1 0x2EA660, P2 0x2EA65C; 3 x 7 grid, UP slot - 3 (sub_00113AB0), DOWN + 3 (sub_00113E50), LEFT / RIGHT (sub_00112C80 / sub_001133C0); A sub_00111102 -> pick sub_001112A0(player) |
| grid -> character | 0x3F9210 + slot * 0x28 | 0x273E38 + slot * 0x2C (character +0, x / y +0x18 / +0x1C; 0x18 ends it); BLAZE / MOKAP: DOWN held while picking on slot 18 (RAIDEN) / 20 (CYRAX) |
| picked state | 2 | 3 (the select screen sets it back to 2) |
| unlock flags | 0x601E60, 0x601E68, 0x601E90, defaults 0x3F8FF0 | profiles 0x421530 (alternates 0x421534, rebuilt by sub_0010C0B0), built-in 0x273580 = 0x0038723D (alternates 0x2EA844); sub_00116380(character, alternate) |
| state switch / state task | sub_000E0190(priority, task) / 0x46CCB8 | sub_000CB140(priority, task) / 0x2D4BF8 (priority 0x2D4C40): makes the task (sub_000DF020(0x2001, 0x1B, task, 0, 0)) and longjmps to the main loop (buffer 0x2D4C00); screen state 0x2D4C4C (sub_0012FDC0: 1 title, 0xB main menu, 2 select, 3 ladder, 5 fight, 0xC continue, 0x14 switching) |
| main menu task | sub_000E3770 | sub_000E24E0 (state task, pushes menu 1), page loop sub_000E25C0; before it the attract state task sub_000238E0 runs the sequence 0x24C4A8 (index 0x42D358), the title screen sub_00023930 among it |
| engine RNG | MT variant: seed 0x14BDB0, draw 0x14BFB0, wrappers 0x1BC190 / 1A0 / 260 / 2D0 | the same generator: seed sub_000EC810 (once, from main 0xCACFB, with the boot time from timer sub_0012FA80(3): differs per boot), state 0x320A40 (624 dwords), index 0x320A28, draw sub_000ECA20 (sets 0x2E8404 when the index wraps; eight call sites then regenerate with sub_000EC8F0); wrappers sub_00130770 raw, sub_00130780(n) below n, sub_00130830(n) -n..n, sub_00130850(x) 0..x, sub_001308B0(x) -x..x; drawn from 331 functions |
| C runtime rand / srand | 0x29B8AC / 0x29B89F | rand sub_001C60C4 (_getptd() + 0x14); no srand linked (the seed stays 1); called from 0xFCDE0, 0xFEAE0, 0x151D00 to 0x151DB0, 0x153600, 0x371B0, 0x173A92 |
| engine heap | sub_001DD3B0 | sub_001550B0(heap, size, align, flags, file, line) -> sub_001550E0 (named heaps, overflow heap [0x2F4A90]); free sub_001554D0(block, file, line) (sub_0014ABC0); RenderWare's memory functions 0x2F0D80 use heap [0x31FA44]; sub_000D7F80 (first call of every frame) frees the blocks queued on 0x321488 when their countdown ends |
| async file I/O | I/O manager [0x5F0E90], scheduler pass 0x2911A0, worker thread 0x28D230, drain 0x2917B0 | the "mfl" file manager, main thread only: command queue [0x31CCCC] {head, tail, count}, commands from a pool (sub_00172010: +0x10 type, +0x14 state 0 queued / 2 done / 3 called back, +0x18 file, +0x1C callback(bytes, +0x20)); pump sub_001665A0: SleepEx(0, TRUE) (sub_0015BAE7) runs the ReadFileEx completions, then issues the head command: open sub_0016FAB4 (CreateFile overlapped, unbuffered), read sub_0016FD07 (ReadFileEx, 32 KB pieces through a bounce buffer, sub_0016FF07), completion APC sub_001702CA; pumped by the main loop (sub_00022F90 at 0xCAE21, the sound library's sub_0016B6DB at 0xCAE1C) and right after a command is queued; no I/O thread |

### What netplay will need

- The barrier: `BeforeFightSetup` runs inside the fight task's last loading frame. The arena loads first and its
  objects may run while the fighters load, before the barrier (as in Deception, where that made the RNG diverge); the
  RNG is seeded from the boot time, so it differs between two games from the start.
- The attract demo also sets up fights (mode 8, both players in state 4 on recorded input): check the mode.
- Input: the pad update writes a player's block only for the port assigned to it, and the game reads actions from the
  blocks alone (sub_00075CA0 / sub_00075B50: P1 0x2CE778 / 0x2CE774, P2 0x2CE784 / 0x2CE780), so the remote player's
  block should be writable after the pad update without a port (not tried). The button configuration is applied
  when actions are read (0x42C640 / 0x42C560): both games need the same two tables, or the bits have to be translated.
- VERSUS needs two open controllers in the main menu (report port 1 connected) or the redirect (`RequestMode(1)` /
  `MKDA_MODE=versus`). A second pad joins at character select with its first press, and that press also counts as
  that player's press.
- Pause: START in a fight (sub_000CB1B0, then sub_000CB2C0 pushes menu 0: CONTINUE, MOVELIST/PROFILE, PLAYER SELECT,
  MAIN MENU) sets 0x321424 and stops the game counter; a removed controller pauses too (keep remote ports connected).
  A controller that is plugged in (sub_00076B20 with 0) sets 0x2CE764 to 10: the pad update skips its work and every
  action reads 0 until that has counted down, on that game only.
- Match end: winner 0x42C758 set at the last KO; a versus match goes back to the versus select screen about 8 s later
  (`autopilot::Rematch`); the idle check above ends a session where nobody fought.

## The invisible fighters (2026-10-25)

Only the fighters' shadows, blood and effects were drawn. The fighters are skinned by RenderWare's own vertex
programs, whose viewport constants (the c-96 offset and the projection rows) sub_001C3E80 computes from the back
buffer's description: the library's Get2DSurfaceDesc 0x1D71F0 reports the device's multisample type (CDevice +
0x211C in XDK 4721) for the back buffer, and RenderWare looks it up in a table of the eleven console types. The
runtime's CreateDevice clears the device object, so the type read 0; the lookup ran past the table, the scale came
from whatever followed it, and the fighters were drawn far off screen. The shadows go through the 256x256 shadow
render targets (not the back buffer) and were right. Found by capturing a fight frame (`gpu::g_capture`;
`MKDA_CAPTURE=<seconds>` in `diag.cpp` arms it that long after the first fight setup) and following the fighters'
draws back to their constants.

Fix, game-local (`xdk_fixes.cpp`, `KeepMultiSampleType`): at every frame's top the field is set to 0x11
(D3DMULTISAMPLE_NONE) when it holds anything else; the runtime draws without the console's multisampling anyway. No
runtime file is changed. The general fix would be in the runtime's `xdk/d3d8.cpp` CreateDevice and Reset: write the
presentation parameters' multisample type (or 0x11) into the device field that holds it, as the console's
CreateDevice does, with the field's offset a per-XDK layout key (`CDevice_MultiSampleType`, 0x211C for 4721) next
to the others in `xdk/layout.h`; Deception and Armageddon may read it the same way.

Checked: `build/shots/mkda-fix1-36s.png` (both fighters), then every fight since (`mkda-st1-45s.png`,
`mkda-ar2-38s.png` Blaze vs Li Mei, the netplay shots below).

## Netplay (2026-10-25)

On the shared core (`recomp/common/src/mk3d`, namespace `mk3d`), as Deception uses it; `CMakeLists.txt` adds its
sources and include directory.
- `game.cpp`: the variable prefix (`mk3d::EnvPrefix()`: `MKDA`), and the 4721 device's vertical blank fields
  (0x1E10E0 + 0x2550..0x259C, written by the runtime's vblank thread) as fixed volatile bytes for the rollback.
- `frame.h/.cpp`: `mk3d::Frame`, `MainContext`, `OnFrameStart`, `OnPadsRead` on the two main loop hooks.
- `diag.cpp`: `mk3d::DescribeCode`. `pads.cpp`: `mk3d::ParseButtons` / `InjectPad`; the physical pads read nothing
  while the launcher's autopilot runs (`mk3d::link::SuppressPhysicalPads()`); while a match is in sync each port's
  connection is latched at its first read (ports 0 and 1 connected), so a pad pulled out reads as connected and idle
  and one plugged in stays unseen: the insertion path (0x2CE764 = 10, every action 0 for ten frames) and the removal
  pause can no longer happen on one game only.
- `task_fibers.cpp`: the engine's tasks on the shared snapshot-able fibers (`mk3d/sfiber.h`), with
  `mk3d::tasks::Save/Load/Alive`.
- `rng.cpp`: in a match in sync the draw sub_000ECA20 returns a hash of (seed, frame, call site, count), the call
  site being where the outermost wrapper (sub_00130770 / 780 / 830 / 850 / 8B0) was called from.
- `heap.cpp`: new engine heap blocks (sub_001550B0) are zeroed on the main thread.
- `aio.cpp`: the file manager (main thread, ReadFileEx APCs) is drained at the end of every recorded frame, so no
  completion crosses a frame's top; `LoaderSettle` drains it too. The reads go through NtReadFile, so
  `kernel::RecordedFileReads()` sees them (the core marks frames 1-60 and the state switch at the end as loading).
- `netgame.cpp` (`mk3d/netgame.h`): input is the logical bits of the player on port 0 (P1's block, else the scratch
  block of pads on no player) without START and BACK (logical 0x800 / 0x100, read once from the button map 0x281438
  / 0x266B50), written to both players' blocks (0x2CE770 + player * 0xC, sticks 0x2CE7A0 + player * 4). The meeting
  point is the fight setup sub_0006F680, except in the attract's demo fights (mode 8). READY words: mode, both
  characters, both frame counters, the arena, five options (rounds 0x31FD18, round time 0x31FD1C, fatalities
  0x31FD24, bonus games 0x31FD20, difficulty 0x31FD14) and this player's button configuration (17 actions x 3 words
  of 0x42C640 or 0x42C560, whichever player port 0 is). `Agree`: the larger frame counters, the left player's
  options, P(side) gets this game's configuration and the other player the other game's, so each player keeps their
  own buttons and both games hold the same two tables; mode, characters and arena are only checked (logged when
  they differ). `Reseed`: sub_000EC810(seed), 0x2E8404 = 0, and the C runtime's rand state (_getptd sub_001CA1EE +
  0x14) = seed. `MatchOver`: the mode changed, the front end (main menu or select task), or the attract task
  0x238E0 (a round nobody touched). Checks hash health, wins, characters, mode, round, time, winner and both fighter
  objects (0xE2C bytes, pointers left out); rand sub_001C60C4 is counted (`mk3d::net::CountRand`) from the
  barrier's reseed on. Some arenas draw from it while they load (two bursts of 105 as their props arrive, then 7 a
  frame from frame 15): one game's loader finished the bursts in the setup frame, the other's at its frames 1 and 8,
  so the draws (how many, the last) go in a check only once neither game is loading (netplay.cpp `Checks`); a check
  that counted them from the sync's first frame, keeping the last value of the match before, parted two games at
  frame 0 that played alike.
- `linkgame.cpp` (`mk3d/linkgame.h`): the autopilot's launcher API; the arena comes from the launcher's seed before
  the fight loads (the arena list 0x24D800 filtered by the built-in open mask 0x273584, 10 arenas; seed % n, or the
  named stage), set through the engine's own chosen arena 0x42C754 right before the pick sub_0006F570
  (`events.h` `SetNextArena`); rounds to win from the launcher, bonus games off (0xB); `Park` is the state switch
  sub_000CB140(4, 0xE24E0) (the pause flag cleared first, as the pause menu's quit does); the result is the settled
  round wins against the rounds needed.

### Results

- Sync test (`MKDA_SYNCTEST=1;MKDA_SYNCTEST_FRAMES=900`, versus autopilot Scorpion vs Sub-Zero, both players
  attacking from `MKDA_INPUT`): 900 frames, 0 mismatches, debug and release (`build/shots/mkda-st1-45s.png`,
  `mkda-st2-45s.png`, `mkda-strel-45s.png`; P2's health 1.0 -> 0.12 and P1's 1.0 -> 0.80 inside the tested frames).
- `tools\netplay-local.ps1 -Game mkda -Port 7201` with `MKDA_NET_LAG=70;MKDA_NET_JITTER=30;MKDA_NET_LOSS=3`, both
  players scripted: one whole match each (to the return to the select screen), rbcmp 0 differing frames: `mkda6`
  release 3871 frames (232 rollbacks, deepest 6), `mkda3` release 3878, `mkda5` debug 3864, `mkda1` debug 3292 (its
  log also holds a second, unsynced match the scripts started from the select screen after the first: different
  picks on the two games, logged as "match setups differ" and a desync at frame 0, as expected without a launcher).
  Matching shots: `mkda6-a-88s.png` / `mkda6-b-88s.png` (SCORPION WINS, two skulls, time 40), `mkda5-a-75s.png` /
  `-b-`, `mkda3-a-75s.png` / `-b-`, `mkda1-a-80s.png` / `-b-`.
- A match nobody touches (`mkda4`, release, Kitana vs Raiden): round 1 times out at full health, both games go to
  the attract task at the same frame and leave the match there (3926 frames, 0 differing); shot `mkda4-a-105s.png`.
- `tools\online-e2e.ps1 -Game mkda -Port 7877 -Exe recomp\mkda\out\build\release\MKDA.exe -Seconds 300
  -CharB SubZero` (`-CharB`: the default Kobra is not in this game): both games `result winner=2 p1_rounds=0
  p2_rounds=2`, the server logs RESULT; the same with `-Queue lobby`, with `-Queue lobby -Matches 2` (two matches,
  park and start again, the arena from each match's seed) and with the debug build. Net logs 0 differing frames
  (`build/netplay/mkda-e2e-*`). The debug run's logs show the launcher flow: hold point, start (seed, picks,
  rounds), `link: arena 1 (seed ..., 10 open)` on both, the requested arena loaded, the meeting, the result, the park
  straight from the fight task to the main menu, the hold point again.
- Autopilot regressions after the changes: practice Kano vs Raiden (mode 6), arcade Blaze vs the CPU (mode 0).

### Open

- The attract's demo fights are kept out of netplay by their mode (8); no demo fight came up within the 170 s
  watched (the intro movie is long), so that filter is unobserved.
- A practice match through the launcher (offline) was not run; the practice autopilot itself works.
- The two games may leave a finished match one frame apart (the core leaves at the first agreed frame top that
  shows it over); that is after the result, and the next match's meeting point agrees again.

## Frame data in practice (2026-10-28)

Practice shows player 1's last attack (`recomp/common/src/mk3d/framedata.cpp`, this game's side
`src/game/framedata.cpp`). Deadly Alliance has the rules of Deception (research/mkd/notes.md, "Frame data in
practice") with its own offsets, but no attack phase field. Found with the debug monitor stepping a versus fight and
both players pressing a new button every frame for the ground truth:

- Fighter objects (static, 0xE2C bytes) P1 0x2E88D8, P2 0x2E9704 (pointers 0x2EA540 / 0x2EA544), their tasks
  [0x2EA530] / [0x2EA534] (+0x30 the task's function). Game frame counter 0x42A8A0, game mode 0x42A89C (6 practice),
  the fighter whose task runs 0x320A18.
- Fighter fields (Deception's in brackets): +0x154 strike contact 0 none / 1 hit / 2 blocked, reset at every strike
  [0x1BC], +0x160 no blocking until, +0x164 stance wait until [0x1D4], +0x168 no new move until [0x1D8], +0x194 state
  [0x214], +0x19C controls: the player number its actions are read for [0x21C], +0x20C CPU flag [0x294], +0x9F8 the
  move script's current command (a strike's stays the same for its whole window), +0xDFC strike kind (0xF neutral),
  +0xE18 block-hold entries [0x6AC].
- Move gate sub_000EA250 (Deception's rule, and player state 3 or 4: practice's dummy has 0, so the panel leaves that
  term out); strike sub_00054900 (and sub_00054CF0, sub_00055000, moves' own loops), its hit test sub_000547B0 (from
  the frame after the strike starts); block held sub_00057E20 -> sub_00075B50(player, action) (action 1 block, 0xE
  down); block hold sub_000E7480, crouching hold sub_000E7B70 (state 0x900); neutral sub_00053070 -> sub_000BFB00
  (reset sub_00050E40, waits for +0x164) -> stance sub_000BFCC0.
- Practice: no port drives the dummy (P2 port -1, player state 0); the pause menu is CONTINUE, MOVELIST/PROFILE, PLAYER
  SELECT, MAIN MENU, with no dummy settings.

Scorpion against Scorpion (move frames, the press is frame 1; "mashed": checked in versus against both players
pressing every frame, the rest from the practice panel):

| move | startup | active | player 1 can act | on hit | on block | crouch block |
|---|---|---|---|---|---|---|
| X | 12 | 6 | 37 (mashed) | +9 hitting on frame 12 (mashed), +12 on frame 15 | -21 hitting on frame 12 (mashed), -19 on frame 14 | whiffs over it |
| Y | 16 | 2 | 30 | +7 | -10 | |
| B | 14 | 7 | 38 | +10 | -20 | |
| crouching A (a low) | 19 | 2 | 50 | +34 | | -18 (mashed) |
| A, 2 hits | 12, again 25 | 6 (7) 5 | 51 (mashed) | +5 (mashed) | -22 | |

## Practice record (2026-11-09)

Practice records the dummy and plays it back (`recomp/common/src/mk3d/record.cpp`, this game's side
`src/game/record.cpp`): F9 records, F10 plays, F11 the slot, F12 once / repeat / random. What it took in this game:
- Controls are per player, not per port: the pad update sub_00143030 writes P1's block (0x2CE770: previous, current
  0x2CE774, new presses 0x2CE778; sticks 0x2CE7A0) from the port at 0x266B24 and P2's (0x2CE77C / 0x2CE780 / 0x2CE784;
  sticks 0x2CE7A4) from 0x266B28, -1 none, and a press on a port on no player joins it (sub_00076B20). A fighter's
  actions are read for its player (+0x19C) by sub_00075B50(player, action) (held: current & the player's
  configuration mask, P1 0x42C640, P2 0x42C560, 17 actions of 12 bytes) and sub_00075CA0 (new presses), both
  returning 0 while the player's word at 0x2CE7B8 + player * 4 is set, and a player in state 4 (the attract demo)
  reads 0x42AAE4 / 0x42AAE8 instead.
- The fight task (sub_0006E010) sets practice's dummy up as player state 0 with 0x2CE7BC = 1 (no controls), and the
  move gate sub_000EA250 starts no move for a player whose state (0x2EA550 / 0x2EA554) is not 3 or 4. Driven, the
  dummy has state 3, 0x2CE7BC = 0 and P1's configuration copied over P2's, and its block is written right after the
  pad update (OnPadsRead); the dummy is the game's own again while the pause menu is up (so a PLAYER SELECT or MAIN
  MENU from it finds state 0; the select setup sub_0010CB00 turns a 3 into 2) and when the fight ends.
- Left means forward or back by sub_00057B60 for the fighter whose task runs: the control object's (fighter +0x28, id
  +0x2C; sub_00102F20) flags +8 bit 17, turned by the sign of sub_000543F0 (the cross product of the camera's lines to
  the two control objects, x +0x84 and z +0x8C, camera object [0x42CBF4]), and by bit 3 of +0x30 of the animation
  state sub_000E0300 gives (left out here). The word is true when right is forward (a dummy on the right of player 1:
  false). Each recorded frame keeps the dummy's facing; played facing the other way, LEFT and RIGHT (logical 0x8000 /
  0x2000) are swapped.
- Tested with `tools/py/practice_record_test.py` (release build, the debug monitor's pad command and window keys):
  the dummy walks in and hits on playback from the same side and, after player 1 jumps over it, from the other
  (mirrored); a pause in a recording records nothing.

## The online select screen (2026-11-02)

Players who pick on the select screen (the launcher's `p1=- p2=-`), brought in line with Armageddon's:
- Its own time: every input handler of the select screen, and its setup sub_0010CB00, call sub_0010D250 (fastcall,
  no arguments), which ends and starts again task 0x203E (sub_0010D710): it sleeps 2700 frames, then with 0x2EA83C
  set (the screen is up) resets the picks and switches to the main menu (sub_000CB140(4, sub_000E24E0) from 0x10D82A,
  about 46 s without a press). 0.1.17 kept it off by moving the waiting side's cursor every 8 s, and picked the
  cursor's character for a player after 75 s. Online and watched the link now skips sub_0010D250
  (autopilot HoldsIdle): there is no count, and nobody's pick is made for them.
- No pick lag: the pick sub_001112A0 takes the grid's character under the cursor as it is confirmed. A direction and
  A two frames apart can pick the slot the cursor was on (the direction's handler had not run yet): the game's own
  order, and the pick word carries what was picked.
- Leaving while choosing: B and BACK (the select loop's check sub_000CE000, logical actions 5 and 8 of either
  player), and RT and LS left the screen as well in a probe (port 0, offline). The pads keep all four from the game
  while this PC's player picks online; BLACK and START stay. A B that got through anyway (the monitor's, past the
  pads) took the game to the main menu, the autopilot's A presses into ARCADE, and the meeting point found the
  setups different.
- The hold point and the attract sequence (2026-11-02): a launcher's game waiting at the main menu (task 0xE24E0) with
  nobody pressing anything goes over to the attract state task 0x238E0 (title, movies, demo fights) after ~2760 frames
  (46 s; release, idle probe scratchpad r19/idle_probe.py) and stays there; the next start's menu presses (UP/DOWN/A)
  then found no menu ("the main menu did not take item 1", a spectator's game that waited for its match). The
  autopilot now presses START while the attract task runs, at the hold point and before a start's menu choice
  (BackFromAttract), as at boot.
- The arena choice: BLACK while choosing (sub_0010EC90), with both players in (P2 joins with its first press on
  port 1; X moves nothing, so the autopilot has a waiting P2 join with X at once) and nobody's choice open: +0x800 in
  the player's word *(0x273DD4 + player * 0x38) (the word events.h kCostume points to), index 0x2EA658 (-1 to 0),
  and the panel slides in (task 0x204C: START does nothing until it is in). LEFT / RIGHT move the index; START
  (sub_00111F60) stores table 0x274398[index * 3] in 0x42C754 (sub_001121F0) and clears +0x800. There is no backing
  out: B and a second BLACK leave it open. Nobody picks while a choice is open (sub_00110F90 returns). The setup sets
  0x42C754 to -1; the fight's arena pick sub_0006F570 takes 0 to 24 from it and sets -1.
- sub_0010D2B0 resets the screen's state: at the setup (sub_0010CB00) and at the select task's end, before the
  switch to the ladder, Konquest or the fight (calls 0x10CAAE, 0x10CAC4, 0x10CAD8). There the link puts the agreed
  characters in 0x271F00 / 0x271F04 and the arena the two choices give (pick words [character, costume, arena or
  0xFFFFFFFF]: both chose, the seed's bit 7 takes P2's, else P1's; one chose, that one) into the fight's arena pick
  (SetNextArena); neither, the match's own from the seed (PickArena).
- Tests (release, two games and a spectator on a local relay, one round): 60 s without a press (3000 frames) left
  all three screens as they were; the other screen and the spectator's followed the moving cursor (one step behind
  9 frames after a move, 19 and 37 of 67 samples on the same slot; settled alike 1.5 s after the last move); one
  choice (3, also on a rematch), two (1 and 4: 1 with bit 7 clear, 4 with it set), none (the seed's 14), and RT,
  LS, B, BACK, then BLACK, B, BACK, RIGHT, START through the player's own pad (the screen stayed; arena 1): all games
  loaded that arena. 0 frames differ between the players and against the spectator in every match (1433 to 1861
  frames).

## Combos, reach and blocking (0.1.22, 2026-11-03)

Found with the debug monitor stepping versus fights (scratchpad r30: `versus_trace.py`, `stringedge.py`,
`jaxblock.py`) and the generated code:

- Fighter states (+0x194) go through the setter sub_000565C0(state) for the fighter whose task runs. Besides 0 (stance),
  0x12xx / 0x13xx (attacks), 0x20xx (walking: 0x2000 forward, 0x2001 back, 0x2003 sub_000C0630) and 0x06xx (hit
  reactions, 0x600 to 0x604): 0x100 / 0x101 crouching (sub_000BFB00, sub_000C0290 the crouch, sub_000E7B70 letting go
  of block with down held), 0x900 / 0x901 crouch blocking (sub_000E7110 starting to block, sub_000E7B70 holding it,
  sub_000E7480), 0xA00 to 0xA03 standing block (sub_000E7480 holding it, its reactions), 0x42xx down and getting up,
  0x3200 to 0x3203 jumps. A hit's victim who crouch blocks goes from 0x600 straight to 0x900, and the game clears its
  combo count (+0x1A0) there; the 0.1.21 rules took only 0xA00 for blocking and kept the combo going until the
  victim stood (Bo's puddle, Frost's Ground Ice held back). 0.1.22 takes 0x01xx and 0x09xx as free too.
- A strike's reach: the hit test sub_000547B0(kind, scale, reach) stores the kind at +0xDFC and calls sub_00047DE0 ->
  sub_000483D0(kind, scale, reach), which walks the limb list 0x25B18C[kind] (ids, a negative one between limbs, 0
  at the end), finds each id in the table at 0x2324A8 (0x14 bytes an entry: id | 0x1000, a side flag at +4 for the
  mirrored limb, the radius at +0xC), and puts a sphere of scale x radius on that bone. With a reach, the end of each
  limb gets one more sphere that far past the last one along the line from the one before (the length is (L + reach)
  / L of that segment). Then sub_00048A80 tests the spheres against the other fighter: 1 a hit. The strike loop
  sub_00054900 passes reach 0.
- Moves run scripts: +0x9F0 is the block of the move's script (it starts with the names of its animations,
  "drunken_2", "mizong_entry_2center", ...), +0x9F8 the command it is at, which stays the same through a strike's
  window. Bo' Rai Cho's Mi Zong 2, 2, 1, 1, style change (Y, Y, X, X, LT; 5 hits, startup 11, hits on frames 10,
  26, 45, 61 and 79 from 1.24 apart) strikes from +0x5C4, +0x688, +0x76C and +0x810 of the "drunken_2" block and,
  after the style change (routine 0x11C360), from +0x1D4 of the "mizong_entry_2center" block. Each hit pushes the
  victim about 0.4 back and the next strike lunges in again; started from the first hit's longer reaches (1.25 apart
  and more), the later hits came out short: 4 of 5 in one of three tries against Scorpion on 0.1.21 (the user's clip:
  "4 hits, last on frame 63"; Mavado: 2 of 5 started 1.25 apart, the second hit landing late on frame 29).
  `recomp/mkda/src/game/combos.cpp` makes hits 2 to 5 test with their spheres 2.5 times larger (scale x 2.5) while
  the victim is in a hit reaction from the hit before: all five landed on all 23 fighters. A reach did not do it:
  the limb lists for kinds 6 and 7 (0x25B18C: [15, 18, 20, 22, 24] and [17, 19, 21, 23, 25], the arms, radius
  0.125 at the hand) end in the hand, and the extra sphere goes along its last bone, up for an upright palm strike.
- A body point of each fighter: +0xA28 / +0xA2C / +0xA30 (x, y about 1.0, z), recomputed every frame (a write does
  not move the fighter); the distance between the two fighters' x and z is about 1.0 when a jab lands, 2.6 at a
  round's start.
- Blocking and the move gate: standing (sub_000BFCC0) and crouching (sub_000C0290) check block first each frame,
  through sub_00057E20 (not in 0x605 or 0x3202, +0x158 and +0x15C clear, +0x160 no later than now, 0x42C744 set, the
  block action held), which leaves out the move gate's +0x168. A strike that lands is blocked or not by sub_00057F30
  (sub_00054710 writes the contact): the same flags, whatever the victim is doing, so a fighter holding block in the
  reset between a move and stance (sub_000BFB00, which does not block itself) still blocks a hit. A move's end sets the
  waits apart: sub_000558E0 only +0x168 (from the move's +0x1EC), the recovery sub_00055AC0 also +0x164 and +0x160.
  So the block and the next attack open at different frames: Jax's Piston Punch (down, forward + 1, six hits) clears
  +0x158 at move frame 76 and keeps the gate shut until 94: on block -9, -27 to attack; its last hits push the blocker
  from about 1.6 to 2.3 apart, so nothing that fast reaches. Scorpion's B opens the gate at 37 and the block (+0x160) at
  42: -25 on block, -20 to attack. Measured (2026-11-07, scratchpad r51 punish.py, verify.py): a 9 frame jab put in
  reach lands on the last frame the rule says no and is blocked on the first it says yes, on Jax's X and Piston Punch,
  Scorpion's X and B, Hsu Hao's X. Of the 1,000 or so moves on the move lists that a standing blocker blocks, about 100
  open the two apart (attack first by 5, 10 or up to 25 frames in most; block first in 17). The frame data follows
  both since then (Adapter::block_rule): the number is the next attack's advantage when that is ahead, else the
  block's, the other beside it ("On block -9, -27 to attack").
- A projectile's strike (Jax's Machine Gun) writes contact 1 when it is blocked too; the victim's combo count
  (+0x1A0) stays 0 on a block, so the frame data takes a contact as a hit only when that count is set.
- Tech rolls: a knockdown that can be teched runs the check sub_00051290 for the fighter as it lands (up or down
  rolls, away rolls back, the computer rolls at random; a roll switches the fighter's task to 0xE4A90, 0xE4AF0 or
  0xE4BA0). Ten reaction routines call it: sub_001183C0, sub_00119280, sub_001198A0, sub_00119AD0, sub_0011A160,
  sub_0011A250, sub_0011A350, sub_0011AA50, sub_0011BC40, sub_0011DB60. Untechable knockdowns run twins without the
  call: Scorpion's uppercut (down + 3) knocks down through sub_00119280 (the check about 45 frames after the hit, then
  sub_00057690 down on the floor, sub_00053160 up again), Bo' Rai Cho's sweep (back + 3) through sub_00119390 (the same
  calls but the check), Frost's slide through the scripted reaction sub_0011AE80 and sub_00053160. The game clears a
  victim's combo count (+0x1A0) about 22 frames into the knockdown. Since 0.1.23 the rules against infinites end a
  combo there (noinfinites.cpp hooks sub_00051290): landing from a knockdown that can be teched, rolled or not, the
  fighter is free until hit again; untechable knockdowns still count on.
- Who may block after a hit: a hit's reaction kind (the table at 0x2455D8, 0x14 bytes a kind, its +4 read by
  sub_0011B050) leaves the victim unable to block (1: +0x15C set), or clears +0x158 and +0x15C (0), or those and +0x160
  (2). The end of the attacker's move lets it go: sub_000558E0 (at 0x559B1) and the recovery 0x55AC0 call sub_00057DA0,
  which clears the other fighter's +0x15C; the fighter's own reset sub_00050E40 (from 0xBFB00, 0x53070) and
  sub_00057D60 (+0x158, +0x15C, +0x160) clear its own. Frost's slide (back, down, forward + 4; sub_000EBFD0: blocked,
  state 0x4206 and 0x53070, else 0x53130 then 0x53070) ends without it, so a fighter it knocked down stayed unable to
  block through its whole get-up or roll, while one knocked down by his kicks blocked lying, getting up and rolling
  (the user's match 2026-11-07, Frost vs Bo' Rai Cho, replayed exactly: a slide under the juggled Bo, then Frost's
  Yuan Yang 4 as he landed hit him 12 frames into getting up, or 26 frames into his 31-frame roll, and juggled him
  again). The tech check (sub_00051290: away rolls back 0xE4A90, up 0xE4AF0, down 0xE4BA0) runs as the fighter lands
  from the ten techable reactions; since 0.1.33 noinfinites.cpp clears +0x158, +0x15C and +0x160 there for a human
  fighter, as sub_00057D60 does, so it may block from landing on, whatever knocked it down.
- Bo' Rai Cho's Puke Puddle (0.1.24, scratchpad r32: `slip_trace.py`, `trade_trace.py`, `block_diff.py`): the puke
  lands about 42 frames after the 2 under the opponent, wherever it stands; stepping in counts as a hit (+0x1A0 1, a
  frame of 0x600) and starts the scripted reaction 0x11AE80 with state 0x4207, about 180 frames of slipping, then the
  recovery 0x55AC0 (still 0x4207) and stance. The slip sets the fighter's +0x15C, which keeps a hit from being blocked
  (a hit's victim gets it too; +0x158 is set while a fighter attacks), until the fighter's own reset clears both.
  sub_00057DA0 clears the OTHER fighter's +0x15C ([0x320A0C]): the end of a move (sub_000558E0 and the recovery
  0x55AC0 when +0x1F0 is set) and the reaction script command sub_0011DBA0 call it, which is how a combo's victim may
  block again. In a trade (the opponent's jab or forward + 1 hits Bo as the puke leaves him, 16 to 28 frames after
  the 2 at about 1.0 apart), Bo's reaction ends about 6 frames after the slip began and cleared the slipping
  fighter's +0x15C: it blocked Bo's follow-up (0x11C260, 0x11BD80, then the standing block 0xE7480). combos.cpp keeps
  +0x15C while the other fighter is in 0x4207; after the slip the fighter blocks as before.
- The fighter blocks the desync checks hash hold pointers that change in a fight (+0x9F0 / +0x9F8 the move script,
  +0xD38 the animation sub_001030A0 copies from the animation player [0x3209FC]+0x2C, +0xBC set by the slip); the
  hash leaves pointer-like words out (netgame.cpp Pointer: 0x10000 to 0x4000000 and from 0x80000000, which also
  takes in negative floats).
- The back dash: the press handlers sub_00126E00 (+1) and sub_00126F40 (-1) run for a press left or right whatever the
  fighter is doing (the tables 0x246640 and 0x246698 by [0x2D4C4C], 5 in a fight) and call sub_00126E20(dir): two presses
  away within 13 frames of the count 0x42A888, each stamped in +0x1C0, no wait for 0x6208 (sub_00050D90) and no bit of
  0x4200 in the state put the fighter's task on sub_000E5B60 (state 0x6208, about 20 frames, ending in a 40 frame wait
  for the next one). A move's reset to state 0 opens it, before the move gate +0x168 ends, which the dash does not read.
- Move scripts (the .ssf's type 7 sections): four words (entries, code size, names size, 0), then per entry its name's
  offset and its code's in words, the names, the code. A command is a word (top byte 1, or 2 for the end; next byte the
  argument words; low 16 bits the opcode, handlers put in 0x31FD40 by sub_0011D230) and its arguments; +0x9F8 already
  points at the next command while one runs, so a strike is known by the command after it, counted from +0x9F0, the name
  of the entry the script started at. Opcodes: 0x8A strike (sub_00054900 to an animation frame), 0xD wait to an animation
  frame, 0xE wait to so many before the end, 0x8 end with the recovery sub_00055AC0 (cap, +0x1EC, +0x164, +0x160, +0x1F0,
  +0x1F4 turn around), 0x75 start the attack.
- Bo' Rai Cho's Jojutsu forward + 3 (jojutsu_3_towards, both costumes): strikes from commands +0x918 and +0x94C, then
  the wait and the recovery (5, 5, 15, 1, 1), so 12 frames after the second strike's last the gate shuts for 5 more
  and the block for 15: recovery 16, -19 to attack, -29 on block.
  Since 2026-11-09 -14 on block when the second strike is blocked (noinfinites.cpp ForwardThreeBlock and the hook of
  sub_0011D560: the block opens at the top of the frame 12 after that strike's last); -19 to attack, on hit and on a
  whiff as they were.
- His Jojutsu standing 4 (jojutsu_4, the command after its recovery +0xAD8) and 2,4 (jojutsu_2, +0x5E0; a 2 alone has its
  own recovery): 14 frames of block time in the script, -27 and -25 on block. Since 2026-11-09 the block time is 1 and 4
  when the last strike is blocked (the hook of sub_0011D560): -14 on block for both, -27 and -25 to attack, on hit and on
  a whiff as they were. The contact word +0x154 is cleared when a strike starts, so a blocked 2 and a 4 that misses is
  not this case. Jax's X pressed at his first chance hits, a frame later it is blocked, on both moves.
- The Puke Puddle's test: button 2's routine sub_000E97A0 offers it only while the opponent's count +0x1A0 is 0
  (0xE98CE). The count goes back to 0 only on the way to stance (sub_00056480, called from sub_00050E40, 00057510,
  00117DA0 and 0011D740), so a fighter who was hit and goes from the reaction straight into holding block (0x900) keeps
  1 for as long as it holds, and back, forward + 2 comes out as a plain 2. Since 2026-11-09 a free opponent
  (noinfinites.cpp Free) shows 0 to that test alone: the hook of sub_000E97A0 sets the count aside in guest memory and
  the hook of sub_00076700 puts it back before the move starts. A count on a fighter still in a reaction refuses it as before.

## Tools

`tools/ghidra-import.ps1` takes `-Research research\mkda-rev1` (new optional parameter; the default stays
`research\<game>`): `build/ghidra-mkda`, 8,456 functions, 419 renamed from XbSymbolDatabase and the config, about
65 s of analysis. Queries written so far: `q-movie.txt` (the XMV player), `q-ps.txt`, `q-rt.txt` (RenderWare camera
raster), `q-pads.txt`; for the game layer `q-mode.txt` (mode setter and readers), `q-menu.txt` / `q-menu2.txt` (main
menu, menu system), `q-select.txt` to `q-select4.txt` (character select), `q-fight.txt` to `q-fight3.txt` (fight
task, setup, rounds), `q-aio.txt` to `q-aio3.txt` (file manager), `q-heap.txt`, `q-xrefs1.txt`. The generated code
(`recomp/mkda/generated/game/x86_recomp.*.cpp`) carries every guest instruction as a comment, which makes grepping
it for a global's writers (`mov dword ptr [0x0042A89C]`) a quick cross-check of Ghidra's references.

No runtime code was changed for this game: everything is config (`recomp/mkda/config/game.toml`), the new
`xdk/4721/` (patch `recomp/patches/xboxxberecomp-0008-xdk-4721.patch`) and game-local code (`recomp/mkda/src/game`).

## Next

- Audio is not verified by ear (the runs are unattended); no DirectSound error is logged. The XMV movies' sound goes
  through `DirectSoundCreateStream` and the bound stream methods.
- The legal screen holds for about 18 s at 60 fps before the logo movie when nothing is pressed; START presses (the
  autopilot's) pass it and both movies, the main menu is up at about 11.6 s.
- The runtime could set the device's multisample type itself (see "The invisible fighters"); then the game-local
  `KeepMultiSampleType` can go.
