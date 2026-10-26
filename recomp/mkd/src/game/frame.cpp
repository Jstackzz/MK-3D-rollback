// Frame hooks. The main loop (continuation 0xDFFBF of main) calls sub_00130390 first in every iteration, then the
// pad update sub_001C52F0; both are called from other places too (a movie loop polls the pads), so the main loop's
// calls are told apart by their return addresses.
#include "game/frame.h"

#include <atomic>
#include <vector>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00130390);
X86_EXTERN_FUNC(__imp__sub_001C52F0);

namespace mkd {
namespace {

constexpr uint32_t kMainFrameCall = 0xE000C;  // after "call 0x130390" at 0xE0007
constexpr uint32_t kMainPadsCall = 0xE0011;   // after "call 0x1C52F0" at 0xE000C

std::atomic<uint64_t> g_frame{0};
X86Context* g_main_ctx = nullptr;

}  // namespace
}  // namespace mkd

namespace mk3d {
namespace {

using mkd::g_frame;
using mkd::g_main_ctx;

std::vector<FrameListener>& StartListeners() {
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
void OnPadsRead(FrameListener f) { PadListeners().push_back(f); }

void RunFrameStart() {
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    for (FrameListener l : StartListeners()) l(f);
}
void RunPadsRead() {
    uint64_t f = Frame();
    for (FrameListener l : PadListeners()) l(f);
}

}  // namespace mk3d

namespace mkd {
namespace {
// the main context for the hooks below, cleared on the way out even when the engine longjmps through them
struct MainScope {
    explicit MainScope(X86Context& ctx) { g_main_ctx = &ctx; }
    ~MainScope() { g_main_ctx = nullptr; }
};
}  // namespace
}  // namespace mkd

X86_FUNC(sub_00130390) {
    if (kernel::R32(ctx.esp) == mkd::kMainFrameCall) {
        mkd::MainScope scope(ctx);
        mk3d::RunFrameStart();
    }
    __imp__sub_00130390(ctx, base);
}

X86_FUNC(sub_001C52F0) {
    uint32_t ret = kernel::R32(ctx.esp);
    __imp__sub_001C52F0(ctx, base);
    if (ret == mkd::kMainPadsCall) {
        mkd::MainScope scope(ctx);
        mk3d::RunPadsRead();
    }
}
