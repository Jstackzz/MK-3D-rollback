// The costumes' other colours (colours.h).
#include "game/colours.h"

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/autopilot.h"
#include "game/events.h"
#include "kernel/kernel.h"

namespace mkda::colours {
namespace {

constexpr uint32_t kHeld = 0x75B50;  // sub_00075B50(player, action): whether the player holds the action
constexpr uint32_t kActionLT = 2;    // LT, the fighting style change in a fight
constexpr uint32_t kColoursBit = 2;  // the costume word: bit 0 the second costume, bit 1 its other colours

}  // namespace

void Picked(X86Context& ctx, uint32_t player) {
    if (player > 1) return;
    const uint32_t word = kernel::R32(kCostume[player]);
    if (!word || !kernel::CallGuest(ctx, kHeld, {player, kActionLT})) return;
    kernel::W32(word, kernel::R32(word) | kColoursBit);
    LOGR("colours: P%u picked holding LT: the costume's other colours (costume word %X)", player + 1,
         kernel::R32(word));
}

void SelectEnds() {
    const uint32_t va[2] = {kernel::R32(kCostume[0]), kernel::R32(kCostume[1])};
    if (!va[0] || !va[1]) return;
    uint32_t word[2] = {kernel::R32(va[0]), kernel::R32(va[1])};
    for (int s = 0; s < 2; s++) {
        int character = -1, arena = -1;
        uint32_t costume = 0;
        if (autopilot::AgreedPick(s, character, costume, arena)) word[s] = (word[s] & ~3u) | (costume & 3u);
    }
    if (kernel::R32(kCharacter[0]) == kernel::R32(kCharacter[1]) && (word[0] & 3u) == (word[1] & 3u)) {
        word[1] ^= kColoursBit;
        LOGR("colours: both players have character %u in the same costume and colours: P2 the other colours",
             kernel::R32(kCharacter[0]));
    }
    kernel::W32(va[0], word[0]);
    kernel::W32(va[1], word[1]);
}

}  // namespace mkda::colours
