// Armageddon's side of the rank badges (mk3d/ranks.h): beside the timer, standing on the health bars. Its fight HUD
// (sub_0020B820) keeps each bar's red back (BBAR_RED) as a quad in the HUD's own space, 480 lines from the bottom up and
// 0x831E1C wide (720 for a 16:9 frame), kept at 0x5BDA00 / 0x5BD9F8 (names.cpp: x 77..336 and 384..643, y 409..427);
// the timer's ring reaches 26 either side of the centre. The HUD draws the breaker bolts under the bars, then the bars,
// the timer and the bars' chrome: the badges go before the bars' backs, so all of that goes over them where they meet.
// A sprite object holds its image at +0x10 (the D3D texture header at the image's +0x34). The bars grow in from the
// timer as a fight starts: the badges wait until they are out far enough, as the names do.
#include "mk3d/names.h"
#include "mk3d/ranks.h"

#include <cstdint>

#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mka {
namespace {

constexpr mk3d::names::BarQuads kBars{{0x5BDA00, 0x5BD9F8}, 0x831E1C, 7.0f, 6.0f, 34.0f};  // as names.cpp's
constexpr float kGrown = 40.0f;   // frame pixels of a bar out from the timer before the badges show
constexpr float kClear = 32.0f;   // the badges' inner edges either side of the centre: the timer's ring (26) and a gap
constexpr float kTuck = 2.0f;     // lines of a badge's foot under the bars' frame
constexpr float kHeight = 48.0f;  // the badges' box: the room between the bars and the top of the screen

// an object kept at a slot, still the one listed there; 0 for none
uint32_t Object(uint32_t slot) {
    const uint32_t obj = kernel::R32(slot);
    if (!obj || !mem::IsCommitted(obj) || !mem::IsCommitted(obj + 0x38) || kernel::R32(obj + 4) != kernel::R32(slot + 4))
        return 0;
    return obj;
}

// a sprite object's texture header, 0 for none
uint32_t Texture(uint32_t obj) {
    const uint32_t image = obj ? kernel::R32(obj + 0x10) : 0;
    if (!image || !mem::IsCommitted(image) || !mem::IsCommitted(image + 0x38)) return 0;
    return kernel::R32(image + 0x34);
}

bool Place(mk3d::ranks::Place& out) {
    mk3d::names::Place bars;
    if (InFrontEnd() || !mk3d::names::PlaceAboveBars(kBars, bars)) return false;
    if (bars.p1_end - bars.p1_start < kGrown || bars.p2_end - bars.p2_start < kGrown) return false;
    const float width = float(kernel::R32(kBars.hud_width));
    out.hud_width = width;
    out.inner[0] = width / 2 - kClear;
    out.inner[1] = width / 2 + kClear;
    out.bottom = bars.y + kBars.above + kTuck;  // the bars' top edge: the names' line is above it
    out.height = kHeight;
    for (int side = 0; side < 2; side++) out.anchor[side] = Texture(Object(kBars.back[side]));
    return out.anchor[0] != 0;
}

const bool g_installed = [] {
    mk3d::ranks::Install({Place});
    return true;
}();

}  // namespace
}  // namespace mka
