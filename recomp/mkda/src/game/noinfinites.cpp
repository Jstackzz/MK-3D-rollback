// Deadly Alliance's rules against infinites (MK3D Online 0.1.21; crouch blocking ends a combo since 0.1.22):
//   - Bo' Rai Cho's Puke Puddle (back, forward + 2) cannot come out while the opponent is in a combo: hit since they
//     last stood, crouched, walked, attacked or blocked (down or getting up counts), or slipping in a puddle (the
//     puddle as they get up from a knockdown, a hit while they slip, a knockdown, the puddle again);
//   - Frost's Ground Ice (down, back + 3) cannot come out once the combo has 3 hits or more;
//   - Drahmin's Ground Smash (back, down + 4) cannot come out once the combo has 6 hits or more (it stands a fighter
//     who is down or getting up back up, stunned, for another combo, another knockdown, the smash again);
//   - Sonya's Kiss of Death (down, back + 1) cannot come out in a combo of 9 hits or more;
//   a combo's hits count from when the opponent was last free, across knockdowns that cannot be teched (the game
//   starts its count again at each one); landing from one that can be teched ends the combo, rolled or not, as the
//   fighter could have rolled away (0.1.23).
// Blocking out of a knockdown is the game's own, as on the original Xbox: from 0.1.29 to 0.1.31 a fighter knocked off its
// feet could not block until it stood (hooks of sub_00057E20 and sub_00057F30), and with a fighter on its knees counted
// down too, Sonya's 2-Hit Axe Kick (Tae Kwon Do 4) became an infinite on a fighter holding block, so it went again.
// Also here: Scorpion's Hellfire and a fighter getting up, and an attack over Bo' Rai Cho's puddle; see the hooks of
// sub_00051B20 and sub_001381E0.
//
// The game's knockdowns that can be teched run the check sub_00051290 for the fighter as it lands (up or down rolls,
// away rolls back, the computer rolls at random): ten reaction routines call it (sub_00119280 after an uppercut, for
// one), and their twins for knockdowns that cannot be teched do not (sub_00119390 after Bo' Rai Cho's sweep; Frost's
// slide lands through sub_00053160).
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
#include <cstring>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_00076700);
X86_EXTERN_FUNC(__imp__sub_00051290);
X86_EXTERN_FUNC(__imp__sub_000537B0);
X86_EXTERN_FUNC(__imp__sub_00053650);
X86_EXTERN_FUNC(__imp__sub_001381E0);
X86_EXTERN_FUNC(__imp__sub_00051B20);

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

// the block lock, see the hook of sub_000537B0
constexpr uint32_t kLockLater = 5;          // frames
constexpr uint32_t kFrameCount = 0x42A888;  // the count sub_000E7A90 times the block reaction with
constexpr uint32_t kAttackNoted = 0x1E4;    // the opponent's attack this fighter's block routine knows of
constexpr uint32_t kHitAt = 0x1B4;          // when the opponent's attack is to land, on that count
constexpr uint32_t kComputer = 0x20C;       // not 0: the computer plays this fighter

// guest memory: per side, the hits of the combo it is in or still getting up from (added up across knockdowns), and
// the game's own count when last looked at; the attack its block routine is not told of yet, and since when
constexpr uint32_t kComboOf = 0, kSeenOf = 8, kLockAttack = 16, kLockSince = 24, kStyleAt = 32, kBytes = 40;
uint32_t g_mem = 0;
uint32_t g_logged = 0;  // rule lines logged so far, the first 200 (LOG_FIRST is silent in release builds)

bool LogMore() { return g_logged < 200 && ++g_logged; }

constexpr uint32_t kShape[2] = {0x25B210, 0x25B248};  // the fighters' shapes: +4 the body spheres, +0x18 how many
constexpr uint32_t kSphereSize = 0x84;

// Bo' Rai Cho's Puke Puddle, see the hook of sub_001381E0
constexpr uint32_t kProjectile = 0x31FA8C, kProjectileObject = 0x31FA88;  // the one whose routine runs, and its object
constexpr uint32_t kProjectileVictim = 0x10;  // the fighter it is against
constexpr uint32_t kObjectPlace = 0x84;       // x, height, z
constexpr uint32_t kSideOf = 0x148;           // a fighter's side: 0 or 1
constexpr uint32_t kSphereCentre = 0x44, kSphereRadius = 0x50;
constexpr float kPuddleReach = 0.5f;  // a sphere's edge this near the puddle's middle
constexpr float kLegOver = 1.1f;      // its underside at most this high above the puddle
constexpr float kOnFloor = 0.2f;      // and the fighter's lowest sphere this near the floor: one foot on it

// Scorpion's Hellfire, see the hook of sub_00051B20
constexpr uint32_t kScorpion = 0x00;
constexpr uint32_t kOpponentObject = 0x320A10;  // the object of the other fighter, while a task runs
constexpr uint32_t kHellfireSpot = 0x200;       // the fighter's: the x and, 4 on, the z it remembers
constexpr float kHellfireGame = 0.5f, kHellfireReach = 0.6f;

// LT in Jax's and Frost's strings, see the hook of sub_00053650: the players' rings of presses, 30 entries {action, frame count
// 0x42A888, ...}, the index of the newest, and the action and frame of the press a move last used
constexpr uint32_t kJax = 0x01;
constexpr uint32_t kObject = 0x320A14;  // the object of the fighter whose task runs
constexpr uint32_t kObjects[2] = {0x2EA538, 0x2EA53C};
constexpr uint32_t kRing[2] = {0x42C220, 0x42C0A0}, kNewest[2] = {0x266B3C, 0x266B40};
constexpr uint32_t kRingEntries = 30, kRingEntry = 12;
constexpr uint32_t kUsedAction[2] = {0x3209F8, 0x3209F0}, kUsedFrame[2] = {0x3209F4, 0x3209EC};
constexpr uint32_t kActionStyle = 2, kActionAttackFirst = 4, kActionAttackLast = 7;  // LT; buttons 2, 4, 3, 1
constexpr uint32_t kStyleWait = 20;  // frames an attack button may come after the LT and still leave it standing

struct Presses {
    uint32_t action, frame, beforeAction, beforeFrame;
};

Presses PressesOf(int side) {
    const uint32_t i = kernel::R32(kNewest[side]) % kRingEntries;
    const uint32_t newest = kRing[side] + i * kRingEntry;
    const uint32_t before = kRing[side] + ((i + kRingEntries - 1) % kRingEntries) * kRingEntry;
    return {kernel::R32(newest), kernel::R32(newest + 4), kernel::R32(before), kernel::R32(before + 4)};
}

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

// free again: standing, crouching (0x100 going down, 0x101 down), walking (0x2000 forward, 0x2001 back), attacking,
// or blocking standing (0x0Axx) or crouching (0x900 holding it, 0x901: the crouch block routines set them)
bool Free(uint32_t state) {
    const uint32_t kind = state & 0xFF00;
    return state == 0 || kind == 0x0100 || kind == 0x2000 || kind == 0x1200 || kind == 0x1300 || kind == 0x0A00 ||
           kind == 0x0900;
}

void EachFrame(uint64_t) {
    const uint32_t m = Mem();
    if (!m) return;
    for (int s = 0; s < 2; s++) {
        const uint32_t f = kernel::R32(kFighter[s]);
        const bool here = f && mem::IsCommitted(f);
        // a style change that went ahead of an attack button: as it started it marked the newest press used, and that
        // is the button. Mark the LT instead, so the new style's script sees the button.
        if (const uint32_t since = kernel::R32(m + kStyleAt + s * 4)) {
            const Presses p = PressesOf(s);
            if (kernel::R32(kUsedAction[s]) == p.action && kernel::R32(kUsedFrame[s]) == p.frame) {
                kernel::W32(kUsedAction[s], p.beforeAction);
                kernel::W32(kUsedFrame[s], p.beforeFrame);
                kernel::W32(m + kStyleAt + s * 4, 0);
            } else if (kernel::R32(kFrameCount) - since > 8) {
                kernel::W32(m + kStyleAt + s * 4, 0);
            }
        }
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

float AsFloat(uint32_t word) {
    float v;
    std::memcpy(&v, &word, sizeof v);
    return v;
}

float ReadFloat(uint32_t at) { return AsFloat(kernel::R32(at)); }

// a fighter attacking on one foot, with its body over the puddle whose routine runs
bool KickOverPuddle() {
    const uint32_t p = kernel::R32(kProjectile), o = kernel::R32(kProjectileObject);
    const uint32_t victim = p ? kernel::R32(p + kProjectileVictim) : 0;
    if (!o || !victim) return false;
    const uint32_t kind = kernel::R32(victim + kState) & 0xFF00;
    const uint32_t side = kernel::R32(victim + kSideOf);
    if ((kind != 0x1200 && kind != 0x1300) || side > 1) return false;
    const uint32_t spheres = kernel::R32(kShape[side] + 4), count = kernel::R32(kShape[side] + 0x18);
    if (!spheres || !count || count > 64 || !mem::IsCommitted(spheres) ||
        !mem::IsCommitted(spheres + count * kSphereSize - 1))
        return false;
    const float px = ReadFloat(o + kObjectPlace), py = ReadFloat(o + kObjectPlace + 4), pz = ReadFloat(o + kObjectPlace + 8);
    float lowest = 1e9f;
    bool over = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t c = spheres + i * kSphereSize + kSphereCentre;
        const float dx = ReadFloat(c) - px, dz = ReadFloat(c + 8) - pz;
        const float r = ReadFloat(spheres + i * kSphereSize + kSphereRadius);
        const float under = ReadFloat(c + 4) - r - py;
        const float reach = kPuddleReach + r;
        lowest = std::min(lowest, under);
        if (under < kLegOver && dx * dx + dz * dz < reach * reach) over = true;
    }
    return over && lowest < kOnFloor;
}

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

// the fighter whose task runs lands from a knockdown it can tech roll out of: its combo ends here, rolled or not, and
// the hits it takes from now start a new one
X86_FUNC(sub_00051290) {
    using namespace mkda::rules;
    const uint32_t self = kernel::R32(kSelf), m = Mem();
    const int side = Side(self);
    if (m && side >= 0) {
        if (kernel::R32(m + kComboOf + side * 4) && LogMore())
            LOGR("rules: P%d lands from a knockdown it can tech roll out of: its combo ends", side + 1);
        kernel::W32(m + kComboOf + side * 4, 0);
        kernel::W32(m + kSeenOf + side * 4, kernel::R32(self + kComboHits));
    }
    __imp__sub_00051290(ctx, base);
}

// The block lock comes five frames later. Holding block (sub_000E7480), a fighter asks every frame which attack the
// opponent is on (this function, the opponent's +0x1B0, against its own +0x1E4). A new one starts a block reaction timed
// to end as the hit lands (sub_000E7A90), and the routine reads no buttons until it ends: a fighter who lets go after
// that stays in block, and then takes the hit in full. Here the routine hears of a new attack kLockLater frames after it
// starts, so for those frames its own check of the block button still lets the fighter go; the frame it hears, it locks
// as before. A fighter who keeps holding blocks as ever. The computer's fighters stay as they are. All of it is game
// state and guest memory, so a rollback puts it back and two games online decide the same.
X86_FUNC(sub_000537B0) {
    using namespace mkda::rules;
    const uint32_t from = kernel::R32(ctx.esp);  // the three places in sub_000E7480 that ask
    __imp__sub_000537B0(ctx, base);
    if (from != 0xE7661 && from != 0xE768F && from != 0xE7875) return;
    const uint32_t self = kernel::R32(kSelf), opp = kernel::R32(kOpponent), m = Mem();
    const int side = Side(self);
    if (!m || side < 0 || !opp || kernel::R32(self + kComputer)) return;
    const uint32_t attack = ctx.eax, noted = kernel::R32(self + kAttackNoted), now = kernel::R32(kFrameCount);
    if (attack == noted) {
        kernel::W32(m + kLockAttack + side * 4, attack);
        return;
    }
    if (kernel::R32(m + kLockAttack + side * 4) != attack) {
        kernel::W32(m + kLockAttack + side * 4, attack);
        kernel::W32(m + kLockSince + side * 4, now);
    }
    // a hit about to land gets its reaction now: sub_000E7A90 divides by the frames left
    const int32_t left = int32_t(kernel::R32(opp + kHitAt) - now);
    if (now - kernel::R32(m + kLockSince + side * 4) >= kLockLater || left <= 2) return;
    ctx.eax = noted;
}

// Jax's and Frost's strings often have LT and then the next attack button a few frames later (2, 2, LT, 4; 4, 4, LT, 4;
// Frost's 1, 1, 2, 2, LT, 4, 1). A move's script
// only asks about the newest press (this function: the newest entry of the player's ring of presses, unless a move has
// used it), and it reaches its style change check some frames after the hit, so an attack button pressed in those frames
// hid the LT and the old style's move came out, or none: the string ended there. Now, for these two, the check
// also sees an unused LT right behind the newest press: the style change happens and the button waits for the new style's
// script (EachFrame). The ring and the mark are guest memory, so a rollback puts them back. The computer's stay as they are.
X86_FUNC(sub_00053650) {
    using namespace mkda::rules;
    const uint32_t self = kernel::R32(kSelf), m = Mem();
    const uint32_t character = self ? kernel::R32(self + kCharacterId) : 0xFFFFFFFF;
    if (kernel::R32(ctx.esp + 4) == kActionStyle && self && m && (character == kJax || character == kFrost) &&
        !kernel::R32(self + kComputer)) {
        const int side = kernel::R32(kObject) == kernel::R32(kObjects[0]) ? 0 : 1;
        const Presses p = PressesOf(side);
        const uint32_t usedAction = kernel::R32(kUsedAction[side]), usedFrame = kernel::R32(kUsedFrame[side]);
        const bool used = (usedAction == p.action && usedFrame == p.frame) ||
                          (usedAction == p.beforeAction && usedFrame == p.beforeFrame);
        if (p.action >= kActionAttackFirst && p.action <= kActionAttackLast && p.beforeAction == kActionStyle && !used &&
            p.frame - p.beforeFrame <= kStyleWait) {
            if (LogMore()) LOGR("rules: an LT right before an attack button in a string: the style change comes first");
            kernel::W32(m + kStyleAt + side * 4, kernel::R32(kFrameCount));
            ctx.eax = 1;
            ctx.esp += 4;  // ret
            return;
        }
    }
    __imp__sub_00053650(ctx, base);
}

// Scorpion's Hellfire (down, back + 2) remembers where the opponent stands (sub_00051AE0, 3 frames after the press) and about
// 18 frames later hits if the opponent is still within 0.5 of that spot (this function, the game's: squared, 0.25). A
// fighter getting up slides about half a unit, up to 0.53 after Scorpion's 4, so pressed 17 to 13 frames before the
// fighter stood it missed, and it hit a few frames either side. Now it reaches 0.6. Someone walking off still gets out,
// a few frames later. Between 0.5 and 0.6 the game's own check is shown the opponent on the spot, so all it does on a hit
// is its own; the spot is put back after. Game state only, so a rollback puts it back and two games online decide the same.
X86_FUNC(sub_00051B20) {
    using namespace mkda::rules;
    const uint32_t self = kernel::R32(kSelf), o = kernel::R32(kOpponentObject);
    if (self && o && kernel::R32(self + kCharacterId) == kScorpion) {
        const uint32_t sx = kernel::R32(self + kHellfireSpot), sz = kernel::R32(self + kHellfireSpot + 4);
        const uint32_t ox = kernel::R32(o + kObjectPlace), oz = kernel::R32(o + kObjectPlace + 8);
        const float dx = AsFloat(ox) - AsFloat(sx), dz = AsFloat(oz) - AsFloat(sz);
        const float d = dx * dx + dz * dz;
        if (d >= kHellfireGame * kHellfireGame && d < kHellfireReach * kHellfireReach) {
            if (LogMore()) LOGR("rules: Scorpion's Hellfire reaches a fighter who slid off the spot");
            kernel::W32(self + kHellfireSpot, ox);
            kernel::W32(self + kHellfireSpot + 4, oz);
            __imp__sub_00051B20(ctx, base);
            kernel::W32(self + kHellfireSpot, sx);
            kernel::W32(self + kHellfireSpot + 4, sz);
            return;
        }
    }
    __imp__sub_00051B20(ctx, base);
}

// The puddle only touches what is low down. A fighter kicking through it, on one foot beside its middle with the kicking leg
// over it, stayed out of it as it turned on, went on past and left it behind: the kick hit Bo and the puddle did nothing.
// Now an attacker with a foot on the floor and its body over the puddle's middle slips too: the kick and the puddle trade.
// sub_001381E0, the puddle's own test, answers 0 for a touch and 4 for none.
X86_FUNC(sub_001381E0) {
    using namespace mkda::rules;
    __imp__sub_001381E0(ctx, base);
    if (ctx.eax == 4 && KickOverPuddle()) {
        if (LogMore()) LOGR("rules: an attack over Bo' Rai Cho's puddle: it slips");
        ctx.eax = 0;
    }
}
