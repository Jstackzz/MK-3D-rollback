// Deadly Alliance's side of the rank badges (mk3d/ranks.h): beside the timer, standing on the health bars. Its fight HUD
// (sub_00116940) puts each bar's chrome as a 256x64 sprite in the HUD's own space, 480 lines from the bottom up and
// 0x42CAA0 wide (640): P1's at (32, 393), P2's at (352, 393), kept at 0x2EA880 / 0x2EA878 with their ids 4 bytes on
// (names.cpp); the bars' top edge is 44 lines above the sprites' base, and the timer's ring between them reaches 25
// either side of the centre, less above the bars. The HUD draws the timer, then both fighters' portraits (64x32, kept at
// 0x3200D8 / 0x3200D0), then the bars' chrome, their fill and the names: the badges go before the first portrait (or
// the chrome), so the bars' top edge goes over their foot. A sprite object holds its image at +0x10 (the D3D texture
// header at the image's +0x34) and its place at +0x14 / +0x18.
#include "mk3d/ranks.h"

#include <algorithm>
#include <cstdint>

#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mkda {
namespace {

constexpr uint32_t kChrome[2] = {0x2EA880, 0x2EA878};    // P1's, P2's bar chrome: the object, its id 4 bytes on
constexpr uint32_t kPortrait[2] = {0x3200D8, 0x3200D0};  // P1's, P2's portrait
constexpr uint32_t kHudWidth = 0x42CAA0;
constexpr float kAbove = 44.0f;   // the bars' top edge over the chrome sprites' base
constexpr float kClear = 31.0f;   // the badges' inner edges either side of the centre: the timer's ring (25) and a gap
constexpr float kTuck = 2.0f;     // lines of a badge's foot under the bars' top edge
constexpr float kHeight = 40.0f;  // the badges' box: the room between the bars and the top of the screen

// an object kept at a slot, still the one listed there; 0 for none
uint32_t Object(uint32_t slot) {
    const uint32_t obj = kernel::R32(slot);
    if (!obj || !mem::IsCommitted(obj) || !mem::IsCommitted(obj + 0x34) || kernel::R32(obj + 4) != kernel::R32(slot + 4))
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
    if (InFrontEnd()) return false;
    const float width = float(kernel::R32(kHudWidth));
    if (width < 320 || width > 2048) return false;
    float line = -1.0e9f;
    for (int side = 0; side < 2; side++) {
        const uint32_t obj = Object(kChrome[side]);
        if (!obj) return false;
        line = std::max(line, float(int32_t(kernel::R32(obj + 0x18))) + kAbove);
        out.anchor[side] = Texture(Object(kPortrait[side]));
    }
    out.anchor[2] = Texture(Object(kChrome[0]));
    out.hud_width = width;
    out.inner[0] = width / 2 - kClear;
    out.inner[1] = width / 2 + kClear;
    out.bottom = 480.0f - line + kTuck;
    out.height = kHeight;
    return out.anchor[2] && out.bottom > 0;  // slid off the top with the HUD
}

const bool g_installed = [] {
    mk3d::ranks::Install({Place});
    return true;
}();

}  // namespace
}  // namespace mkda
