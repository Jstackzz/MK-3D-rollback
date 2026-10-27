// The engine's random numbers in a match in sync.
//
// Deadly Alliance has Deception's random generator (a Mersenne Twister variant: seed sub_000EC810, state 0x320A40 and
// index 0x320A28, draw sub_000ECA20, which only marks 0x2E8404 when the index wraps; eight call sites regenerate the
// table then, sub_000EC8F0) behind a few wrappers: sub_00130770 raw, sub_00130780(n) an integer below n, sub_00130830(n)
// one from -n to n, sub_00130850(x) a float from 0 to x, sub_001308B0(x) one from -x to x. It is drawn from about 330
// functions, ambient stage objects among them that start while the arena loads, before the two games meet, so every
// draw would move one stream that two games use differently.
//
// While a match is in sync, a draw is a hash of the agreed seed, the frame, the call site (where the outermost
// wrapper was called from) and how many times that site drew in the frame: what a fighter draws depends only on the
// fighter. The counts start again at every frame's top, so a frame run again after a rollback draws the same.
#include "game/rng.h"

#include <atomic>
#include <thread>
#include <unordered_map>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_000ECA20);
X86_EXTERN_FUNC(__imp__sub_00130770);
X86_EXTERN_FUNC(__imp__sub_00130780);
X86_EXTERN_FUNC(__imp__sub_00130830);
X86_EXTERN_FUNC(__imp__sub_00130850);
X86_EXTERN_FUNC(__imp__sub_001308B0);

namespace mkda::rng {
namespace {

std::atomic<bool> g_on{false};
uint32_t g_seed = 0, g_frame = 0;
std::unordered_map<uint32_t, uint32_t> g_counts;  // call site -> draws this frame
std::unordered_map<uint32_t, uint32_t> g_crt_counts;  // the C runtime rand's call sites -> draws this frame
std::thread::id g_main;                               // the thread the frames start on
thread_local int t_depth = 0;                     // wrappers entered on this thread
thread_local uint32_t t_site = 0;                 // where the outermost one was called from

uint64_t Mix(uint64_t z) {  // splitmix64's finaliser
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

void BeginFrame(uint32_t seed, uint32_t frame) {
    g_seed = seed;
    g_frame = frame;
    g_counts.clear();
    g_crt_counts.clear();
    g_main = std::this_thread::get_id();
    g_on = true;
}

void End() {
    g_on = false;
    g_counts.clear();
    g_crt_counts.clear();
}

bool CrtDraw(uint32_t site, uint32_t& out) {
    if (!g_on.load(std::memory_order_relaxed) || std::this_thread::get_id() != g_main) return false;
    uint32_t n = g_crt_counts[site]++;
    uint64_t key = (uint64_t(g_seed) << 32) ^ (uint64_t(g_frame) << 1) ^ 0x6372746Eull;  // "crtn": its own stream
    out = uint32_t(Mix(Mix(key) ^ (uint64_t(site) << 20) ^ n) >> 16);
    return true;
}

// a draw on the main thread in a match in sync, false otherwise (the engine's generator then)
bool Draw(uint32_t& out) {
    if (!g_on.load(std::memory_order_relaxed) || !t_depth) return false;
    uint32_t n = g_counts[t_site]++;
    uint64_t key = (uint64_t(g_seed) << 32) ^ (uint64_t(g_frame) << 1);
    out = uint32_t(Mix(Mix(key) ^ (uint64_t(t_site) << 20) ^ n));
    return true;
}

// a wrapper is entered: the outermost one names the call site
struct Site {
    explicit Site(uint32_t ret) {
        if (t_depth++ == 0) t_site = ret;
    }
    ~Site() { t_depth--; }
};

}  // namespace mkda::rng

X86_FUNC(sub_000ECA20) {
    uint32_t v;
    if (!mkda::rng::Draw(v)) return __imp__sub_000ECA20(ctx, base);
    ctx.eax = v;
    ctx.esp += 4;  // ret
}

// the wrappers: only to know where a draw comes from (the draw itself is 0xECA20 above)
X86_FUNC(sub_00130770) {
    mkda::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00130770(ctx, base);
}
X86_FUNC(sub_00130780) {
    mkda::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00130780(ctx, base);
}
X86_FUNC(sub_00130830) {
    mkda::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00130830(ctx, base);
}
X86_FUNC(sub_00130850) {
    mkda::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00130850(ctx, base);
}
X86_FUNC(sub_001308B0) {
    mkda::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_001308B0(ctx, base);
}
