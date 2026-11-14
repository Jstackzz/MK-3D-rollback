# The jukebox and the offline versus

Added 2026-11-11, the jukebox reworked like Tekken 8's on 2026-11-12. The offline versus is in MK3D Online from 1.0.3
(Single Player's VERSUS button). The jukebox is in MK3D Online from 1.0.6 (the JUKEBOX page; the launcher's
`MK3D_JUKEBOX` build option, on by default); before that it was a test-build feature (Deadly Alliance tests 15 to 19).
The games carry the hook either way (harmless without a map file).

## What it does

As Tekken 8's jukebox, each game plays a music set, picked from a row of tiles at the top of the page:

- **Original**: the game's own music.
- **A soundtrack** (the arcade games, MK4, MK vs DC, MK 2011, MKX, MK11, MK1 2023, or any folder of songs), as its
  **Preset** (each stage gets the song of it that fits the stage best by name, e.g. Moloch's Lair a "Goro's Lair",
  Acid Bath a "Dead Pool"; the main menu its menu music, the select screen its select-screen music) or **Shuffled**
  (new songs each time the game starts).
- **Every soundtrack**, shuffled.
- **My Playlist 1, 2 and 3**: the player's own pick for each stage, the main menu and the select screen, the "finish
  him" music and (Deadly Alliance, Deception) the fatality's. A playlist has a screen of its own (Edit): the stages and
  menus on the left, the soundtracks game by game on the right (tabs), each song with a play button to hear it first;
  Fill from a soundtrack starts one from a preset, Reset puts a stage back.

**Finish Him** (1.0.7, all three games): one song for the "finish him" music of every stage. It has no bank of its own
(`"streams": "finish"` in jukebox.json): every stage's bank copy gets it in its finish him loop (`finishStreams`: each cue
that loops a stream shorter than 20 s and plays none of the round music's streams; the round music's loops are longer,
the round-end sting loops nothing, Deadly Alliance's cue of every piece in a row plays the round music, Moloch's Lair's
other loop is 28.6 s), beside the stage's own song when it has one. So a Finish Him song makes a copy of every stage
bank; a stage shares a copy only with stages that want the very same songs (one with its own music never gets
another's through a shared bank), and on Armageddon's jukebox bank, where a stage's song fills every stream, the song on
named streams takes them over (`replaceStreamsMany`): the stage's song everywhere but the 20 finish him loops (6 to
16 s, one a stage tune). Presets and shuffles leave it the game's own. **Fatality** (Deadly Alliance and Deception: the
`music_fatality` bank, every stream; Armageddon has none): a playlist's song, or a soundtrack's fatality song (MK1's,
MK2's, MKX's) in its preset or a shuffle.

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
MB). A song longer than ten minutes is cut; the game loops what it gets. The cache is keyed by the bank and its songs
and trimmed to 3 GB, the copies the maps name kept, the longest unused going first.

**Music in pieces: the descriptor's cues** (from test 18). A bank's music is played by cues in its descriptor, the
`.mbx` beside it: the fourth section (offsets from [0x10], so [0x1C] to [0x20]), elements of 44 bytes in version 6
(Deadly Alliance: +4 flags, +8 seconds) and 48 in version 11 (Deception, Armageddon: +8 flags, +24 seconds). An
element 0x2000NNNN plays sound NNNN once (flags 0x10301, or 0x10101 for a sound the game keeps in memory) or looped
(0x301 / 0x101), the given seconds after the element before it started (that sound's own length); 0xFFFFFFFF with
flags 7 ends the cue (after its seconds, or -1 never: the last sound loops), with flags 8 goes on within the cue after
its seconds, the elements after it timed from it (Armageddon's Tekunin Prison and Goro's Lair start their music so:
until test 19 the launcher fitted none of those times, so a song overlapped itself or left a long silence there). A stage bank has three cues the game plays
during a fight, each stopping the one before: the round music (an intro, then a loop; Deception's a second version
with a short pre-intro for some rounds), the "finish him" loop and the round-end sting (traced: rounds restart the
round music), plus a last cue of every piece in a row. Deadly Alliance's select screen is one cue: stream 26 (7.8 s)
once, then stream 27 looped 7.752 s later; Deception's main menu streams 9 then 10 (12.624 s later).

The game starts a cue's next piece after the seconds the descriptor gives, whatever the stream it plays: a song put in
every piece played over itself (test 17: Acid Bath's loop began on top of the song 20.8 s into a round, the select
screen's after 7.75 s). So the launcher (`roundMusicStreams`, `fitCueTimes`) puts the song only in a stage's round music
(the cues that loop the bank's longest looping stream: `"streams": "cue"` in jukebox.json; the sting and the "finish
him" loop stay the game's, and a round starts the song again as the game's own music does), and writes the copy's
descriptor so that a piece after a replaced one starts once the song has ended (the round music's loop then repeats
the song seamlessly), and a cue that ended after a replaced piece ends with the song. Checked in the games (file traces,
`build/jukebox/stream-timeline.py`; audio dumps): Deadly Alliance's select screen and Acid Bath (one copy of the song,
the loop starting at its end, 61 s in; the sting and "finish him" the game's own between rounds), Deception's main
menu and Beetle Lair. `build/jukebox/round-cues.py <game>` prints every stage bank's cues and the round music chosen,
`--copy <folder>` a made copy's fitted cues.

**Songs loop as game music does** (from test 19). A song no longer goes into every piece whole: the launcher finds its
loop (`findLoop`) and cuts it into the parts the game's pieces play (`cutSong`), each piece's part read from the cues
(`streamParts`): a looped piece plays the song's loop; the pieces a cue plays once before its looped one (a chain)
hold the intro once between them (a chain of one is that piece; a longer chain keeps the piece that is some cue's
intro, else its last), the others a moment of silence (Deception's pre-intros, Armageddon's three-piece chains and
Tekunin Prison, whose later rounds start at the chain's first piece); a piece played once with nothing looped after
it the intro and one loop in a row. The game then plays the intro once and
the loop forever, and `fitCueTimes` starts each piece when the one before has ended. The change is all in the made
copies (the bank and its descriptor): the games are untouched.

`findLoop` (the launcher; `build/jukebox/loop-scan.py`, `loop-diag.py` and `loop-start.py` are how it was worked out
on the whole library): soundtrack rips play the game's loop twice or more and fade out, so the loop is the stretch that
comes back one period later. 0.1 s frames of 40 log-spaced bands (80 Hz to 5 kHz) of a mono 11025 Hz copy, each frame's
own loudness out (a fading repeat still matches), centred and normalised; for each lag from 8 s up, the longest stretch
of frames matching the frames one lag later (over 0.7, gaps under 1 s closed). The loop is the shortest lag whose
stretch verifies 80% of it and 20 s or more, starts in the song's first half and matches 0.8 on average. Its period is
then refined to the sample by waveform correlation, and it is exact when 80% of its 1 s windows repeat sample for
sample; an exact loop's start is found to the waveform (the first moment from which 0.25 s windows match one period on
for 1.5 s: the frames' start can be seconds early, Lost Tomb's by 4 s, and a loop starting in the intro would replay a
moment of it every time round). A loop that repeats only musically (a rip that varies, as the arcade music systems do,
or an extended version edited together) must come back two and a half times or cover most of the song, else a longer
lag is tried; it starts 1 s after its stretch. No loop: the whole song loops, from where its music begins to where it
ends (after the last 0.4 s within 10 dB of its loud level: a fade-out, a final decay and silence left off).

The pieces: the intro is the song up to the loop start; the loop is exactly one period resampled to whole ADPCM blocks
(64 frames: a stretch of at most 32 frames, too little to hear) so that its end runs into its start, its last 60 ms
cross-faded into what comes before the loop start (an exact loop; 1 s, equal-power, for a musical one). A whole-song
loop starts 2 s into the song, the intro being those 2 s, and its end blends over 2 s into them. Checked: on Deadly
Alliance's own music the periods found are the game's own loop lengths to within 0.03 s (Acid Bath 33.707 s against
33.694, Lost Tomb 34.306 against 34.29, Moloch's Lair 43.656 against 43.64), and both versions of Armageddon's select
music give 28.00 s; `build/jukebox/seam-check.py <copy folder>` scores a made loop's seam against the song (exact loops
0.91 to 0.99 against 0.99 to 1.00 inside the loop; musical ones 0.995 against the song's own way into its loop); in the
games (audio dumps followed with audio_match.py), MK2's Dead Pool on Acid Bath went round its 28.96 s loop twice and
Armageddon's Subway (52.38 s, musical) once, each without a gap, a restart or an overlap. `MK3DLauncher --jukebox-loops
<folder>` prints the loop found for every song under a folder.

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
| Deadly Alliance | 17 arenas, each its music bank (the arena table at 0x231750, +0x14 the bank's index into the list at 0x27F5F8), the round music's cues | `shell` stream 1 | `shell` streams 26 (an intro) then 27 |
| Deception | 20 arenas, each its `music_tune_*` bank (the game's own arena ids: 0 Beetle Lair ...), the round music's cues; Yin Yang Island (twelve short pieces) keeps its own | `shell_music` streams 9 (once) and 10 (looped), resident | `shell_music` streams 1 then 2 |
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
