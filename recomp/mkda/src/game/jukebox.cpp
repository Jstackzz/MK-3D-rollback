// The jukebox's view of this game (recomp/common/src/mk3d/jukebox.h): the arena a fight loads, for the music bank
// the launcher's substitute stands in for.
#include "mk3d/jukebox.h"

#include <cstdlib>

#include "game/events.h"
#include "kernel/kernel.h"

namespace mkda {
namespace {

int Arena() {
    // the arena the fight loads; 0x19 ("the next one") or none yet: the one chosen for the next fight, if any
    int a = int(kernel::R32(kArena));
    if (a < 0 || a > 24) a = int(kernel::R32(kNextArena));
    return a >= 0 && a <= 24 ? a : -1;
}

const bool g_installed = [] {
    mk3d::jukebox::Install({Arena});
    // MK3D_TEST_ARENA=<id> (development, offline): every fight on that arena, to map arenas to music banks
    if (const char* t = std::getenv("MK3D_TEST_ARENA")) SetNextArena(std::atoi(t));
    return true;
}();

}  // namespace
}  // namespace mkda
