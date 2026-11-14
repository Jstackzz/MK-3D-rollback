// Deadly Alliance's side of the practice frame data (mk3d/framedata.h; research/mkda-rev1/notes.md, "Frame data"):
// where its fighters are, the game's own rule for when a fighter may start a move, and its hit test reporting live
// attacks; and the practice dummy's block (F8).
#include "mk3d/framedata.h"

#include <cstdint>

#include "cpu/x86_ops.h"
#include "game/combos.h"
#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_000547B0);
X86_EXTERN_FUNC(__imp__sub_00075B50);

namespace mkda::rules { void BlockRulesAtTop(int side); }

namespace mkda {
namespace {

constexpr uint32_t kGameFrames = 0x42A8A0;               // the engine's frame counter, still while paused
constexpr uint32_t kSelf = 0x320A18;                     // the fighter whose task runs
constexpr uint32_t kTask[2] = {0x2EA530, 0x2EA534};      // each fighter's task; a task's function is at +0x30
// The block-hold routines, standing and crouching, let go of block in the frame they see the button up (state 0, or
// 0x101 crouching with down held) and a move can start in the next. After a blocked hit, the standing one is first seen
// at the top of a frame it is about to run in; the crouching one already ran its first frame (the reaction's tail
// waits a frame, then starts it at once), so a crouching blocker who let go there acts in the frame it is first seen
// (as in Deception).
constexpr uint32_t kBlockHold = 0xE7480, kCrouchBlockHold = 0xE7B70;
// A hit landing is blocked by sub_00057F30's rule and the victim's reaction by sub_00057E20's, the same but for two
// states: block held, no +0x158 or +0x15C, and +0x160 no later than now, whatever the fighter is doing. So the block and
// the move gate (+0x168) open apart: the end of Jax's Piston Punch clears +0x158 at move frame 76 and shuts the gate for
// 18 more frames (-9 on block, -27 to attack: a 9 frame jab pressed at the blocker's first chance lands, a frame later
// it is blocked), Scorpion's B opens the gate first and blocks 5 frames later (-25 on block, -20 to attack). Measured
// with a jab put in reach on Jax's X and Piston Punch, Scorpion's X and B and Hsu Hao's X: all land up to the last frame
// this rule says no, and are blocked from the first it says yes.
constexpr uint32_t kBlockingOn = 0x42C744;  // the fight lets fighters block
// a fighter object's fields
constexpr uint32_t kContact = 0x154;    // its strike's result: 0 none (every strike starts there), 1 hit, 2 blocked
constexpr uint32_t kNoBlock = 0x158, kNoBlockToo = 0x15C;  // sub_00057E20: no blocking while either is set
constexpr uint32_t kBlockFrom = 0x160;  // no blocking before this game frame
constexpr uint32_t kBusyUntil = 0x168;  // no new move before this game frame
constexpr uint32_t kState = 0x194;      // 0 stance, 0x1200 attack, 0x60x hit reaction, 0xA0x block ...
constexpr uint32_t kComboHits = 0x1A0;  // the hits of the combo it is taking, 0 when it takes none
constexpr uint32_t kCommand = 0x9F8;    // its move script's current command: a strike holds one for its whole window
constexpr uint32_t kControls = 0x19C;   // the player number its actions are read for (sub_00075B50)
constexpr int kActionBlock = 1, kActionDown = 0xE;  // sub_00075B50's actions: block, crouch

// what this adapter keeps between frames, in one block for the tracker's online saves (Adapter::local)
struct Local {
    uint32_t strike_command[2] = {};  // the command each side's attack was last tested from
    uint32_t raw_contact[2] = {};     // each side's contact field when last read
    int contact[2] = {};              // what that contact was taken for when it came
};
Local g_local;
uint32_t (&g_strike_command)[2] = g_local.strike_command;
uint32_t (&g_raw_contact)[2] = g_local.raw_contact;
int (&g_contact)[2] = g_local.contact;

uint32_t FighterObject(int side) { return kernel::R32(kFighter[side]); }

bool Practice() {
    const uint32_t mode = kernel::R32(kGameMode);
    return (mode == kModePractice || (mk3d::framedata::VersusToo() && mode == kModeVersus)) &&
           kernel::R32(kMainTask) == kFightTask;
}

uint32_t Time() { return kernel::R32(kGameFrames); }

bool Read(int side, mk3d::framedata::Fighter& f) {
    if (!mk3d::framedata::AfterTop()) mkda::rules::BlockRulesAtTop(side);  // online it ran at the frame's top
    const uint32_t obj = FighterObject(side);
    if (!obj || !mem::IsCommitted(obj)) return false;
    const uint32_t state = kernel::R32(obj + kState), now = Time(), busy = kernel::R32(obj + kBusyUntil);
    const bool alive = kernel::R32(kHealth[side]) != 0;  // a float: 0 is knocked out
    // sub_000EA250's rule for starting a move (its player-state term left out: the practice dummy's is 0); holding
    // block, as if it let go at the first chance
    const uint32_t task = kernel::R32(kTask[side]);
    const uint32_t routine = task && mem::IsCommitted(task) ? kernel::R32(task + 0x30) : 0;
    f.can_act = alive && now >= busy &&
                ((!((state & 0x200) && state != 0x420D) && !(state & 0x800) && state != 0x6000) ||
                 routine == kCrouchBlockHold);
    f.can_act_next = alive && routine == kBlockHold && now + 1 >= busy;
    f.neutral = alive && state == 0;
    f.can_block = alive && state != 0x605 && state != 0x3202 && !kernel::R32(obj + kNoBlock) &&
                  !kernel::R32(obj + kNoBlockToo) && now >= kernel::R32(obj + kBlockFrom) && kernel::R32(kBlockingOn);
    f.striking = g_strike_command[side] && kernel::R32(obj + kCommand) == g_strike_command[side];
    // A strike's contact says hit or blocked, but a projectile's (Jax's Machine Gun) says hit when it is blocked too:
    // a hit is one the other fighter counts in a combo, decided once, at the top of the frame after it came
    const uint32_t contact = kernel::R32(obj + kContact);
    if (contact == 1 && g_raw_contact[side] != 1) {
        const uint32_t other = FighterObject(1 - side);
        g_contact[side] = other && mem::IsCommitted(other) && !kernel::R32(other + kComboHits) ? 2 : 1;
    } else if (contact != 1) {
        g_contact[side] = contact == 2 ? 2 : 0;
    }
    g_raw_contact[side] = contact;
    f.contact = g_contact[side];
    return true;
}

const char* Name(int side) {
    const uint32_t id = kernel::R32(kCharacter[side]);
    if (id >= 0x18) return nullptr;
    const uint32_t p = kernel::R32(kCharacterNames + id * kCharacterStride);
    return p ? static_cast<const char*>(kernel::HostPtr(p)) : nullptr;
}

const bool g_installed = [] {
    mk3d::framedata::Install({Practice, Time, Read, Name, true, false, &g_local, sizeof g_local});
    return true;
}();

}  // namespace
}  // namespace mkda

// The hit test: the strike loops (sub_00054900, sub_00054CF0, sub_00055000 and moves' own) test the attack of the
// fighter whose task runs against the other one here, once a frame while the attack is live and has not made contact.
// Bo' Rai Cho's string may reach further first (combos.cpp).
X86_FUNC(sub_000547B0) {
    mkda::combos::BeforeHitTest(ctx);
    if (mk3d::framedata::Following()) {
        const uint32_t self = kernel::R32(mkda::kSelf);
        for (int side = 0; side < 2; side++) {
            if (!self || self != mkda::FighterObject(side)) continue;
            mk3d::framedata::ActiveCheck(side);
            mkda::g_strike_command[side] = kernel::R32(self + mkda::kCommand);
        }
    }
    __imp__sub_000547B0(ctx, base);
}

// Whether a player holds an action (sub_00075B50(player, action)), which every block check asks for the blocking
// fighter (sub_00057E20 and the hit's victim's). No controller drives the practice dummy, so with F8 this answers for
// it: block held (and down, crouching).
X86_FUNC(sub_00075B50) {
    const int tech = mk3d::framedata::TechHolds(int(kernel::R32(ctx.esp + 8)));  // -1: no tech roll being decided
    if (tech >= 0 || mk3d::framedata::DummyBlock() != mk3d::framedata::Dummy::Stands) {
        const uint32_t dummy = mkda::FighterObject(1), player = mkda::FighterObject(0);
        const uint32_t who = kernel::R32(ctx.esp + 4), action = kernel::R32(ctx.esp + 8);
        if (dummy && player && who == kernel::R32(dummy + mkda::kControls) &&
            who != kernel::R32(player + mkda::kControls) &&
            (tech >= 0 || mk3d::framedata::DummyHolds(int(action), mkda::kActionBlock, mkda::kActionDown))) {
            ctx.eax = tech >= 0 ? uint32_t(tech) : 1;  // a tech roll: its direction alone (no block, no buttons)
            ctx.esp += 4;  // ret (the caller pops the arguments)
            return;
        }
    }
    __imp__sub_00075B50(ctx, base);
}

// The practice dummy's tech roll (F6, framedata.h): while the game decides whether a fighter landing from a knockdown
// rolls, the dummy holds the roll's direction. A roll starts through the game's task switch, which leaves these checks
// by unwinding them, the scope with them.
// Deadly Alliance's second tech roll check (script command 0x73, sub_0011DC00), beside sub_00051290 (noinfinites.cpp, which opens the dummy's scope there)
X86_EXTERN_FUNC(__imp__sub_00051420);
X86_FUNC(sub_00051420) {
    const uint32_t self = kernel::R32(mkda::kSelf);
    mk3d::framedata::TechScope tech(self && self == mkda::FighterObject(1));
    __imp__sub_00051420(ctx, base);
}

