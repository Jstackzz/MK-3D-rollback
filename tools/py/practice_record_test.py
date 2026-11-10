"""Drive a release build's practice fight and exercise the dummy record (recomp/common/src/mk3d/record.h): a short
recording to learn which way is towards player 1, a recording that walks in and attacks, its playback from the same
side, then from the other side after player 1 jumps over the dummy (the mirroring), then a pause in the middle of a
recording. Presses go through the debug monitor's pad command, the keys as window messages. Prints RESULT and the
game's "record:" log lines.

  python -I tools/py/practice_record_test.py mkda [--port 47811] [--p1 Scorpion --p2 SubZero] [--snap name] [--keep]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import math
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAMES = {
    # p2 fighter (addr, deref offset), state offset, healths, game counter, control object: (from fighter? offset,
    # else from record: (record addr, offset)), x and z offsets in it
    "mkda": dict(exe="MKDA.exe", prefix="MKDA", mode=(0x42A89C, 6), p2=(0x2EA544, None), p1=(0x2EA540, None),
                 state=0x194, h1=0x42C774, h2=0x42C770, counter=0x42A8A0,
                 ctrl=("fighter", 0x28, None), x=0x84, z=0x8C),
    "mkd": dict(exe="MKD.exe", prefix="MKD", mode=(0x61C414, 4), p2=(0x621030, 0x58), p1=(0x620FC4, 0x58),
                state=0x214, h1=0x620FD0, h2=0x62103C, counter=0x61C418,
                ctrl=("record", 0x5C, (0x620FC4, 0x621030)), x=0xA0, z=0xA8),
    "mka": dict(exe="MKA.exe", prefix="MKA", mode=(0x8292F8, 4), p2=(0x831B34, 0x30), p1=(0x831AEC, 0x30),
                state=0x388, h1=0x831AF8, h2=0x831B40, counter=0x829304,
                ctrl=("record", 0x34, (0x831AEC, 0x831B34)), x=0xC0, z=0xC8),
}

user32 = ctypes.windll.user32
WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_CLOSE = 0x100, 0x101, 0x104, 0x105, 0x10
VK = {"F7": 0x76, "F8": 0x77, "F9": 0x78, "F10": 0x79, "F11": 0x7A, "F12": 0x7B}


class Monitor:
    def __init__(self, port, tries=600):
        for i in range(tries):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=60)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise RuntimeError("no monitor")
        self.buf = b""

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.s.recv(1 << 16)
            if not chunk:
                raise ConnectionError("monitor closed")
            self.buf += chunk
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode()

    def r32(self, addr):
        r = self.cmd(f"r32 {addr:#x} 1")
        if not r.startswith("ok"):
            raise RuntimeError(r)
        return int(r.split()[1], 16)

    def rf(self, addr):
        r = self.cmd(f"rf {addr:#x} 1")
        if not r.startswith("ok"):
            raise RuntimeError(r)
        return float(r.split()[1])


def windows_of(pid):
    out = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            out.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return out


def key(hwnd, name):
    vk = VK[name]
    if name == "F10":
        user32.PostMessageW(hwnd, WM_SYSKEYDOWN, vk, 0x20000000)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_SYSKEYUP, vk, 0xE0000000)
    else:
        user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0000000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", choices=list(GAMES))
    ap.add_argument("--port", type=int, default=47811)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--p1", default="Scorpion")
    ap.add_argument("--p2", default="SubZero")
    ap.add_argument("--snap", default="")  # a name: screenshots build/shots/<name>-rec.png and -play.png
    a = ap.parse_args()
    g = GAMES[a.game]
    d = ROOT / "recomp" / a.game / "out" / "build" / "release"
    env = dict(os.environ)
    env.update({f"{g['prefix']}_AUTOPILOT": "practice", f"{g['prefix']}_P1": a.p1, f"{g['prefix']}_P2": a.p2,
                f"{g['prefix']}_MONITOR": str(a.port), "MK3D_DISPLAY": "windowed", "MK3D_RESOLUTION": "960x720",
                "MK3D_FRAMEDATA": "1", "SDL_ASSERT": "always_ignore"})
    # --autostart: no "start the game now?" page after a rebuild of the game code (a new executable)
    p = subprocess.Popen([str(d / g["exe"]), "--autostart"], cwd=str(d), env=env)
    print("started", g["exe"], "pid", p.pid, flush=True)
    t0 = time.time()
    m = Monitor(a.port)

    def fighter(which):
        addr, off = g[which]
        v = m.r32(addr)
        if off is not None:
            v = m.r32(addr + off) if v else 0
        return v

    def control(side):
        kind, off, recs = g["ctrl"]
        if kind == "fighter":
            f = fighter("p1" if side == 0 else "p2")
            return m.r32(f + off) if f else 0
        return m.r32(recs[side] + off)

    def positions():
        c0, c1 = control(0), control(1)
        if not c0 or not c1:
            return None
        return (m.rf(c0 + g["x"]), m.rf(c0 + g["z"])), (m.rf(c1 + g["x"]), m.rf(c1 + g["z"]))

    def distance():
        pos = positions()
        if not pos:
            return -1
        (x0, z0), (x1, z1) = pos
        return math.hypot(x0 - x1, z0 - z1)

    while True:
        if p.poll() is not None:
            print("the game exited early", p.returncode)
            return 1
        try:
            mode = m.r32(g["mode"][0])
            f2 = fighter("p2")
            h1 = m.rf(g["h1"])
        except Exception as e:  # noqa
            mode, f2, h1 = -1, 0, 0
        if mode == g["mode"][1] and f2 and abs(h1 - 1.0) < 1e-3:
            c1 = m.r32(g["counter"])
            time.sleep(0.5)
            if m.r32(g["counter"]) > c1:
                break
        if time.time() - t0 > 900:
            print("no practice fight (mode %d, fighter %08x)" % (mode, f2))
            p.terminate()
            return 1
        time.sleep(0.5)
    print("practice fight at %.1f s; waiting for the round to start" % (time.time() - t0), flush=True)
    time.sleep(7)
    hwnd = windows_of(p.pid)[0]

    def state2():
        f = fighter("p2")
        return m.r32(f + g["state"]) if f else -1

    def sample(seconds, label):
        states, minh1, dmin, dmax = set(), 1.0, 99.0, 0.0
        end = time.time() + seconds
        while time.time() < end:
            states.add(state2())
            minh1 = min(minh1, m.rf(g["h1"]))
            dd = distance()
            dmin, dmax = min(dmin, dd), max(dmax, dd)
            time.sleep(0.03)
        print("%s: dummy states %s, min health P1 %.3f, distance %.2f..%.2f" % (
            label, sorted(hex(s) for s in states), minh1, dmin, dmax), flush=True)
        return states, minh1

    def snap(name):
        if a.snap:
            subprocess.run(["powershell", "-File", str(ROOT / "tools" / "snap.ps1"), "-Name", f"{a.snap}-{name}",
                            "-Process", g["exe"][:-4]], capture_output=True)

    print("positions at the start:", positions(), "distance %.2f" % distance(), flush=True)
    # which way is towards player 1 for the dummy: a short recording that walks RIGHT
    key(hwnd, "F9")
    time.sleep(0.3)
    d0 = distance()
    m.cmd("pad 0 RIGHT 20")
    time.sleep(0.6)
    d1 = distance()
    key(hwnd, "F9")
    time.sleep(0.4)
    toward = "RIGHT" if d1 < d0 else "LEFT"
    away = "LEFT" if toward == "RIGHT" else "RIGHT"
    print("the dummy's RIGHT took the distance %.2f -> %.2f: towards player 1 is %s" % (d0, d1, toward), flush=True)
    # the real recording: walk in, three attacks, walk out
    key(hwnd, "F9")
    time.sleep(0.3)
    m.cmd(f"pad 0 {toward} 30")
    time.sleep(0.7)
    for i in range(3):
        m.cmd("pad 0 X 6")
        time.sleep(0.6)
    snap("rec")
    m.cmd(f"pad 0 {away} 25")
    time.sleep(0.7)
    key(hwnd, "F9")
    time.sleep(0.5)
    h_before = m.rf(g["h1"])
    key(hwnd, "F10")
    time.sleep(0.8)
    snap("play")
    st, h_play = sample(4.0, "playback on the same side")
    hit1 = h_play < h_before - 0.001
    print("hit on the same side:", hit1, flush=True)
    time.sleep(1.0)
    # the sides swapped: player 1 walks in and jumps over the dummy
    p1_toward = away  # they face each other
    before = positions()
    for i in range(12):
        dd = distance()
        if dd < 1.1:
            break
        m.cmd(f"pad 0 {p1_toward} 12")
        time.sleep(0.3)
    print("player 1 walked in to %.2f" % distance(), flush=True)
    swapped = False
    for attempt in range(4):
        m.cmd(f"pad 0 UP+{p1_toward} 30")
        time.sleep(1.6)
        after = positions()
        # the x order along the line between them, as seen from the start: a sign change means they crossed
        (bx0, bz0), (bx1, bz1) = before
        (ax0, az0), (ax1, az1) = after
        ex, ez = bx1 - bx0, bz1 - bz0
        proj = (ax1 - ax0) * ex + (az1 - az0) * ez
        print("after jump %d: positions %s, projection %.2f (negative: crossed)" % (attempt + 1, after, proj),
              flush=True)
        if proj < 0:
            swapped = True
            break
        for i in range(6):
            if distance() < 1.1:
                break
            m.cmd(f"pad 0 {p1_toward} 12")
            time.sleep(0.3)
    print("sides swapped:", swapped, flush=True)
    time.sleep(0.8)
    h_before = m.rf(g["h1"])
    key(hwnd, "F10")
    time.sleep(0.8)
    snap("mirrored")
    st2, h_play2 = sample(4.0, "playback from the other side")
    hit2 = h_play2 < h_before - 0.001
    print("hit from the other side (mirrored):", hit2, flush=True)
    time.sleep(0.5)
    # a pause in the middle of a recording: the recording stands still with the game
    key(hwnd, "F9")
    time.sleep(0.3)
    m.cmd(f"pad 0 {toward} 20")
    time.sleep(0.5)
    m.cmd("pad 0 START 4")
    time.sleep(1.5)
    snap("paused")
    c_a = m.r32(g["counter"])
    time.sleep(0.5)
    c_b = m.r32(g["counter"])
    print("paused: the game counter %d -> %d over 0.5 s" % (c_a, c_b), flush=True)
    m.cmd("pad 0 START 4")
    time.sleep(1.0)
    m.cmd("pad 0 X 6")
    time.sleep(0.6)
    key(hwnd, "F9")
    time.sleep(0.5)
    if a.keep:
        print("keeping the game open; press Enter to close")
        input()
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill()
    logs = sorted((d / "logs").glob("*.log"), key=lambda x: x.stat().st_mtime)
    log = logs[-1] if logs else None
    if log:
        print("=== log", log.name)
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if "record:" in line or "[error]" in line:
                print(line[:220])
    print("RESULT: same side hit %s, swapped %s, mirrored hit %s" % (hit1, swapped, hit2))
    return 0 if hit1 and (not swapped or hit2) else 2


if __name__ == "__main__":
    sys.exit(main())
