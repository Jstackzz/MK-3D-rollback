"""List XDK library functions a recompiled game calls directly that still run their original console code.

The runtime replaces the XDK at API level: Direct3D, DirectSound and friends are host code bound by name through the
game's config [symbols]. A library function the game calls that is neither bound nor replaced by the game's own
overrides runs as lifted console code against host-owned objects, and may crash. This lists those calls, by library
section, with the XbSymbolDatabase name when there is one, and flags the ones that touch the D3D device.

usage: python -I tools/py/lib_audit.py <game dir, e.g. recomp/mkd> <xbsdb symbols txt> [--sections D3D,DSOUND,...]
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "external" / "XboxXBERecomp" / "runtime"


def load_toml_table(path: Path, table: str) -> dict:
    out, cur = {}, None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            cur = line.strip("[]").strip()
            continue
        if cur == table and "=" in line:
            k, v = (x.strip() for x in line.split("=", 1))
            try:
                out[k] = int(v, 0)
            except ValueError:
                pass
    return out


def runtime_names() -> set:
    names = set()
    pats = [
        (r"^HLE_(?:STDCALL|FASTCALL|CDECL|WRAP)\((\w+)", "{}"),
        (r"^BUFFER\((\w+)", "IDirectSoundBuffer_{}"),
        (r"^STREAM\((\w+)", "CDirectSoundStream_{}"),
        (r"^COMPLEX_STATE\((\w+)", "D3DDevice_SetRenderState_{}"),
        (r"^CRT_MATH\((\w+)", "{}"),
        (r"X86_FUNC\(GUEST\((\w+)\)\)", "{}"),
    ]
    for f in list(RUNTIME.rglob("*.cpp")) + list(RUNTIME.rglob("*.h")):
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            for pat, fmt in pats:
                m = re.search(pat, line)
                if m:
                    names.add(fmt.format(m.group(1)))
    return names


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("xbsdb")
    ap.add_argument("--sections", default="D3D,D3DX,XGRPH,DSOUND,XPP,XNET,XONLINE")
    ap.add_argument("--info", help="xbe_info.json (default research/<short>/xbe_info.json)")
    args = ap.parse_args()
    game = Path(args.game)
    cfg = game / "config" / "game.toml"
    symbols = load_toml_table(cfg, "symbols")
    info_path = Path(args.info) if args.info else ROOT / "research" / game.name / "xbe_info.json"
    info = json.loads(info_path.read_text())
    lib = set(args.sections.split(","))
    ranges = []
    for s in info["sections"]:
        va = int(s["vaddr"], 16)
        ranges.append((s["name"], va, va + s["vsize"]))

    def section(a):
        for n, lo, hi in ranges:
            if lo <= a < hi:
                return n
        return "?"

    xb = {}
    for line in Path(args.xbsdb).read_text(encoding="utf-8").splitlines():
        if " = 0x" in line:
            n, a = line.split(" = 0x")
            a = int(a.strip(), 16)
            n = n.strip()
            short = n.split("__", 1)[1] if "__" in n else n
            xb.setdefault(a, short)

    bound = {a for n, a in symbols.items() if a and n in runtime_names()}
    # the game's own overrides
    for f in (game / "src").rglob("*.cpp"):
        for m in re.finditer(r"X86_FUNC\(sub_([0-9A-Fa-f]{8})\)", f.read_text(encoding="utf-8", errors="replace")):
            bound.add(int(m.group(1), 16))

    calls = {}  # callee -> callers
    device_refs = set()
    dev = load_toml_table(cfg, "data")
    dev_addrs = {f"{dev.get('D3D_pDevice', 0):X}", f"{dev.get('D3D_Device', 0):X}"}
    func_re = re.compile(r"^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)")
    call_re = re.compile(r"\bsub_([0-9A-F]{8})\(ctx, base\)")
    for f in sorted((game / "generated" / "game").glob("x86_recomp.*.cpp")):
        cur = None
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = func_re.match(line)
            if m:
                cur = int(m.group(1), 16)
                continue
            if cur is None:
                continue
            for c in call_re.findall(line):
                calls.setdefault(int(c, 16), set()).add(cur)
            if any(f"0x{d}u" in line or f"0x{d.lstrip('0')}u" in line for d in dev_addrs if d != "0"):
                device_refs.add(cur)

    rows = []
    for callee, callers in calls.items():
        sec = section(callee)
        if sec not in lib or callee in bound:
            continue
        outside = sorted(c for c in callers if section(c) not in lib)
        if not outside:
            continue
        rows.append((sec, callee, xb.get(callee, ""), callee in device_refs, outside))
    rows.sort()
    for sec, callee, name, devref, outside in rows:
        who = ", ".join(f"{c:08X}" for c in outside[:4]) + (" ..." if len(outside) > 4 else "")
        print(f"{sec:8} {callee:08X} {'DEV' if devref else '   '} {name or '-':52} <- {who}")
    print(f"# {len(rows)} library functions called from game code still run console code", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
