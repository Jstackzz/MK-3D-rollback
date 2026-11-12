// The jukebox's view of this game (recomp/common/src/mk3d/jukebox.h): the arena a fight loads, for the music bank
// the launcher's substitute stands in for.
#include "mk3d/jukebox.h"

#include <cstdlib>

#include "game/events.h"
#include "kernel/kernel.h"
#include "mk3d/game.h"

namespace mka {
namespace {

int Arena() {
    const int a = int(kernel::R32(kArena));
    return a >= 0 && a < 0x40 ? a : -1;
}

// MK3D_TEST_ARENA=<id> (development, offline): every versus fight on that arena, to map arenas to music banks: the
// arena cycle (linkgame.cpp PickArena) held at the arena's place on the versus list while the front end is up
constexpr uint32_t kTestArenaList = 0x548F88, kTestArenaCycle = 0x831A64;
// ... and the select screen's own arena choice (linkgame.cpp kTrackChoice), which reaches arenas off the versus list
constexpr uint32_t kTestArenaChoice = 0x831A58;

const bool g_installed = [] {
    mk3d::jukebox::Install({Arena});
    if (const char* t = std::getenv("MK3D_TEST_ARENA")) {
        static int wanted = std::atoi(t);
        mk3d::OnFrameStart([](uint64_t) {
            if (!InFrontEnd()) return;
            kernel::W32(kTestArenaChoice, uint32_t(wanted));
            for (uint32_t i = 0; i < 64; i++) {
                const int32_t id = int32_t(kernel::R32(kTestArenaList + 4 * i));
                if (id == -1) break;
                if (id == wanted) {
                    kernel::W32(kTestArenaCycle, i);
                    break;
                }
            }
        });
    }
    return true;
}();

}  // namespace
}  // namespace mka
