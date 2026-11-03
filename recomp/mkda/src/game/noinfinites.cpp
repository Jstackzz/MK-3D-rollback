// Deadly Alliance's rules against infinites (MK3D Online 0.1.21):
//   - Bo' Rai Cho's Puke Puddle (back, forward + 2) cannot come out while the opponent is in a combo: hit since they
//     last stood, walked, attacked or blocked (down or getting up counts), or slipping in a puddle (the puddle as they
//     get up from a knockdown, a hit while they slip, a knockdown, the puddle again);
//   - Frost's Ground Ice (down, back + 3) cannot come out once the combo has 3 hits or more;
//   - Drahmin's Ground Smash (back, down + 4) cannot come out once the combo has 6 hits or more (it stands a fighter
//     who is down or getting up back up, stunned, for another combo, another knockdown, the smash again);
//   - Sonya's Kiss of Death (down, back + 1) cannot come out in a combo of 9 hits or more;
//   a combo's hits count from when the opponent was last free, across knockdowns (the game starts its count again
//   at each one).
//
// Deadly Alliance offers each character's special moves per attack button through sub_00076700(table): the button
// routines (sub_000E9440 button 1, sub_000E97A0 button 2, sub_000E9A40 button 3, sub_000E9D30 button 4) switch on
// the attacker's character (fighter +0x144) and pass that character's table for the button, some only below a
// number of hits (the game's own limits: Bo's button 2 only at 0 hits, Frost's button 3 under 3, which a knockdown
// that ends the combo gets round). A table's entries: a condition, a kind and an input window, the move, the
// directions before the button. The hits of the combo a fighter is in are its +0x1A0 (sub_00056480 shows them as
// "%d HITS" when the combo ends, and clears them).
//
// What these rules remember lives in guest physical memory, which a rollback puts back with the rest of the game,
// so two games playing online decide the same.
#include <algorithm>
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_00076700);

namespace mkda::rules {
namespace {

constexpr uint32_t kSelf = 0x320A18;      // the fighter whose task runs
constexpr uint32_t kOpponent = 0x320A0C;  // the other one, while it runs
constexpr uint32_t kFighter[2] = {0x2EA540, 0x2EA544};
constexpr uint32_t kCharacterId = 0x144;
constexpr uint32_t kState = 0x194;      // 0 stance, 0x12xx 0x13xx attacks, 0x0Axx blocks, others reactions
constexpr uint32_t kComboHits = 0x1A0;  // the hits of the combo it is in
constexpr uint32_t kSonya = 0x09, kFrost = 0x0B, kDrahmin = 0x12, kBoRaiCho = 0x14;
constexpr uint32_t kPuddleSlip = 0x4207;  // slipping in Bo' Rai Cho's puddle

struct Limit {
    uint32_t character, table, hits;
    const char* move;
};
constexpr Limit kLimits[] = {
    {kBoRaiCho, 0x2712AC, 1, "Bo' Rai Cho's Puke Puddle"},  // button 2: back, forward
    {kFrost, 0x271330, 3, "Frost's Ground Ice"},             // button 3: down, back (the table's only move)
    {kDrahmin, 0x271200, 6, "Drahmin's Ground Smash"},       // button 4: back, down (the table's only move)
    {kSonya, 0x270D90, 9, "Sonya's Kiss of Death"},          // button 1: down, back
};

// guest memory: per side, the hits of the combo it is in or still getting up from (added up across knockdowns), and
// the game's own count when last looked at
constexpr uint32_t kComboOf = 0, kSeenOf = 8, kBytes = 16;
uint32_t g_mem = 0;
uint32_t g_logged = 0;  // rule lines logged so far, the first 200 (LOG_FIRST is silent in release builds)

bool LogMore() { return g_logged < 200 && ++g_logged; }

uint32_t Mem() {
    if (!g_mem) {
        g_mem = mem::AllocContiguous(kBytes, 0, 0xFFFFFFFF, 16);
        if (g_mem)
            for (uint32_t i = 0; i < kBytes; i += 4) kernel::W32(g_mem + i, 0);
    }
    return g_mem;
}

int Side(uint32_t fighter) {
    if (!fighter) return -1;
    for (int s = 0; s < 2; s++)
        if (kernel::R32(kFighter[s]) == fighter) return s;
    return -1;
}

// free again: standing (crouched too), walking (0x2000 forward, 0x2001 back), attacking or blocking
bool Free(uint32_t state) {
    const uint32_t kind = state & 0xFF00;
    return state == 0 || kind == 0x2000 || kind == 0x1200 || kind == 0x1300 || kind == 0x0A00;
}

void EachFrame(uint64_t) {
    const uint32_t m = Mem();
    if (!m) return;
    for (int s = 0; s < 2; s++) {
        const uint32_t f = kernel::R32(kFighter[s]);
        const bool here = f && mem::IsCommitted(f);
        const uint32_t hits = here ? kernel::R32(f + kComboHits) : 0;
        if (!here || Free(kernel::R32(f + kState))) {
            kernel::W32(m + kComboOf + s * 4, 0);
            kernel::W32(m + kSeenOf + s * 4, hits);
            continue;
        }
        const uint32_t seen = kernel::R32(m + kSeenOf + s * 4);
        if (hits > seen) kernel::W32(m + kComboOf + s * 4, kernel::R32(m + kComboOf + s * 4) + hits - seen);
        kernel::W32(m + kSeenOf + s * 4, hits);
    }
}

// the hits of the combo a fighter is in, with those of this frame so far
uint32_t ComboHits(uint32_t fighter, int side, uint32_t m) {
    const uint32_t hits = kernel::R32(fighter + kComboHits);
    if (side < 0 || !m) return hits;
    const uint32_t seen = kernel::R32(m + kSeenOf + side * 4);
    return std::max(hits, kernel::R32(m + kComboOf + side * 4) + (hits > seen ? hits - seen : 0));
}

const bool g_registered = (mk3d::OnFrameStart(&EachFrame), true);

}  // namespace
}  // namespace mkda::rules

// a character's special moves for a button: the limited ones are not looked at past their hits
X86_FUNC(sub_00076700) {
    using namespace mkda::rules;
    const uint32_t table = kernel::R32(ctx.esp + 4), self = kernel::R32(kSelf), opp = kernel::R32(kOpponent);
    for (const Limit& l : kLimits) {
        if (table != l.table || !self || !opp || kernel::R32(self + kCharacterId) != l.character) continue;
        uint32_t hits = ComboHits(opp, Side(opp), Mem());
        // slipping in a puddle is a combo too, before any hit has counted
        if (kernel::R32(opp + kState) == kPuddleSlip) hits = std::max<uint32_t>(hits, 1);
        if (hits >= l.hits) {
            if (LogMore()) LOGR("rules: %s held back: the opponent is in a %u hit combo", l.move, hits);
            ctx.esp += 4;  // ret: the caller pops the table
            return;
        }
        if (LogMore()) LOGR("rules: %s may come out: the opponent is at %u hits", l.move, hits);
    }
    __imp__sub_00076700(ctx, base);
}
