# Toolchain setup (Windows 11)

State of this machine on 2026-10-24 and what each piece is for.

## Present

| Tool | Where | Used for |
|---|---|---|
| Git | `C:\Program Files\Git` | everything |
| Visual Studio Build Tools 2026 (MSVC 14.50, Windows SDK 10.0.26100) | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` | GekkoNet, GGPO, launcher, host tools |
| CMake 4.2 and Ninja (bundled with the Build Tools) | `...\Common7\IDE\CommonExtensions\Microsoft\CMake` | all C++ builds; `tools/vsenv.cmd` puts them on PATH |
| LLVM 23.1.4 (Clang, LLD), portable | `external/llvm/clang+llvm-23.1.4-x86_64-pc-windows-msvc/bin` | XboxXBERecomp compiles its lifted C++ with `clang++` and links with `lld`; the winget installer needs an elevation prompt this session could not answer, so the release tarball is unpacked in place instead |
| Python 3.13 | `%LOCALAPPDATA%\Programs\Python\Python313` | `tools/py/*`, the toolkits' scripts (`capstone` for xboxrecomp) |
| Ghidra 12.1.4 | `external/ghidra/ghidra_12.1.4_PUBLIC/` | engine map; x86 support is built in |
| Java 26 | | Ghidra (needs 21+) |
| winget 1.29 | | installs |

Not present: xemu, Cxbx-Reloaded binaries (sources are cloned for study).

## Install commands

```
powershell -ExecutionPolicy Bypass -File tools/setup-windows.ps1 -Llvm     # 860 MB tarball from llvm/llvm-project releases, unpacked under external/llvm
python -m pip install capstone
```

## Dumps

Disc images are read in place from wherever you keep your own dumps;
executables are extracted into `orig/<game>/` (git-ignored):

```
python tools/py/xiso.py "<image.xiso.iso>" --list
python tools/py/xiso.py "<image.xiso.iso>" --extract default.xbe --out orig/<game>
python tools/py/xiso.py "<image.xiso.iso>" --extract-all --out orig/<game>     # full disc contents, for the port's data folder
python tools/py/xbe_info.py orig/<game>/default.xbe --json research/<game>/xbe_info.json
```

## Rollback libraries

`powershell -File tools/build-deps.ps1 -Targets GekkoNet,ggpo` builds both as
static libraries into `build/<name>/` with MSVC + Ninja (GGPO needs
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, already in the script). Both built
clean on 2026-10-24: `build/GekkoNet/GekkoLib/GekkoNet_STATIC.lib`,
`build/ggpo/lib/x64/Release/GGPO.lib`.

If a policy blocks the script, the same by hand:

```
tools\vsenv.cmd cmake -S external/GekkoNet -B build/GekkoNet -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
tools\vsenv.cmd cmake --build build/GekkoNet --parallel
```

## Ghidra helpers

`tools/ghidra/ApplyNames.java` applies `NAME = 0xADDR` lists (symbol database
output, the recompiler config) to a program; `tools/ghidra/DecompileFuncs.java`
decompiles the functions containing given addresses to a text file. Both run
headless: `analyzeHeadless <project dir> <name> -process mkd.exe -noanalysis
-postScript DecompileFuncs.java out.txt 0x12D950 ... -scriptPath tools\ghidra`.

## Recompiler toolkits

- `external/XboxXBERecomp` (primary): see `docs/research/xboxxberecomp.md`
  for the pipeline. Needs Clang, LLD, Ninja, CMake; fetches Zydis, toml++,
  Dear ImGui and SDL3 itself.
- `external/xboxrecomp` (secondary): Python 3.10+ with `capstone`, VS 2022+
  or Clang, CMake 3.20+.
- `external/XbSymbolDatabase`: OOVPA signature catalogue used to name XDK
  functions in the XBEs.

## Game data

Never in the repo. `orig/` is git-ignored and `.xbe`, `.xiso`, `.iso` are
ignored everywhere.
