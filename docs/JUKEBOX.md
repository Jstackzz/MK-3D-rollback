# The jukebox and the offline versus

Added 2026-11-11. The offline versus is in MK3D Online from 1.0.3 (Single Player's VERSUS button). The jukebox page is
a test-build feature for now: the launcher only has it when built with `-DMK3D_JUKEBOX=ON` (the Deadly Alliance test
launcher, test 15 on); the public launcher is built without it, while the games carry the hook either way (harmless
without a map file).

## What it does

The jukebox plays the music of another Mortal Kombat on a game's stages, as Tekken 8's does: the player picks a
soundtrack (the arcade games, MK4, MK vs DC, MK 2011, MKX, MK11, MK1 2023, or any folder of songs) and every stage
gets a song of it, the best fit by name (Moloch's Lair gets a "Goro's Lair", Acid Bath a "Dead Pool"); the select
screen plays that soundtrack's own select-screen music. Any stage can get any song of any soundtrack instead, and
each of the three games is set up on its own. Nothing is downloaded or shipped: the music comes from the player's own
folder, `Music\MK3D Jukebox` unless changed on the page, one subfolder per soundtrack.

Online nothing changes: the music is swapped at the file level (below), so the simulation the two games run is the
same whatever each player listens to, and a lobby's watchers hear their own choice.

## How it works

Every game streams its music from `sndsxbox\<bank>.msx`, a table of RIFF WAVE streams in Xbox ADPCM, which is plain
IMA ADPCM in 36-byte-per-channel blocks of 64 samples (`tools/py/msxbank.py` lists, extracts and replaces them; the
format is described there). A fight's arena names its music bank in the game's arena table (Deadly Alliance: the
table at 0x231750, +0x14 the bank's index into the bank name list at 0x27F5F8), the front end's bank holds the menu and
select-screen music (Deadly Alliance `shell`, stream 27). The launcher re-encodes a chosen song into a copy of the
bank, every long stream replaced (a bank holds a few parts the game plays one after the other, so the song loops
whole), into a cache under the music folder, and writes a map file beside its settings, `jukebox-<game>.txt`
(`MK3D_JUKEBOX`): `arena <id> <bank> <folder>` and `select <bank> <folder>` lines. The runtime asks a game-layer hook
before opening any title path (patch 0045, `kernel::SetOpenHook`); `recomp/common/src/mk3d/jukebox.cpp` answers with
the substitute for the arena the fight loads (each game's `jukebox.cpp` reads its arena variable). The game logs
`jukebox: bank X opened, arena N` for every bank a fight loads, which is how the arena-to-bank tables in
`launcher/data/jukebox.json` were made (`tools/py/jukebox_map.py <game> --write` reads the mapping runs' logs;
Deception's arena ids are its own numbering: 0 Beetle Lair, 19 Slaughterhouse ...). Deception's select screen plays
`shell_music` stream 2, Armageddon's stream 5 of its `shell_music`, a resident sound (the resident block is rebuilt
when one is replaced). Armageddon's fights open `shell_jukebox` (108 streams) with the arena's `bg_` bank: which
jukebox stream each arena plays comes from `tools/py/jukebox_mka_match.py` over audio dumps of a fight per arena.
Deception's Yin Yang Island (twelve short pieces) keeps its own music.

The soundtrack folders downloaded on 2026-11-11 carry a `soundtrack.json` (`tools/py/make_soundtracks.py` writes
them, and copies into `launcher/data/soundtracks`, compiled in): each track's kind (stage, select, menu, stinger,
theme, variant), keywords for the picks, and the select-screen track. A folder without one is read from its file
names.

Development: `MK3DLauncher --jukebox <game>` prepares the banks from the saved settings and prints the plan;
`MK3D_TEST_ARENA=<id>` makes every offline versus fight load that arena (the mapping runs).

## The offline versus

VERSUS on the Single Player page starts the game with `MK3D_VERSUS=1`: the autopilot goes to the versus select
screen with every character, costume and arena unlocked, and the two players pick with their own controllers. Player
2's controller is chosen on the same page (the controls file's `[ports] player2 = "<name>"`; patch 0046): a
controller by that name takes port 1 whenever it is plugged in, every other controller keeps clear of port 1, and
with no name the second controller plugged in is player 2's, as before.
