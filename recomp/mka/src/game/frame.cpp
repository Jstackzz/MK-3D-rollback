// Frame hooks. The main loop (continuation 0x19C863 of main 0x19C680) counts its frame counters, then calls
// sub_001B6750 first in every iteration (it counts down a list of delayed calls), then the pad update sub_0026A9C0;
// both are called from other places too (sub_001B6750 from 0x7D33F and itself, the pad update from movie and loading
// loops), so the main loop's calls are told apart by their return addresses.
#include "game/frame.h"

#include <atomic>
#include <vector>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_001B6750);
X86_EXTERN_FUNC(__imp__sub_0026A9C0);

namespace mka {
namespace {

constexpr uint32_t kMainFrameCall = 0x19C8B1;  // after "call 0x1B6750" at 0x19C8AC
constexpr uint32_t kMainPadsCall = 0x19C8B6;   // after "call 0x26A9C0" at 0x19C8B1

std::atomic<uint64_t> g_frame{0};
X86Context* g_main_ctx = nullptr;

}  // namespace
}  // namespace mka

namespace mk3d {
namespace {

using mka::g_frame;
using mka::g_main_ctx;

std::vector<FrameListener>& StartListeners() {
    static std::vector<FrameListener> v;
    return v;
}
std::vector<FrameListener>& BeforePadListeners() {
    static std::vector<FrameListener> v;
    return v;
}
std::vector<FrameListener>& PadListeners() {
    static std::vector<FrameListener> v;
    return v;
}

}  // namespace

uint64_t Frame() { return g_frame.load(std::memory_order_relaxed); }
X86Context* MainContext() { return g_main_ctx; }
void OnFrameStart(FrameListener f) { StartListeners().push_back(f); }
void OnBeforePads(FrameListener f) { BeforePadListeners().push_back(f); }
void OnPadsRead(FrameListener f) { PadListeners().push_back(f); }

void RunFrameStart() {
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    for (FrameListener l : StartListeners()) l(f);
}
void RunBeforePads() {
    uint64_t f = Frame();
    for (FrameListener l : BeforePadListeners()) l(f);
}
void RunPadsRead() {
    uint64_t f = Frame();
    for (FrameListener l : PadListeners()) l(f);
}

}  // namespace mk3d

namespace mka {
namespace {
// the main context for the hooks below, cleared on the way out even when the engine longjmps through them
struct MainScope {
    explicit MainScope(X86Context& ctx) { g_main_ctx = &ctx; }
    ~MainScope() { g_main_ctx = nullptr; }
};
}  // namespace
}  // namespace mka

X86_FUNC(sub_001B6750) {
    if (kernel::R32(ctx.esp) == mka::kMainFrameCall) {
        mka::MainScope scope(ctx);
        mk3d::RunFrameStart();
    }
    __imp__sub_001B6750(ctx, base);
}

X86_FUNC(sub_0026A9C0) {
    uint32_t ret = kernel::R32(ctx.esp);
    if (ret == mka::kMainPadsCall) {
        mka::MainScope scope(ctx);
        mk3d::RunBeforePads();
    }
    __imp__sub_0026A9C0(ctx, base);
    if (ret == mka::kMainPadsCall) {
        mka::MainScope scope(ctx);
        mk3d::RunPadsRead();
    }
}
