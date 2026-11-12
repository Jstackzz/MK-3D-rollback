#pragma once
// The jukebox: another sound bank in place of one of the game's own, so a stage or the select screen plays the music
// the player chose in the launcher (any game's soundtrack, a song per stage). The launcher writes the substitutes
// (<bank>.msx with the music stream re-encoded in the bank's own format, beside a copy of its <bank>.mbx) into a
// cache and names them in a map file, MK3D_JUKEBOX=<file>, read again whenever it changes:
//
//   select <bank> <folder>          the front end's bank (shell, shell_music): the select screen's music
//   arena <id> <bank> <folder>      the music bank a fight on that arena loads (the game's own arena id; * any)
//
// The runtime asks (kernel::SetOpenHook) before every open of a title path: a sndsxbox\<bank>.msx or .mbx with a
// substitute in the folder opens that file instead. Nothing in the simulation changes: the stream's bytes differ,
// which the sound mixer alone reads, so two players with different music stay in sync and a watcher hears their own.
#include <cstdint>

namespace mk3d::jukebox {

struct Adapter {
    // the arena the fight being loaded (or the next one) plays on, the game's own id; -1 when there is none
    int (*arena)();
};

// the game's adapter, once at start; installs the open hook when MK3D_JUKEBOX names a map file
void Install(const Adapter& adapter);

}  // namespace mk3d::jukebox
