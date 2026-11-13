// Stages: Acid Bath's acid never sprays (the user, 2026-11-12: "make it so the acid pool stage never shoots acid").
//
// The stage's setup (arena 0's function, sub_00024FF0, from the arena table at 0x231750 +0x20) starts its process
// sub_000C8F20, which watches four spots: a table of 0x2C-byte records at 0x268B50 (+0x00 where the spray rises,
// +0x0C and +0x18 two points that set it off, +0x28 that spot's spray running), one in each corner. A fighter within
// 1.5 of a point fires the spray there (sub_000C9240: the spray's own process sub_000C93B0, its sounds, the running
// flags 0x42A93C and 0x42A938), and while it runs a fighter within 3 of it loses health every frame (sub_00117350 /
// sub_001173E0). The setup hook moves those points far off the stage, so nothing ever sets the spray off; the process
// still runs and does what it does with no spray (it keeps both fighters' "in the acid" state at 0), and the stage's
// other sounds and effects are as they are. Both games of a match do the same, so the simulation stays the same.
//
// MK3D_TEST_ACID (development, offline): "1" keeps player 1 on spot 0's first trigger point during an Acid Bath fight
// and logs the acid's running flags once a second; "keep" does the same with the trigger points left where the game
// has them (the acid as it was), to compare.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/game.h"

X86_EXTERN_FUNC(__imp__sub_00024FF0);

namespace {

constexpr uint32_t kAcidSpots = 0x268B50, kSpotSize = 0x2C;
constexpr uint32_t kTriggerA = 0x0C, kTriggerB = 0x18;  // x, y, z floats each
constexpr int kSpotCount = 4;
constexpr float kFarAway = 1.0e7f;  // the reach the process checks is 1.5
constexpr uint32_t kAcidRunning = 0x42A93C, kAcidRunningToo = 0x42A938;
constexpr uint32_t kFightOn = 0x42C744;                   // the fight is on (fighters may block)
constexpr uint32_t kBody[2] = {0x2EA538, 0x2EA53C};       // what the acid process measures (+0x84 x, +0x8C z)
constexpr int kAcidBath = 0;

const char* const g_test = std::getenv("MK3D_TEST_ACID");
uint32_t g_trigger0[3];  // spot 0's first point as the game has it

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

const bool g_test_installed = [] {
    if (!g_test) return false;
    mk3d::OnFrameStart([](uint64_t frame) {
        if (int(kernel::R32(mkda::kArena)) != kAcidBath || !kernel::R32(kFightOn) || !g_trigger0[0]) return;
        const uint32_t body = kernel::R32(kBody[0]);
        if (!body) return;
        kernel::W32(body + 0x84, g_trigger0[0]);
        kernel::W32(body + 0x8C, g_trigger0[2]);
        if (frame % 60 == 0)
            LOGR("stages: test: player 1 on the acid's trigger, acid running %u %u", kernel::R32(kAcidRunning),
                 kernel::R32(kAcidRunningToo));
    });
    return true;
}();

}  // namespace

// Acid Bath's setup: the stage as the game makes it, then its acid spots' triggers out of every fighter's reach
X86_FUNC(sub_00024FF0) {
    __imp__sub_00024FF0(ctx, base);
    if (!g_trigger0[0] && !g_trigger0[2])
        for (int k = 0; k < 3; k++) g_trigger0[k] = kernel::R32(kAcidSpots + kTriggerA + 4 * uint32_t(k));
    if (g_test && std::strcmp(g_test, "keep") == 0) {
        LOGR("stages: test: Acid Bath's acid as the game has it");
        return;
    }
    for (int i = 0; i < kSpotCount; i++)
        for (uint32_t point : {kTriggerA, kTriggerB}) {
            const uint32_t at = kAcidSpots + kSpotSize * uint32_t(i) + point;
            kernel::W32(at, Bits(kFarAway));      // x
            kernel::W32(at + 8, Bits(kFarAway));  // z
        }
    LOGR_FIRST(1, "stages: Acid Bath's acid spots are out of reach: the acid never sprays");
}
