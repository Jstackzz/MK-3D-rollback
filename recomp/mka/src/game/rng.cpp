// The engine's random numbers in a match in sync.
//
// The engine has one random generator (a Mersenne Twister: seed 0x1D6070, draw 0x1D6280, state 0x5BA708, index
// 0x5BB0C8, draws since its regeneration 0x5BB0CC) behind six wrappers (0x2591C0 raw, 0x2591D0 an integer below n,
// 0x259260 a signed integer, 0x259280 a float below x, 0x2592E0 a signed float, 0x259300 a chance), drawn from about
// a thousand places: hundreds of times a second in the menus and up to 5,700 times a second in a fight, in bursts
// at hits. Main seeds it from the time stamp counter at boot, and what each game drew in its menus differs, so the
// fighters' next random choice would come out differently on the two games.
//
// While a match is in sync, a draw on the main thread is a hash of the agreed seed, the frame, the call site (where
// the outermost wrapper was called from) and how many times that site drew in the frame: what a fighter draws
// depends only on the fighter. The counts start again at every frame's top, so a frame run again after a rollback
// draws the same. The counts live in a small fixed table (no allocation per draw).
//
// The C runtime's rand (0x39C6B8; srand 0x39C6AB) is counted for the netplay state checks (mk3d::net::CountRand);
// it was not called at all in a whole versus session.
#include "game/rng.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "mk3d/netplay.h"

X86_EXTERN_FUNC(__imp__sub_001D6280);
X86_EXTERN_FUNC(__imp__sub_002591C0);
X86_EXTERN_FUNC(__imp__sub_002591D0);
X86_EXTERN_FUNC(__imp__sub_00259260);
X86_EXTERN_FUNC(__imp__sub_00259280);
X86_EXTERN_FUNC(__imp__sub_002592E0);
X86_EXTERN_FUNC(__imp__sub_00259300);
X86_EXTERN_FUNC(__imp__sub_0039C6B8);

namespace mka::rng {
namespace {

std::atomic<bool> g_on{false};
std::atomic<DWORD> g_main_thread{0};
uint32_t g_seed = 0, g_frame = 0;
thread_local int t_depth = 0;      // wrappers entered on this thread
thread_local uint32_t t_site = 0;  // where the outermost one was called from

// call site -> draws this frame: open addressing, entries of an older frame count as empty
constexpr uint32_t kSlots = 2048;
struct Count {
    uint32_t site = 0, generation = 0, n = 0;
};
Count g_counts[kSlots];
uint32_t g_generation = 1;

uint64_t Mix(uint64_t z) {  // splitmix64's finaliser
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint32_t NextCount(uint32_t site) {
    uint32_t i = (site * 2654435761u) & (kSlots - 1);
    for (uint32_t probe = 0; probe < kSlots; probe++, i = (i + 1) & (kSlots - 1)) {
        Count& c = g_counts[i];
        if (c.generation != g_generation) {
            c = {site, g_generation, 1};
            return 0;
        }
        if (c.site == site) return c.n++;
    }
    return 0;  // a frame with more than kSlots sites: not seen
}

}  // namespace

void BeginFrame(uint32_t seed, uint32_t frame) {
    g_main_thread = GetCurrentThreadId();
    g_seed = seed;
    g_frame = frame;
    g_generation++;
    g_on = true;
}

void End() {
    g_on = false;
    g_generation++;
}

// a draw on the main thread in a match in sync, false otherwise (the engine's generator then)
bool Draw(uint32_t& out) {
    if (!g_on.load(std::memory_order_relaxed) || !t_depth || GetCurrentThreadId() != g_main_thread.load()) return false;
    uint32_t n = NextCount(t_site);
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

}  // namespace mka::rng

X86_FUNC(sub_001D6280) {
    uint32_t v;
    if (!mka::rng::Draw(v)) return __imp__sub_001D6280(ctx, base);
    ctx.eax = v;
    ctx.esp += 4;  // ret
}

// the wrappers: only to know where a draw comes from (the draw itself is 0x1D6280 above)
X86_FUNC(sub_002591C0) {
    mka::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_002591C0(ctx, base);
}
X86_FUNC(sub_002591D0) {
    mka::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_002591D0(ctx, base);
}
X86_FUNC(sub_00259260) {
    mka::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00259260(ctx, base);
}
X86_FUNC(sub_00259280) {
    mka::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00259280(ctx, base);
}
X86_FUNC(sub_002592E0) {
    mka::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_002592E0(ctx, base);
}
X86_FUNC(sub_00259300) {
    mka::rng::Site site(kernel::R32(ctx.esp));
    __imp__sub_00259300(ctx, base);
}

// the C runtime's rand: counted for the netplay state checks
X86_FUNC(sub_0039C6B8) {
    __imp__sub_0039C6B8(ctx, base);
    mk3d::net::CountRand(ctx.eax);
}
