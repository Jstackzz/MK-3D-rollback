MK3D Online: player guide
=========================

Native PC versions of the original Xbox Mortal Kombat: Deadly Alliance, Deception
and Armageddon, with online play (rollback netcode): Kasual and ranked matchmaking,
lobbies, and the side modes (Chess Kombat, Puzzle Kombat, Motor Kombat).

This package contains no game. You build each game once from your own disc image,
on your PC; nothing from the games is downloaded.


What you need
-------------

- Windows 10 or 11, 64-bit, and a graphics card with Direct3D 12.
- About 6 GB free for each game you build (its files and its compiled code).
- Your own disc image of the game, in these releases:
    Mortal Kombat: Deception (USA)
    Mortal Kombat: Armageddon (USA)
    Mortal Kombat: Deadly Alliance (USA) (Rev 1)
  An .iso works: an XISO image or a whole-disc dump. Unpack a .7z or .zip first.
  Other releases (PAL, Japanese, the first Deadly Alliance print, the Armageddon
  Premium Edition bonus disc) are different builds of the game: the launcher tells
  you so, and the game would most likely crash with them.
- A controller (Xbox-style), or the keyboard (see Controls).


Install
-------

1. Unpack the whole zip into a folder you can write to, for example
   Documents\MK3D-Online (not Program Files).
2. Start MK3DLauncher.exe. If Windows says "Windows protected your PC" (the files
   are not signed), choose More info, then Run anyway.
3. When Windows Firewall asks about MK3DLauncher or a game (MKD, MKA, MKDA), allow
   it. Online matches need it.


Updates
-------

From 0.1.7 on, the launcher updates itself. When a new version is out, a window
says so, with what is new: press UPDATE NOW (or LATER: the footer's UPDATE
button brings it back). The launcher downloads it, checks it is exactly the
release that was published, puts it in place and starts again. It waits until
your match is over and you close any game you are playing. Your settings,
saves, built games and disc images stay. Each game you have built prepares its
code for the new version right after, one at a time, each in its own small
window (seconds to a minute or two); a game can be played once its turn is
done, and the launcher says so.


Build a game (once for each game)
---------------------------------

1. Choose the game at the top of the launcher and open Build game.
2. Browse to your disc image and press EXTRACT AND BUILD.
3. The launcher extracts the disc into games\<game>\assets, then starts the game
   once: a window shows it "preparing the game for this PC" (a few minutes, only
   this once). When it says "The game is ready", choose Exit (B), or Start (A)
   to try it offline.


Play
----

The launcher connects to the server this package was made for. The first time, it
opens a window to create your account: a name and a password, kept on that
server. If you close it, SIGN IN at the top right opens it again.

If the launcher cannot connect (a package made without a server, or the host's
address changed): open Settings, type the address your host gave you under
Server (name-or-address:7777) and the fingerprint they gave you, then Connect.
With the fingerprint left empty, the launcher shows the server's and asks
whether to trust it: compare it with your host's.

- Kasual and Ranked: choose the mode, then Find match. With "Pick my fighter on
  the game's own select screen" ticked (the default) you choose your fighter in
  the game when the match starts, with your own controller, while your opponent
  does the same on their PC; the game shows their pick once they made it. Untick
  it to choose your fighter in the launcher instead. Puzzle Kombat's fighter is
  picked the same way, and Chess Kombat's team (leader, champion, sorcerer,
  shifter, grunt) always on the game's own select screen. Your record (wins, losses,
  and Elo in Ranked) is on the right of the page, and above it how many play the
  game right now: searching (and how many of them in the mode you picked, the
  ones you can meet), in matches and in lobbies. When a match is found, its
  ACCEPT window comes up over the game; once you accept, you are back in the
  game.
- After a Kasual match, once the game has played its ending, a window comes up
  over the game with the score between you and two ways to play on, same
  sides, no search: REMATCH (when you both press it: the same fighters again,
  straight into the fight) or SELECT (from either of you: both go back to
  character select and pick again). You have 60 seconds from the match's end.
  LEAVE (or Find match) ends it. The Kasual page shows the same choices.
- Ranked is played in sets against one opponent: the first to win 2 matches
  takes the set, and your Elo changes once, when it ends. The set's next match
  starts by itself once both games are ready. Leaving a set after one of its
  matches has counted (LEAVE SET, searching again, quitting, or not being back
  within 90 seconds) loses it: one match won and gone gains nothing.
- Closing your game during a match (Alt+F4, or it crashes) ends the match for
  both of you at once. In Kasual the series ends there, with no rematch; in
  Ranked it loses you the set, whatever the score. If the connection between
  the two games breaks instead, the match does not count and the series ends.
- Lobby, to play someone you know: one of you presses Create lobby and gives the
  other the 5-letter code; the other presses Join code and types it. Both choose
  a fighter (or "Pick in the game") and press Ready. Once both are ready the
  match starts by itself after a 3-2-1 countdown (un-ready to stop it). After
  every lobby match the launcher comes back in front on the Lobby page with
  Ready highlighted: press A on a controller (or START anywhere on the Lobby
  page) to ready up for the next one.
  If your connection drops, sign back in within a minute (the launcher does it
  by itself) and you are put back in your lobby. Leave closes the game the
  lobby started (once a match it is playing is over), so there is nothing left
  to close by hand; a dropped connection keeps it.
- Watching a lobby: select it in the list and press Watch selected (or join it,
  then press Watch). Each of its matches plays in your own game, a moment
  behind the players, select screen and ending included; a match already under
  way is caught up from its start, fast, and one that ends early (a player left)
  is dropped. If your connection drops for a moment, your game keeps watching.
  Up to eight people can watch a lobby.
  Press Play to take a free place to play between matches, Watch to step aside.
- Death traps: the arenas' death traps (Deception and Armageddon: the Dead
  Pool's acid, the Lower Mines' spikes and the others) kill on the spot. The
  Death traps box under Matchmaking on the Kasual page says whether you want
  them, and you only meet players who chose the same: untick it and every
  Kasual opponent has death traps off too. Ranked matches never have them. A
  lobby's host chooses for its matches when creating it, and can switch it in
  the lobby (the Death traps box under its name) between matches; both players
  then press Ready again. Online matches also play with the game's default
  round time and blood, whatever your own game's options say, so both players
  play the same fight.
- Every online match (Kasual, Ranked, lobbies) has every fighter, alternate
  costume and arena open, whatever your own profile has unlocked: hold START
  as you confirm your fighter on the select screen for their alternate
  costume, and your opponent's game shows it too. Armageddon's kreated fighters
  stay out of online matches (your opponent's game has no copy of yours). Your
  profile's own unlocks stay as they are.
- Single player, then PLAY, starts the whole game with its own menus, as on the
  console: arcade, its practice mode and the rest. What you unlock and set is
  saved in the game's folder (games\<game>\hdd).
- Single player, then PRACTICE, goes straight to practice's character select
  with every fighter, costume and arena unlocked: pick yours, then the dummy,
  with one controller.
  What your profile has unlocked stays as it is. You can search for a match
  while you practice, as below.
- Play while you wait: search for a match (Kasual or Ranked) and play single
  player meanwhile, in either order. When a match is found, its ACCEPT window
  comes up over your game. Accept it and your single-player game is set aside,
  frozen exactly where it is, while the online game takes you into the match.
  When the match is over (with its rematches, or the whole ranked set), you are
  back in your single-player game where you left it. Decline, or let the match
  go, and you just play on.
- Practice while you wait (the box under Matchmaking on the Ranked and Kasual
  pages, on by default): once you search, the game opens its practice mode by
  itself, at the character select screen, where you pick your fighter and then
  the dummy with your controller (everything is unlocked there, as with
  PRACTICE). A match found comes up over it as above, and
  after the match you are back in practice. Untick the box to wait at the game's
  menu instead.
- Frame data in practice (all three games): a small panel at the left shows
  player 1's last attack, counted in frames (60 a second) from the press as
  frame 1. **Startup** is the frame the attack can first hit, **Active** how
  many frames it can hit (a move that hits more than once lists each hit's
  frames, with the gap between them in brackets), **Recovery** the frames
  after that until you can act again, **Total** the whole move. After a hit or
  a block it shows the frame advantage, **On hit** or **On block**: how many
  frames before (+, green) or after (-, red) the dummy you can act again. A hit
  that lands on a later active frame than the first (from further away) gives
  more advantage, so the panel names that frame. The dummy stands still in
  every game's practice; **F8** makes it hold block, then crouch-block (lows
  are blocked only crouching), then stand again, for the on-block numbers.
  **F7** shows or hides the panel; **Settings → Frame data in practice** sets
  whether it is shown when practice starts. None of it is ever in an online
  match.

Both players need the same package: VERSION.txt lists fingerprints of its programs
to compare.

Matches connect by themselves: the launcher opens its game port (UDP 7001) on
your router when the router allows it (UPnP; Settings shows it on the Router
line and can turn it off), and when two PCs cannot reach each other directly the
match runs through the server, with a little more delay. Nothing to set up on
your router. A network that lets no UDP through to the server (some firewalls,
VPNs, school or work networks) is fine too: the launcher notices and plays
those matches through its own connection to the server, no VPN needed.

Your IP address is never shown in the launcher, its pages or its logs, nor in
the games' logs, and a game only listens to its own match's opponent (or the
server), so nobody else can send it anything. A match normally connects your PC
straight to your opponent's, for the lowest ping; like any direct connection,
their PC can see the address it talks to (with network tools). To keep it from
everyone you play, tick **Settings → Hide my IP address from opponents**: your
matches then go through the server, and nobody you play learns your address.
That costs ping the farther you and your opponent are from the server (in
Virginia, USA): little on the US east coast, a lot for two players far from it
(two players on the west coast: about 20 ms straight, about 140 ms through the
server).

When a game crashes, the launcher tells the server which game, the crash's code
and where in the program it happened (nothing else: no files, nothing about
you), so crashes many players have can be found and fixed.

When a match is found, the MATCH FOUND window shows your opponent's connection:
the Wi-Fi symbol in orange (wireless drops and delays packets now and then) or
green bars for a wired (Ethernet) connection, and their ping to the server, or
the ping between the two of you when it is known (from a match you played in the
last ten minutes). Ranked shows it too, without who it is. A lobby's Connection
column shows every member's the same way.

A search that finds nobody raises your Max ping by itself: after 30 s by 25 ms,
and again every 15 s, up to 250 ms (the Kasual and Ranked pages say "Max ping
raised to ... ms"), so two players a little too far apart for their settings
still meet. Strict ranked matchmaking keeps your Max ping as it is.

Searching and nobody shows up? Kasual and Ranked are separate searches, and so
are Kasual with death traps on and with them off; the Region you search in
waits 20 s for your own region first; and your **Max ping** (Settings, 150 ms
unless you change it) keeps out anyone whose connection to you would be
slower. Two players far apart (Europe and South America, say) only meet with a
higher Max ping or none.


Controls
--------

A controller works as on the Xbox: Xbox (360, One, Series), PlayStation and
Switch controllers, and fight sticks in their Xbox/PC or PlayStation mode, with
LB and RB (L1 and R1) as the Xbox's white and black buttons; the d-pad, both
sticks and the triggers reach the game as on the console. Online one controller
plays for you: with several plugged in, it is the one the game found first, so
unplug the ones you do not use. The left stick moves like the d-pad in all
three games (Deadly Alliance on the console took the d-pad alone), so a stick
that drifts (reads as pushed while you do not touch it) can walk or crouch your
fighter by itself. (Up to 0.1.6, in Deception and Armageddon, the left
player's stick also moved the right player's fighter online; fixed in 0.1.7.)

The launcher works with a controller too, while its window or one of its
prompts is in front: the d-pad or the left stick moves the gold highlight, A
(Cross) presses, B (Circle) closes a window (never MATCH FOUND or the rematch
prompt), LB and RB switch pages and START finds a match on the Ranked and Kasual
pages, or presses Ready on the Lobby page. A prompt that comes up over the game waits for the buttons you were
holding in the game to be let go, so a combo cannot accept or decline anything.
While a match is played or watched, the launcher ignores the controller
altogether, even if its window is the one in front: what you press is the
game's alone.
Typing (sign in, lobby names, chat) still takes the keyboard. The keyboard plays as player 1
while the game window is in front: arrows or WASD move, J I K L are X Y A B,
Space is the right trigger, O the left trigger, U and P are white and black,
Enter is START and Backspace is BACK.

To change them, open Controls in the launcher: click a key to give a button
another one (two keys each at most), and choose what each button of your
controller presses. The page names each button by what it does in the game
chosen at the top, then the Xbox button: Attack 1 (X), Attack 2 (Y), Attack 3
(A), Attack 4 (B), Block (right trigger), Stance change (left trigger), Throw
(black button; Throw / special in Deadly Alliance), Jump and Crouch (d-pad up
and down), Pause (START). A game that is running picks up the change at once,
and it works the same in online matches: it only changes what your own keys
and buttons press. That is each game's default layout; its own controller
options can move them, and online Armageddon plays with its default one.


Settings
--------

The Settings page has the server address, region, maximum ping, input delay (1
frame unless you change it: rollback covers the rest of a match's ping), the
game's UDP port, and the display: borderless (fills the screen), fullscreen or
a window; the resolution, your monitor's own unless you pick another of its
sizes from the list (a window at your monitor's resolution is as large as fits
on the screen); v-sync; brightness (100 % is the picture as the game draws it;
more lifts the dark tones, blacks and whites stay); and whether Deadly Alliance
(which has no widescreen mode) is stretched to fill a widescreen monitor, as it
is unless you untick it, or keeps its original 4:3 shape with black bars at the
sides. A game that is running takes a change at once, its window's mode and
size the next time you switch to it.

V-sync has three choices. On, the default: every frame waits for your monitor,
so the picture never tears, even when your graphics driver is set to force
v-sync off. On, G-SYNC or FreeSync monitor: a monitor with a variable refresh
rate follows the game's 60 frames a second, the smoothest there is (G-SYNC or
FreeSync has to be on in your graphics driver). Off: each frame is shown at
once and may tear.


When something goes wrong
-------------------------

- "Could not start ...MKD.exe: An Application Control policy has blocked this
  file", right after extracting a disc or when a match starts, or Windows
  refuses to start the launcher and offers no "Run anyway": Smart App Control
  (Windows 11) runs only programs signed by a publisher it knows, and it has no
  exception list. The games can never be signed: every PC builds its own copy
  of the game's code from its own disc. The launcher warns at startup when it is
  on. To play, switch it off in Windows Security > App & browser control > Smart
  App Control settings; on some versions of Windows 11 it can only be switched
  back on by resetting Windows, so that choice is yours.
- "The game's files are not there yet": build that game first (Build game).
- The launcher says the disc is a different release (an earlier print, the PAL
  or Japanese release, another game): it names what your disc is. Each game
  here is built from one release only, the one listed above; another release's
  code is laid out differently, so the game would crash with it. Extract a disc
  image of the listed release over it (Build game). A modified copy of the right
  release (patched for a modded console) may work: the launcher asks.
- Windows Security (or another antivirus) reports a threat in a game's files, or
  a game stops at start saying its code was blocked or removed: a false alarm.
  Every PC builds its own copy of each game's code (MKD.dll and the others) and
  a new release brings new files, so no antivirus has seen them before. Open
  Windows Security > Virus & threat protection > Protection history, pick the
  entry, choose Actions > Restore (or Allow on device), then start again.
- "This is not an Xbox disc image": the .iso is a broken or padded conversion
  without the Xbox file system (some downloads carry one beside the good image).
  Use the XISO version of the disc, often named .xiso.iso.
- Click into the game window after accepting a match in the launcher if you
  play on the keyboard: it only reaches the window in front (controllers work
  either way).
- The launcher says "The server did not answer on UDP": something between your
  PC and the server blocks UDP (a firewall, an antivirus's network protection,
  some VPNs, your network or internet provider). Matches still work, through
  your connection to the server, with a little more delay and a hitch now and
  then when that connection loses a packet. When a firewall on your PC is the
  cause, allowing the launcher and the games through it brings the usual way
  back.
- The picture tears (a line across it, the parts above and below it from
  different moments): choose V-sync On in Settings, which never tears whatever
  your graphics driver says. With On, G-SYNC or FreeSync monitor, tearing means
  the monitor is not following the game: turn G-SYNC or FreeSync on in your
  graphics driver (and leave its v-sync to the application, AMD's Enhanced Sync
  off), or choose On.
- Movement looks uneven on a 144 Hz or 165 Hz monitor: the games run at 60
  frames a second, which such a monitor cannot show evenly. Use its G-SYNC or
  FreeSync (V-sync: On, G-SYNC or FreeSync monitor), or set it to 120 Hz or
  60 Hz in Windows' display settings.
- Logs: mk3d-launcher.log and the sessions folder beside the launcher: one
  folder each time the launcher starts a game, with the game's own game.log and,
  for each online match, a net-<match>.log with every frame's inputs. When a
  match plays wrong, send these along with what happened.


Legal
-----

Mortal Kombat is a trademark of Warner Bros. Entertainment Inc. This project is not
affiliated with Warner Bros., NetherRealm Studios, Midway or Microsoft. Play with
your own copies of the games. The licenses of the software in this package are in
the LICENSES folder.
