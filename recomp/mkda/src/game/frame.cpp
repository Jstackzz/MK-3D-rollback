// Frame hooks. The main loop (continuation 0xCAD3E of main 0xCABF0) counts its frames (0x42A888 always, 0x42A8A0
// while 0x321424 is clear), then calls sub_000D7F80 (deferred frees) first in every iteration and the pad update
// sub_00143030 a little later; both are also called from other places (sub_000D7FF0, and the movie loop's skip check
// sub_00132EA0 polls the pads), so the main loop's calls are told apart by their return addresses.
#include "game/frame.h"

#include <atomic>
#include <vector>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_000D7F80);
X86_EXTERN_FUNC(__imp__sub_00143030);

namespace mkda {
namespace {

constexpr uint32_t kMainFrameCall = 0xCAD9A;  // after "call 0xD7F80" at 0xCAD95
constexpr uint32_t kMainPadsCall = 0xCADF1;   // after "call 0x143030" at 0xCADEC

std::atomic<uint64_t> g_frame{0};
X86Context* g_main_ctx = nullptr;

// the main context for the hooks below, cleared on the way out even when the engine longjmps through them
struct MainScope {
    explicit MainScope(X86Context& ctx) { g_main_ctx = &ctx; }
    ~MainScope() { g_main_ctx = nullptr; }
};

}  // namespace
}  // namespace mkda

namespace mk3d {
namespace {

using mkda::g_frame;
using mkda::g_main_ctx;

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

X86_FUNC(sub_000D7F80) {
    if (kernel::R32(ctx.esp) == mkda::kMainFrameCall) {
        mkda::MainScope scope(ctx);
        mk3d::RunFrameStart();
    }
    __imp__sub_000D7F80(ctx, base);
}

X86_FUNC(sub_00143030) {
    uint32_t ret = kernel::R32(ctx.esp);
    if (ret == mkda::kMainPadsCall) {
        mkda::MainScope scope(ctx);
        mk3d::RunBeforePads();
    }
    __imp__sub_00143030(ctx, base);
    if (ret == mkda::kMainPadsCall) {
        mkda::MainScope scope(ctx);
        mk3d::RunPadsRead();
    }
}
