#!/usr/bin/env python3
"""The jukebox's soundtrack.json for the ten soundtracks downloaded on 2026-11-11 (tools/py/msxbank.py's companion):
each folder under the music library gets a file naming the soundtrack, its select-screen track and each track's kind
(stage, select, menu, stinger, theme, variant) and keywords, which the launcher's automatic song-per-stage picks use.
Also writes a copy of each into launcher/data/soundtracks (compiled into the launcher: a folder without its own file
but with one of these names gets the built-in one).

  python tools/py/make_soundtracks.py ["C:\\Users\\Owner\\Music\\MK3D Jukebox"]
"""
import json
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('USERPROFILE', ''), 'Music', 'MK3D Jukebox')
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'launcher', 'data', 'soundtracks')

STOP = {'the', 'of', 'a', 'and', 'ver', 'medley', 'ost', 'soundtrack', 'mortal', 'kombat', 'mk', 'arcade', 'original',
        'music', 'stage', 'round', 'feat', 'ii', 'iii', 'extended', 'universe', 'vs', 'dc'}


def tags_of(title):
    t = re.sub(r"[^a-z0-9' /]+", ' ', title.lower())
    return sorted({w for w in re.split(r'[ /]+', t) if w and w not in STOP and not w.isdigit()})


# per soundtrack: the title, the select-screen track's index (None: the playlist has none), the track kinds by index
META = {
    'mk1': dict(title='Mortal Kombat (1992 arcade)', select=1, stage=[3, 4, 5, 6, 7, 9], stinger=[2, 10], theme=[8]),
    'mk2': dict(title='Mortal Kombat II (arcade)', select=3, stage=[4, 5, 6, 7, 8, 9, 10, 17], menu=[1, 2, 11, 16],
                stinger=[12, 13, 14, 15]),
    'mk3': dict(title='Mortal Kombat 3 (arcade, extended)', select=3, stage=[5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
                menu=[1, 2, 4, 15]),
    'umk3': dict(title='Ultimate Mortal Kombat 3 (arcade)', select=11, stage=[2, 3, 5, 9, 10, 12, 13, 14, 15, 16],
                 menu=[4, 8, 17], stinger=[1, 7], theme=[6]),
    'mk4': dict(title='Mortal Kombat 4 (arcade)', select=None, stage=[1, 2, 3, 4, 5, 6, 7, 8], theme=[9]),
    'mkvsdc': dict(title='Mortal Kombat vs. DC Universe', select=1, stage=list(range(2, 16)), theme=[1]),
    'mk9': dict(title='Mortal Kombat (2011)', select=3, stage=list(range(4, 28)), menu=[1], theme=[2, 28, 29]),
    'mkx': dict(title='Mortal Kombat X', select=71, stage=list(range(1, 13)),
                variant=[14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 29, 30, 31, 42, 43, 44, 47, 48, 49, 52, 53,
                         54, 56, 57, 58, 59, 60, 61, 63, 64, 65, 67, 68, 69],
                menu=[13], stinger=[24, 28, 36, 37, 38, 39, 40, 41, 50, 55],
                theme=[20, 32, 33, 34, 35, 45, 46, 51, 62, 66, 70, 71]),
    'mk11': dict(title='Mortal Kombat 11', select=1, stage=list(range(29, 37)),
                 theme=list(range(1, 29)) + [37, 38, 39, 40]),
    'mk1-2023': dict(title='Mortal Kombat 1 (2023)', select=1, stage=[5, 7, 11, 13, 15, 18, 21, 24, 27, 29],
                     theme=[1, 2, 3, 4, 6, 8, 9, 10, 12, 14, 16, 17, 19, 20, 22, 23, 25, 26, 28, 30, 31]),
}

# keywords a title alone does not give: what the stage looks like, and the MK3D stages it suits
EXTRA = {
    ('mk1', 3): ['courtyard', 'wu shi', 'academy', 'palace grounds'], ('mk1', 4): ['temple', 'gate', 'palace', 'grounds'],
    ('mk1', 5): ['bridge', 'pit', 'cliffs'], ('mk1', 6): ['throne', 'palace', 'shang tsung', 'kahn'],
    ('mk1', 7): ['goro', 'lair', 'cave', 'moloch'], ('mk1', 9): ['cavern', 'shrine', 'warrior', 'cave'],
    ('mk2', 4): ['dead pool', 'acid', 'bath', 'pool'], ('mk2', 5): ['tomb', 'krypt'], ('mk2', 6): ['tower', 'evil tower', 'portal'],
    ('mk2', 7): ['living forest', 'forest'], ('mk2', 8): ['armory', 'weapon'], ('mk2', 9): ['portal', 'kahn', 'arena', 'drum'],
    ('mk2', 10): ['goro', 'lair', 'cave', 'moloch'], ('mk2', 17): ['wasteland', 'pit', 'desert', 'ruins'],
    ('mk3', 5): ['rooftop', 'roof', 'city'], ('mk3', 6): ['bank', 'city'], ('mk3', 7): ['street', 'city'], ('mk3', 8): ['subway'],
    ('mk3', 9): ['church', 'house'], ('mk3', 10): ['bridge', 'pit'], ('mk3', 11): ['pit', 'cliffs'], ('mk3', 12): ['graveyard', 'tomb'],
    ('mk3', 13): ['soul chamber', 'soul', 'chamber'], ('mk3', 14): ['dorfen', 'noob', 'portal', 'nexus'],
    ('umk3', 2): ['bank', 'desert', 'cave', 'jade', 'ruins'], ('umk3', 3): ['bridge', 'pit'], ('umk3', 5): ['church', 'house'],
    ('umk3', 9): ['graveyard', 'tomb'], ('umk3', 10): ['lost portal', 'portal', 'nexus'],
    ('umk3', 12): ['pit', 'scorpion', 'lair', 'balcony', 'hell'], ('umk3', 13): ['rooftop', 'bell tower', 'tower'],
    ('umk3', 14): ['soul chamber', 'chamber'], ('umk3', 15): ['street', 'city'], ('umk3', 16): ['subway', 'waterfront', 'ship'],
    ('mk4', 1): ['tomb', 'krypt'], ('mk4', 2): ['reptile', 'lair', 'snake', 'cave'], ('mk4', 3): ['elder gods', 'arena', 'drum'],
    ('mk4', 4): ['goro', 'lair', 'moloch'], ('mk4', 5): ['well', 'fire well', 'pit'], ('mk4', 6): ['prison'],
    ('mk4', 7): ['woods', 'living forest', 'forest', 'ice', 'pit'], ('mk4', 8): ['wind', 'rain', 'sky', 'dragonfly'],
    ('mkvsdc', 2): ['graveyard', 'tomb'], ('mkvsdc', 3): ['wu shi', 'academy', 'courtyard'], ('mkvsdc', 4): ['special forces', 'base'],
    ('mkvsdc', 5): ['raiden', 'temple', 'sky temple'], ('mkvsdc', 6): ['netherrealm', 'hell', 'nethership', 'fortress'],
    ('mkvsdc', 7): ['metropolis', 'city'], ('mkvsdc', 8): ['gotham', 'city'], ('mkvsdc', 9): ['batcave', 'cave', 'lair'],
    ('mkvsdc', 10): ['space', 'station', 'ship'], ('mkvsdc', 11): ['themyscira', 'outworld', 'ruins'],
    ('mkvsdc', 12): ['fortress', 'ice', 'lin kuei'], ('mkvsdc', 13): ['senate', 'elder gods', 'arena'],
    ('mkvsdc', 14): ['apokolips', 'hell', 'lava', 'fire'], ('mkvsdc', 15): ['throne', 'palace', 'kahn'],
    ('mk9', 4): ['courtyard', 'wu shi', 'academy'], ('mk9', 5): ['courtyard', 'night'], ('mk9', 6): ['garden', 'shang tsung', 'palace grounds'],
    ('mk9', 7): ['pit', 'bridge', 'cliffs'], ('mk9', 8): ['goro', 'lair', 'moloch', 'cave'], ('mk9', 9): ['shang tsung', 'throne', 'palace'],
    ('mk9', 10): ['dead pool', 'acid', 'bath', 'pool'], ('mk9', 11): ['wastelands', 'wasteland', 'desert', 'ruins'],
    ('mk9', 12): ['monastery', 'temple', 'house'], ('mk9', 13): ['living forest', 'forest'], ('mk9', 14): ['armory', 'weapon'],
    ('mk9', 15): ['coliseum', 'kahn', 'arena', 'drum'], ('mk9', 16): ['subway'], ('mk9', 17): ['street', 'city'],
    ('mk9', 18): ['rooftop', 'roof', 'city'], ('mk9', 19): ['soul chamber', 'chamber', 'soul'], ('mk9', 20): ['temple', 'sky temple', 'lung hai'],
    ('mk9', 21): ['bell tower', 'tower'], ('mk9', 22): ['graveyard', 'tomb'], ('mk9', 23): ['desert', 'ruins', 'sarna'],
    ('mk9', 24): ['hell', 'netherrealm', 'lava', 'fire'], ('mk9', 25): ['flesh pits', 'pit', 'lab'],
    ('mk9', 26): ['shao kahn', 'throne', 'kuatan', 'palace'], ('mk9', 27): ['rooftop', 'dusk', 'city'],
    ('mkx', 1): ['jinsei', 'chamber', 'temple', 'sky'], ('mkx', 2): ['sky temple', 'temple', 'sky', 'dragonfly'],
    ('mkx', 3): ['refugee', 'kamp', 'camp', 'ruins'], ('mkx', 4): ['quan chi', 'fortress', 'netherrealm'],
    ('mkx', 5): ['krossroads', 'crossroads', 'outworld', 'market'], ('mkx', 6): ['kove', 'cove', 'ship', 'sea', 'nethership'],
    ('mkx', 7): ['kuatan', 'jungle', 'forest'], ('mkx', 8): ['emperor', 'courtyard', 'palace', 'kahn'],
    ('mkx', 9): ['lin kuei', 'temple', 'ice'], ('mkx', 10): ['outworld', 'marketplace', 'market', 'street'],
    ('mkx', 11): ['dead woods', 'woods', 'forest', 'graveyard'], ('mkx', 12): ['destroyed city', 'city', 'ruins'],
    ('mk11', 29): ['kytinn', 'hive', 'cave', 'lair'], ('mk11', 30): ['koliseum', 'coliseum', 'kahn', 'arena', 'drum'],
    ('mk11', 31): ['black dragon', 'fight club', 'club', 'city'], ('mk11', 32): ['special forces', 'desert', 'base', 'ruins'],
    ('mk11', 33): ['shang tsung', 'island', 'ruins', 'palace'], ('mk11', 34): ['kharon', 'ship', 'sea', 'nethership'],
    ('mk11', 35): ['sea of blood', 'sea', 'blood', 'ship', 'acid'], ('mk11', 36): ['hourglass', 'portal', 'nexus'],
    ('mk1-2023', 5): ['cage', 'mansion', 'house', 'palace'], ('mk1-2023', 7): ['wu shi', 'academy', 'courtyard'],
    ('mk1-2023', 11): ['great hall', 'hall', 'throne', 'palace', 'kahn'], ('mk1-2023', 13): ['flesh pits', 'pit', 'lab'],
    ('mk1-2023', 15): ['living forest', 'forest'], ('mk1-2023', 18): ['sun do', 'city', 'street', 'market'],
    ('mk1-2023', 21): ['treasure', 'chamber', 'tomb', 'krypt'], ('mk1-2023', 24): ['fire temple', 'temple', 'fire', 'lava', 'shrine'],
    ('mk1-2023', 27): ['pyramid', 'ruins', 'desert'], ('mk1-2023', 29): ['pyramid', 'summit', 'portal', 'sky'],
}

PREFIXES = [
    r'^\d+\.\s*', r'(?i)^(ultimate )?mortal kombat[^-|:]*[-|:]\s*', r'(?i)^mk 1 soundtrack \| ',
    r'(?i)^original music soundtrack - ', r'(?i)^ost music soundtrack - ', r'(?i)^arcade ost - ',
    r'(?i)^arcade music - *', r'(?i)^\(2011\) - ', r'(?i)^9 - soundtrack: ', r'(?i)^9 \(', r'(?i)^9 ',
    r'(?i)^soundtrack: ',
]
SUFFIXES = [
    r'(?i)\s*[-|]\s*(mortal kombat x ost|mk 11 ost|mortal kombat 11 ost|mk iii \(arcade\) ost extended|mortal kombat iii \(arcade\) ost extended)\s*$',
    r'(?i)\s*-\s*(wilbert roget, ii|casey edwards|nathan grigg|joel corelitz|dan negovan|dean grinsfelder|dan forden|stephanie economou|super square).*$',
    r'(?i)\s*\|\s*watertower\s*$', r'(?i)\s*- stage$',
]


def clean_title(title):
    t = title.replace('｜', '|')  # a full-width bar in some YouTube titles
    for p in PREFIXES:
        t = re.sub(p, '', t)
    for s in SUFFIXES:
        t = re.sub(s, '', t)
    t = t.replace(' - Stage ', ' ').replace('Screen Music)', 'screen)')
    return t.strip(' -|')


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    os.makedirs(OUT, exist_ok=True)
    for sid, m in META.items():
        folder = os.path.join(ROOT, sid)
        listing = os.path.join(folder, 'downloaded.txt')
        if not os.path.exists(listing):
            print(sid, 'has no downloaded.txt')
            continue
        tracks = {}
        for line in open(listing, encoding='utf-8'):
            if not line.strip():
                continue
            r = line.rstrip('\n').split('|')  # index|id|title|seconds|path, and a title may hold '|' itself
            idx, title, secs, path = int(r[0]), '|'.join(r[2:-2]), r[-2], r[-1]
            name = os.path.basename(path)
            if not os.path.exists(os.path.join(folder, name)):
                continue
            kind = 'other'
            for k in ('stage', 'menu', 'stinger', 'theme', 'variant'):
                if idx in m.get(k, []):
                    kind = k
            if m['select'] == idx:
                kind = 'select'
            t = clean_title(title)
            tags = tags_of(t) + EXTRA.get((sid, idx), [])
            tracks[idx] = dict(file=name, title=t, seconds=float(secs) if secs not in ('NA', '') else 0.0, kind=kind,
                               tags=sorted(set(tags)))
        select = tracks[m['select']]['file'] if m['select'] and m['select'] in tracks else ''
        out = dict(id=sid, title=m['title'], select=select, tracks=[tracks[i] for i in sorted(tracks)])
        text = json.dumps(out, indent=1, ensure_ascii=False)
        open(os.path.join(folder, 'soundtrack.json'), 'w', encoding='utf-8').write(text)
        open(os.path.join(OUT, sid + '.json'), 'w', encoding='utf-8').write(text)
        print(f"{sid}: {len(out['tracks'])} tracks, select: {select[:60] or '(none)'}")
        for tr in out['tracks'][:3]:
            print('   ', tr['kind'], '|', tr['title'], '|', ', '.join(tr['tags'][:6]))


if __name__ == '__main__':
    main()
