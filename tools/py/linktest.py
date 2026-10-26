"""Launcher stand-in for testing the game's side of the launcher protocol (docs/PROTOCOL.md section 3) with two real
game instances on this PC: ready -> start -> in_match -> result -> park -> ready -> start again -> quit.

usage: python -I tools/py/linktest.py [--exe recomp/mkd/out/build/debug/MKD.exe] [--p1 Kenshi] [--p2 Kobra]
Instance A is side 1 and attacks with a scripted player (MKD_INPUT), B is side 2 and stands still.
"""
import argparse
import itertools
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def attacks(start, count):
    seq = ["RIGHT:0.4", "X", "X", "Y", "A", "B", "X", "Y", "X+Y"]
    return ",".join(f"{start + i * 0.35:.2f}:{s}" for i, s in enumerate(itertools.islice(itertools.cycle(seq), count)))


class Instance:
    def __init__(self, name, exe, extra_env):
        self.dir = ROOT / "build" / "linktest" / name
        self.dir.mkdir(parents=True, exist_ok=True)
        for f in self.dir.glob("*"):
            f.unlink()
        self.cmd = self.dir / "cmd.txt"
        self.status = self.dir / "status.txt"
        env = dict(os.environ)
        env.update({"MK3D_NET": "1", "MK3D_GAME": "mkd", "MK3D_SESSION_DIR": str(self.dir), "MK3D_CMD": str(self.cmd),
                    "MK3D_STATUS": str(self.status), "MK3D_INSTANCE": name, "MKD_PADS": "2", "MKD_EVENT_LOG": "1",
                    "SDL_ASSERT": "always_ignore"})
        env.update(extra_env)
        self.proc = subprocess.Popen([str(exe)], cwd=str(Path(exe).parent), env=env)
        self.last = None
        self.name = name

    def read(self):
        try:
            line = self.status.read_text(encoding="ascii", errors="replace").splitlines()[0].strip()
        except (OSError, IndexError):
            return None
        if line != self.last:
            print(f"  [{time.strftime('%H:%M:%S')}] {self.name}: {line}", flush=True)
            self.last = line
        return line

    def send(self, line):
        tmp = self.cmd.with_suffix(".tmp")
        tmp.write_text(line + "\n", encoding="ascii")
        os.replace(tmp, self.cmd)
        print(f"  -> {self.name}: {line}", flush=True)


def wait(insts, pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        states = [i.read() for i in insts]
        if all(s is not None and pred(s) for s in states):
            return True
        if any(i.proc.poll() is not None for i in insts):
            print(f"FAIL: a game exited while waiting for {what}")
            return False
        time.sleep(0.1)
    print(f"FAIL: timed out waiting for {what}")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "recomp/mkd/out/build/debug/MKD.exe"))
    ap.add_argument("--p1", default="Kenshi")
    ap.add_argument("--p2", default="Kobra")
    a = ap.parse_args()
    exe = Path(a.exe)
    A = Instance("a", exe, {"MKD_INPUT": attacks(20, 1500)})
    time.sleep(0.3)
    B = Instance("b", exe, {})
    ok = False
    try:
        if not wait([A, B], lambda s: s == "ready", 90, "ready"):
            return 1
        common = f"game=mkd mode=kombat delay=2 seed=12345 p1={a.p1} p2={a.p2} rounds=3 ranked=0"
        A.send(f"start {common} side=1 local_port=7001 remote=127.0.0.1:7002 match=t1")
        B.send(f"start {common} side=2 local_port=7002 remote=127.0.0.1:7001 match=t1")
        if not wait([A, B], lambda s: s.startswith("in_match"), 90, "in_match"):
            return 1
        if not wait([A, B], lambda s: s.startswith(("result", "desync", "disconnected")), 300, "a result"):
            return 1
        ra, rb = A.read(), B.read()
        print(f"results: A '{ra}' / B '{rb}'", "AGREE" if ra == rb else "DIFFER")
        for i in (A, B):
            i.send("park")
        if not wait([A, B], lambda s: s == "ready", 60, "ready after park"):
            return 1
        A.send(f"start {common} side=1 local_port=7001 remote=127.0.0.1:7002 match=t2")
        B.send(f"start {common} side=2 local_port=7002 remote=127.0.0.1:7001 match=t2")
        if not wait([A, B], lambda s: s.startswith("in_match match=t2"), 90, "the second in_match"):
            return 1
        ok = ra == rb and ra.startswith("result")
        print("PASS" if ok else "FAIL: the results")
    finally:
        for i in (A, B):
            if i.proc.poll() is None:
                try:
                    i.send("quit")
                except OSError:
                    pass
        time.sleep(3)
        for i in (A, B):
            if i.proc.poll() is None:
                i.proc.kill()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
