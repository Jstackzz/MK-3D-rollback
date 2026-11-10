// Puzzle Kombat and Chess Kombat online: the screen's width, which each game would otherwise take from its own PC.
//
// The guest's viewport width (0x6212D4) is 720 on a screen 3:2 or wider, else 640 (app::Widescreen, sub_001C43D0).
// Puzzle Kombat's fighters at the top of the screen (a director, 0x181D30..0x18E3D0) take their walk limits and places
// from it, and Chess Kombat's sliding panels (0x105CA0..0x11D310) their stops, so a slide ends a few frames apart. A 4:3
// player and a wide one played one match by different numbers and their games parted. In a match in sync those
// functions see the width both games agreed on at the meeting point, the narrower one; two players with the same shape
// of screen see no change at all.
#include <algorithm>
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/events.h"
#include "game/agreed_width.h"
#include "kernel/kernel.h"
#include "mk3d/netplay.h"

namespace mkd::width {
namespace {

constexpr uint32_t kViewportWidth = 0x6212D4;

uint32_t g_agreed_width = 0;  // what the match in sync uses, 0 before the first one meets
bool g_standing_in = false;   // the agreed width is in the guest's word now

bool SideMode() {
    const uint32_t mode = GameMode();
    return mode == kModePuzzle || mode == kModeChess || mode == kModeChessBattle;
}

bool InSync() {
    const mk3d::net::Status st = mk3d::net::GetStatus();
    return st == mk3d::net::Status::Lockstep || st == mk3d::net::Status::Rollback;
}

}  // namespace

uint32_t ViewportWidth() { return kernel::R32(kViewportWidth); }

void AgreeWidth(uint32_t a, uint32_t b) {
    g_agreed_width = std::min(a, b);
    if (a != b) LOGR("netplay: the two screens differ (%u and %u wide): the match uses %u", a, b, g_agreed_width);
}

// the agreed width in the guest's word for one call of a director function; they call one another
class AgreedWidth {
public:
    AgreedWidth() {
        if (g_standing_in || !g_agreed_width || !SideMode() || !InSync()) return;
        own_ = kernel::R32(kViewportWidth);
        if (own_ == g_agreed_width) return;
        kernel::W32(kViewportWidth, g_agreed_width);
        g_standing_in = true;
    }
    ~AgreedWidth() {
        if (!own_ || !g_standing_in) return;
        kernel::W32(kViewportWidth, own_);
        g_standing_in = false;
    }
    AgreedWidth(const AgreedWidth&) = delete;
    AgreedWidth& operator=(const AgreedWidth&) = delete;

private:
    uint32_t own_ = 0;
};

}  // namespace mkd::width

#define AGREED_WIDTH(fn)                 \
    X86_EXTERN_FUNC(__imp__##fn);        \
    X86_FUNC(fn) {                       \
        mkd::width::AgreedWidth width;  \
        __imp__##fn(ctx, base);          \
    }

AGREED_WIDTH(sub_00181D30)
AGREED_WIDTH(sub_00181EB0)
AGREED_WIDTH(sub_001822B0)
AGREED_WIDTH(sub_00182B90)
AGREED_WIDTH(sub_00183C10)
AGREED_WIDTH(sub_00185140)
AGREED_WIDTH(sub_001858C0)
AGREED_WIDTH(sub_00185E30)
AGREED_WIDTH(sub_001869E0)
AGREED_WIDTH(sub_00186BF0)
AGREED_WIDTH(sub_00189B40)
AGREED_WIDTH(sub_0018ABF0)
AGREED_WIDTH(sub_0018B2A0)
AGREED_WIDTH(sub_0018B310)
AGREED_WIDTH(sub_0018E3D0)

// Chess Kombat's panels
AGREED_WIDTH(sub_00105CA0)
AGREED_WIDTH(sub_00105F90)
AGREED_WIDTH(sub_0010A870)
AGREED_WIDTH(sub_0010BDB0)
AGREED_WIDTH(sub_0010C910)
AGREED_WIDTH(sub_0010E200)
AGREED_WIDTH(sub_0010E6D0)
AGREED_WIDTH(sub_0010EA40)
AGREED_WIDTH(sub_0010F880)
AGREED_WIDTH(sub_00111180)
AGREED_WIDTH(sub_001135C0)
AGREED_WIDTH(sub_00113950)
AGREED_WIDTH(sub_00114020)
AGREED_WIDTH(sub_001190C0)
AGREED_WIDTH(sub_001198B0)
AGREED_WIDTH(sub_00119FA0)
AGREED_WIDTH(sub_0011C890)
AGREED_WIDTH(sub_0011C9F0)
AGREED_WIDTH(sub_0011D200)
AGREED_WIDTH(sub_0011D310)
AGREED_WIDTH(sub_00105270)
AGREED_WIDTH(sub_0010CE30)
AGREED_WIDTH(sub_00110D60)
AGREED_WIDTH(sub_00111A10)
AGREED_WIDTH(sub_00112220)
AGREED_WIDTH(sub_0011B040)
AGREED_WIDTH(sub_0011B320)
AGREED_WIDTH(sub_0011B4E0)
AGREED_WIDTH(sub_0011B580)
