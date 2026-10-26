# Prior art

Collected 2026-10-24. Detailed surveys are in `docs/research/`.

## The model: Tekken 6 (Ekspym, October 2026)

- [Ekspym/tekken6-native-online-source](https://github.com/Ekspym/tekken6-native-online-source):
  the native Xbox 360 port. ReXGlue static recompiler (Xenia-derived) turns
  `Default.xex` into C++, GGPO fork for netplay, Qt 6 launcher, builder that
  compiles the game from the player's disc. Our launcher/builder template.
- [Ekspym/Tekken6-rollback](https://github.com/Ekspym/Tekken6-rollback) and
  [Ekspym/rpcs3](https://github.com/Ekspym/rpcs3) branch `tekken6-rollback`:
  the emulator fork. Its snapshot, frame-boundary and audio lessons are
  platform-neutral.
- [Ekspym/ggpo](https://github.com/Ekspym/ggpo): GGPO with their settings.
- Coverage: [DSOGaming](https://www.dsogaming.com/news/tekken-6-gets-unofficial-pc-port-with-online-multiplayer-rollback-netcode/).

## Original Xbox static recompilation (2026)

- [TGP482/XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp): x86 XBE to
  native 64-bit Windows via Zydis lifting; kernel and XDK (D3D8 on D3D12,
  DirectSound on XAudio2, XAPI, XNet) reimplemented at API level; per-XDK
  signatures (`xbe_symbols`); player-side first-launch build with bundled
  Clang/LLD; System Link over LAN or direct IP. MIT. Ports:
  [BLACK](https://github.com/TGP482/BLACK-XBERecomp) (V2, 2026-10-18),
  [Splinter Cell: Double Agent](https://github.com/TGP482/SCDoubleAgent-XBERecomp),
  [007: From Russia With Love](https://github.com/TGP482/007FRWL-XBERecomp).
  Write-up: [PortedGames on the BLACK port](https://portedgames.com/black-pc/).
- [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp): v0.16.0
  (October 2026), MIT. Lifts to C, kernel ordinals mapped to Win32, D3D8 to
  D3D11 with NV2A combiner/vertex-program translation and a pushbuffer
  executor, DirectSound software mixer, MCPX APU from xemu, OHCI USB
  emulation. Burnout 3 and others in bring-up, nothing playable yet.

## Original Xbox decompilations and ports

- [kirklandsig/halo2-decompiled](https://github.com/kirklandsig/halo2-decompiled): matching decomp of Halo 2.
- [halo-re/halo](https://github.com/halo-re/halo) and [OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE): Halo CE research decomp and its native port (AGPL-3.0 from 2026-10-17).
- GitHub topic [original-xbox](https://github.com/topics/original-xbox) lists further recomp ports (Def Jam: Fight for NY, DOA Xtreme Beach Volleyball, the LOTR games).

## Emulators and symbol databases

- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded): runs XBE
  code natively in a 32-bit process, reimplements the kernel, patches XDK
  functions found by OOVPA signatures. Its
  [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) is the
  broadest public catalogue of XDK function signatures across versions.
  [OOVPA wiki](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/wiki/Maintaining-OOVPAs-for-HLE-function-detection).
  Library code is MIT but the signature tables are ODbL-1.0, so symbol lists
  derived from them (`research/*/symbols-xbsdb.txt`) carry attribution and
  share-alike obligations. [mborgerson/ghidra-xbe](https://github.com/mborgerson/ghidra-xbe)
  is a Ghidra XBE loader with the database's pattern matching built in.
- [xemu](https://github.com/xemu-project/xemu): QEMU-based LLE emulator;
  compatibility entries for [Deadly Alliance](https://xemu.app/titles/4d57000c/)
  and [Deception](https://xemu.app/titles/4d570019/).

## Rollback libraries

- [HeatXD/GekkoNet](https://github.com/HeatXD/GekkoNet): C++20 P2P rollback SDK, BSD-2-Clause.
- [pond3r/ggpo](https://github.com/pond3r/ggpo): the original, MIT.

## Online for the originals

- [Insignia](https://insignia.live): Xbox Live 1.0 replacement for real
  hardware, about 200 supported titles as of 2026
  ([PureXbox](https://www.purexbox.com/news/2026/10/insignia-brings-back-online-multiplayer-for-lots-more-xbox-games-including-burnout-3)).
  Whether Deception and Armageddon are on its list was not confirmed.

## Ghidra

- [NationalSecurityAgency/ghidra](https://github.com/NationalSecurityAgency/ghidra/releases): 12.1.4 installed; x86 is built in. A Ghidra XBE loader that uses XbSymbolDatabase exists and is worth adding (see the Cxbx survey).
