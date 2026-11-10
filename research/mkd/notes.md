# mkd (MK Deception, USA, 4D570019) running notes

## 2026-10-24: identity and SDK surface

- XBE sha1 ccbcfe58d253e96fdb016907ee80b8a5fe53ca26, 4,657,152 bytes, retail,
  North America, cert version 5, XDK 1.0.5849, entry 0x001EB42B, kernel thunk
  0x00380880, image 6,556,992 bytes, 20 sections (see `xbe_info.json`).
  Internal name mk6 (`c:\mk6\Code\mk6xbox_Release\mk6.exe`).
- Engine code: `.text` 0x00011000 + 2,972,444 bytes. SDK code lives in its own
  sections: D3D, D3DX, XGRPH, XNET, XONLINE, DSOUND, XPP, DOLBY, plus the shader
  sections PSGSFD00/_I/_B/_P and the MCPX `dspimage`.
- Online: Xbox Live (XONLINES, XVOICE, VOICMAIL) and System Link (XNET).
  Insignia lists this exact title (MW-025 / 4D570019) as supported.

### XbSymbolDatabase (OOVPA signatures, Cxbx-Reloaded's catalogue)

`XbSymbolDatabaseCLI.exe orig/mkd/default.xbe` named 380 symbols
(`symbols-xbsdb.txt`), by library: D3D8 174, DSOUND 108, XAPILIB 73, XONLINES 21, XGRAPHC 4.
Same tool on Armageddon: 675 symbols; on Deadly Alliance Rev 1: 325.

The signature tables behind these names are ODbL-1.0 (Cxbx-Reloaded's
XbSymbolDatabase); keep the attribution if the lists are redistributed.

These are the XDK entry points the native runtime replaces (D3D8 device and
render-state calls, DirectSound, XAPI device/pad functions, XOnline
matchmaking and sockets). They also seed the Ghidra naming pass.

### XboxXBERecomp xbe_symbols (XDK 5849 signatures)

Project `recomp/mkd` (from the SDK's `templates/game`, SDK junction at
`sdk/`, target renamed MKD). `xbe_symbols assets/default.xbe sdk/xdk
--library MKD.dll --out config/game.toml`: found 176 of 243 functions and
10 of 12 data items; `D3DDevice_Swap` found by constants at 0x002ED320 (the
frame boundary candidate). The 67 misses are: the Bink group (no BINK
section here, irrelevant), a block of DirectSound 3D/stream/EG/filter setters
and IDirectSound listener/effects calls, `D3DDevice_BeginPush/EndPush`,
`SetIndices`, `SetPalette`, `SetRenderTargetFast`, `InsertFence/BlockOnFence`,
`SwitchTexture`, `FlushVertexCache`, `PersistDisplay`, `EnableOverlay`,
`SetTile`, `GetVertexShaderFunction`, `D3D_IndexData`, `XNetCreateKey`,
`WSARecvFrom`, `XPP_GamepadType` and the BSD socket calls. Unmatched entries
stay 0 and trap at run time if the game reaches them.

Config edits: the four `PSGSFD*` shader-data sections removed from
`code_sections`; misses with an identical XDK function name in
`symbols-xbsdb.txt` filled from there (marked `# from XbSymbolDatabase`),
to be verified when the first traps appear.

### XboxXBERecomp xbe_recomp, first lift (10 s)

`cmake --build out/build/release --target recompile_MKD`: 1,171 functions
found directly plus 3,278 by gap filling, 19,271 functions in total,
1,076,750 instructions, 3,350,586 of 3,602,552 code bytes covered, 455 jump
tables, 14 callback entries, 81 overlapping decodes. The "unimplemented"
list (aaa, aas, daa, das, bound, enter, lds, les, arpl, iretd, outsb, salc,
verr and friends, about 170 instructions in all) is data decoded as code in
the gap-fill pass, not game code. Compare the PS2 attempt: the lift is
complete and correct by construction here; the open questions are all in
the runtime.

### Full native build, first attempt

`cmake --build out/build/release` with Clang 23.1.4: 364 of 377 steps in
about a minute (the 9800X3D makes short work of the 159 MB of generated C++),
then one toolkit gap: three `fprem1` sites in `x86_recomp.46.cpp` call
`std::remainder`, which the SDK's freestanding cmath shim lacks. Fixed with
`recomp/patches/xboxxberecomp-0001-cmath-remainder.patch`; build resumed.
The disc image is hard-linked into `recomp/mkd/assets/game.iso` (the runtime
scans `assets/` for any `.iso`).

### First launches of the native build

1. `MKD.exe` found the game (sha1 match) but refused to load code: the
   template's `game_info.cpp` still mapped `default.xbe` to an exe named
   "Game". Fixed (`.images = {{"default.xbe", "MKD"}}`, titles).
2. The runtime then ran its player-side flow: rebuilt `MKD.dll` itself with
   the bundled `xbe_recomp` + Clang (65 files, 16 at a time, under a minute)
   and stopped at its "The game is ready. Start the game now?" page, which
   needs a click (Start (A) / Exit (B)).
3. D3D12 came up on the RTX 5080, then the first unresolved symbol trapped:
   `XPP_GamepadType` (found only through `.XTLID` in the 5849 signatures,
   which Deception lacks). Filled from XbSymbolDatabase's
   `g_DeviceType_Gamepad` = 0x377834 (its MU descriptor matched the toolkit's
   `XPP_MemoryUnitType` at 0x3777B8, so the table is trustworthy).
4. With that, the lifted game code runs for 0.54 s and faults in its own
   startup: access violation reading guest address 0x10 (a NULL object
   pointer) on guest thread 14, esp 0xD005FDE0. Return chain from the top:
   0x12DAAB <- 0x12D14C <- 0x12D1AC <- 0x12B8FC <- 0x12B959 <- 0x12B9EE <-
   0x12BAA6 <- 0x12CA8A <- 0x1B31AC <- 0xDFF62 <- 0x1EB418 <- 0x1ED1CE <-
   0x29A04C <- 0x1EB3B7 (entry 0x1EB42B). The faulting read is in
   `sub_0028A660` (`mov eax,[ecx+0x10]; mov edx,[ecx+0x14]`, a 64-bit field
   getter) called from `sub_0012DA80` with `*(DWORD*)obj`, which is still
   NULL after `sub_0012D950(obj)`: a lazy-initialised object whose
   conversion (`sub_0012DB70/0x12D810`, helpers at 0x28A8xx) did not produce
   a result. No kernel or XDK trap was logged before it, and no file-open
   warning either. Crash dumps: `out/build/release/CrashDumps/`.

### Bring-up loop, round 1: the startup crash (Ghidra)

Tooling: `tools/py`-free this time; `sdk/tools/symbols/xbe2pe.py` wrote a
flat PE with the 19,271 recompiled functions as exports, headless Ghidra
12.1.4 analysed it in 120 s, `tools/ghidra/ApplyNames.java` renamed 509
functions from `symbols-xbsdb.txt` + `config/game.toml`, and
`tools/ghidra/DecompileFuncs.java` wrote the crash chain to
`decomp-crash-chain.txt`, the file layer to `decomp-io-layer.txt` and the
async I/O manager to `decomp-io-manager.txt`. Ghidra project:
`build/ghidra-mkd` (git-ignored).

What the chain is:

- `sub_000DFE20` is the game's `main` (engine init, then the frame loop
  with `sub_00130390`, `sub_0013BE30`, `sub_000E00B0` per frame).
- During init, `sub_001B3170` calls `sub_0012CA60(0x10000,
  "permanent_strings_eng.mko")` (string table). That becomes
  `sub_0012B7B0(slot, name)`: open via `sub_0012CFD0` -> `sub_0012CDF0`
  (succeeds; `sub_0012D0A0(file)` = size at +8), allocate `size` rounded up
  to 0x800 from heap `DAT_00475268` via `sub_001DD3B0`, then
  `sub_0012D190(buffer, 1, size, file)` -> `sub_0012DA80` -> wait + fetch
  the 64-bit bytes-done field (`sub_0028A660` reads result+0x10).
- The async request object: [0] = result, [1] = async handle, [2]/[3] =
  completion callback and context. `sub_0012D3A0` creates it: builds the
  path, allocates from pool `DAT_0046dcf4`, `sub_0028A7A0(path, device,
  ...)` opens asynchronously (path resolution in `sub_0028AD30` /
  `sub_0028BE90`, then device vtable[0xC]).
- `sub_0012D950` waits: while `sub_0028A850(handle, &st)` (handle
  vtable[0xC], "is complete") is false, pump `sub_0012DCA0` ->
  `sub_0028A750` (or `DAT_005ff3d4`->vtable[0x18]).
- Completion callback `sub_0012D6B0(_, result, error, obj)`: on error it
  stores the code in `DAT_0046dd0c` and leaves obj[0] NULL; the game never
  checks and dereferences NULL+0x10. So the runtime completes this open or
  read with a non-zero error. Next step is to read that code: `[xbe]
  trace = [0x12D6B0, 0x28A7A0, 0x28AD30, 0x28BE90, 0x12CDF0]` in the
  config (traces are emitted as `X86_TRACED_FUNC`), which needs a
  non-NDEBUG build because info-level log lines are compiled out of
  Release (`runtime/core/log.h`): the `debug` preset.

### Round 1 result: root cause found, and it is not in the game

The `debug` preset (verbose log, numbered `logs/game_NNN.log`) with
`SDL_ASSERT=always_ignore` (debug SDL3 asserts on event pumping off the
main thread; harmless) showed it all within 0.7 s:

- `boot: kernel import 232 (NtUserIoApcDispatcher) has no implementation`
  (the only missing import; noted for later, XAPI's ReadFileEx path).
- `files: D: is <assets>` with no disc mounted: `FindGame` prefers a loose
  `assets/default.xbe` over any `.iso` beside it ("the extracted files
  before any image"), so my hard-linked `game.iso` was never used.
- The engine's I/O worker thread (thread 1C, entry 0x28D230) then logged
  `missing d:\sndsxbox\dsstdfx.bin`, `d:\sndsxbox\permanent.mbx`,
  `d:\art\gameart.ssf`, `d:\sndsxbox\announcr.mbx`,
  `d:\art\permanent_strings.ssf`; the completion callback `sub_0012D6B0`
  received error 0xFFFFFFEE (-18, "not found") for the strings file, left
  the result NULL, and `sub_0028A660` read NULL+0x10.
- Also visible: the engine mounts `/cdrom`, `/game/art`, `/game/sndsxbox`,
  `/game/logs`, `/game/moviexbox`, `/game/moviexbox/krypt`, `/game/anims`
  (strings at 0x39F09C..0x39F130) through `sub_0028BE90`; D3D device
  720x480, 18 vertex shaders compiled at boot; the Midway dragon logo
  renders before the crash.

Fix: extract the whole disc into `recomp/mkd/assets/` (documented layout:
`default.xbe` with the game's folders beside it), no override needed.

### Round 2: the task system (cooperative stackful coroutines)

After the disc fix the boot reached: all `d:\art` and `d:\sndsxbox` opens
succeed, DirectSound effects image loaded (7 effects), xnet bound, then
`kernel export 232 (NtUserIoApcDispatcher)` trapped on the I/O thread:
implemented in `recomp/mkd/src/game/kernel_apc.cpp` (0 missing imports now).

Then the engine's task system hit the lifter's limits. Every game object is a
stackful coroutine on the main guest thread; two classes:
- A, vtable 0x3F5E70 (copy-stack): Run 0x13C1F0, Yield 0x13C360, SetFunction
  0x13C430. Yield copies the frames between esp and the scheduler's saved
  esp (0x46DDBC) into the task's buffer (+0x40 end, +0x44 saved sp) and jumps
  to the scheduler's return address (0x46DDAC); Run copies them back and
  jumps to the saved resume address (+0x34).
- B, vtable 0x3F5E98 (dedicated stack): Run 0x13C470, Yield 0x13C5A0,
  SetFunction 0x13C650, stack hops 0x13C610/0x13C630. `mov ss` in the switch
  is why the log shows "privileged instruction ignored".
- Task layout: +0x04 alive, +0x0C result/wait (float; 0.0 reruns the
  function at once, negative destroys the task via vtable+0x10), +0x10..+0x1C
  saved ebp/ebx/esi/edi, +0x2C hook, +0x30 function, +0x34 resume address.
  Globals: 0x5FF3D4 current task, 0x5FF3D0 wait time, 0x5FF3C8.
- The lifter emits `ret` as `ctx.esp += 4; return;` and resumes continuations
  only into live host frames, so a resumed task cannot work as lifted code.
  The `[[function]] at = 0x13C08B` entry I added is wrong for this reason
  (it turns the scheduler's return point into a callable) and should be
  removed once the fiber path is proven.

Fix in progress: `recomp/mkd/src/game/task_fibers.cpp` gives every task a
host fiber (CreateFiberEx, 8 MB reserve) and overrides the eight routines
above, reproducing the engine's bookkeeping (register saves, stack image
copies, result/rerun loop, post-run hook and destroy) so other engine code
sees identical memory. SetFunction ends the task invocation by throwing a
`TaskReturn` caught in the fiber entry.

Result of the first run with it: the int3 sentinel is gone and the game gets
further, then an access violation reading guest address 0 on the main thread
inside the main loop (esp D005FEB0, ecx = edx = 0, frame under 0x1EB418).
Prime suspect: a yield (vtable+0x18, e.g. from `sub_00266380`) arriving when
`g_running` does not match, so `YieldTask` returns instead of parking, the
task runs on and finishes, and a later frame dereferences a cleared current
task pointer (`sub_0013BE30` zeroes 0x5FF3D4). Next: log every RunTask/
YieldTask with task id, flavour, g_running and esp; check whether the main
loop itself runs inside a task; verify B-class first-run esp (+0x44 is the
stack top only before the first run) and that TaskReturn propagates through
lifted frames.

### Round 3: from the title screen to full fights (2026-10-25)

All of these are in place and the game plays complete Kombat rounds (a
150 s soak with scripted input finished without a fault):

- Task system: every scheduler primitive is a host override on fibers
  (`src/game/task_fibers.cpp`): class A (copy stack) Run 0x13C1F0, Yield
  0x13C360, SetFunction 0x13C430; class B (own stack) Run 0x13C470, Yield
  0x13C5A0, SetFunction 0x13C650, stack hops 0x13C610 / 0x13C630; class C
  (no yield, vtable 0x3F5E48) Run 0x13C130, its Yield an int3 at 0x13C1E0;
  exit 0x13C0B0. Globals: scheduler return / esp / ebp 0x46DDAC / BC / CC,
  last yield return 0x46DDA8, hop esp 0x46DDC0, SetFunction time 0x46DDA4.
  The `[[function]] 0x13C08B` entry is gone.
- CRI ADX/Sofdec (`src/game/cri.cpp`): the SVM lock (0x2427D0 / 0x242A90)
  only excluded other threads on one CPU (priority 15); it is a real
  recursive mutex now, and the is-locked check 0x24BB10 asks it. The ADX idle
  thread 0x2406F0 sleeps instead of spinning.
- Thread exit codes (patch 0002): CRI's shutdown polls GetExitCodeThread.
- longjmp: `main` (0xDFE20) calls `_setjmp(0x46CCC0)` at 0xDFFBA and the
  engine longjmps there from inside tasks after each movie or sequence;
  `continuations = [0xDFFBF]` plus patch 0003, and the task fibers carry an
  `X86Resume` out of the fiber to the scheduler's caller.
- Input: the game opens a port only when the pad appears after its input
  system is up, as USB enumeration does on the console. Reported at once,
  the insertion is consumed too early and no button ever works, so pads are
  hidden for `MKD_PAD_DELAY` (1.5 s). Per-port block 0x62109C + port * 0x1C:
  flags (bit 1 opened), current buttons +0x10, new presses +0x14. Movie skip
  is sub_00024570 (logical buttons 0xB and 6).
- Sound (patch 0004): the sound manager tick sub_0025B210 polls 56 events at
  0x5D3570 with a zero timeout: 48 "near the end" events of one-shot voices
  (replay count +0x5C of +0x58, then free) and 8 stream events at offsets 0
  and half for double-buffered streams (refill sub_0025DBC0 fills the next
  half in order, so an event at the start offset must not fire on Play).
- Direct3D (patch 0005 and config): `D3D_FvfShader = 0x2FA740` (the device
  init 0x2EFBEB stores it at CDevice+0x794; recompiled code reads its flags at
  +4, so a zero there crashed SetDepthClipPlanes); `D3DDevice_BeginPush =
  0x2EDE80`; a host SetDepthClipPlanes; device mirrors for the getters.
- Recompiler (patch 0006): switch bounds from stack-variable compares.
- Main loop (continuation 0xDFFBF of `main`): per frame the counters
  0x61C3F8 (always) and 0x61C418 (when not paused), then sub_00130390,
  sub_001C52F0 (pads), sub_0008B4E0, sub_00265E20, sub_001BE400, the
  sub_0006AF70 group, sub_0008A080, sub_00139B50, sub_0013BE30 (task
  scheduler: the game logic), sub_000E00B0 (async I/O sub_0028A750, sound
  manager sub_0025E530, render sub_0006C010 and Swap), sub_0013D910. The top
  of this loop is the frame boundary for netplay.
- Tools: `tools/run-game.ps1` captures the window itself (PrintWindow), so
  other windows over it no longer spoil screenshots; `tools/py/lib_audit.py`
  lists library calls that still run console code.

Menu path used by the scripted runs (times from the first pad read): START
at 15 s and 19 s (skip the opening, the title), A at 25 s (Kombat), then
A / START presses through the mode and character screens.

### Round 4: netplay, the front end and the launcher (2026-10-25)

Netplay version 1 (lockstep with input delay) plays real matches between two game instances, and the launcher
drives the game through `docs/PROTOCOL.md` section 3: two launchers on one PC matched by the lobby server, both
games fighting in sync and the server recording the result (`tools/online-e2e.ps1`).

Frame and input:
- Frame hooks (`src/game/frame.cpp`): the main loop calls sub_00130390 at 0xE0007 (returns to 0xE000C) and the
  pad update sub_001C52F0 at 0xE000C (returns to 0xE0011); both are also called from elsewhere (movie loops), so
  the hooks key on those return addresses. Movies run their own loop: the frame counter stops during them.
- The pad update maps each pad through the port's button table (0x6210A0 + port * 0x1C, default 0x3E95A0) into
  logical bits: previous +0xC, current +0x10, new presses +0x14 ((cur ^ prev) & cur), sticks +0x18 (four bytes,
  centred 0x807F807F); analog face buttons count from 9. Netplay exchanges these logical values and writes both
  ports' blocks right after the pad update, so each player's own button configuration applies on their own PC.
- The pad update rebuilds each open port's block from zero every frame (previous = current, then current from the
  XInput state; it never looks at the packet number). Port block: +0 flags (2 open, 4 calls sub_0006AC90 after the
  read), +4 button table, +8 the player record the port drives (a press on a port without one calls sub_0006AB10).
  sub_0008A4F0(port, stick, &x, &y) reads a stick (0 left, 1 right) as floats past the deadzone byte 0x3EBB24.
- sub_0006AC90 ORs the left stick's directions (the button table's entries at +0x90 / +0x9C / +0xA8 / +0xB4) into
  the current buttons from the block's sticks (+0x18), which the pad update only reads afresh after it: the
  directions are last frame's stick. Online the netplay writes both ports' blocks after the pad update, so on the
  right player's PC port 0 held the left player's sticks, and the left player's stick directions went into the right
  player's own input (0.1.0-0.1.6: the user's net logs show the same direction bits at the same frame on both sides,
  with only the left player's stick off centre). The netplay puts port 0's sticks back to this PC's before the pad
  update (`mk3d::OnBeforePads`, `game::RestoreLocalSticks`).

Game options (0.1.7, for the death traps setting):
- The options block 0x5FC980 (0x78 bytes; volumes as floats first): difficulty 0x5FC998, chess / puzzle difficulty
  0x5FC99C / 0x5FC9A0, rounds to win 0x5FC9A4, puzzle rounds 0x5FC9A8, round time 0x5FC9AC (seconds, 20..95 in
  steps of 5), blood level 0x5FC9B4 (0..3), death traps 0x5FC9B8 (0/1). Defaults sub_001B4640 (2, 2, 2, 2, 2, 60,
  5 at 0x5FC9B0, blood 3, traps 1); sub_001B4430(item, value) sets one by its menu item (0x332C difficulty ...
  0x3331 round time, 0x3332 death traps, 0x3333 blood: the Game Options screen of screen_engine.ssf lists
  ROUND_TIME, DEATH_TRAPS, BLOOD_LEVEL in that order); sub_001B4300 writes them out.
- sub_001B3DC0(dst, src) copies a whole block (0x1E dwords). sub_001B3E30(profile) makes a profile's (0x601EE0 +
  profile * 0x2ED0) the game's: it runs when the main menu takes a mode (seen: the death traps back on by the select
  screen, after the launcher had set them off), so a launcher match's options go back after every copy over
  0x5FC980 and every sub_001B4640 / sub_001B4430 (linkgame.cpp hooks).
- sub_001BF070 says whether death traps work: 0 when the blood level (sub_001BF010, the option or an Xbox Live
  session's 0x621244..) is 0, else the option (Xbox Live: the session's 0x621248 for its game types 0 and 2). The
  arenas' scripts call it through their natives table (0x6597E8; their .ssf files hold the death trap code:
  `dp_do_close_arena_death_trap`, `lm_do_lion_death_trap` ...), the fight code from sub_00044080 and sub_001AE540.
  In a launcher match the hook returns the match's setting.

Game state:
- Game mode 0x61C414, written only by sub_001BC170: 13 front end, 0 arcade, 1 versus, 4 practice (health refills),
  6 puzzle kombat, 7 konquest, 9 chess kombat (10 during a chess battle, 12 on its game over screen).
- Player records: P1 0x620FC4, P2 0x621030 (0x6C apart): +0 profile (-1 none), +4 port, +8 select state (2 =
  picked), +0xC health (float, 1.0 full; the HUD's animated copies are 0x471C44 / 0x471C48), +0x10 start health,
  +0x14 costume flags, +0x40 round wins (zeroed by the fight setup), +0x54 character, +0x58 fighter object.
- The fighter objects are static, 0x710 bytes apart: P1 0x470C78, P2 0x471388. Right after P2's (0x471A98 on)
  lies the select screen's state, the cursors 0x471B18 / 0x471B1C among it, and 0x471B08 / 0x471B0C, which depend
  on how each cursor reached its pick: a state hash of P2's object must stop at 0x710 (netgame.cpp kFighterBytes),
  or two games whose cursors moved differently (a player picking by hand, the other game's autopilot making the
  same pick) seem to part at frame 0.
- Fight setup sub_00083570 runs once per match, before the intro: the netplay barrier.
- The C runtime's rand (0x29B8AC, srand 0x29B89F) is never seeded and is not called during a fight.

Front end:
- The main menu task sub_000E3770 dispatches on the menu action 0x46CDB8 (0x18 none): 6 arcade, 7 versus, 8 chess
  kombat, 9 puzzle kombat, 0xB practice, 2 konquest. The menu UI sets it through sub_0014EF20(port, action), which
  also marks who takes part (sub_0016FBC0(record, 1)). VERSUS is greyed out without a second signed-in profile;
  `events.cpp` redirects the next arcade/versus/practice confirmation to a requested action with both players in.
- Character select: cursor slots 0x471B1C (P1) and 0x471B18 (P2), 8 x 3 grid (chess 6 x 2, 0x471B24 == 2), slot to
  character 0x3F9210 + slot * 0x28 (chess 0x3F95D0), names 0x3F89E0 + id * 16. Selectable when the character's bit
  is set in the masks 0x601E60 (0x601E68 alternates, 0x601E90 chess) or the defaults 0x3F8FF0; sub_0017ACA0 moves
  a cursor (skipping locked slots), sub_0017C380 confirms.
- `src/game/autopilot.cpp` presses through all of it watching this state (START to the main menu, START/A to a
  confirmation, cursor steering by reading the slot back, A to pick); 15 of 15 boots reached the fight.
- sub_000E0190(type, task) switches the top-level state (ends the current task, starts the new one, longjmps to
  the main loop); `park` uses sub_000E0190(6, 0xE3770) from the frame hook to return to the main menu from anywhere.
- After a Versus match the game returns to character select (a rematch is another pick).

CRI on several cores, two more races (`src/game/cri.cpp`), found with `MKD_HANG_DUMP` stacks of an intermittent
black screen when a movie started:
- the SJ and server critical sections (lock 0x2483C0 / 0x248470, count 0x4A12A0 / 0x4A12AC) checked their nesting
  count before taking the lock, so a second thread skipped it;
- the ADX server pass 0x243350 and Sofdec's mwPlyExecSvrHndl 0x224BD0 check busy flags under the lock but set them
  after unlocking (and the Sofdec request flag at handle+0x44 is tested and cleared with no lock), so two threads
  decoded the same stream at once. Either corrupted a ring buffer and its consistency check called the default
  error handler 0x240ED0, a jump to itself, with the lock held. Serialising every server pass instead broke movies
  (a server function waits on another thread), so only these are locked.

Tools: the debug monitor (`MKD_MONITOR=<port>`, `tools/py/mkdbg.py`: read/write memory, dump and scan, pause and
step frames, inject pad presses, call game functions on the main thread), `tools/snap.ps1`,
`tools/netplay-local.ps1` + `tools/py/netcmp.py` (two instances, per-frame logs compared), `tools/py/linktest.py`
(the launcher protocol with two instances), `tools/online-e2e.ps1` (server, two launchers, two games).

### Round 5: rollback (2026-10-25)

A fight can be put back to the top of any of the last 16 frames and run again, and a run again writes exactly what
the first run wrote: the sync test (`MKD_SYNCTEST=1`) rolls back and re-runs every frame of a fight and compares
the simulation's memory byte for byte; 3 runs of 1,200 frames each, 0 differences, with sound and streaming on.

The machinery (`src/game/rollback.cpp`, `rollback.h`):
- Guest pages copy-on-write: tracked pages (committed user memory, the 64 MB of physical memory in both views, the
  main thread's guest stack) are write-protected at the top of each frame; the first write logs the page into the
  frame's undo log (VEH). A restore plays the logs back newest first. Arming tracks 22,181 pages (86.6 MB) in
  4.5 ms; a frame costs about 380 faults, 0.9 ms to close and 0.4 ms to restore.
- Other threads: a write by another thread to a page the main thread uses makes only the bytes it wrote
  "volatile" (a restore keeps their current value, comparisons skip them); the D3D device's vertical blank fields
  (0x2F8070 + 0x1DB8..0x1E04, written by the runtime's vblank thread, on the page with the palette slots and render
  targets) and what other threads' file reads fill are volatile from the start. Only physical memory no main-thread
  write ever touched (sound buffers other threads fill) is "foreign", never put back. `MKD_RB_PROBE=<ms>` protects
  the main thread's dirty pages again every <ms> ms to catch more of other threads' writes (diagnostic).
- Host state: the task fibers and the main thread's registers (round 4), and through the runtime (patch 0010,
  `kernel/host_rollback.h`): pool allocations and frees, kernel objects' signal states, handles and pointer
  objects, DirectSound voices. The guest clock (patch 0009, `src/game/gclock.cpp`) makes time a function of the
  frame on the main thread.
- DirectSound on the frame clock (`xdk/dsound.h`): `MixFrames(800, heard)` once per simulated frame moves the voices,
  sets their notification events and queues the sound; the real time mixer had made the sound manager's polls
  (56 events at 0x5D3570, GetStatus about 460 times a second) differ between runs.

What made runs differ, in the order found (the watchpoint `MKD_WATCH=<va>,...` names the lifted function and guest
instruction behind every write to a dword, `rollback: ... belongs to another thread` names the writer of shared
bytes):
1. The D3D scratch push buffer (physical memory the runtime rewinds on its own): comparisons cover user space only.
2. Palette references (0x01775120, D3DResource internal count bits 19+): the device page went foreign because the
   vblank thread writes VBlankCount on it, so the palette slots were never put back. Fix: volatile byte ranges.
3. Pool addresses (0xB01D6xxx, 0x823Fxxxx): DirectSound buffers made and released during the fight came from host
   allocators that a rollback did not rewind. Fix: the allocation journal; frees happen at once and an undone free
   takes the same block back (holding frees until a frame left the window changed the pools between the runs).
4. The sound manager's polls: the frame clock.
5. The engine's asynchronous loader (announcer lines `announcr.msx`, stage music `music_tune_3.msx`, effects
   `fightfx1.msx`, 32 KB reads): an I/O worker thread (0x28D230; loop 0x291500 runs busy schedulers 0x2911A0
   with SleepEx(0, TRUE) for the ReadFileEx completions) finished requests whenever the disc allowed, and both
   threads wrote the request objects and queues (completion queue 0x5F0E34, drained by 0x2917B0 from 0x28A750 in
   the main loop; deferred calls 0x5F0F28; the I/O manager is the object at [0x5F0E90]). Recording what the main
   thread took and handing it to the run again was not enough: the worker kept writing a stream request's progress,
   a run again saw a later state, destroyed a request the worker was still using, and the next completion called
   a pure virtual (the CRT's R6025 trap at 0x1E6D98). Fix (`src/game/aio.cpp`): while recording, the worker skips
   its passes and the main thread runs them itself at the start of every frame's drain, then the completions
   (`RunApcs`), until nothing is busy. Reads complete as they are made here, so a request is back in the frame it
   was made in and every loader write is the main thread's. The recording starts once the loader is idle.
6. A read that failed for ever: a page the probe had protected again made the OS read fail (no fault in user mode)
   and the loader retries a failed read without end. Every page an OS write fills is made writable whatever its
   state, and the probe leaves those alone in that frame.

### Round 6: rollback netplay (2026-10-25)

Netplay version 2 (`src/game/netplay.cpp`, the default; `MKD_NET_MODE=lockstep` keeps version 1) plays whole
matches with rollback: two games on one PC through a simulated link of 140 to 200 ms round trip with 3% of packets
lost played four complete matches with every frame equal on both (health, round wins, the C runtime's random
calls, both fighter objects; `tools/py/rbcmp.py` compares the per-frame logs), 7 to 8 ms per rollback of 3 to 4
frames in the release build (1.75 ms a frame run again; about 6 ms in the debug build, whose host code is
unoptimised), the two games within a frame of each other.

How a frame goes (frame hooks of `frame.h`): at its top the frame that ran is closed (`rollback.h`), what arrived is
read, and if the other player's real input for some frame differs from what that frame used, the game is put back
to that frame and runs again up to the present: unseen (`d3d::SetSkipPresent`), unpaced, its sound mixed but not
heard (`dsound::MixFrames(800, false)`). After the pad update the local input is taken for frame + delay (1 by
default) and both ports' blocks get their inputs; the other player's is predicted as their last one repeated.
Each port's "previous buttons" come from the per-frame tables, so a frame run again sees the same presses. Time
sync: a game never runs more than 12 frames past the frames both agree on, and the one running ahead of the other
(both send how far ahead they think they are) moves its frame cadence by a frame at most every quarter second
(`app::PaceShift`; a sleep was made up by the pacer). Checks every 30 agreed frames hash the state; on a mismatch
both games write the fighter objects and the game state block 0x620000..0x622000 into their logs
(`tools/py/rbdump.py` compares them). Round wins for the launcher come only from agreed frames.

What made the two games differ, in the order found:
1. Hits a frame apart: the main loop's frame counters (0x61C3F8 always, 0x61C418 while not paused) differed by the
   time each game spent in the menus; the engine's timers count on them and some code reads them as they are. The
   games now meet right before the fight setup runs (`events.h` BeforeFightSetup, inside that frame) and both take
   the larger values, before any fight timer starts; the rest of that frame runs with neutral pads.
2. Knockouts that played out differently: the engine's random generator (a Mersenne Twister variant: seed 0x14BDB0,
   draw 0x14BFB0, state 0x5FE9F4 + 0x5FEA00; wrappers 0x1BC190 raw, 0x1BC1A0 below n, 0x1BC260 float, 0x1BC2D0
   signed) is drawn from about 300 places, among them ambient stage objects such as the creatures that wander and
   flee from the fighters (0x38EA0, 0x39170, 0x39830). Those start while the stage loads, before the games meet, so
   their states and how often they draw differ, and every draw moves the one stream: reseeding it at the barrier
   was not enough (`# mt` counts per call site showed 22 to 33 draws a frame, different on the two games). In a
   fight in sync a draw is now a hash of the agreed seed, the frame, the call site and its count in the frame
   (`src/game/rng.cpp`): what a fighter draws depends only on the fighter.
3. A crash when the session ended: Disarm marked pages untracked before letting them go, and a CRI thread's write
   that faulted in between was passed on as a real fault; a fault on an untracked page that is writable now is
   retried.

Other differences between the games' static data at the first frame (`MKD_NET_MEMDUMP=<frame>` writes it) are not
the simulation's: the stack cookie (0x464F60), texture animation matrices (0x45D174), the vertical blank count and
swap count, loader and allocator statistics, CRI and Sofdec state.

### Round 7: Chess and Puzzle Kombat online (2026-10-25)

Both side modes now go through the launcher end to end like a versus match: queue (`mkd/chess`, `mkd/puzzle`),
both games in rollback sync from the match's first frame, the same result from both, the server's RESULT. Two games
on one PC (40 ms + up to 20 ms, 2% lost) stayed equal on every frame through a 5.5 minute chess game with five
battles, and through whole puzzle matches; versus still plays as before.

Front end automation (`autopilot.cpp`):
- Chess Kombat's select (task 0x17A500, grid kind 0x471B24 = 1) fills a five piece team (leader, champion,
  sorcerer, shifter, grunt) from one 8 x 3 grid; the select state stays 1 throughout. The select task hands its team
  block to its screen (`sub_0015A930(screen name 0x3A2840, ..., block, 0)`, hooked in `events.cpp`): P1's five
  picks at +0xC, P2's at +0x2C, 0x29 while empty. A full team asks "save team to profile?" with NO chosen: A
  answers it (UP would choose YES). Presses are decided once per press cycle now, so the walk over the grid no
  longer skips slots. The match starts about 6 s after the select screen opens (it timed out at 90 s before).
- Puzzle Kombat's select (task 0x17A270, kind 2, 6 x 2 grid) picks as versus does; with two players the match task
  is 0xF2850 (0xDCE60 is the game against the computer), hooked like the chess one.

Meeting point and agreement (`netplay.cpp`): the side mode match tasks run the BeforeFightSetup hooks, so the games
meet before either loads anything of the match. The select screens ran apart, so READY also carries the arena
(0x620F38) and both chess teams (0x60166C and 0x601688, which the board builds its pieces from): side 0's arena,
and each team as its own player's game picked it.

When a match ends (`MatchOver`): the game leaves the match's modes (Chess Kombat goes from its board, mode 9, to
battles, mode 10, and back; its game over screen is mode 12), or the state task (0x46CCB8, set by the state switch
`sub_000E0190(priority, task)`) is the main menu or a select screen again (a versus or puzzle match returns to its
select screen without changing the mode). Both games leave at the same frame, on the agreed state; one that left
first sends its last inputs again when the other still asks for them (a lost packet had left the other game
waiting until it gave up with "disconnected"). What the launcher reports is taken from that agreed state and kept
until the next match: round wins for fights; Chess Kombat's winner 0x600518 once mode 12 shows (set by the battle
end `sub_0010EF40` or the win square `sub_0010BDB0`); Puzzle Kombat's match end `sub_000F7180` counts 0x620324 up
and names the winner in 0x620320 (1 or 2), its own rounds are not the fight counters. Both side modes report 1-0.

The state checks now cover a side mode's board: Chess Kombat's game object (0x46CEDC, 0x1BC bytes), its board
(+0x38, 0x1454 bytes) and both sides' 17 pieces (+0x3C / +0x40, 0x90 bytes each); Puzzle Kombat's game (0x46CE5C,
0x148 bytes), its two wells (+0x4C / +0x50, 0x8C0 bytes each) and the players' blocks at 0x5FDEAC. The first try
saw false desyncs from the first frame: new heap blocks hold whatever the heap last kept there, which differs
between two games. Every block the engine's allocator (`sub_001DD3B0`) hands out on the main thread is now zeroed
(`src/game/heap.cpp`).

Loading under rollback: a chess battle loads its arena and fighters in the middle of the match, and rollbacks that
ran those frames again crashed a game within a second (a corrupted XAPI heap: `XMemAlloc` 0x1E6D9D through
`sub_001EA257`). What a load changes outside guest memory does not go back with a rollback. From a state switch (the
switching frame ends there; the new task loads from the next one) and from the match's start, every frame now waits
for the other player's real input instead of predicting it, until 60 frames run without a file read (`load` in the
per-frame log). Reads alone do not start it: while recording, the music streams through the main thread every two
seconds. A battle costs about two seconds of lockstep, the rest stays on rollback. The runtime also records file
positions per frame now (patch 0012).

Tools: `MKD_NET_DATA=1` hashes the image's data section per frame in 64 KB blocks (`rbcmp.py` lists the blocks that
differ: at the first frame they always do, UI animation and per-object timestamps from before the meeting point);
`MKD_NET_SIDEDUMP=<frame>` writes the side mode blocks; `tools/online-e2e.ps1 -Mode chess|puzzle -InputA -InputB`.

### Round 8: the shared core (2026-10-25)

Netplay, rollback, the guest clock, the snapshot-able fibers and the launcher link moved to `recomp/common/src/mk3d`
(namespace `mk3d`, `recomp/common/README.md`), so Deadly Alliance and Armageddon plug in instead of copying them.
A game provides its frame hooks, `EnvPrefix()`, task snapshots, and two adapters: `netgame.cpp` (input blocks,
meeting point words and agreement, reseeding, match start and end, state hash, settled values, loader, random
streams, log fields) and `linkgame.cpp` (autopilot driving, matches started, result per mode). Deception's old
headers forward to the core under its names. The per-frame log line is now `<frame> [again] <inputs>
[predicted] [loading] confirmed <n> rand <n> <game fields>`; `rbcmp.py` reads both forms. Regression after the move:
versus under 70 + 30 ms and 3% loss, the chess battle test, and the launcher flow for versus and puzzle, all equal
on every frame with the same results.

Arena agreement for versus: the arena comes from `sub_00040640`, the next entry of a list (0x3E4D30) from a cycle
index (0x620F44) that moves on every match, so two players with different match histories since starting their
games would load different arenas (the meeting point is after the load; netgame.cpp now logs "the arenas differ").
The launcher's start carries the same seed to both games: the link sets the index from it (or to a named `stage`).

Rounds: a fight's round end (`sub_00083960`) ends the match once the round's winner has as many round wins as
0x5FC9A4 (the options' rounds, 2) when no profile is in use (0x620F24 bit 0; a profile makes it 2), so a lobby's best
of 1 or 5 would never have reported a result. The link's start writes it from the launcher's rounds (checked offline:
`rounds=5` reports `result winner=1 p1_rounds=3`). (`sub_000BE5A0` sets a rounds field of another mode's fight
object, 0x46CAD4 + 0x3D0, used by mode 8's round end `sub_000BB9F0`.)

Pause: START and BACK (logical bits 0x900: the pad update maps 16 physical inputs, 0x453A48, through the port's
table of logical bits, default 0x3E95A0) are kept out of what a player sends during a match in sync, so one player
cannot open the pause menu on both games (a run pressing START throughout played its match through, equal on every
frame).

All three ways into an online match now pass end to end with the real games (`tools/online-e2e.ps1 -Queue
casual|ranked|lobby`): the casual queue (versus, chess, puzzle), the ranked queue (the server needs
`--allow-same-ip` for two players on one PC; the Elo result is recorded) and a lobby (the launcher's new `--lobby
host|join --lobby-name` options create, join, ready and start it). Two lobby matches in a row (`-Matches 2`) found
a launcher bug: the next match's UDP rendezvous binds the game's port while the parked game still held it (netplay
lets go at the game's next frame), and the launcher declined the match; it now tries again every 0.1 s for 3 s.

Shared core fixes from Armageddon's Motor Kombat (a race's XAPI heap grows and shrinks while it loads): a commit
over pages the rollback tracks made them writable again, so their next writes went unlogged (`OnCommit` protects
them anew); page hashes and detail copies skip pages that are not committed; and the runtime keeps decommitted and
released virtual memory until the frame is final (patch 0014). Armageddon's whole-race sync test went from 3
mismatches to 1, the known dead fiber-stack slot written while the race loads (frame 4; netplay never runs a
loading frame again). Regression after the change: Deception's sync test (1200 frames, 0 mismatches) and chess
battle netplay (16200 frames equal), Deadly Alliance's sync test (900 frames, 0) and versus netplay (3415 frames
equal), Armageddon's versus netplay (3972 frames equal).

### Frame data in practice (2026-10-28)

Practice shows player 1's last attack: startup, active, recovery and the advantage on hit or block
(`recomp/common/src/mk3d/framedata.cpp`, this game's side `src/game/framedata.cpp`). Found with the debug monitor
stepping a fight a frame at a time and both players pressing a new button every frame for the ground truth.

Fighter object (player record + 0x58, P1 0x470C78, P2 0x471388), the fields that matter:
- +0x20 / +0x24 its task (pointer, id), task +0x30 the task's function: stance sub_00098B00, block hold sub_001455F0,
  move routines (X sub_00147F20, A sub_001487A0, Y sub_00148410), hit reactions (sub_0019E540 ...)
- +0x1BC its strike's contact: -1 none, 1 hit, 2 blocked, 3 armed for the next strike (sub_00094AA0)
- +0x1D4 stance wait until, +0x1D8 no new move until (game frames, 0x61C418)
- +0x20C an attack's phase (sub_0014AE00: 1 startup, 2 the hit window sub_00071880, 3 recovery; 0 otherwise, and
  special moves never set it), +0x214 state (0 stance, 0x2000 walk, 0x1200 attack, 0x60x hit reaction, 0xA0x block),
  +0x244 the game frame the attack command started, +0x2C8 the strike's reaction id, +0x6AC block-hold entries
- the fighter whose task runs: 0x5FE9E0, its opponent 0x5FE9D4

The game's rules:
- A move starts in frame F (sub_0014A070, from the command task, before the fighter's task runs in that frame) unless,
  at the top of F, the state has 0x200 (but is not 0x420D) or 0x800, the game frame is below +0x1D8, health is 0, or
  the record's select state is not 2 or 3.
- The hit test sub_00071730 runs once a frame for each live strike that has not made contact (from sub_00071880,
  sub_00071E90 and the special moves' strike loops sub_00146400..sub_001472C0); a contact calls sub_0019E8D0 in the
  victim's context. sub_00071880 tests from the frame after it starts (the first frame whose top shows phase 2), but
  some moves test in the frame the window opens (Scorpion's crouching A), and special moves keep phase 0, so the
  overlay counts the hit test itself (an observing override of sub_00071730).
- Moves end through sub_00072A40 / sub_00072D50, which set +0x1D4 / +0x1D8 = frame + their recovery, then
  sub_0006FE00 -> sub_00098860 (reset sub_0006E1C0: state 0, phase 0, contact -1; waits while frame < +0x1D4) ->
  stance. Hit reactions end the same way (Scorpion's X reaction: busy 19 frames after its animation).
- Blocked: reaction 0xF1/F2/F4/F5/F6 by the attack's block kind (table 0x3A30A0 + id * 0x14, +8: 0 F2, 3 F4, 4/5
  F5, 100 F6, else F1); F2 (sub_0019D020) sets +0x1D8 = frame + 6, F4 none; the tail sub_0019D080 goes back to block
  hold, or to sub_0006FE00 when the button is up. The block-hold routine lets go (state 0) in a frame it sees the
  button up, so a blocking fighter can attack one frame after the first frame its task is back in block hold.
- Blocking opens before the move gate (2026-11-07): sub_00075610 decides both a fighter's own block and a hit's
  victim's (not in 0x605 or 0x3202, the body on the ground: its control object [+0x30, id +0x34] with +0xB4 or +0x30 at
  [0x381024], or state 0x60C; +0x1C4 and +0x1C8 clear, +0x1CC and +0x1D0 no later than the frame, 0x620F20 bit 2,
  block held), and a move's end sets +0x1CC or +0x1D0 and +0x1D8 from different numbers. Scorpion's X blocks from move
  frame 29 and attacks from 36 (-15 on block, -22 to attack), his B from 44 and 54 (-28, -38), back + X 26 and 40
  (-12, -26): a jab put in reach lands up to the frame before and is blocked from it (scratchpad r51
  other_punish.py). The fighter's own task clears +0x1C4 as its move ends, before the other fighter's strike tests in
  that frame, so a hit there is blocked though the flag is first seen clear at the next frame's top; the frame
  counts (+0x1CC, +0x1D0) count from the top of the frame they reach. The frame data follows this since then
  (Adapter::block_rule, block_flags_early); before, it showed only the gate's number, 6 to 14 frames worse on
  every move tried.
- Practice: player 2 is no one's (port 1's block +8 is 0, its fighter's controls +0x21C are -1, +0x294 CPU flag 0):
  the dummy stands. The game's menus have no dummy settings. Every block check ends in sub_0008A210(port, action)
  (action 1 block, 0xE down; sub_00075610 for standing block, sub_00075860 for crouching), so the overlay's F8 answers
  "held" there for the dummy's port -1 (practice only).
- Crouch block: the block-hold routine goes to sub_00140380 (state 0x901, then 0x900) and the crouching hold
  sub_00145C60 while block and down are held; a blocked hit's tail (sub_0019D080, state 0xF00) waits one frame longer
  before starting it when block is still held, and it starts at once, so a crouching blocker can act in the frame the
  hold is first seen (letting go keeps state 0x101 with down held, which the move gate allows), a standing one the
  frame after.

Measured, Scorpion against Scorpion (move frames, the press is frame 1). In versus both players pressed a new button
every frame for the ground truth, and the gate above predicted every mashed move on both sides; the rest comes from
the practice panel (the same gate), with the dummy standing, blocking (F8) or crouch-blocking:

| move | startup | active | player 1 can act | on hit | on block | crouch block |
|---|---|---|---|---|---|---|
| X | 12 | 6 | 37 (36 frames) | +26 (mashed; +28 hitting on frame 14) | -22 (mashed) | whiffs over it |
| Y | 16 | 2 | 30 | +50 | -11 | whiffs over it |
| B | 14 | 7 | 55 | -8 | -38 | |
| crouching A (a low) | 19 (tested and hit in the frame its window opens) | 2 | 50 | -7 | hits a standing blocker: +6 | -19 (mashed) |
| A, 2 hits | 12, again 25 | 6 (7) 1 | 51 | +3 | -20 (second block: +0x1D8 = frame + 6, mashed) | |
| spear (back, forward, X) | contact in 37 (phase stays 0) | | | | | |

### The online select screen (2026-11-02)

Players who pick on the select screen (the launcher's `p1=- p2=-`), brought in line with Armageddon's:
- Nothing times out by itself: Kombat's select (task 0x179A60) stayed up 2 minutes without a press (frames 720 to
  7925), Chess Kombat's team select (0x17A500) 405 s (726 to 25041), Puzzle Kombat's (0x17A270) 120 s. 0.1.17's
  autopilot picked for a player after 75 s (a chess team after 180 s) and moved the waiting side's cursor every 8 s:
  both gone, nothing else keeps the screen.
- No pick lag: the cursor setter sub_0017ACA0 writes the slot and the record's character together (sub_0017AC60),
  and the confirm sub_0017C380 sets the record's character from the cursor again, so the character read at the pick
  is the one under the cursor (Armageddon's lag does not exist here).
- Before the pick, the cursor goes to the other game as a pick word 0xC5000000 | slot; that game and a spectator's
  steer that side's cursor after it, a press every 0.1 s (chess teams are not followed).
- The arena choice 0x62031C (-1 none): the versus fight's arena setup sub_000834C0 takes 0 to 0x22 from it over the
  cycle's and sets it back to -1. Its chooser: sub_0017BF70 opens it (0x471AF8 = 1; entry 0 when nothing is chosen),
  sub_0017BFD0(i) takes entry i of the list 0x3F9000 (16 bytes an entry, 0x16 of them; grid kind 1: 0x3F9160 and 5,
  kind 2: 0x3F91B0 and 6) into 0x62031C (index 0x471B20, locked arenas skipped), sub_0017C210 closes it. The screen's
  handler sub_0014DC20 opens it only on the events 0x92828 / 0x92829 (accept 0x1FB9, random 0x1FBA), which only an
  Xbox Live match raises (0x620F24 bit 0, the Live session: sub_00269610 sets it, sub_002665A0 clears it); no button
  of either port in either select state opens it in a launcher match. Calling those functions through the monitor
  left that player unable to pick (the handler keeps state of its own), so tests write 0x62031C instead. Other
  writers: -1 from sub_00179DB0, sub_000859F0 and sub_0013D940; values from sub_0017C4C0, sub_000B96D0, sub_0010A6D0
  (not traced).
- Online the pick words are [character, costume flags, arena or 0xFFFFFFFF] (as Armageddon's and Deadly Alliance's).
  At the versus fight's arena setup each game (a spectator's too) puts the agreed picks in the player records and the
  arena the two choices give into 0x62031C: both chose, the seed's bit 7 takes P2's, else P1's; one chose, that one;
  neither, -1 (the cycle's place from the seed).
- Tests (release, two games and a spectator on a local relay, one round): the other screen and the spectator's had
  the moving cursor on the same slot 9 frames after each move in 67 of 67 samples each way; 60 s without a press
  left all three screens as they were; directions and A after the confirm changed nothing; one choice (21), two
  (18 and 6: 18 with bit 7 clear, 6 with it set), none (the seed's 15) and a rematch with one (21): all three games
  loaded that arena. 0 frames differ between the players in all 10 matches (1336 to 2161 frames each); the
  spectator's logs match the players' in every watched match (1432 to 2086 frames) but for the match counter 0x620324
  once it had sat out a rematch the players played. RT, LS, B, BACK, LT and WHITE through the player's own pad while
  choosing: the screen stays.

### Players' reports for 0.1.20 (2026-11-03)

- Puzzle Kombat's false desyncs. The state check's puzzle blocks took 0x5E0 * 2 bytes from 0x5FDEAC as "the
  players' blocks", because the setup sub_000F2BD0 hands the players 0x5FDEAC and 0x5FE48C (their +0x4C8). That range
  is not the match's: dozens of variables (refs-in-range over Ghidra), among them the memory unit state (0x5FDF40,
  0x5FDF44, 0x5FDF48: P1 signed in), P1's profile (0x5FDF60, 0x5E0 bytes, name "EMPTY" at +8 without one; P2's is
  0x5FD980), the chess and puzzle records in it, loaded resources' handles (0x5FE540..0x5FE8C0) and the random
  generator's index and first state words (0x5FE9F4, 0x5FEA00). A profile from one PC's save made every puzzle match
  between two players part at frame 0: the 0.1.20 build with one game's profile name and koins changed through the
  monitor gave `DESYNC at frame 0 (ours B75DA635/9BE17165, theirs B71978C9/9BE17165, loading)`, the players'
  reports exactly. The match is its game block, the two wells, the two players (+0x54 / +0x58, 0x554 bytes: the well,
  the next pieces, the record, the character's puzzle type +0x510 and its table values +0x500 / +0x51C) and their next
  pieces (+0x5C..+0x68, 0x28 bytes; two offline runs differ there first, the random generator not reseeded offline);
  that is what the check hashes now. With the same profile change: a whole puzzle match in sync (11249 frames, 120
  ms, the mirror with costume flags 2 and 0), another at 180 ms (10 vs 6, 8762 frames). Two mid-match reports
  (frames 2520 and 11310 between one pair) were not reproduced; the old range could change mid-match on one PC only
  (the memory unit state, a profile's records), and the check now leaves it out.
- A crash outside a match, `C0000005 MKD.dll+20508E`. The code DLL is built on the player's PC, so offsets need that
  version's DLL: 0.1.20's, built from its zip (`MKD.exe --prepare` with the zip's toolchain), puts +0x20508E in
  sub_000DEFF0 at the read of a light's +8 just after sub_000DF200 (the current build's exports had said
  sub_000DEEB0). sub_000DEFF0(list, group) sets an arena area's lights (sub_0002D210) from three settings, each
  kind 1 or 3 looked up in the scene's light group by sub_000DF200, which gives 0 when the group no longer has that
  light (it holds weak references) and sub_000DEFF0 never asks (sub_000DFA60 does). `src/game/lights.cpp` makes the
  same walk with lookups first and leaves the lights as they are when one is gone; through the monitor (a kind 1
  setting, an empty group: the 0.1.20 crash) it logs `lights: ... a light is gone` and the game runs on.

### Next

- The same layers for Armageddon (with its Motor Kombat side mode) and Deadly Alliance, from a shared core.
- Rematches without the main menu in between (the game takes `rematch`; the launcher and server do not send it).
- In-game character and team select in sync, instead of the launcher's picks.

### Puzzle and Chess Kombat desyncs: the screen's width and what a game remembers (2026-11-09)

- The guest's viewport width 0x6212D4 is 720 on a screen 3:2 or wider, else 640. Puzzle's fighter director (15 functions
  in 0x181D30..0x18E3D0: walk limits, places) and Chess's panels (the width readers in 0x100000..0x120000: a slide's end
  decides when kFlags bit 0x4000 and the chess game's +0x34 / +0x50 change, a few frames apart) ran by it, so a 4:3
  player and a wide one parted. In a match in sync those functions see the narrower of the two players' widths, told at
  the meeting point (src/game/agreed_width.cpp; READY word kWordWidth, a spectator gets both). Same-shape screens: no change.
- sub_000F52A0 draws a variant and draws again while it equals 0x3EE904, the last match's, which nothing resets (boot
  0x2710): each Puzzle match in sync starts from the boot value (netgame.cpp Agree).
- Known and left: a game that played Puzzle earlier in the session and has the same fighter on the same side again loads
  that fighter about 3 frames sooner, so its intro starts at 86 where a fresh game's starts at 91: the two part at frame
  8, a no contest at frame 30. One fighter is enough: after 0:10 or 10:4, a match of 0 vs 4 against a fresh game parted;
  after 10:0 or 4:0 (those fighters on the other side) or 19:10 (other fighters) it did not. A rematch is the same on
  both games, so it takes a player who changed opponents. That the side matters suggests a per-side check rather than
  the table 0x3FC490 alone (not confirmed). Ruled out 2026-11-09: emptying the director's slots 0x3FDB80 with the game's
  own unload changes nothing; both games open the same files in the same order, but at the load's frame 7 they read
  different parts of the fighter's .ssf (warm 1869824, 2000896, 2766848; fresh 1808384, 1835008), so what the load asks
  for differs. 0x3EBB28.. are the positions of an input event ring (0x6200A0, 30 entries). Writing fresh values over the
  words that differ did not find it, and most such writes hang the game.
- CheckBlocks now includes a side mode's board, so a desync's dump shows it.
