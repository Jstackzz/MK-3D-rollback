"""Compare the state dumps two games wrote into their netplay logs (MKD_NET_LOG) when a check disagreed.

    python tools/py/rbdump.py build/netplay/<tag>-a.log build/netplay/<tag>-b.log

Each dump is `# dump <frame> block <i> at <va>` followed by `# <i> <offset> <4 dwords>` lines: block 0 and 1 are
the fighter objects, 2 the game state block 0x620000..0x622000.
"""
import re
import sys

HEAD = re.compile(r'# dump (\d+) block (\d+) at (\w+)')
ROW = re.compile(r'# (\d) (\w{4}) (\w{8}) (\w{8}) (\w{8}) (\w{8})')


def load(path):
    dumps = {}  # (frame, block) -> (va, {offset: value})
    key = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = HEAD.match(line)
        if m:
            key = (int(m.group(1)), int(m.group(2)))
            dumps[key] = (int(m.group(3), 16), {})
            continue
        m = ROW.match(line)
        if m and key:
            off = int(m.group(2), 16)
            for i in range(4):
                dumps[key][1][off + 4 * i] = int(m.group(3 + i), 16)
    return dumps


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    names = {0: 'P1 fighter', 1: 'P2 fighter', 2: 'game state'}
    for key in sorted(set(a) & set(b)):
        va_a, da = a[key]
        va_b, db = b[key]
        diffs = [o for o in sorted(da) if da[o] != db.get(o)]
        print(f'frame {key[0]} {names.get(key[1], key[1])} at {va_a:08X} / {va_b:08X}: {len(diffs)} dwords differ')
        for o in diffs[:60]:
            print(f'   +{o:04X} ({va_a + o:08X}): {da[o]:08X} vs {db[o]:08X}')
    if not set(a) & set(b):
        print('no dump in both logs')


if __name__ == '__main__':
    main()
