#!/usr/bin/env python3
"""The arena-to-music-bank tables for launcher/data/jukebox.json from the mapping runs' logs (build/jukebox/map-<game>.txt,
the games' "jukebox: bank X opened, arena N" lines; build/jukebox/map-runs.ps1 makes them).

  python tools/py/jukebox_map.py mkd [--write]     # print the table; --write puts it into jukebox.json

A fight opens its music bank (music_*, mk5tune*, or a bg_* bank with a long stream in Armageddon) and its ambience
bank (bg_*): the stage's name comes from jukebox.json when the arena id is already there, else from the banks' names.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
JSON = os.path.join(ROOT, 'launcher', 'data', 'jukebox.json')

PRETTY = {
    'beetle_lair': 'Beetle Lair', 'slaughterhouse': 'Slaughterhouse', 'sky_temple': 'Sky Temple', 'nexus': 'Nexus Arena',
    'kollapsing_kliffs': 'Falling Cliffs', 'lost_warriors': 'Golden Desert', 'nether_belly': 'Nethership Interior',
    'qc_fortress': "Quan Chi's Fortress", 'courtyard': 'Courtyard', 'forest': 'Living Forest', 'portal': 'Portal',
    'pit': 'The Pit', 'deadpool': 'Dead Pool', 'yinyang': 'Yin Yang Island', 'red_dragon': 'Chamber of Artifacts',
    'hell_foundry': "Hell's Foundry", 'dark_prison': 'Dark Prison', 'dragon_mountain': 'Dragon Mountain',
    'dking_temple': "Dragon King's Temple", 'kuatan_palace': 'Kuatan Palace', 'lower_mines': 'Lower Mines',
    'liukang_tomb': "Liu Kang's Tomb", 'yin_yang': 'Yin Yang Island',
}


def parse(game):
    path = os.path.join(ROOT, 'build', 'jukebox', f'map-{game}.txt')
    runs = {}
    arena = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'=== \w+ arena (\d+)', line)
        if m:
            arena = int(m.group(1))
            runs.setdefault(arena, [])
            continue
        m = re.search(r'jukebox: bank (\S+) opened, arena (-?\d+)', line)
        if m and arena is not None and int(m.group(2)) == arena:
            runs[arena].append(m.group(1).lower())
    return runs


# keywords for the automatic picks, by the stage's name (the names come from the banks the runs saw)
TAGS = {
    "Nexus Arena": [
        "nexus",
        "portal",
        "hourglass",
        "arena"
    ],
    "Liu Kang's Tomb": [
        "tomb",
        "krypt",
        "graveyard",
        "liu kang",
        "treasure"
    ],
    "Golden Desert": [
        "desert",
        "sand",
        "ruins",
        "wasteland",
        "golden"
    ],
    "The Pit": [
        "pit",
        "bridge",
        "cliffs",
        "spikes"
    ],
    "Living Forest": [
        "living forest",
        "forest",
        "woods",
        "trees",
        "jungle"
    ],
    "Lower Mines": [
        "mines",
        "cave",
        "cavern",
        "lair",
        "underground"
    ],
    "Hell's Foundry": [
        "hell",
        "foundry",
        "fire",
        "lava",
        "netherrealm"
    ],
    "Dead Pool": [
        "dead pool",
        "acid",
        "pool",
        "bath",
        "sewer"
    ],
    "Nethership Interior": [
        "nethership",
        "ship",
        "kove",
        "sea",
        "netherrealm",
        "warship"
    ],
    "Dragon Mountain": [
        "mountain",
        "dragon",
        "cliffs",
        "sky",
        "wind"
    ],
    "Quan Chi's Fortress": [
        "quan chi",
        "fortress",
        "netherrealm",
        "evil"
    ],
    "Kuatan Palace": [
        "kuatan",
        "palace",
        "throne",
        "outworld",
        "kahn",
        "great hall"
    ],
    "Falling Cliffs": [
        "cliffs",
        "falling",
        "pit",
        "bridge",
        "kollapsing"
    ],
    "Dark Prison": [
        "prison",
        "dark",
        "dungeon",
        "tekunin"
    ],
    "Chamber of Artifacts": [
        "chamber",
        "artifacts",
        "treasure",
        "red dragon",
        "temple"
    ],
    "Dragon King's Temple": [
        "temple",
        "dragon king",
        "throne",
        "hall",
        "sky temple"
    ],
    "Courtyard": [
        "courtyard",
        "wu shi",
        "academy",
        "palace grounds"
    ],
    "Yin Yang Island": [
        "island",
        "yin yang",
        "sea",
        "shang tsung",
        "water"
    ],
    "Slaughterhouse": [
        "slaughterhouse",
        "flesh pits",
        "lab",
        "meat",
        "butcher"
    ],
    "Beetle Lair": [
        "lair",
        "beetle",
        "hive",
        "kytinn",
        "cave",
        "insect"
    ],
    "Portal": [
        "portal",
        "nexus",
        "hourglass",
        "lost portal"
    ],
    "Sky Temple": [
        "sky temple",
        "temple",
        "sky",
        "jinsei",
        "raiden",
        "lung hai"
    ],
    "Subway": [
        "subway",
        "train",
        "street",
        "city"
    ],
    "Tekunin Prison": [
        "prison",
        "tekunin",
        "dungeon",
        "dark",
        "base"
    ],
    "Armory": [
        "armory",
        "weapon",
        "arsenal"
    ],
    "Bell Tower": [
        "bell tower",
        "tower",
        "rooftop",
        "bell"
    ],
    "Goro's Lair": [
        "goro",
        "lair",
        "cave",
        "moloch",
        "pit"
    ],
    "Battle Arena": [
        "arena",
        "battle",
        "coliseum",
        "drum",
        "kahn",
        "elder gods"
    ],
    "Wastelands": [
        "wastelands",
        "wasteland",
        "desert",
        "ruins",
        "sand"
    ],
    "Reptile's Lair": [
        "reptile",
        "lair",
        "snake",
        "cave",
        "jungle"
    ],
    "Hell": [
        "hell",
        "netherrealm",
        "fire",
        "lava",
        "apokolips"
    ],
    "Lumber Mill": [
        "lumber",
        "mill",
        "forest",
        "woods",
        "saw"
    ],
    "Pyramid of Argus": [
        "pyramid",
        "summit",
        "argus",
        "sky",
        "portal"
    ],
    "Meteor Storm": [
        "meteor",
        "storm",
        "sky",
        "space",
        "station",
        "rain"
    ],
    "Fire Well": [
        "fire",
        "well",
        "lava",
        "fire well",
        "shrine"
    ],
    "Soul Chamber": [
        "soul chamber",
        "chamber",
        "soul",
        "flesh pits"
    ],
    "Evil Tower": [
        "tower",
        "evil",
        "evil tower",
        "portal",
        "kahn"
    ],
    "Outworld Spire": [
        "spire",
        "outworld",
        "tower",
        "throne",
        "inner spire"
    ],
    "Botan Jungle": [
        "jungle",
        "kuatan jungle",
        "forest",
        "living forest",
        "woods"
    ],
    "Arctika": [
        "arctika",
        "ice",
        "snow",
        "lin kuei",
        "temple",
        "mountain"
    ],
    "Lin Kuei Palace": [
        "lin kuei",
        "palace",
        "ice",
        "temple",
        "fortress"
    ],
    "Netherrealm Cliffs": [
        "netherrealm",
        "cliffs",
        "hell",
        "fire",
        "lava"
    ],
    "Shinnok's Spire": [
        "spire",
        "shinnok",
        "netherrealm",
        "tower",
        "throne"
    ],
    "Shinnok's Throne Room": [
        "throne",
        "shinnok",
        "netherrealm",
        "palace",
        "hall"
    ],
    "Reiko's War Room": [
        "war room",
        "reiko",
        "map",
        "base",
        "special forces"
    ],
    "Shao Kahn's Throne Room": [
        "throne",
        "shao kahn",
        "kahn",
        "palace",
        "great hall",
        "kuatan"
    ],
    "Shao Kahn's Balcony": [
        "balcony",
        "shao kahn",
        "kahn",
        "coliseum",
        "arena"
    ],
    "Scorpion's Lair": [
        "scorpion",
        "lair",
        "hell",
        "fire",
        "netherrealm",
        "pit"
    ],
    "Edenian Ruins": [
        "ruins",
        "edenia",
        "edenian",
        "temple",
        "garden"
    ],
    "Krimson Forest": [
        "forest",
        "krimson",
        "living forest",
        "woods",
        "dead woods"
    ],
    "Red Dragon Caverns": [
        "cavern",
        "cave",
        "red dragon",
        "lair",
        "mines"
    ],
    "Chamber of Daegon": [
        "chamber",
        "daegon",
        "red dragon",
        "temple",
        "cave"
    ]
}


def main():
    game = sys.argv[1]
    write = '--write' in sys.argv
    runs = parse(game)
    data = json.load(open(JSON, encoding='utf-8'))
    old = {s['name']: s for s in data['games'][game]['stages']}
    rows = []
    for arena in sorted(runs):
        banks = runs[arena]
        music = [b for b in banks if b.startswith('music_') and b != 'music_fatality' or b.startswith('mk5tune')]
        bg = [b for b in banks if b.startswith('bg_')]
        if not music and game == 'mka':
            music = bg[:1]  # Armageddon keeps some arenas' music in their bg bank
        key = (bg[0][3:] if bg else (music[0].replace('music_tune_', '') if music else ''))
        name = PRETTY.get(key, key.replace('_', ' ').title())
        rows.append((arena, name, music[0] if music else '', bg[0] if bg else ''))
        print(f"{arena:3d}  {name:24s} music {music[0] if music else '-':22s} bg {bg[0] if bg else '-'}")
    if write:
        stages = []
        for arena, name, music, bg in rows:
            tags = TAGS.get(name) or (old[name]['tags'] if name in old else [w.lower() for w in re.findall(r"[A-Za-z']+", name)])
            stages.append({'id': arena, 'name': name, 'bank': music, 'tags': tags})
        data['games'][game]['stages'] = stages
        json.dump(data, open(JSON, 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
        print('written', JSON)


if __name__ == '__main__':
    main()
