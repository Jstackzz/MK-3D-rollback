// Deception's side of the rank badges (mk3d/ranks.h): beside the timer, standing on the health bars. Its fight HUD
// (sub_0017DAA0) keeps each bar's red back as a quad in the HUD's own space, 480 lines from the bottom up and 0x6212D4
// wide (720 for a 16:9 frame), kept at 0x471BA0 / 0x471B98 (names.cpp: x 65..334 and 386..655, y 417..431); the
// timer's horns reach 59 either side of the centre, their tips above the bars. The HUD draws the bars, then both
// fighters' portraits (kept at 0x5FD948 / 0x5FD940), then the breaker bolts, the timer and the bars' chrome: the badges
// go before the bars, so all of that goes over them where they meet. A sprite object holds its image at +0x10 (the D3D
// texture header at the image's +0x34). The bars grow in from the timer as a fight starts: the badges wait until they
// are out far enough, as the names do.
#include "mk3d/names.h"
#include "mk3d/ranks.h"

#include <cstdint>

#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mkd {
namespace {

constexpr mk3d::names::BarQuads kBars{{0x471BA0, 0x471B98}, 0x6212D4, 18.0f, 6.0f, 66.0f};  // as names.cpp's
constexpr uint32_t kPortrait[2] = {0x5FD948, 0x5FD940};
constexpr float kGrown = 40.0f;   // frame pixels of a bar out from the timer before the badges show (as the names)
constexpr float kClear = 66.0f;   // the badges' inner edges either side of the centre: past the timer's horns
constexpr float kTuck = 2.0f;     // lines of a badge's foot under the bars' frame
constexpr float kHeight = 46.0f;  // the badges' box: the room between the bars and the top of the screen

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
    if (bars.p1_end - bars.p1_start < kGrown || bars.p2_end - bars.p2_start < kGrown) return false;  // growing in
    const float width = float(kernel::R32(kBars.hud_width));
    out.hud_width = width;
    out.inner[0] = width / 2 - kClear;
    out.inner[1] = width / 2 + kClear;
    out.bottom = bars.y + kBars.above + kTuck;  // the bars' top edge: the names' line is above it
    out.height = kHeight;
    for (int side = 0; side < 2; side++) {
        out.anchor[side] = Texture(Object(kBars.back[side]));
        out.anchor[2 + side] = Texture(Object(kPortrait[side]));
    }
    return out.anchor[0] != 0;
}

const bool g_installed = [] {
    mk3d::ranks::Install({Place});
    return true;
}();

}  // namespace
}  // namespace mkd
