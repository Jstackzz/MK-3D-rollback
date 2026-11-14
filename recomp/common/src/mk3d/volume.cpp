// The launcher's sound levels (volume.h).
#include "mk3d/volume.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "audio/audio.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/game.h"

namespace mk3d::volume {
namespace {

constexpr int kMaxPercent = 150;
constexpr float kWhisper = 0.0005f;  // 0 %: as quiet as can be without being 0
const char* const kNames[kLevels] = {"master", "music", "environment", "announcer", "effects", "voices"};

std::atomic<int> g_percent[kLevels] = {100, 100, 100, 100, 100, 100};
uint32_t g_va = 0;
float g_base = 1.0f;

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// the categories' floats as the levels want them, at the top of every frame (main thread)
void EachFrame(uint64_t) {
    if (!g_va || !mem::IsCommitted(g_va) || !mem::IsCommitted(g_va + 19)) return;
    for (int i = 0; i < 5; i++) {
        const float level = std::clamp(g_base * float(g_percent[kMusic + i].load()) / 100.0f, kWhisper, 1.0f);
        const uint32_t va = g_va + 4 * uint32_t(i);
        if (kernel::R32(va) != Bits(level)) kernel::W32(va, Bits(level));
    }
}

}  // namespace

const char* Name(Level level) { return level >= 0 && level < kLevels ? kNames[level] : ""; }

void Set(Level level, int percent) {
    if (level < 0 || level >= kLevels) return;
    percent = std::clamp(percent, 0, kMaxPercent);
    if (g_percent[level].exchange(percent) == percent) return;
    LOGR("volume: %s %d %%", kNames[level], percent);
    if (level == kMaster) audio::SetMasterVolume(float(percent) / 100.0f);
}

void Install(uint32_t va, float base) {
    g_va = va;
    g_base = base;
    OnFrameStart(&EachFrame);
}

}  // namespace mk3d::volume
