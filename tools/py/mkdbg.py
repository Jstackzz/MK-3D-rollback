"""Client for the game's debug monitor (MKD_MONITOR=<port>, recomp/mkd/src/game/monitor.cpp), plus memory dump
scanning in the style of a cheat finder.

  python -I tools/py/mkdbg.py cmd frame                  one command, print the reply
  python -I tools/py/mkdbg.py cmd "r32 0x61C3F8 4"
  python -I tools/py/mkdbg.py dump fight1                the game writes build/dumps/fight1.bin
  python -I tools/py/mkdbg.py scan new fight1 --type f32 --eq 1.0       candidates in one dump
  python -I tools/py/mkdbg.py scan next fight2 --type f32 --cmp lt       keep those that went down since the last dump
  python -I tools/py/mkdbg.py scan show [--limit 50]     the current candidates with their last values

Scan types: u8 u16 u32 i32 f32. Comparisons against the previous dump: lt gt le ge eq ne same (eq/ne with --eq value
compare to that value instead). Candidates live in build/dumps/scan.json. Addresses are guest virtual addresses.
"""
import argparse
import json
import socket
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DUMPS = ROOT / "build" / "dumps"
STATE = DUMPS / "scan.json"
TYPES = {"u8": ("<B", 1), "u16": ("<H", 2), "u32": ("<I", 4), "i32": ("<i", 4), "f32": ("<f", 4)}


class Monitor:
    def __init__(self, port: int, host: str = "127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=120)
        self.buf = b""

    def cmd(self, line: str) -> str:
        self.s.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.s.recv(1 << 16)
            if not chunk:
                raise ConnectionError("monitor closed the connection")
            self.buf += chunk
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode()

    def r32(self, addr: int, n: int = 1) -> list:
        r = self.cmd(f"r32 {addr:#x} {n}")
        if not r.startswith("ok"):
            raise RuntimeError(r)
        return [int(x, 16) for x in r.split()[1:]]

    def read(self, addr: int, n: int) -> bytes:
        r = self.cmd(f"r {addr:#x} {n}")
        if not r.startswith("ok"):
            raise RuntimeError(r)
        return bytes.fromhex(r.split()[1])


def load_dump(name: str) -> list:
    p = Path(name)
    if not p.exists():
        p = DUMPS / (name if name.endswith(".bin") else name + ".bin")
    d = p.read_bytes()
    if d[:7] != b"MKDUMP1":
        raise ValueError(f"{p} is not a monitor dump")
    out, o = [], 8
    while o + 8 <= len(d):
        va, size = struct.unpack_from("<II", d, o)
        out.append((va, d[o + 8:o + 8 + size]))
        o += 8 + size
    return out


def value_at(ranges: list, addr: int, fmt: str, size: int):
    for va, data in ranges:
        if va <= addr and addr + size <= va + len(data):
            return struct.unpack_from(fmt, data, addr - va)[0]
    return None


def scan_new(args):
    fmt, size = TYPES[args.type]
    cands = {}
    for va, data in load_dump(args.dump):
        if args.lo and va + len(data) <= args.lo:
            continue
        if args.hi and va >= args.hi:
            continue
        step = size if args.aligned else 1
        for off in range(0, len(data) - size + 1, step):
            addr = va + off
            if args.lo and addr < args.lo or args.hi and addr >= args.hi:
                continue
            v = struct.unpack_from(fmt, data, off)[0]
            if args.eq is not None and not close(v, args.eq, args.type):
                continue
            if args.min is not None and v < args.min or args.max is not None and v > args.max:
                continue
            cands[addr] = v
            if len(cands) > 5_000_000:
                sys.exit("too many candidates; narrow with --eq, --lo/--hi or --min/--max")
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps({"type": args.type, "cands": {f"{k:x}": v for k, v in cands.items()}}))
    print(f"{len(cands)} candidates")


def close(v, target, t):
    return abs(v - target) <= 1e-4 * max(1.0, abs(target)) if t == "f32" else v == target


def scan_next(args):
    st = json.loads(STATE.read_text())
    t = st["type"]
    fmt, size = TYPES[t]
    ranges = load_dump(args.dump)
    keep = {}
    for k, old in st["cands"].items():
        addr = int(k, 16)
        v = value_at(ranges, addr, fmt, size)
        if v is None or (t == "f32" and v != v):
            continue
        ok = {
            "lt": v < old, "gt": v > old, "same": close(v, old, t), "le": v <= old or close(v, old, t),
            "ge": v >= old or close(v, old, t), "eq": close(v, args.eq if args.eq is not None else old, t),
            "ne": not close(v, args.eq if args.eq is not None else old, t),
        }[args.cmp]
        if ok and (args.min is None or v >= args.min) and (args.max is None or v <= args.max):
            keep[k] = v
    st["cands"] = keep
    STATE.write_text(json.dumps(st))
    print(f"{len(keep)} candidates")
    show(st, args.limit)


def show(st, limit):
    items = sorted(st["cands"].items(), key=lambda kv: int(kv[0], 16))
    for k, v in items[:limit]:
        print(f"  {int(k, 16):08X}  {v}")
    if len(items) > limit:
        print(f"  ... {len(items) - limit} more")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47800)
    sub = ap.add_subparsers(dest="what", required=True)
    c = sub.add_parser("cmd")
    c.add_argument("line", nargs="+")
    d = sub.add_parser("dump")
    d.add_argument("name")
    s = sub.add_parser("scan")
    s.add_argument("step", choices=["new", "next", "show"])
    s.add_argument("dump", nargs="?")
    s.add_argument("--type", default="u32", choices=list(TYPES))
    s.add_argument("--eq", type=float)
    s.add_argument("--min", type=float)
    s.add_argument("--max", type=float)
    s.add_argument("--lo", type=lambda x: int(x, 0), default=0)
    s.add_argument("--hi", type=lambda x: int(x, 0), default=0)
    s.add_argument("--cmp", default="ne", choices=["lt", "gt", "le", "ge", "eq", "ne", "same"])
    s.add_argument("--aligned", action="store_true", default=True)
    s.add_argument("--limit", type=int, default=40)
    a = ap.parse_args()
    if a.what == "cmd":
        print(Monitor(a.port).cmd(" ".join(a.line)))
    elif a.what == "dump":
        DUMPS.mkdir(parents=True, exist_ok=True)
        path = DUMPS / f"{a.name}.bin"
        print(Monitor(a.port).cmd(f"dump {path}"))
    elif a.step == "new":
        if a.eq is not None and a.type != "f32":
            a.eq = int(a.eq)
        scan_new(a)
    elif a.step == "next":
        if a.eq is not None and a.type != "f32":
            a.eq = int(a.eq)
        scan_next(a)
    else:
        show(json.loads(STATE.read_text()), a.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
