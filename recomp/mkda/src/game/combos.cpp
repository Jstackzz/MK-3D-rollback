// Bo' Rai Cho's string 2, 2, 1, 1, style change (Mi Zong into Drunken Fist; MK3D Online 0.1.22): once its first hit
// lands, the other four land too, on every opponent. Each hit pushes the opponent back, and started from the first
// hit's longer reaches the later hits came out short (of Scorpion, Mavado and others): the string stopped at 2 to 4
// hits.
//
// A strike tests its attack in each frame of its window with sub_000547B0(kind, scale, reach): sub_000483D0 puts
// spheres along the striking limb, from the list 0x25B18C[kind] (kinds 6 and 7, the arms: five bones each) and the
// table at 0x2324A8 (the hand's radius 0.125), their radii times scale, and they are tested against the other
// fighter. (A reach adds one more sphere past the limb's end along its last bone, which for this string's upright
// palm strikes points up, not at the opponent.) A fighter's move runs a script from a block of its style's moves
// (+0x9F0), and +0x9F8 is the command it is at, the same through a strike's whole window: the string's hits 2 to 4
// strike from commands +0x688, +0x76C and +0x810 of the block whose names start "drunken_2", its style change hit
// from +0x1D4 of the one that starts "mizong_entry_2center".
//
// Those strikes' spheres are larger while the opponent still reels from the string's last hit (a hit reaction,
// 0x06xx, in a combo); blocked, or out of the reaction, they test as the game made them. It is all the game's own
// state, so two games online decide the same.
//
// Jax's string ... 4, 4, style change (Judo into tonfa), its two swings: a juggle. Once the Judo 4
// starts, the kicks and the two swings come on fixed frames, and each only reaches the falling opponent at the right
// height: the whole string landed on one exact frame of the 4, and that frame moves with the launch before it (7 of 70
// timings of the launch and the 4 landed all 10 hits, on Frost). So those strikes' spheres are larger too while the
// opponent still reels in a combo, the first kick 2.5 times and the rest 4.5 times: 59 of the 70 land all 10, the others 9.
//
// Frost's string 1, 1, 2, 2, style change, 4, 1 and the 1s after it (Tong Bei into Yuan Yang, 8 hits): the same. Its second
// hit fell short of Mavado, Hsu Hao and Kung Lao from further off, its seventh of Li Mei and its eighth of Kenshi, Moloch,
// Quan Chi, Mavado and Drahmin from one side of the arena or the other, so the strikes from its second on are larger too
// while the opponent reels.
//
// Her Yuan Yang string 4, 4, 1, 1, style change and the 1s after it (into the ice daggers, 8 hits; MK3D Online 1.0.10): the
// same. Its third hit, the first 1, reaches about 1.24: a start a few hundredths further off (what the fight before it
// left) and it missed, the opponent stood up and the rest began a new combo (the user's clip, against Li Mei: 2 hits,
// then 5). Its strikes from the second on, the daggers' too, are larger while the opponent reels.
//
// A fighter slipping in Bo' Rai Cho's Puke Puddle cannot block (MK3D Online 0.1.24). The slip (state 0x4207, the
// scripted reaction 0x11AE80) sets the slipping fighter's +0x15C, which keeps a hit from being blocked, until it
// stands again. The end of the other fighter's move (sub_000558E0, the recovery 0x55AC0) or of its reaction's
// script (command sub_0011DBA0) clears it through sub_00057DA0. When Bo is hit as the puke leaves him (a trade),
// his reaction ends after the slip began, and the slipping fighter blocked his follow-up. Now the slip keeps it.
#include "game/combos.h"

#include <cstdint>
#include <cstring>
#include <iterator>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_00057DA0);

namespace mkda::combos {
namespace {

constexpr uint32_t kSelf = 0x320A18, kOpponent = 0x320A0C;  // the fighter whose task runs, the other one
constexpr uint32_t kCharacterId = 0x144, kState = 0x194, kComboHits = 0x1A0;
constexpr uint32_t kBlock = 0x9F0, kCommand = 0x9F8;  // its move's script block, the command it is at
constexpr uint32_t kBoRaiCho = 0x14, kJax = 0x01, kFrost = 0x0B;
constexpr uint32_t kPuddleSlip = 0x4207;  // slipping in Bo' Rai Cho's puddle

struct Hit {
    const char* block;  // the block's first name
    uint32_t command;   // the strike's command in it
    int number;         // its hit in the string
    float grow;         // its spheres' radii, times (the hand's 0.125 at 2.5 becomes about 0.31)
};
constexpr Hit kHits[] = {{"drunken_2", 0x688, 2, 2.5f},
                         {"drunken_2", 0x76C, 3, 2.5f},
                         {"drunken_2", 0x810, 4, 2.5f},
                         {"mizong_entry_2center", 0x1D4, 5, 2.5f}};
constexpr Hit kJaxHits[] = {{"judo_4", 0x78C, 1, 2.5f},
                            {"judo_4", 0x840, 1, 2.5f},
                            {"judo_4", 0x860, 2, 4.5f},
                            {"tonfa_entry_4neutral", 0x170, 3, 4.5f},
                            {"tonfa_entry_4neutral", 0x240, 4, 4.5f},
                            {"tonfa_entry_4neutral", 0x264, 4, 4.5f}};
constexpr Hit kFrostHits[] = {{"tongbei_1", 0x2C8, 2, 2.5f},
                              {"tongbei_1", 0x498, 3, 2.5f},
                              {"tongbei_1", 0x550, 4, 2.5f},
                              {"yuanyang_entry_4", 0x20C, 5, 2.5f},
                              {"yuanyang_entry_4", 0xAB4, 6, 2.5f},
                              {"yuanyang_entry_4", 0x3B4, 7, 2.5f},
                              {"yuanyang_entry_4", 0x47C, 8, 2.5f},
                              // Yuan Yang's 4, 4, 1, 1 into the daggers
                              {"yuanyang_4", 0x9A4, 2, 2.5f},
                              {"yuanyang_4", 0x2A4, 3, 2.5f},
                              {"yuanyang_4", 0x36C, 4, 2.5f},
                              {"double_entry_1", 0x208, 5, 2.5f},
                              {"double_entry_1", 0x380, 6, 2.5f},
                              {"double_entry_1", 0x428, 7, 2.5f},
                              {"double_entry_1", 0x4C8, 8, 2.5f}};

bool BlockIs(uint32_t block, const char* name) {
    const uint32_t n = uint32_t(std::strlen(name)) + 1;  // with its zero
    return mem::IsCommitted(block) && mem::IsCommitted(block + n - 1) &&
           std::memcmp(kernel::HostPtr(block), name, n) == 0;
}

}  // namespace

void BeforeHitTest(X86Context& ctx) {
    const uint32_t self = kernel::R32(kSelf), opp = kernel::R32(kOpponent);
    if (!self || !opp) return;
    const uint32_t character = kernel::R32(self + kCharacterId);
    if (character != kBoRaiCho && character != kJax && character != kFrost) return;
    // still reeling from the string's last hit
    if ((kernel::R32(opp + kState) & 0xFF00) != 0x0600 || !kernel::R32(opp + kComboHits)) return;
    const uint32_t block = kernel::R32(self + kBlock), command = kernel::R32(self + kCommand) - block;
    const Hit* const first = character == kJax ? kJaxHits : character == kFrost ? kFrostHits : kHits;
    const size_t count = character == kJax ? std::size(kJaxHits) : character == kFrost ? std::size(kFrostHits) : std::size(kHits);
    for (size_t i = 0; i < count; i++) {
        const Hit& h = first[i];
        if (command != h.command || !BlockIs(block, h.block)) continue;
        float scale;
        std::memcpy(&scale, kernel::HostPtr(ctx.esp + 8), sizeof scale);
        LOGR_FIRST(16, "combos: %s string, hit %d: its spheres %.2f times larger (scale %.2f)",
                   character == kJax ? "Jax's" : character == kFrost ? "Frost's" : "Bo' Rai Cho's", h.number, h.grow, scale);
        scale *= h.grow;
        std::memcpy(kernel::HostPtr(ctx.esp + 8), &scale, sizeof scale);
        return;
    }
}

}  // namespace mkda::combos

// the fighter whose task runs lets the other one block again: not while the other one slips in the puddle
X86_FUNC(sub_00057DA0) {
    using namespace mkda::combos;
    const uint32_t opp = kernel::R32(kOpponent);
    if (opp && kernel::R32(opp + kState) == kPuddleSlip) {
        LOGR_FIRST(8, "combos: the opponent slips in the puddle: it still cannot block");
        ctx.eax = opp;  // as the game's own leaves it
        ctx.esp += 4;   // ret
        return;
    }
    __imp__sub_00057DA0(ctx, base);
}
