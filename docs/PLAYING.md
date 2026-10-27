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
  it to choose your fighter in the launcher instead. Your record (wins, losses,
  and Elo in Ranked) is on the right of the page. When a match is found, its
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
- Lobby, to play someone you know: one of you presses Create lobby and gives the
  other the 5-letter code; the other presses Join code and types it. Both choose
  a fighter (or "Pick in the game") and press Ready, then the host presses START.
- Single player, then PLAY, starts the whole game with its own menus, as on the
  console: arcade, its practice mode and the rest. What you unlock and set is
  saved in the game's folder (games\<game>\hdd).
- Play while you wait: search for a match (Kasual or Ranked) and play single
  player meanwhile, in either order. When a match is found, its ACCEPT window
  comes up over your game. Accept it and your single-player game is set aside,
  frozen exactly where it is, while the online game takes you into the match.
  When the match is over (with its rematches, or the whole ranked set), you are
  back in your single-player game where you left it. Decline, or let the match
  go, and you just play on.

Both players need the same package: VERSION.txt lists fingerprints of its programs
to compare.

Matches connect by themselves: the launcher opens its game port (UDP 7001) on
your router when the router allows it (UPnP; Settings shows it on the Router
line and can turn it off), and when two PCs cannot reach each other directly the
match runs through the server, with a little more delay. Nothing to set up on
your router.


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
fighter by itself. The keyboard plays as player 1
while the game window is in front: arrows or WASD move, J I K L are X Y A B,
Space is the right trigger, O the left trigger, U and P are white and black,
Enter is START and Backspace is BACK.

To change them, open Controls in the launcher: click a key to give a button
another one (two keys each at most), and choose what each button of your
controller presses. A game that is running picks up the change at once, and it
works the same in online matches: it only changes what your own keys and
buttons press. Which button punches or kicks is each game's own controller
setting; online, Armageddon plays with its default one.


Settings
--------

The Settings page has the server address, region, maximum ping, input delay, the
game's UDP port, and the display: borderless (fills the screen), fullscreen or
a window, the resolution (Desktop is your monitor's own) and v-sync. A game
takes them when it starts.


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
- The launcher or the game says the disc is a different release: it is not one of
  the releases above.
- "This is not an Xbox disc image": the .iso is a broken or padded conversion
  without the Xbox file system (some downloads carry one beside the good image).
  Use the XISO version of the disc, often named .xiso.iso.
- Click into the game window after accepting a match in the launcher if you
  play on the keyboard: it only reaches the window in front (controllers work
  either way).
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
