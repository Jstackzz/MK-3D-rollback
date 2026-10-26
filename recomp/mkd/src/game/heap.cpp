// The engine's heap blocks come back zeroed on the main thread.
//
// Every allocation goes through sub_001DD3B0(heap, size, alignment, name, ...), and a new block holds whatever the
// heap last kept there. What the engine leaves uninitialised in a block (padding, fields set later or never) then
// depends on everything this game did before, which differs between two games about to play the same match; netplay's
// state checks hash such blocks (Chess Kombat's board, Puzzle Kombat's wells), and a stale value read by mistake
// would part the two games. Zeroed, a new block is the same in both. Other threads (sound, streaming) are left alone.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>

#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_001DD3B0);

namespace mkd::heap {
namespace {

std::atomic<DWORD> g_main_thread{0};

void FrameStart(uint64_t) {
    if (!g_main_thread.load(std::memory_order_relaxed)) g_main_thread = GetCurrentThreadId();
}

const bool g_registered = [] {
    OnFrameStart(&FrameStart);
    return true;
}();

}  // namespace

bool OnMainThread() { return GetCurrentThreadId() == g_main_thread.load(std::memory_order_relaxed); }

}  // namespace mkd::heap

X86_FUNC(sub_001DD3B0) {
    uint32_t size = kernel::R32(ctx.esp + 8);
    __imp__sub_001DD3B0(ctx, base);
    uint32_t block = ctx.eax;
    if (block && size && size < (64u << 20) && mkd::heap::OnMainThread() && mem::IsCommitted(block) &&
        mem::IsCommitted(block + size - 1))
        std::memset(kernel::HostPtr(block), 0, size);
}
