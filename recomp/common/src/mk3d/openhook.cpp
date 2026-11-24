// Several open hooks over the runtime's one (openhook.h).
#include "mk3d/openhook.h"

#include <atomic>

#include "core/log.h"
#include "kernel/kernel.h"

namespace mk3d {
namespace {

constexpr int kMax = 8;
OpenHook g_hooks[kMax] = {};
std::atomic<int> g_count{0};

bool Dispatch(const std::string& xbox_upper, std::string& host) {
    const int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_hooks[i] && g_hooks[i](xbox_upper, host)) return true;
    return false;
}

}  // namespace

void AddOpenHook(OpenHook hook) {
    const int n = g_count.load(std::memory_order_relaxed);
    if (n >= kMax) {
        LOGE("open hooks: no room for another");
        return;
    }
    g_hooks[n] = hook;
    g_count.store(n + 1, std::memory_order_release);
    if (n == 0) kernel::SetOpenHook(Dispatch);
}

}  // namespace mk3d
