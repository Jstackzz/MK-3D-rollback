# Target builds (original Xbox)

Every hook address, snapshot region and recompiler config is tied to one
exact XBE build. We identify builds by the XBE's SHA-1 and certificate, both
produced by `tools/py/xbe_info.py` after `tools/py/xiso.py` extracts
`default.xbe` from the disc image.

## Games, from the dumps on this machine (2026-10-24)

| Game | Title ID | Internal name | XDK build | XBE size | XBE SHA-1 | Cert version | Online |
|---|---|---|---|---|---|---|---|
| MK: Deadly Alliance (USA, Rev 1) | 4D57000C | mk5 (`C:\mk5\Code\mk5xbox_Release\mk5.exe`) | 1.0.4721 | 2,895,872 | 5aba0b06995b2179673a9ff05bcf5d0e16a9bc57 | 4 | none (no XNET/XONLINE) |
| MK: Deception (USA) | 4D570019 | mk6 (`c:\mk6\Code\mk6xbox_Release\mk6.exe`) | 1.0.5849 | 4,657,152 | ccbcfe58d253e96fdb016907ee80b8a5fe53ca26 | 5 | Xbox Live + System Link (XONLINES, XNET, XVOICE, VOICMAIL) |
| MK: Armageddon (USA) | 4D570034 | mk7 (`c:\games\mk7xbox\mk7\Code\mk7xbox_Release\mk7.exe`) | 1.0.5849 | 6,033,408 | 24074b8f58a0c10f2dee6862be3ebd267ac1e164 | 2 | Xbox Live + System Link |

All three: retail XBEs, North America region, base 0x00010000, publisher
code MW (Midway). Full headers, section tables and library lists are in
`research/<game>/xbe_info.json`.

Facts that shape the work:

- Deception and Armageddon were linked against the same XDK (5849), so one
  set of XDK symbol signatures covers both. Deadly Alliance needs 4721.
- Deception's `.text` is 2.97 MB and Armageddon's 4.15 MB of x86 code, plus
  the XDK libraries as separate sections (D3D, D3DX, XGRPH, DSOUND, XNET,
  XONLINE, XPP, DOLBY). The engine code and the SDK code are cleanly
  separated by section, which makes replacing the SDK at API level natural.
- Both later games carry pixel/vertex shader sections (`PSGSFD*` / `PSFD*`)
  and a `dspimage` section (MCPX DSP program for the Dolby encoder), so the
  renderer and the sound path both have hardware-specific pieces to replace.
- Deception and Armageddon have the game's own online code (Xbox Live
  matchmaking, voice, System Link). That is the same situation Ekspym had
  with Tekken 6's RPCN netcode: a native delay-based online mode already
  exists inside the game and can be a starting point, or be bypassed.
  Both titles are on Insignia's supported list (the fan-run Xbox Live 1.0
  replacement: MW-025 / 4D570019 and MW-052 / 4D570034), so that online code
  is exercised today on real hardware and its protocol is documented by the
  Insignia work.

## Disc images

Two kinds of image work: an XISO (the Xbox file system at offset 0, sizes
here 1.5 to 4.4 GB) and a whole-disc dump with the file system behind the
video partition; `tools/py/xiso.py` and the launcher's Build game page read
both. Images that start with kilobytes of one repeated byte and show no
XDVDFS descriptor in their first gigabytes are broken or padded conversions,
not dumps, and do not work.

## Which game first

Recommendation: **Deception** first, then Armageddon on the same XDK 5849
signatures, then Deadly Alliance.

- Deception is the smaller of the two XDK 5849 titles, has the active
  competitive history, and its Xbox Live code gives a built-in online mode
  to study.
- Armageddon reuses everything from Deception (same XDK, same engine
  lineage, same sound and shader setup).
- Deadly Alliance is the oldest engine and the only one on XDK 4721, so it
  needs its own signature set; it also has no online code of its own.

## Extraction

```
python tools/py/xiso.py "<image.xiso.iso>" --list
python tools/py/xiso.py "<image.xiso.iso>" --extract default.xbe --out orig/<name>
python tools/py/xbe_info.py orig/<name>/default.xbe --json research/<name>/xbe_info.json
```

`orig/` is git-ignored. Nothing from the discs is ever committed.
