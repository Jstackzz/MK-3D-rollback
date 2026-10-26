"""Compare two rollback netplay logs (MKD_NET_LOG): the last run of every frame must agree on both games.

    python tools/py/rbcmp.py build/netplay/<tag>-a.log build/netplay/<tag>-b.log

With MKD_NET_DATA=1 the logs also hash the image's data section per frame, in 64 KB blocks from 0x3E3000: the first
frames whose data differ are listed with the blocks that differ.
"""
import re
import sys

DATA = re.compile(r'(\d+) data (\w+)(?: from (\w+))? blocks (.*)')
# frame, inputs, how it ran, then the state: "<frame> [again] <mine> <sticks> <theirs> <sticks> [predicted] [loading]
# confirmed <n> rand <n> <the game's fields>" (older logs: the game's fields right after the flags)
LINE = re.compile(r'(\d+)( again)? (\w+) (\w+) (\w+) (\w+)( predicted)?( loading)?(?: confirmed \d+)? (.*)')


def load(path):
    final, stats, data = {}, [], {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('#'):
            stats.append(line.strip())
            continue
        m = DATA.match(line)
        if m:
            data[int(m.group(1))] = (m.group(2), m.group(4).split(), int(m.group(3) or '3E3000', 16))
            continue
        m = LINE.match(line)
        if m:
            final[int(m.group(1))] = m.group(9).strip()  # the state after the inputs
    return final, stats, data


def main():
    a, sa, da = load(sys.argv[1])
    b, sb, db = load(sys.argv[2])
    common = sorted(set(a) & set(b))
    if not common:
        print('no frames in both logs')
        return 1
    diff = [f for f in common if a[f] != b[f]]
    print(f'{len(common)} frames in both logs (0..{common[-1]}), the last runs differ in {len(diff)}')
    for f in diff[:10]:
        print(f'  frame {f}: {a[f]} vs {b[f]}')
    for s in (sa[-1:] + sb[-1:]):
        print(' ', s)
    both = sorted(set(da) & set(db))
    ddiff = [f for f in both if da[f][0] != db[f][0]]
    if both:
        print(f'data hashed in {len(both)} frames, differs in {len(ddiff)}')
        for f in ddiff[:8]:
            blocks = [i for i, (x, y) in enumerate(zip(da[f][1], db[f][1])) if x != y]
            print(f'  frame {f}: blocks ' + ' '.join(f'{da[f][2] + i * 0x10000:06X}' for i in blocks))
    return 1 if diff or ddiff else 0


if __name__ == '__main__':
    sys.exit(main())
