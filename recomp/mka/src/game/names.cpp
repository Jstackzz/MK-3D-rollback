// Armageddon's side of the players' names (mk3d/names.h): its fight HUD (sub_0020B820 makes it, as Deception's) keeps
// each health bar's red back (BBAR_RED) as a quad in the HUD's own space, 480 lines from the bottom up and 0x831E1C wide
// (720 for a 16:9 frame): x 77..336 and 384..643, y 409..427 there, the fighter's name lettered in its outer half (0x24
// from the margin 0x562358). Its screen fades are FADE_BOX sprites among the objects listed at 0x830900 (sub_0009DC50
// goes through them), the alpha the top byte of each corner's colour (sub_0025B050).
#include "mk3d/names.h"

#include "game/events.h"

namespace mka {
namespace {

// the timer's ring covers the bars' inner ends, 26 either side of the centre
constexpr mk3d::names::BarQuads kBars{{0x5BDA00, 0x5BD9F8}, 0x831E1C, 34.0f};
constexpr mk3d::names::FadeBoxes kFades{{0x830900, 0}, 0x28, 0x34, 0x13};

bool Place(mk3d::names::Place& out) {
    if (InFrontEnd() || !mk3d::names::PlaceInBars(kBars, out)) return false;
    out.shade = mk3d::names::ShadeUnder(kFades);
    return true;
}

const bool g_installed = [] {
    mk3d::names::Install({Place});
    return true;
}();

}  // namespace
}  // namespace mka
