// Sync test (<prefix>_SYNCTEST=1), shared by the games: from the first frame of a match every frame is run, rolled
// back to its top and run again with the same inputs; what it wrote must be the same both times. This proves the
// snapshot covers everything the match reads and that the game is deterministic, before any network. Bytes other
// threads own (rollback.h's volatile bytes: a loader thread's queues, what its reads fill) are not compared, and a
// page only one of the runs wrote must have ended as it began.
//
// <prefix>_SYNCTEST_FRAMES=<n> stops after n tested frames (default: the whole match, until the game says it is
// over); <prefix>_SYNCTEST_ALL=1 compares physical memory and the stack too.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "mk3d/game.h"
#include "mk3d/gclock.h"
#include "mk3d/netgame.h"
#include "mk3d/rollback.h"
#include "mk3d/tasks.h"
#include "xdk/dsound.h"

namespace mk3d::synctest {
namespace {

enum class Phase { Off, Wait, Normal, Replay, Done };
constexpr uint32_t kMixFrames = 48000 / 60;  // the sound of one frame
constexpr uint32_t kMaxBlock = 0x40;

Phase g_phase = Phase::Off;
bool g_setup = false;       // a match was set up in the frame that ran
uint32_t g_n = 0;           // the frame under test
uint8_t g_ports[2][kMaxBlock] = {};  // the frame's input blocks, recorded on the normal run
uint32_t g_tested = 0, g_bad = 0, g_limit = 0;

// the pages a run of the frame wrote, their contents at its end and at its top
struct Run {
    std::vector<uint32_t> pages;
    std::vector<uint8_t> end, top;
};
Run g_a;

// the pages the two runs left differently (volatile bytes aside), described when `explain`; how many
int Compare(uint32_t frame, const Run& a, const Run& b, bool explain) {
    constexpr uint32_t kPage = 0x1000;
    std::map<uint32_t, size_t> ia, ib;
    for (size_t i = 0; i < a.pages.size(); i++) ia[a.pages[i]] = i;
    for (size_t i = 0; i < b.pages.size(); i++) ib[b.pages[i]] = i;
    int bad = 0, shown = 0;
    auto check = [&](uint32_t page, const uint8_t* x, const uint8_t* y, const char* what) {
        static uint8_t mx[kPage], my[kPage];
        std::memcpy(mx, x, kPage);
        std::memcpy(my, y, kPage);
        rb::MaskVolatile(page, mx);
        rb::MaskVolatile(page, my);
        if (std::memcmp(mx, my, kPage) == 0) return;
        bad++;
        if (!explain || shown++ >= 12) return;
        int diffs = 0, first = -1;
        for (uint32_t o = 0; o < kPage; o += 4)
            if (std::memcmp(mx + o, my + o, 4)) {
                if (first < 0) first = int(o);
                diffs++;
            }
        uint32_t vx = 0, vy = 0;
        std::memcpy(&vx, mx + first, 4);
        std::memcpy(&vy, my + first, 4);
        LOGE("synctest:   frame %u: page %08X %s: %d dwords, first at %08X: %08X then %08X", frame, page, what, diffs,
             page + uint32_t(first), vx, vy);
    };
    for (auto& [page, i] : ia) {
        auto it = ib.find(page);
        if (it != ib.end())
            check(page, a.end.data() + i * kPage, b.end.data() + it->second * kPage, "differs");
        else  // the run again left it as it was at the top
            check(page, a.top.data() + i * kPage, a.end.data() + i * kPage, "changed only the first time");
    }
    for (auto& [page, i] : ib)
        if (!ia.count(page)) check(page, b.top.data() + i * kPage, b.end.data() + i * kPage, "changed only on the run again");
    return bad;
}

double g_close_sum = 0, g_restore_sum = 0;

void Report(const char* why) {
    rb::Stats st = rb::GetStats();
    LOGR("synctest: %s after %u frames: %u mismatches; %llu faults, %llu foreign pages, %llu KB logged, close %.3f ms, "
         "restore %.3f ms per frame on average, %d task fibers",
         why, g_tested, g_bad, (unsigned long long)st.faults, (unsigned long long)st.foreign_pages,
         (unsigned long long)(st.logged_bytes >> 10), g_tested ? g_close_sum / g_tested : 0.0,
         g_tested ? g_restore_sum / g_tested : 0.0, tasks::Alive());
}

void Stop(const char* why) {
    Report(why);
    rb::Disarm();
    net::game::RngEnd();
    dsound::SetFrameClock(false);
    gclock::Stop();
    g_phase = Phase::Done;
}

// a match is set up (netgame.h OnMatchSetup): the test starts at the next frame's top, once the loader is idle
void MatchSetup(X86Context&) {
    if (g_phase == Phase::Wait) g_setup = true;
}

void FrameStart(uint64_t) {
    if (g_phase == Phase::Off || g_phase == Phase::Done) return;
    X86Context* ctx = MainContext();
    if (!ctx) return;
    if (g_phase == Phase::Wait) {
        if (!g_setup) return;
        if (!net::game::LoaderSettle(*ctx, std::chrono::milliseconds(0))) return;  // requests in flight: once back
        g_n = 0;
        net::game::MatchBegins();
        net::game::RngFrame(0x5EED, g_n);
        gclock::Start(g_n);
        dsound::SetFrameClock(true);  // before arming: the output thread no longer touches the voices
        if (!rb::Arm(*ctx, g_n)) {
            LOGE("synctest: cannot arm");
            dsound::SetFrameClock(false);
            g_phase = Phase::Done;
            return;
        }
        dsound::MixFrames(kMixFrames, true);
        g_phase = Phase::Normal;
        return;
    }
    rb::CloseFrame();
    g_close_sum += rb::GetStats().close_ms;
    if (g_phase == Phase::Normal) {
        rb::LastFrameDetail(g_a.pages, g_a.end, &g_a.top);
        if (!rb::Restore(g_n, *ctx)) {
            Stop("stopped (restore failed)");
            return;
        }
        g_restore_sum += rb::GetStats().restore_ms;
        net::game::RngFrame(0x5EED, g_n);
        gclock::SetFrame(g_n);
        rb::OpenFrame(g_n, *ctx);
        dsound::MixFrames(kMixFrames, false);
        g_phase = Phase::Replay;
        return;
    }
    // the replay of frame g_n just ran
    static Run b;
    rb::LastFrameDetail(b.pages, b.end, &b.top);
    g_tested++;
    if (int pages = Compare(g_n, g_a, b, false)) {
        g_bad++;
        if (g_bad <= 20)
            LOGE("synctest: frame %u differs on replay: %d pages (the first run wrote %zu, the run again %zu)", g_n, pages,
                 g_a.pages.size(), b.pages.size());
        if (g_bad <= 8) Compare(g_n, g_a, b, true);
    }
    if (g_tested % 300 == 0) Report("progress");
    if ((g_limit && g_tested >= g_limit) || net::game::MatchOver()) {
        Stop("done");
        return;
    }
    g_n++;
    net::game::RngFrame(0x5EED, g_n);
    gclock::SetFrame(g_n);
    rb::OpenFrame(g_n, *ctx);
    dsound::MixFrames(kMixFrames, true);
    g_phase = Phase::Normal;
}

void PadsRead(uint64_t) {
    if (g_phase != Phase::Normal && g_phase != Phase::Replay) return;
    for (int p = 0; p < 2; p++) {
        uint32_t size = 0, va = net::game::PortBlock(p, size);
        size = std::min(size, kMaxBlock);
        for (uint32_t o = 0; o + 4 <= size; o += 4) {
            uint32_t v;
            if (g_phase == Phase::Normal) {
                v = kernel::R32(va + o);
                std::memcpy(g_ports[p] + o, &v, 4);
            } else {
                std::memcpy(&v, g_ports[p] + o, 4);
                kernel::W32(va + o, v);
            }
        }
    }
}

const bool g_registered = [] {
    const char* s = Env("SYNCTEST");
    if (!s || !std::atoi(s)) return false;
    if (const char* n = Env("SYNCTEST_FRAMES")) g_limit = uint32_t(std::atoi(n));
    g_phase = Phase::Wait;
    rb::SetDetail(true);
    // the simulation's state lives in user space (the game heap and globals); physical memory holds what the GPU and
    // the sound mixer read, written again every frame, and the main stack holds dead frames below esp
    if (!Env("SYNCTEST_ALL")) rb::SetHashRange(0x10000, 0x80000000);
    net::game::OnMatchSetup(&MatchSetup);
    OnFrameStart(&FrameStart);
    OnPadsRead(&PadsRead);
    return true;
}();

}  // namespace
}  // namespace mk3d::synctest
