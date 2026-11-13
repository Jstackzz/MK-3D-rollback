# The jukebox and the offline versus

Added 2026-11-11, the jukebox reworked like Tekken 8's on 2026-11-12. The offline versus is in MK3D Online from 1.0.3
(Single Player's VERSUS button). The jukebox page is a test-build feature for now: the launcher only has it when
built with `-DMK3D_JUKEBOX=ON` (the Deadly Alliance test launcher, test 15 on); the public launcher is built without
it, while the games carry the hook either way (harmless without a map file).

## What it does

As Tekken 8's jukebox, each game plays a music set, picked from a row of tiles at the top of the page:

- **Original**: the game's own music.
- **A soundtrack** (the arcade games, MK4, MK vs DC, MK 2011, MKX, MK11, MK1 2023, or any folder of songs), as its
  **Preset** (each stage gets the song of it that fits the stage best by name, e.g. Moloch's Lair a "Goro's Lair",
  Acid Bath a "Dead Pool"; the main menu its menu music, the select screen its select-screen music) or **Shuffled**
  (new songs each time the game starts).
- **Every soundtrack**, shuffled.
- **My Playlist 1, 2 and 3**: the player's own pick for each stage, the main menu and the select screen. A playlist has
  a screen of its own (Edit): the stages and menus on the left, the soundtracks game by game on the right (tabs), each
  song with a play button to hear it first; Fill from a soundtrack starts one from a preset, Reset puts a stage back.

Under the tiles the page shows what the chosen set plays where (a click hears it). The soundtracks are listed in the
order the games came out (`year` in a soundtrack.json, else a table of the known ones). Each of the three games is set
up on its own. Nothing is downloaded or shipped: the music comes from the player's own folder, `Music\MK3D Jukebox`
unless changed on the page, one subfolder per soundtrack (a `cover.jpg` in it gives its tile a picture).

Online nothing changes: the music is swapped at the file level (below), so the simulation the two games run is the
same whatever each player listens to, and a lobby's watchers hear their own choice.

## How it works

Every game streams its music from `sndsxbox\<bank>.msx`, a table of RIFF WAVE streams in Xbox ADPCM, which is plain
IMA ADPCM in 36-byte-per-channel blocks of 64 samples (`tools/py/msxbank.py` lists, extracts and replaces them; the
format is described there). The launcher re-encodes a chosen song into a copy of the bank, in a cache under the music
folder, and writes a map file beside its settings, `jukebox-<game>.txt` (`MK3D_JUKEBOX`): `arena <id> <bank> <folder>`
lines for stages, `select <bank> <folder>` lines for the front end's banks (substituted whenever the game opens them).
The runtime asks a game-layer hook before opening any title path (patch 0045, `kernel::SetOpenHook`);
`recomp/common/src/mk3d/jukebox.cpp` answers with the substitute for the arena the fight loads (each game's
`jukebox.cpp` reads its arena variable) and logs `jukebox: bank X opened, arena N` for every bank a fight loads.

A bank copy holds every song that goes into it (one copy for a menu bank with the main menu's and the select screen's
songs), every stream a song fills pointing at one copy of it (Armageddon's jukebox bank: 108 streams, one song, a few
MB). A song longer than ten minutes is cut; the game loops what it gets, and a bank whose music is several parts (an
intro and loops) plays the song whole in each part: every part is replaced (Deadly Alliance's select screen opens with
a 7.8 s intro, stream 26, before its loop, stream 27, and its arena banks have short parts too; with those left as they
were, the game's own music played for a few seconds first). The cache is keyed by the bank and its songs and trimmed to
3 GB, the copies the maps name kept, the longest unused going first.

Each song is made as loud as the music it replaces (from test 17): the launcher measures the longest stream it fills as
the game has it and the song itself (ITU-R BS.1770 loudness, LUFS: K-weighted, 400 ms blocks, gated) and changes the
song by the difference, at most 20 dB up or 15 dB down, with a limiter that looks 5 ms ahead holding its peaks under
-1 dBFS (the gain eases back over about 200 ms); what the limiter takes off a big boost is made up, twice at most. The
games' own music is loud (Deadly Alliance's stages -9 to -12.6 LUFS, its select screen -11.4; Deception's -8 to -10.5;
Armageddon's -11.4 to -14.5) and many soundtracks are much quieter (MKX's -25.5 LUFS with peaks at -11.6 dBFS), so
without this a song sounded far softer than the game's music it replaced. Measured in made banks, songs land within
1 dB of the game's music (`build/jukebox/level-check.py <map> <game>`), and in Deadly Alliance's own output (audio
dumps) MKX's songs played at -26.4 LUFS on the select screen against -27.3 for its own music, and at -30.6 against -30.0
on Acid Bath. `track.txt` in a copy's folder says by how much each song was changed.

The table of what plays from where is `launcher/data/jukebox.json`, made from mapping runs and traces
(`XBE_FILE_TRACE=.MSX`, patch 0047, which logs every read of a sound bank; `build/jukebox/read-trace.py` maps the reads
to streams):

| | Stages | Main menu | Select screen |
|---|---|---|---|
| Deadly Alliance | 17 arenas, each its music bank (the arena table at 0x231750, +0x14 the bank's index into the list at 0x27F5F8), every part | `shell` stream 1 | `shell` streams 26 (an intro) then 27 |
| Deception | 20 arenas, each its `music_tune_*` bank (the game's own arena ids: 0 Beetle Lair ...), every part; Yin Yang Island (twelve short pieces) keeps its own | `shell_music` streams 9 (once) and 10 (looped), resident | `shell_music` streams 1 then 2 |
| Armageddon | 34 arenas, all from `shell_jukebox` (each arena always starts the same stream of it, 20 different ones, many arenas sharing one): a copy per song with every stream that song | `shell` stream 25 | `shell_music` stream 5, resident |

A **resident** sound is one the game loads whole into memory at boot: a longer one is not loaded at all (56 to 150 s
versions of Armageddon's 28 s select music played nothing). In a copy, a replaced resident sound becomes a streamed
one: out of the resident block (which follows the file header on its 0x800 boundary), its resident flag cleared, and
its record in the bank's `.mbx` marked streamed (36-byte sound records from [0x10], +32 1 when streamed); the game then
streams it like any other music.

A shuffle is drawn again each time the launcher starts the game (`jukebox::Service::gameStarting`); the map follows a
moment later, and until then the last draw plays.

The soundtrack folders downloaded on 2026-11-11 (the arcade games, MK4, MK vs DC and the NetherRealm games) and
2026-11-12 (Deadly Alliance and Deception whole; Armageddon's 70-minute "extended" videos cut to their first 6
minutes, and the player's own file for its select screen, a `local` line in its `downloaded.txt`) carry a
`soundtrack.json` (`tools/py/make_soundtracks.py` writes them, and copies into `launcher/data/soundtracks`, compiled
in): each track's kind (stage, select, menu, stinger, theme, variant), keywords for the picks, its length (from the
file), the select-screen track and the year the game came out (the page's order). A folder without one is read from
its file names.

Verified 2026-11-12 in all three games with launcher-made banks, by audio dumps and a test tone where a stage's own
sounds drown the comparison: Deadly Alliance's main menu, select screen, Acid Bath and Palace Grounds; Deception's
main menu (made streamed), select screen and two stages; Armageddon's main menu, select screen (made streamed),
Subway and Sky Temple; and from a fresh unzip of the test package, whose own launcher made the banks.

Development: `MK3DLauncher --jukebox <game> [--jukebox-set <set>]` prepares the banks from the saved settings (the set
given first) and prints the plan; `MK3D_TEST_ARENA=<id>` makes every offline versus fight load that arena;
`XBE_FILE_TRACE` and `XBE_DSOUND_TRACE` (patch 0047) show what a game reads and plays.

## The offline versus

VERSUS on the Single Player page starts the game with `MK3D_VERSUS=1`: the autopilot goes to the versus select
screen with every character, costume and arena unlocked, and the two players pick with their own controllers. Player
2's controller is chosen on the same page (the controls file's `[ports] player2 = "<name>"`; patch 0046): a
controller by that name takes port 1 whenever it is plugged in, every other controller keeps clear of port 1, and
with no name the second controller plugged in is player 2's, as before.
