// Deadly Alliance's side of the players' names (mk3d/names.h): its fight HUD (sub_00116940 makes it, as Deception's
// later one) puts each health bar's chrome, the bar's frame with its blade ends, as a 256x64 sprite in the HUD's own space,
// 480 lines from the bottom up and 0x42CAA0 wide (640): P1's at (32, 393), P2's at (352, 393), each object's position
// at +0x14 / +0x18 and its image (+0x54 its width) at +0x30. The bars themselves run from 20 in from the sprites' outer
// ends to under the timer, their middle 33 lines above the sprites' base; the fighters' names are lettered below them.
// Its screen fades are box sprites (sub_000583E0 at a match's end) in one of its two draw lists, 0x42C718 and 0x42C714
// on top (sub_000722B0 goes through both), the alpha a byte in each corner (sub_00131E90).
#include "mk3d/names.h"

#include <cstdint>

#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mkda {
namespace {

constexpr uint32_t kChrome[2] = {0x2EA880, 0x2EA878};  // P1's, P2's: the object, its id 4 bytes on
constexpr uint32_t kHudWidth = 0x42CAA0;
constexpr float kInset = 20.0f;   // from a sprite's outer end to its bar's
constexpr float kMiddle = 33.0f;  // from a sprite's base up to its bar's middle
constexpr float kClear = 33.0f;   // either side of the centre: the timer's ring (25) and a margin
constexpr mk3d::names::FadeBoxes kFades{{0x42C718, 0x42C714}, 0x24, 0x30, 0x10};

bool Place(mk3d::names::Place& out) {
    if (InFrontEnd()) return false;
    const float width = float(kernel::R32(kHudWidth));
    if (width < 320 || width > 2048) return false;
    float outer[2] = {}, middle = 0;
    for (int side = 0; side < 2; side++) {
        const uint32_t obj = kernel::R32(kChrome[side]);
        if (!obj || !mem::IsCommitted(obj) || kernel::R32(obj + 4) != kernel::R32(kChrome[side] + 4)) return false;
        const uint32_t image = kernel::R32(obj + 0x30);
        if (!image || !mem::IsCommitted(image)) return false;
        const float left = float(int32_t(kernel::R32(obj + 0x14))), base = float(int32_t(kernel::R32(obj + 0x18)));
        outer[side] = side == 0 ? left + kInset : left + float(kernel::R32(image + 0x54)) - kInset;
        middle += base + kMiddle;
    }
    const float k = 640.0f / width;
    out.p1_start = outer[0] * k;
    out.p1_end = (width / 2 - kClear) * k;
    out.p2_start = (width / 2 + kClear) * k;
    out.p2_end = outer[1] * k;
    out.y = 480.0f - middle / 2;
    out.shade = mk3d::names::ShadeUnder(kFades);
    return out.y > 0;  // slid off the top with the HUD
}

const bool g_installed = [] {
    mk3d::names::Install({Place});
    return true;
}();

}  // namespace
}  // namespace mkda
