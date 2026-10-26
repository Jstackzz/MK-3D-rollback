// The engine's heap blocks come back zeroed on the main thread.
//
// Every engine allocation goes through sub_001550B0(heap, size, alignment, flags, file, line) (the named heaps of
// sub_001550E0; RenderWare's own allocations come through it too, from heap [0x31FA44]), and a new block holds whatever
// the heap last kept there. What the engine leaves uninitialised in a block (padding, fields set later or never) then
// depends on everything this game did before, which differs between two games about to play the same match; a stale
// value read by mistake would part them. Zeroed, a new block is the same in both. Other threads are left alone.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>

#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_001550B0);

namespace mkda::heap {
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

}  // namespace mkda::heap

X86_FUNC(sub_001550B0) {
    uint32_t size = kernel::R32(ctx.esp + 8);
    __imp__sub_001550B0(ctx, base);
    uint32_t block = ctx.eax;
    if (block && size && size < (64u << 20) && mkda::heap::OnMainThread() && mem::IsCommitted(block) &&
        mem::IsCommitted(block + size - 1))
        std::memset(kernel::HostPtr(block), 0, size);
}
