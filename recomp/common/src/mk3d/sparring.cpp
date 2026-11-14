// Online Practice's health rules (sparring.h).
#include "mk3d/sparring.h"

#include <atomic>
#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/game.h"
#include "mk3d/netplay.h"

namespace mk3d::sparring {
namespace {

std::atomic<bool> g_on{false};

// below this at a frame's top a fighter is full again at once: no hit takes more in one frame, so nobody is knocked out
constexpr float kFloor = 0.3f;
// frames a fighter's health stays the same before it is full again (a combo's hits come sooner than this)
constexpr uint32_t kRefillAfter = 90;
constexpr uint32_t kOne = 0x3F800000;  // 1.0f

// guest memory: per side, the health last seen and the frames since it last changed
constexpr uint32_t kLastOf = 0, kStillOf = 8, kBytes = 16;
uint32_t g_mem = 0;
uint32_t g_logged = 0;

float AsFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// taken at the first frame after boot, the same moment on both games of a match, so the game's own allocations land
// where they would on the other game
const bool g_registered = (OnFrameStart([](uint64_t) {
                               if (g_mem) return;
                               g_mem = mem::AllocContiguous(kBytes, 0, 0xFFFFFFFF, 16);
                               if (g_mem)
                                   for (uint32_t i = 0; i < kBytes; i += 4) kernel::W32(g_mem + i, 0);
                           }),
                           true);

}  // namespace

void Set(bool on) {
    if (g_on.exchange(on) != on) LOGR("sparring: Online Practice %s", on ? "on" : "off");
}

bool On() { return g_on.load(); }

void KeepHealth(const uint32_t health[2], bool fighting) {
    if (!g_on.load() || !g_mem) return;
    const net::Status st = net::GetStatus();
    if (st != net::Status::Lockstep && st != net::Status::Rollback) return;
    for (int s = 0; s < 2; s++) {
        const uint32_t last = g_mem + kLastOf + 4 * uint32_t(s), still = g_mem + kStillOf + 4 * uint32_t(s);
        const uint32_t bits = kernel::R32(health[s]);
        const float h = AsFloat(bits);
        if (!fighting || !(h > 0.0f && h < 1.0f)) {  // full, down for good, or no round under way
            kernel::W32(last, bits);
            kernel::W32(still, 0);
            continue;
        }
        const uint32_t frames = bits == kernel::R32(last) ? kernel::R32(still) + 1 : 0;
        if (h < kFloor || frames >= kRefillAfter) {
            kernel::W32(health[s], kOne);
            kernel::W32(last, kOne);
            kernel::W32(still, 0);
            if (g_logged < 40 && ++g_logged)
                LOGR("sparring: P%d's health %.3f back to full (%s)", s + 1, h, h < kFloor ? "below the floor" : "no damage for a while");
        } else {
            kernel::W32(last, bits);
            kernel::W32(still, frames);
        }
    }
}

}  // namespace mk3d::sparring
