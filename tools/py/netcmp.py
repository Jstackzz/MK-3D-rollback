"""Compare the per-frame netplay logs of two game instances (MKD_NET_LOG), from tools/netplay-local.ps1.

Each line: frame, my buttons, my sticks, their buttons, their sticks, "health" p1 p2, "rand" calls.
Side A's "mine" must equal side B's "theirs" (and the other way round) on every frame, and the game state columns
(health, rand calls) must agree; the first frame where they do not is the desync.

usage: python -I tools/py/netcmp.py build/netplay/np1-a.log build/netplay/np1-b.log
"""
import sys


def load(path):
    rows = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) < 9 or not p[0].isdigit():
            continue
        f = int(p[0])
        rows[f] = {"mine": (p[1], p[2]), "theirs": (p[3], p[4]), "health": (p[6], p[7]), "rand": p[9] if len(p) > 9 else None}
    return rows


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    common = sorted(set(a) & set(b))
    print(f"A {len(a)} frames, B {len(b)} frames, {len(common)} in common")
    input_errors = state_errors = 0
    first_state = None
    moves = 0
    for f in common:
        ra, rb = a[f], b[f]
        if ra["mine"] != rb["theirs"] or rb["mine"] != ra["theirs"]:
            input_errors += 1
            if input_errors <= 5:
                print(f"frame {f}: inputs differ A mine {ra['mine']} B theirs {rb['theirs']} / B mine {rb['mine']} A theirs {ra['theirs']}")
        if ra["health"] != rb["health"] or ra["rand"] != rb["rand"]:
            state_errors += 1
            if first_state is None:
                first_state = f
                print(f"frame {f}: STATE differs A health {ra['health']} rand {ra['rand']} / B health {rb['health']} rand {rb['rand']}")
        if ra["mine"][0] != "00000000" or ra["theirs"][0] != "00000000":
            moves += 1
    last = common[-1] if common else None
    if last is not None:
        print(f"last common frame {last}: A health {a[last]['health']} rand {a[last]['rand']}, B health {b[last]['health']} rand {b[last]['rand']}")
    print(f"frames with buttons held: {moves}; input mismatches: {input_errors}; state mismatches: {state_errors}")
    print("IN SYNC" if common and not input_errors and not state_errors else "NOT IN SYNC")
    return 0 if common and not input_errors and not state_errors else 1


if __name__ == "__main__":
    sys.exit(main())
