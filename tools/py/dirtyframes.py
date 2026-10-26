"""Measure how much guest memory one fight frame changes: pause the game through the debug monitor, dump, step one
frame, dump again (several times), and report the changed 4 KB pages by region.

usage: python -I tools/py/dirtyframes.py [--port 47800] [--steps 3]
Run it while a fight is on screen (MKD_MONITOR set).
"""
import argparse
import importlib.util
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("mkdbg", HERE / "mkdbg.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def pages(dump):
    out = {}
    for va, data in dump:
        for off in range(0, len(data), 0x1000):
            out[va + off] = data[off:off + 0x1000]
    return out


def region(va):
    if va < 0x80000000:
        return "user" if va < 0xD0000000 else "?"
    if va < 0x84000000:
        return "physical"
    if 0xB0000000 <= va < 0xC0000000:
        return "system"
    if 0xD0000000 <= va < 0xE0000000:
        return "stacks"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47800)
    ap.add_argument("--steps", type=int, default=3)
    a = ap.parse_args()
    mon = m.Monitor(a.port)
    print(mon.cmd("pause"))
    lo_hi = [("user", 0x10000, 0x7FFE0000), ("physical", 0x80000000, 0x84000000), ("system", 0xB0000000, 0xC0000000),
             ("stacks", 0xD0000000, 0xE0000000)]
    def dump(name):
        paths = []
        for r, lo, hi in lo_hi:
            p = m.DUMPS / f"{name}-{r}.bin"
            mon.cmd(f"dump {p} {lo:#x} {hi:#x}")
            paths.append(p)
        allp = {}
        for p in paths:
            allp.update(pages(m.load_dump(str(p))))
        return allp
    m.DUMPS.mkdir(parents=True, exist_ok=True)
    prev = dump("dirty0")
    total = sum(len(v) for v in prev.values())
    by_region = Counter(region(va) for va in prev)
    print(f"committed: {total / 2**20:.1f} MB in {len(prev)} pages: " + ", ".join(f"{k} {v * 4 / 1024:.1f} MB" for k, v in by_region.items()))
    for i in range(1, a.steps + 1):
        print(mon.cmd("step 1"))
        cur = dump(f"dirty{i}")
        changed = [va for va, d in cur.items() if prev.get(va) != d]
        c = Counter(region(va) for va in changed)
        print(f"frame +{i}: {len(changed)} pages changed ({len(changed) * 4} KB): " + ", ".join(f"{k} {v}" for k, v in c.most_common()))
        # the busiest page runs, to see what they are
        runs = []
        for va in sorted(changed):
            if runs and va == runs[-1][1]:
                runs[-1][1] = va + 0x1000
            else:
                runs.append([va, va + 0x1000])
        runs.sort(key=lambda r: r[1] - r[0], reverse=True)
        print("   largest runs: " + ", ".join(f"{lo:08X}+{(hi - lo) // 1024}K" for lo, hi in runs[:10]))
        prev = cur
    print(mon.cmd("resume"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
