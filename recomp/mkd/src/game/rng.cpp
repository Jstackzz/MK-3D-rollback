// The engine's random numbers in a fight in sync.
//
// The engine has one random generator (a Mersenne Twister variant: seed 0x14BDB0, draw 0x14BFB0, state 0x5FE9F4 and
// 0x5FEA00) behind a few wrappers (0x1BC190 raw, 0x1BC1A0 an integer below n, 0x1BC260 a float, 0x1BC2D0 a signed
// float), drawn from about 300 places: the fighters, and ambient stage objects such as the creatures that wander and
// flee from the fighters (0x38EA0, 0x39170, 0x39830). Those start running while the stage loads, before the two
// games meet, so their states differ between the games and so does how often they draw; every draw moves the one
// stream, and the fighters' next random choice (a knockout reaction, say) came out differently on the two games.
//
// While a fight is in sync, a draw is a hash of the agreed seed, the frame, the call site (where the outermost
// wrapper was called from) and how many times that site drew in the frame: what a fighter draws depends only on the
// fighter. The counts start again at every frame's top, so a frame run again after a rollback draws the same.
#include "game/rng.h"

#include <atomic>
#include <unordered_map>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_0014BFB0);
X86_EXTERN_FUNC(__imp__sub_001BC190);
X86_EXTERN_FUNC(__imp__sub_001BC1A0);
X86_EXTERN_FUNC(__imp__sub_001BC260);
X86_EXTERN_FUNC(__imp__sub_001BC2D0);

namespace mkd::rng {
namespace {

std::atomic<bool> g_on{false};
uint32_t g_seed = 0, g_frame = 0;
std::unordered_map<uint32_t, uint32_t> g_counts;  // call site -> draws this frame
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
    g_on = true;
}

void End() {
    g_on = false;
    g_counts.clear();
}

// a draw on the main thread in a fight in sync, false otherwise (the engine's generator then)
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

}  // namespace mkd::rng

X86_FUNC(sub_0014BFB0) {
    uint32_t v;
    if (!mkd::rng::Draw(v)) return __imp__sub_0014BFB0(ctx, base);
    ctx.eax = v;
    ctx.esp += 4;  // ret
}

// the wrappers: only to know where a draw comes from (the draw itself is 0x14BFB0 above)
X86_FUNC(sub_001BC190) {
    mkd::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_001BC190(ctx, base);
}
X86_FUNC(sub_001BC1A0) {
    mkd::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_001BC1A0(ctx, base);
}
X86_FUNC(sub_001BC260) {
    mkd::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_001BC260(ctx, base);
}
X86_FUNC(sub_001BC2D0) {
    mkd::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_001BC2D0(ctx, base);
}
