// The jukebox's view of this game (recomp/common/src/mk3d/jukebox.h): the arena a fight loads, for the music bank
// the launcher's substitute stands in for.
#include "mk3d/jukebox.h"

#include <cstdlib>

#include "game/events.h"
#include "kernel/kernel.h"
#include "mk3d/game.h"

namespace mkd {
namespace {

int Arena() {
    const int a = int(kernel::R32(kArena));
    return a >= 0 && a < 0x40 ? a : -1;
}

// MK3D_TEST_ARENA=<id> (development, offline): every versus fight on that arena, to map arenas to music banks: the
// arena cycle (linkgame.cpp PickArena) held at the arena's place on the versus list while the front end is up
constexpr uint32_t kTestArenaList = 0x3E4D30, kTestArenaCycle = 0x620F44;

const bool g_installed = [] {
    mk3d::jukebox::Install({Arena});
    if (const char* t = std::getenv("MK3D_TEST_ARENA")) {
        static int wanted = std::atoi(t);
        mk3d::OnFrameStart([](uint64_t) {
            if (!InFrontEnd()) return;
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
}  // namespace mkd
