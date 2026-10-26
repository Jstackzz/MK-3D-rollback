MK3D Online: player guide
=========================

Native PC versions of the original Xbox Mortal Kombat: Deadly Alliance, Deception
and Armageddon, with online play (rollback netcode): casual and ranked matchmaking,
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
asks you to create an account: a name and a password, kept on that server.

If the launcher cannot connect (a package made without a server, or the host's
address changed): open Settings, type the address your host gave you under
Server (name-or-address:7777) and the fingerprint they gave you, then Connect.
With the fingerprint left empty, the launcher shows the server's and asks
whether to trust it: compare it with your host's.

- Casual and Ranked: choose the mode and your fighter, then Find match.
- Lobby, to play someone you know: one of you presses Create lobby and gives the
  other the 5-letter code; the other presses Join code and types it. Both choose
  fighters and press Ready, then the host presses START.
- Practice starts the game offline.

Both players need the same package: VERSION.txt lists fingerprints of its programs
to compare.

Matches connect by themselves: the launcher opens its game port (UDP 7001) on
your router when the router allows it (UPnP; Settings shows it on the Router
line and can turn it off), and when two PCs cannot reach each other directly the
match runs through the server, with a little more delay. Nothing to set up on
your router.


Controls
--------

A controller works as on the Xbox: Xbox, PlayStation and Switch controllers,
with LB and RB as the Xbox's white and black buttons. The keyboard plays as
player 1 while the game window is in front: arrows or WASD move, J I K L are
X Y A B, Space is the right trigger, O the left trigger, U and P are white and
black, Enter is START and Backspace is BACK.

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

- Windows refuses to start the launcher and offers no "Run anyway": Smart App
  Control (Windows 11) blocks programs without a publisher's signature, and it
  has no exception list. Only switching it off in Windows Security lets them run;
  that choice is yours.
- "The game's files are not there yet": build that game first (Build game).
- The launcher or the game says the disc is a different release: it is not one of
  the releases above.
- Logs: mk3d-launcher.log and the sessions folder beside the launcher, and each
  game's logs folder in games\<game>.


Legal
-----

Mortal Kombat is a trademark of Warner Bros. Entertainment Inc. This project is not
affiliated with Warner Bros., NetherRealm Studios, Midway or Microsoft. Play with
your own copies of the games. The licenses of the software in this package are in
the LICENSES folder.
