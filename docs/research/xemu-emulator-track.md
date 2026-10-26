# xemu as the "emulator track" for MK:DA / MKD / MKA rollback

Scope: xemu shallow clone at `reference/xemu` (HEAD `25e1ee2`, QEMU 10.x base). Paths below are relative to
that clone. The question is whether xemu can play the role RPCS3 plays in Ekspym's Tekken 6 fork
(per-frame snapshots, inputs injected at the pad poll, GGPO re-simulation).

## 1. Snapshots (savestates)

**Mechanism.** xemu savestates are stock QEMU `savevm`/`loadvm` internal snapshots written into the
qcow2 HDD image; there is no in-memory path.

- `ui/xemu-snapshots.c:241-258` – `xemu_snapshots_save()` is `save_snapshot(name, overwrite=true, ...)`;
  `xemu_snapshots_load()` does `vm_stop(RUN_STATE_RESTORE_VM)` → `load_snapshot()` → `vm_start()`.
- `migration/savevm.c:3203-3302` (`save_snapshot`): `vm_stop`, `bdrv_drain_all_begin`, open a QEMUFile on the
  block device (`qemu_fopen_bdrv`, :3285), serialise all vmstate, then `bdrv_all_create_snapshot` creates a
  qcow2 internal disk snapshot (the HDD contents are snapshotted too, copy-on-write).
- `migration/savevm.c:1817` – xemu prepends its own header (magic, disc path, XBE title name, PNG thumbnail of
  the framebuffer) via `xemu_snapshots_save_extra_data()` (`ui/xemu-snapshots.c:260-302`); skipped on load at
  `:3050`.
- `migration/savevm.c:3396-3470` (`load_snapshot`): `bdrv_all_goto_snapshot` (reverts the disk image),
  `qemu_system_reset(SHUTDOWN_CAUSE_SNAPSHOT_LOAD)` (full machine reset of every device), then
  `qemu_loadvm_state`.

**What is saved.**

| Component | Where | Notes |
|---|---|---|
| RAM 64/128 MB | `hw/xbox/xbox.c:194` (`xbox.ram`), size from `system/vl.c:3056` `(mem_limit+1)*64` | Standard RAM migration; zero pages compressed. |
| VRAM | `hw/xbox/nv2a/nv2a.c:217-218` "xbox is UMA - vram *is* ram" | Same region as RAM; nothing extra. |
| RAMIN 1 MB | `nv2a.c:227` separate RAM region | Saved with RAM. |
| NV2A regs | `nv2a.c:465-593` `vmstate_nv2a` v5 | PGRAPH regs (0x2000 words), PFIFO, PVIDEO, PFB, PCRTC, PRAMDAC, PTIMER, vsh program/constants, inline vertex arrays, `waiting_for_flip`. `vertex_attributes` subsection is empty (`:455-463` "FIXME"). |
| MCPX APU | `hw/xbox/mcpx/apu/apu.c:468-560` | Full DSP core (pc, regs, xram/yram/pram, mixbuffer), DMA state, `gp.regs` 0x10000 words. |
| nvnet | `hw/xbox/mcpx/nvnet/nvnet.c:1032-1047` v2 | MMIO regs, PHY regs, ring indices only; in-flight packets and link state are reconstructed in `post_load` (`:995-1030`). |
| USB gamepad | `hw/xbox/xid-gamepad.c:223-239` | `VMSTATE_USB_DEVICE` only, "FIXME": the 20-byte input report is not saved. |
| ACPI PM, SMBus, LPC | `hw/xbox/acpi_xbox.c:135-148`, `xbox_pci.c:281-305,409-453` | Includes the PM timer (`VMSTATE_TIMER_PTR`). |
| CPU | stock `target/i386/machine.c` | Pentium III model (`xbox.c:457`). |

**Pre-save / post-load work (the expensive and lossy parts).**

- `nv2a.c:396-424` `nv2a_vm_state_change`: on `RUN_STATE_SAVE_VM` the PFIFO thread is halted and the renderer
  is asked to download every dirty surface from the GPU back to guest RAM, then the BQL is dropped while
  waiting (`pgraph/gl/renderer.c:151-166`, `pgraph/gl/surface.c:843-855`, `surface_download` at `:801`). A
  snapshot therefore embeds host-rendered pixels.
- `apu.c:348-371` `mcpx_apu_vm_state_change`: waits for the APU thread to go idle and `dsp_sync_to_vm`.
- `nv2a.c:440-453` `nv2a_post_load`: sets `flush_pending`; the PFIFO thread then runs `pgraph_gl_flush`
  (`gl/renderer.c:108-117`) which (a) `flush_surfaces` (`gl/surface.c:1465-1484`) **invalidates every surface
  binding** (a commented-out download notes a "corruption issue"), (b) marks all textures over the whole of
  VRAM possibly dirty (`gl/texture.c:60`), and (c) re-uploads the entire memory buffer used for vertex data
  (`gl/vertex.c:56`). FBOs and GL textures are re-created on the next draw. The shader cache is a host object
  cache keyed by pipeline state (`gl/shaders.c`, disk-backed when `perf.cache_shaders`) and survives a load.
- `accel/tcg/tcg-all.c:88-101` `tcg_vm_change_state`: on `RUN_STATE_RESTORE_VM` **all translation blocks are
  flushed** because loadvm writes RAM behind the back of the dirty-code tracking.

**Cost in code terms.** Save = stop VM + drain block layer + synchronous GPU readback of dirty surfaces +
serialise 64-128 MB of RAM plus device state into the qcow2 + create a qcow2 snapshot. Load = revert qcow2 +
full machine reset + re-read RAM + TB flush + re-create all GPU surfaces/textures. This is a "tens to hundreds
of milliseconds, touches the disk" operation, not a per-frame one. QEMU does have an in-memory QEMUFile
(`io/channel-buffer.c`, used by `migration/colo.c` and `savevm.c:3341` for device-state-only dumps) so a fork
could redirect `qemu_savevm_state` into RAM, but it would still iterate the whole RAM region and still trigger
the reset/TB-flush/surface-invalidation semantics above unless those are rewritten.

## 2. Determinism

**No icount; everything is wall-clock.** The synthetic command line in `system/vl.c:2965-3115` is
`-machine xbox,... -m 64|128 -drive ... -display xemu -device usb-hub,...` with no `-icount` and no `-rtc`.
Without icount `QEMU_CLOCK_VIRTUAL` is host monotonic time minus pause time (`system/cpu-timers.c:74-80`),
so the number of guest instructions executed per unit of guest time varies run to run and host to host.

Timer sources the guest sees:

- TSC / `rdtsc`: `hw/i386/x86-cpu.c:35-39` (`#ifdef XBOX`) returns `QEMU_CLOCK_VIRTUAL` scaled to
  733,333,333 Hz; `target/i386/tcg/misc_helper.c:64-73`. The kernel's `KeQueryPerformanceCounter` therefore
  reads host time at 733 MHz, not retired instructions.
- ACPI PM timer: 3.375 MHz under `XBOX` (`include/hw/acpi/acpi.h:53-54`), from `QEMU_CLOCK_VIRTUAL`
  (`hw/acpi/core.c:499,555`).
- NV2A PTIMER: `QEMU_CLOCK_VIRTUAL` × `pramdac.core_clock_freq` (`hw/xbox/nv2a/ptimer.c:72-77`).
- **Vblank**: `nv2a.c:203-213` raises `NV_PCRTC_INTR_0_VBLANK` from the display `gfx_update` callback, which
  is driven by the UI's vblank timer/thread on `QEMU_CLOCK_REALTIME` every 16.666 ms
  (`ui/xemu.c:78, 758-790, 1217-1222`). The guest's frame interrupt is host-paced and unrelated to how
  much the vCPU executed. `FLIP_STALL` (`pgraph/pgraph.c:921-927`) blocks the PFIFO until the PCRTC
  read/write surface indices differ (`pfifo.c:107-143`), so game frame pacing is coupled to this timer.

**GPU (NV2A) on the host.** PGRAPH executes on the `nv2a.pfifo_thread` (`nv2a.c:245`, `pfifo.c:456-485`)
with its own GL/Vulkan context (`gl/renderer.c:53,87`), asynchronously from the vCPU. Guest-visible
host-dependent state:

- Occlusion reports: `NV097_GET_REPORT` → `glGetQueryObjectuiv` (`pgraph/gl/reports.c:36-50`), result divided by
  the surface scale factor and written into guest RAM with a fake timestamp (`pgraph/pgraph.c:3068-3083`).
  Values depend on the host driver and on `display.quality.surface_scale`.
- Surface downloads (`gl/surface.c:801`) copy host-rendered colour/depth back into UMA RAM whenever the CPU
  may read it; any CPU-side readback (screenshots, render-to-texture tricks) sees host pixels.
- Everything else (register state, vertex/constant data) is CPU-written and deterministic.

**APU.** The MCPX APU runs on `mcpx.apu_thread` (`apu.c:428`) paced by `QEMU_CLOCK_REALTIME` and the SDL audio
queue depth (`apu.c:168-215` throttle, `:258-300` loop); DSP DMA reads/writes guest RAM asynchronously, and
`VP` voice workers are extra host threads (`apu/vp/vp.c:1786`). Relative ordering against the vCPU is not
reproducible. It is quiesced only around save/load (`apu.c:348-371`).

**x87 "hard FPU".** `target/i386/tcg/translate.c:38-60, 4231` swaps the x87 helpers for host `long double`
arithmetic when `perf.hard_fpu` is on (default true, `config_spec.yml:370-372`), x86-64 hosts only.
`target/i386/tcg/fpu_helper.c:73-120` carries "FIXME: rounding and exceptions" (control-word rounding mode is
ignored; precision control is applied via `pack()`). Soft-float is used on ARM hosts and when the option is
off, so two peers must match this setting and host architecture. SSE stays on QEMU softfloat.

**USB input timing.** OHCI start-of-frame every 1 ms on `QEMU_CLOCK_VIRTUAL` (`hw/usb/hcd-ohci.c:1181-1189,
1291, 1905-1912`); the gamepad interrupt IN endpoint has `bInterval = 4` (`xid-gamepad.c:50-60`). Each IN token
calls `update_input()` (`xid-gamepad.c:137-141` → `hw/xbox/xid.c:55-104`), which samples SDL state no more
often than every 2.5 ms of real time (`ui/xemu-input.c:49, 508-521`). The UI loop also polls at
`ui/xemu.c:996`.

**Net effect.** Two xemu instances given identical inputs will not stay in sync: vblank count, TSC, PM timer
and APU progress are all wall-clock functions, and the GPU readbacks are driver functions. Determinism would
require `-icount`-style deterministic virtual time, a vblank re-based on virtual time (e.g. a PTIMER/PCRTC
deadline, ~12.2 M cycles per frame at 733 MHz), a synchronous or frame-locked APU, and either disabling or
emulating occlusion queries. QEMU's own record/replay (`replay/`) is the existing hook for this but is not
wired into xemu's UI and conflicts with the realtime vblank thread.

## 3. Input path and injection point

- Devices: two `pci-ohci` controllers (`hw/xbox/xbox.c:317-323`), a 4-port `usb-hub` on port 1
  (`vl.c:3102`), and `usb-xbox-gamepad` / `usb-xbox-gamepad-s` XID devices (`xid-gamepad.c:41-127`:
  VID 0x045e, PID 0x0202/0x0289, 20-byte input report `XIDGamepadReport` in `hw/xbox/xid.h:97-106`).
- Binding/hot-plug is done from the UI via qdev (`ui/xemu-input.c:679-735`, `qdev_unplug` on unbind).
- **Poll hook**: `hw/xbox/xid.c:55-104` `update_input()` is the single place where host controller state is
  converted into the guest report; it already has a bypass flag (`xemu_input_get_test_mode()`, `:57`). A
  rollback fork would replace `xemu_input_update_controller(state)` with "read the GGPO-synchronised input for
  the current frame for `device_index`". The OUT endpoint (`xid-gamepad.c:145-149`) is rumble.
- **Frame boundary**: the only emulator-level "frame" is the host-paced vblank in `nv2a.c:203-213`; the
  game-level boundary is `FLIP_STALL` (`pgraph.c:921`) / PCRTC surface swap (`pfifo.c:107-125`). The game reads
  the pad via the kernel's USB stack, so inputs sampled at a 4 ms USB interval land in the game's per-frame
  `XInputGetState` poll; a fork must latch one report per guest frame rather than per USB frame.

## 4. Network

- NIC: nForce `nvnet` (`hw/xbox/mcpx/nvnet/nvnet.c`), default NIC of the machine (`xbox.c:459`);
  `NetClientInfo` at `:716-723`, descriptor-ring DMA (`:398-555`), autoneg timer on the virtual clock (`:670`).
- Backends (`ui/xemu-net.c:32-55`, `config_spec.yml:327-351`): `nat` (slirp user-mode, with host port
  forwards `:82-103`), `udp` (QEMU `socket` netdev, `udp=remote,localaddr=bind`, defaults `0.0.0.0:9368` ↔
  `1.2.3.4:9368`), `pcap` (raw host NIC via libpcap/npcap). All attach through `hubport` on hub 0 and
  `qmp_set_link("nvnet.0")`.
- System Link between two xemu instances is therefore "two emulated Ethernet cards on a UDP tunnel or a
  bridged LAN", running the game's own System Link netcode unchanged (the MK titles ship System Link). It is a
  delay-based LAN protocol; nothing in xemu interprets or rewinds it.
- No netplay/rollback/GGPO material exists in the repo: `README.md` only points to xemu.app; greps of
  `docs/`, `ui/`, `hw/xbox/`, `AGENTS.md`, `CONTRIBUTING.md`, `xemu.metainfo.xml` return nothing.

## 5. Performance

- Single vCPU: `xbox.c:452` `max_cpus = 1`, so one TCG thread; MTTCG is moot. Other host threads:
  `nv2a.pfifo_thread` (GPU), `mcpx.apu_thread` + voice workers, shader-cache disk thread
  (`gl/shaders.c:580`), `vblank-timer` thread (`ui/xemu.c:1217`), SDL/UI main thread.
- Re-simulating one frame means executing ~1/60 s of guest time on the TCG thread (~12.2 M P3 cycles at
  733 MHz, in practice several ms of host time for a 3D fighter) *and* re-running the GPU push buffer on the
  PFIFO thread; the NULL renderer (`pgraph/null/`, `config_spec.yml:227-230`) could suppress drawing during
  rollback frames, but surfaces the CPU reads back would then be wrong.
- JIT survival: **no**. `accel/tcg/tcg-all.c:88-101` discards every translated block on each restore, so every
  rollback is followed by a retranslation storm. A fork would need page-granular invalidation
  (`tb_invalidate_phys_range`) driven by a RAM diff instead of the blanket flush, plus removal of the
  `qemu_system_reset` in `load_snapshot` (`savevm.c:3446`).

## 6. Compatibility notes for the MK titles

Nothing in-repo. `data/` holds fonts/icons only; greps for `4D57000C`, `4D570019`, `4D570034`,
"Mortal Kombat", "Deadly Alliance" across the tree return no hits. Title IDs come from the XBE certificate
(`xemu-xbe.h:80` `m_titleid`), used only to label snapshots (`ui/xemu-snapshots.c:267-274`). Compatibility
reports live on xemu.app, not in the source; they must be captured separately for this project.

## 7. Verdict

**An xemu rollback fork is not a practical path.** Unlike RPCS3 (where the Tekken 6 fork could checkpoint a
recompiled game thread and inject at the pad read), xemu is whole-system emulation whose only savestate is a
disk-backed, reset-everything QEMU snapshot (§1), whose guest clocks and vblank are wall-clock functions (§2),
whose GPU and APU run on asynchronous host threads with guest-visible host-dependent readbacks (§2), and whose
JIT cache is flushed on every restore (§5). Making it rollback-capable means: deterministic virtual time
(icount or equivalent) and a virtual-time vblank; an in-memory, dirty-page RAM snapshot with page-level TB
invalidation and no machine reset; a frame-locked synchronous APU; GPU readback emulation or suppression; a
per-frame input latch in `xid.c`; and all of it kept in step with upstream xemu. That is a new emulator
core wearing xemu's device models, with per-frame costs (full TCG re-execution plus GPU replay) that leave
little budget for 2-5 frames of rollback at 60 fps. A native recompiled port (see `xboxrecomp.md`,
`xboxxberecomp.md`) owns the game loop, the RNG, the pad read and the frame boundary directly and avoids
every item above.

**What xemu remains useful for.**

- Reference behaviour and a *loose* determinism oracle: with fixed inputs it shows what the game does, and
  diffs between runs identify which game state is timing-sensitive (useful for the port's desync hunting).
- Memory-map and savestate capture: a loadvm image is 64/128 MB of raw Xbox RAM plus device registers; the
  qcow2 vmstate stream can be parsed offline to locate the fight-state structures, RNG seeds and input
  buffers the native port must checkpoint.
- GPU tracing of the MK renderer: the PFIFO/PGRAPH method stream (`pgraph/pgraph.c` `DEF_METHOD` handlers,
  `nv2a` trace-events, RenderDoc hook in `pgraph/debug_renderdoc.c`) exposes the exact NV2A state, vertex
  programs and register combiners the games use, which is the specification for the port's renderer.
- Input/USB protocol reference: `xid.h`/`xid.c` document the XID report format the kernel expects.
- System Link ground truth: two instances on the `udp` backend exercise the games' own LAN protocol, which
  the native port may need to reproduce or replace.
