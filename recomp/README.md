# recomp: the native ports

One XboxXBERecomp game project per title, created from its `templates/game`:

```
recomp/mkd/    Mortal Kombat: Deception   (4D570019, XDK 5849)   first
recomp/mka/    Mortal Kombat: Armageddon  (4D570034, XDK 5849)
recomp/mkda/   Mortal Kombat: Deadly Alliance Rev 1 (4D57000C, XDK 4721)
recomp/common/ what the games share: netplay, rollback, the launcher link
```

Each holds the toolkit's config (`config/game.toml` from `xbe_symbols`), our
overrides and hooks (`src/game`), its side of the shared netplay layer
(`recomp/common/README.md`), and the build scripts. The player's XBE and disc data are never here; they are read
from `orig/<game>/` locally and from the player's own disc in the builder.

Pipeline and toolkit assessment: `docs/research/xboxxberecomp.md`.

## SDK patches

`recomp/patches/` holds the changes we need in the toolkit checkout
(`external/XboxXBERecomp`, git-ignored). Reapply after a fresh clone with:

```
for p in recomp/patches/xboxxberecomp-*.patch; do git -C external/XboxXBERecomp apply "../../$p"; done
```

- `0001-cmath-remainder`: the recompiler emits `std::remainder` for the x87
  `fprem1` instruction, but the SDK's freestanding `cmath` shim did not
  declare it; Deception's generated code hit this three times. Worth sending
  upstream.
- `0002-thread-exit-status`: the runtime never wrote ETHREAD.ExitStatus
  (+0x120) when a thread exits, so XAPI's GetExitCodeThread (which reads the
  guest object) returned garbage. CRI's ADX/Sofdec shutdown polls it for each
  worker thread with a 30 s timeout each, which froze Deception after the
  first movie. Worth sending upstream.
- `0003-setjmp-continuations`: `[xbe] continuations = [...]` makes a
  setjmp return point a resumable continuation, so a `longjmp` (an
  `X86Resume`) thrown from deep inside lifted code, even from a task fiber,
  resumes in the frame that called setjmp. The generated catch loops, since
  the resumed code can longjmp again. Deception's main loop needs it
  (`0xDFFBF`): the engine longjmps back to it after each movie or sequence.
- `0004-dsound-notifications`: `IDirectSoundBuffer_SetNotificationPositions`
  on the host mixer, with offset crossings and `DSBPN_OFFSETSTOP` signalled
  as the play cursor moves. Deception's sound manager polls 56 such events to
  replay looped sounds, free voices and refill streamed audio; without it the
  library's own code ran on a host object and crashed entering character
  select.
- `0005-d3d-device-mirrors`: a host `D3DDevice_SetDepthClipPlanes` (kept,
  not yet applied by the renderer), and the device fields the recompiled
  getters read (`GetVertexShader` +0x798, `GetPixelShader` +0x784,
  `GetMaterial` +0xF00, the clip planes +0x1C08) kept current by the host
  setters. New 5849 layout keys, proven in Deception's D3D8.
- `0006-switch-bounds`: jump table bounds from a compare on a stack variable
  (`cmp dword ptr [ebp-x], n; ja; mov r, [ebp-x]; (movzx i, byte [r+t];)
  jmp [i*4+table]`), the shape of unoptimised code; a bound whose targets
  leave the code is ignored. Without it the tables of Deception's hit
  reaction (0x20C30) were over-read, a bogus function was decoded over the
  index bytes, and the third table lost a case: a crash 12 s into a fight.
  Also `XBE_RECOMP_TRACE_TABLE=<jmp>`, `XBE_RECOMP_TRACE_WALK=<entry>` and
  `XBE_RECOMP_LIST_TABLES=1` for the next one.
- `0007-guest-memory-hooks`: two opt-in hooks for a game layer that
  write-protects guest pages to track changes (rollback netplay):
  `kernel::SetGuestWriteHook` is called before the runtime has the OS write
  guest memory itself (a file read straight into a guest buffer, where no
  fault would be seen), and `mem::SetCommitHook` after user pages are
  committed or decommitted.
- `0008-xdk-4721`: a new, self-contained `xdk/4721/` (no runtime code
  changed) for Deadly Alliance, the only XDK 4721 title here. `layout.toml`
  holds every key the runtime reads, each proven at an instruction of
  Deadly Alliance's D3D8, DSOUND or XAPI code (the 4721 CDevice has the 4831
  layout; two 5849-style mirror keys have 4721 values: `VertexShaderHandle`
  +0x384, `Material` +0xAB0). `signatures.toml` was made with
  `xbe_symbols --sign` from Deadly Alliance's completed config; a scan with
  it reproduces that config (145 functions, 12 data). Until it existed the
  title was scanned with the 4831 set as a stand-in (116 found) plus the 5849
  set for the LIBCMT math (same CRT code). Details:
  `research/mkda-rev1/notes.md`.
- `0009-guest-clock`: every clock the guest reads (the time stamp counter,
  the performance counter, the interrupt time, KeTickCount) comes from
  `kernel::GuestNanoseconds()`, real time unless a game layer installs a
  hook (`kernel::SetClockHook`). Netplay and rollback make it a function of
  the simulated frame on the main thread, so every peer and every run of a
  frame reads the same times; the game layer then publishes the tick count
  and interrupt time itself (`kernel::PublishGuestTime`).
- `0010-host-rollback`: the state a title's simulation keeps outside guest
  memory, recorded per frame so a rollback can undo it (opt-in,
  `kernel/host_rollback.h`; nothing changes unless a game layer calls
  `host_rollback::Begin`). The recording thread's contiguous and pool
  allocations and frees (an undone free takes the same block back), kernel
  objects' signal states, handles and pointer objects (left alone when
  another thread uses the object too), and DirectSound voices (a copy before
  each voice's first change in a frame). DirectSound also gets a frame clock
  (`xdk/dsound.h`): the voices move by `MixFrames` once per simulated frame
  instead of with the output device, so buffer status, play cursors and
  notification events are the same on every peer and in every run of a
  frame; voices other threads drive (CRI's streaming buffers) move only with
  the frames that are heard. The file status block write is reported
  through the 0007 hook too.
- `0011-netplay-pacing`: `d3d::SetSkipPresent` (a frame a rollback runs
  again to catch up is neither shown nor paced; Swap still rotates the
  frame buffers and counts the swap) and `app::PaceShift` (moves the frame
  cadence: netplay time sync slows a game down this way, a sleep elsewhere
  would be made up by the pacer running the next frames back to back).
- `0012-file-position-rollback`: a file's position as the recording thread
  moves it is recorded per frame too (a frame run again reads on from where
  it read the first time), and `kernel::RecordedFileReads()` counts the reads
  made while recording, so a game layer can tell frames that load.
- `0013-keyboard`: the keyboard plays as the controller on port 0 (merged with
  a pad there) while one of the game's windows is in front: arrows or WASD
  move, J I K L are X Y A B, Space the right trigger, O the left trigger, U
  and P white and black, Enter START, Backspace BACK; `XBE_KEYBOARD=0` turns it
  off. Port 0 counts as connected from the start.
- `0014-virtual-memory-rollback`: the recording thread's user virtual memory
  is journaled per frame like the pools: reservations, commits, decommits and
  releases change the guest's view at once and are undone by a rollback, while
  a decommit or release only zeroes its pages and keeps them committed until
  the frame is final (a rollback across the free finds them, a commit of them
  again finds them zeroed as a fresh one is). Found by Armageddon's Motor
  Kombat, whose XAPI heap grows and shrinks while a race loads.
- `0015-deterministic-math`: the transcendental functions the game's state
  depends on (x87 `fsin`, `fcos`, `fptan`, `fpatan`, `fyl2x`, `f2xm1` and the
  CRT's sin/cos/tan/asin/acos/atan/log/log10/pow the runtime replaces) come
  from musl 1.2.5 built into the executable (`runtime/detmath`, MIT) instead of
  `ucrtbase.dll`, whose results change between Windows releases and with the
  processor's FMA3: two PCs in a rollback match compute the same bits. Within
  1 ulp of the C runtime's (a million random inputs per function); `sqrt`,
  `fmod`, `ldexp` and `frexp` are exact and stay the C runtime's.
- `0016-controls`: what each key and each controller button presses comes
  from a controls file (`XBE_CONTROLS`, else `controls.toml` in the exe's
  folder or a folder above it; the launcher's Controls page writes it):
  `[keys]` lists each Xbox button's keys, `[buttons]` the Xbox button each
  button of a PC controller presses (by SDL's positions: south, east, west,
  north, the shoulders and triggers, the d-pad, start, back, the stick clicks,
  misc, four paddles, the touchpad), `keyboard = false` turns the keyboard
  off. Read again within a second of a change; a broken file keeps the
  controls it had, a missing one means the layout of 0013. Netplay is
  untouched: a remap only changes the pad the game reads on this PC.
- `0017-file-internal-information`: `NtQueryInformationFile` answers
  `FileInternalInformation` (class 6) with an index per Xbox path. XAPI's
  `GetFileInformationByHandle` asks for it; Deception's profile kollection
  load does that before it reads the file, so without it every profile saved
  in an earlier session failed to load ("no space for new profiles").
- `0018-xinput-controllers`: SDL's XInput backend is on. With Xbox devices
  left to Windows.Gaming.Input alone, an XInput device WGI does not count as
  a gamepad came through as a plain joystick, which the runtime never reads:
  a PDP MK X Fight Stick (0E6F:0143, Xbox 360 mode) did nothing in any game.
  XInput takes every XInput device as a gamepad (triggers apart, read whether
  or not the window is in front), WGI skips those and keeps the rest, HIDAPI
  still takes PlayStation and Switch pads.
- `0019-audio-queue`: on the frame clock (0010), the sound reaches the output
  through a jitter buffer: the queue plays with about 50 ms in it, read up to
  2% faster or slower to stay there, fading out where it runs dry and filling
  to 50 ms before it plays on. It held a frame or two before, so every frame
  that came late (a wait for the other game's inputs, a frame shown later for
  time sync, a rollback catching up) broke the sound off: online the music cut
  in and out, at a fight's start above all. Also what the output lost, once a
  second at most (`dsound: stream ... ran dry`, `dsound: frame clock: ... of
  silence`), and `XBE_AUDIO_DUMP=<file.wav>` (`{pid}` in it keeps two games
  apart) records everything sent to the speakers.
- `0020-frozen-time`: `kernel::RealNanoseconds` leaves out the time the
  process stood still. The launcher freezes a single-player game for the
  length of an online match (standby); thawed, the game would have found the
  minutes gone at once (menu idle timers, timeouts). The clock thread reads it
  every millisecond, so more than a second between two reads is time nothing
  ran, all but a millisecond of which is dropped. Read under a lock, it never
  goes backwards.
- `0021-notify-at-play-start`: a buffer notification at the position play
  starts from counts as passed when the buffer starts. All three games stream
  their music through a looping double buffer (Deception: 144144 bytes,
  32 kHz stereo ADPCM, notifications at 0 and at the half) that plays with
  only its first half filled and refills the other half on that first
  notification. Without it the empty half played as two seconds of silence,
  then the first half again: the music stopped and started over on every
  screen and when the announcer spoke (arcade recordings: Deception 2.0 s,
  Armageddon 2.0 s, Deadly Alliance 1.5 s, gone with it).
- `0033-resume-after-the-handler` (0.1.16): a frame that catches an
  `X86Resume` (0003) copies it, leaves the catch block, and only then runs
  the continuation, in a loop in the function's body. Before, the
  continuation ran inside the handler: after a longjmp the whole game ran
  inside main's catch block, and after a task's exit outside its function
  the rest of the task walk ran inside the walk's. The Visual C++ runtime
  keeps one list of open catch blocks a thread; it links a new one onto the
  list only when its record sits lower on the stack than the list's head,
  else it starts the list over, and it aborts when a catch block that ends
  is not the head (`_CreateFrameInfo`, `_FindAndUnlinkFrame`). The task
  fibers (`recomp/common/src/mk3d/sfiber.*`) share the thread, and their
  stacks lie above it: a task that caught anything (a `TaskReturn`) while
  the walk's handler was open dropped the walk's record, and the walk's
  catch block then aborted (`C0000409`, the 0.1.14+ crash line
  `MKD.dll+2E01CE`, `MKA.dll+4361C6`, `MKDA.dll+2169D5`: death traps,
  fatalities, Quan Chi). No catch block may stay open while game code runs;
  `sfiber::SwitchTo` logs it once if one ever does.
- `0034-window-icon-from-the-title-image` (0.1.17): the game window's icon
  (title bar, taskbar, Alt+Tab) is the game's own logo: the title image its
  xbe carries for the Xbox dashboard (`$$XTIMAGE`, an XPR0 texture, 128x128
  DXT1 in all three games), decoded and box-filtered to the system's icon
  sizes (`app/title_image.cpp`). The player's own copy supplies it, so no
  release carries it; an xbe without one keeps the executable's icon. The
  launcher shows the same image on its game buttons (`TitleImage.cpp`).
- `0035-fatal-errors-hangs-and-the-pointer` (0.1.18): `app::Fatal`, for a
  failure the game cannot go on from: the player is told why in a message box,
  the log gets `crash: fatal at <where>` (which the launcher reports) and the
  process ends with exit code 3. The renderer uses it when no graphics card of
  the PC offers Direct3D 12 (`CreateDevice` tries each one and logs them;
  before, the game went on and crashed at its first draw, `C0000005 at
  <G>.exe+688C9`, 60 reports from 4 players on 0.1.17), when it gets no swap
  chain, or when its init fails otherwise; `XBE_TEST_NO_DEVICE=1` acts out the
  first. `WatchWindow`: a window thread that takes no message for 5 s (Windows
  turns the window white, "Not Responding") is logged (`hang: ...`), the guest
  threads' stacks with it and a dump of every thread in CrashDumps
  (`<G>.hang.<time>.dmp`, `WriteDump`), and again when it answers;
  `XBE_TEST_HANG_WINDOW=<s>` stalls the window thread 7 s once (no dump from
  in the process: a game that wrote one of itself stood still after). No mouse
  pointer over the game (`WM_SETCURSOR`; ImGui's own pointer changes are off,
  an overlay that takes the mouse would get the arrow).
- `0036-point-sprites` (0.1.18): point sprites. Deadly Alliance draws
  Scorpion's spear rope (game code at 0x14F606, sub_0014F5D0: a point list of
  3 to 21 points, vertex shader B01D26D1 writing `oPts`, a 32x32 bead texture
  on stage 3) and its blood spray as point sprites, which were drawn as one
  pixel each without coordinates, the stage chain stopping at the disabled
  stage 1: the rope was invisible. The point size comes from the vertex shader
  or `D3DRS_POINTSIZE` (with `POINTSCALEENABLE`'s distance scaling), clamped to
  the min/max states; a geometry shader (`PointHlsl`) makes the quads, and in
  sprite mode stage 3 alone samples, with coordinates (0,0) to (1,1), as the
  hardware gives them to stage 3 only. A capture lists a draw dropped early;
  `XBE_CAPTURE_VERTS=1` also lists every vertex, the stream inputs, the point
  states and each stage's ops.
- `0037-rollback-waits-for-the-renderer` (0.1.18): `d3d::WaitForRenderer`,
  which the games' rollback calls before a restore writes guest memory: the
  render thread runs a frame's draws after the game thread recorded them and
  reads vertices, textures and surfaces from guest memory then, so a restore
  in between showed that frame drawn half from the restored state
  (Armageddon online: a camera inside a fighter, the health bars gone, for one
  frame). Frames swapped without a present (a spectator catching up unseen,
  thousands in a row) flush the renderer every 8 (`kFlushUnseen`), as a
  present gives its upload and descriptor rings back.
- `0038-unseen-frames-are-not-drawn` (0.1.18): a frame the netplay runs
  unseen (`d3d::SetSkipPresent`: run again in a rollback, or caught up by a
  spectator) drops its draws and clears too, as the Tekken 6 port skips its
  draw tree in a re-simulated frame. A rollback runs up to several frames
  inside one, and their drawing, which nobody sees, made the frame shown after
  them late (Deception at 180 ms and a 200% render scale: 12-13 ms a rollback
  and 14-33 frames late after one a match, against 8-10 ms and 0-4). What the
  title can see of the renderer stays: states, fences, copies, read-backs,
  visibility reports (no pixels counted then). Draws between a swap and the
  next frame's top are drawn whichever frame they turn out to be, and logged
  once (`came between a swap`; none in the three games), as is a surface
  locked in a frame not drawn (none either). `XBE_DRAW_UNSEEN=1` draws them
  as before, to compare.
- `0039-a-null-palette-keeps-the-last` (0.1.19): `SetPalette(stage, NULL)`
  leaves the stage drawing with the palette it had, as the console's GPU
  does: the XDK sends the GPU a palette's address only when one is set
  (Deadly Alliance's `D3DDevice_SetPalette` at 0x1D5760), and Deadly
  Alliance's three calls pass a texture object's palette, NULL for one
  without. Drawn with no palette, a P8 texture decodes as grey with an
  opaque black for index 0 (`XBE_TEST_PALETTE_NULL=drop` after every set
  gives that picture, `hold` the right one). A copy of the palette's header
  stands in, since the palette itself may be freed once released. F3
  captures name each P8 texture's palette (`pal <address>` or `pal none`).
  A streamer's long session had shown such pictures (menu text white on a
  black box for single frames, Shang Tsung's select portrait grey for
  seconds); no game here was seen to set a NULL palette, and 0040 found
  what drew them.
- `0040-p8-textures-are-never-targets` (0.1.20): a P8 texture is never
  sampled as a render target. The renderer keeps a target after its memory
  goes to other data, and a texture of the same size loaded there was taken
  for the target (`BindTargetTexture`), its memory decoded as the target's
  copy without the palette: grey on an opaque black, or what the target
  last held. Deadly Alliance's select screen fire is an animation of
  128x128 P8 textures, and the one loaded where a 128x128 target had been
  came out so once each time it came round (captured as `target` in its
  draw line; `d3d12: P8 texture ... where a target was` in the log now).
  Targets pile up as a session goes on (10 to 76 in 18 minutes of practice),
  so such pictures got more frequent the longer the game ran. The GPU
  renders no palettized surface, so a P8 texture is always its own memory
  and palette. `LOGR_FIRST` (`core/log.h`) writes that note, and 0039's
  `SetPalette(.., NULL)` one, in release logs too: `LOG_FIRST` logs at
  `LOGI`, which a release build leaves out.
- `0041-a-picture-behind-the-select-screen` (0.1.21): a game can see each
  draw before the renderer does (`d3d::SetDrawFilter`, with the vertices of a
  draw from its own array or an unindexed vertex buffer) and let it be drawn,
  skip it, or draw a picture over the whole target in its place: the PNG
  `select_background.png` beside the game (any picture Windows' imaging
  decodes), the 4:3 middle of it, or all of it when the game is stretched to
  16:9. F3 captures write such a draw as `user image`. Deadly Alliance's
  select screen drew it in place of its clouds in 0.1.21; since 0.1.22 no
  game sets a filter, so nothing changes what is drawn.
- `0042-a-rollback-does-not-wait-for-the-display` (0.1.25): `WaitForRenderer`
  (0037) waited until the render thread was through the last frame's
  `Present`, whose end waits for the display (`WaitForDisplay`, up to a refresh:
  16.7 ms at 60 Hz) before the frames of a rollback could start, and so took that
  time from them. A restore only needs the guest memory read, so `Present` now
  says when it has read all it reads (`Backend::on_frame_read`) and such a wait
  (`Backend::WaitRecorded`, and the title's fences and busy checks) ends there.
  The title's frame throttle still waits for the whole `Present`: pacing and the
  fps cap stay as they were. Minimised windows wait for no display, so tests
  never saw this: `XBE_TEST_DISPLAY_HZ=<refresh>` makes the wait without one, and
  `XBE_TEST_RENDER_WAIT_FULL=1` brings the old wait back to compare (at 232 ms
  ping, 60 Hz, with the game's stick filter: 3.1% of frames late against 0.5%).

`tools/py/lib_audit.py <game dir> <xbsdb symbols>` lists the XDK library
functions the game calls that still run console code against host objects,
the usual next crash.

## Assets layout (learned the hard way)

The runtime prefers a loose `assets/default.xbe` over any `.iso` beside it
and then maps drive D: to the assets folder without mounting the image. The
dev build needs the loose XBE (the recompiler reads it), so for development
extract the whole disc into `assets/` (`tools/py/xiso.py --extract-all`).
Players get the ISO-only path, where the runtime mounts the image itself.
