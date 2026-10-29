// Deception's side of the players' names (mk3d/names.h): its fight HUD (sub_0017DAA0 makes it, sub_0017E5E0 deletes
// it, sub_0017F610 slides it off the top in 20 frames) keeps each health bar's red back as a quad in the HUD's own space,
// 480 lines from the bottom up and 0x6212D4 wide (720 for a 16:9 frame): x 65..334 and 386..655, y 417..431 there. Its
// screen fades are WHITE_FADEBOX sprites among the objects listed at 0x620300 (sub_00087810 goes through them), the
// alpha the top byte of each corner's colour (sub_001BD9E0).
#include "mk3d/names.h"

#include "game/events.h"

namespace mkd {
namespace {

// the fighter's name starts 18 in from the bar's outer end (0x23 from the margin 0x3F9828); the names' line clears the
// bar's chrome edge above it; the timer's horns reach 59 either side of the centre
constexpr mk3d::names::BarQuads kBars{{0x471BA0, 0x471B98}, 0x6212D4, 18.0f, 6.0f, 66.0f};
constexpr mk3d::names::FadeBoxes kFades{{0x620300, 0}, 0x28, 0x34, 0x13};

bool Place(mk3d::names::Place& out) {
    if (InFrontEnd() || !mk3d::names::PlaceAboveBars(kBars, out)) return false;
    out.shade = mk3d::names::ShadeUnder(kFades);
    return true;
}

const bool g_installed = [] {
    mk3d::names::Install({Place});
    return true;
}();

}  // namespace
}  // namespace mkd
