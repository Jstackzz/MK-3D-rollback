// The deterministic guest clock for netplay and sync tests (runtime hook kernel::SetClockHook, patch 0008).
//
// The game reads time on its main thread (the online layer's timeouts every frame, the fight's start time, the
// engine's timers) and that must be a function of the simulated frame, or a replayed frame (rollback) or the other
// player's game (netplay) reads different values. While on, the main thread's clock is
//   base + frame * 1/60 s + 1 us per read within the frame
// which still moves inside a frame (so waits on time end) and repeats exactly when a frame is replayed. Other
// threads (CRI streaming, I/O, sound) keep real time: they pace themselves by it and are not part of the simulation.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/gclock.h"

#include <atomic>

#include "core/log.h"
#include "kernel/kernel.h"

namespace mk3d::gclock {
namespace {

std::atomic<bool> g_on{false};
DWORD g_main_tid = 0;
uint64_t g_base_ns = 0;
uint32_t g_base_frame = 0;
std::atomic<uint32_t> g_frame{0};
uint64_t g_reads = 0;  // main thread only

uint64_t Hook() {
    if (!g_on.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_main_tid) return kernel::RealNanoseconds();
    uint64_t frames = uint64_t(g_frame.load(std::memory_order_relaxed) - g_base_frame);
    return g_base_ns + frames * 1000000000ull / 60 + g_reads++ * 1000;
}

}  // namespace

void Start(uint32_t frame, uint64_t base_ns) {
    g_main_tid = GetCurrentThreadId();
    g_base_ns = base_ns ? base_ns : kernel::RealNanoseconds();
    g_base_frame = frame;
    g_frame = frame;
    g_reads = 0;
    g_on = true;
    kernel::SetClockHook(&Hook);
    kernel::PublishGuestTime();
    LOGI("gclock: frame time from frame %u, base %.3f s", frame, double(g_base_ns) / 1e9);
}

void SetFrame(uint32_t frame) {
    if (!g_on) return;
    g_frame = frame;
    g_reads = 0;
    kernel::PublishGuestTime();
}

void Stop() {
    if (!g_on) return;
    g_on = false;
    kernel::SetClockHook(nullptr);
    LOGI("gclock: back to real time");
}

bool On() { return g_on; }

}  // namespace mk3d::gclock
