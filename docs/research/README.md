# Research surveys

Written 2026-10-24 from the checkouts under `reference/` and `external/`.
Each file cites file:line in those checkouts; re-verify before relying on a
line number after updating a checkout.

| File | Source studied | Question answered |
|---|---|---|
| `tekken6-native-online.md` | Ekspym/tekken6-native-online-source | The launcher, builder, netplay runtime and server protocol we copy the shape of (kept private: that source carries no license) |
| `tekken6-rpcs3-rollback.md` | Ekspym/rpcs3 `tekken6-rollback`, Ekspym/ggpo | How rollback was bolted onto a game: snapshots, frame boundary, inputs, audio, determinism (kept private, as above) |
| `xboxxberecomp.md` | TGP482/XboxXBERecomp | The primary toolkit: XDK signature coverage, pipeline, runtime, rollback hooks |
| `xboxrecomp.md` | sp00nznet/xboxrecomp | The alternative toolkit and which pieces to borrow |
| `cxbx-and-xbsymboldatabase.md` | Cxbx-Reloaded, XbSymbolDatabase | Naming the XDK functions in our XBEs; HLE semantics as reference |
| `xemu-emulator-track.md` | xemu | What the emulator can be for us: oracle, savestates, and whether a rollback fork is realistic |

Per-game findings live in `../../research/<game>/`.
