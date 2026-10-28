// Deadly Alliance's side of the practice frame data (mk3d/framedata.h; research/mkda-rev1/notes.md, "Frame data"):
// where its fighters are, the game's own rule for when a fighter may start a move, and its hit test reporting live
// attacks; and the practice dummy's block (F8).
#include "mk3d/framedata.h"

#include <cstdint>

#include "cpu/x86_ops.h"
#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_000547B0);
X86_EXTERN_FUNC(__imp__sub_00075B50);

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
// a fighter object's fields
constexpr uint32_t kContact = 0x154;    // its strike's result: 0 none (every strike starts there), 1 hit, 2 blocked
constexpr uint32_t kBusyUntil = 0x168;  // no new move before this game frame
constexpr uint32_t kState = 0x194;      // 0 stance, 0x1200 attack, 0x60x hit reaction, 0xA0x block ...
constexpr uint32_t kCommand = 0x9F8;    // its move script's current command: a strike holds one for its whole window
constexpr uint32_t kControls = 0x19C;   // the player number its actions are read for (sub_00075B50)
constexpr int kActionBlock = 1, kActionDown = 0xE;  // sub_00075B50's actions: block, crouch

uint32_t g_strike_command[2] = {};  // the command each side's attack was last tested from

uint32_t FighterObject(int side) { return kernel::R32(kFighter[side]); }

bool Practice() {
    const uint32_t mode = kernel::R32(kGameMode);
    return (mode == kModePractice || (mk3d::framedata::VersusToo() && mode == kModeVersus)) &&
           kernel::R32(kMainTask) == kFightTask;
}

uint32_t Time() { return kernel::R32(kGameFrames); }

bool Read(int side, mk3d::framedata::Fighter& f) {
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
    f.striking = g_strike_command[side] && kernel::R32(obj + kCommand) == g_strike_command[side];
    const uint32_t contact = kernel::R32(obj + kContact);
    f.contact = contact == 1 || contact == 2 ? int(contact) : 0;
    return true;
}

const char* Name(int side) {
    const uint32_t id = kernel::R32(kCharacter[side]);
    if (id >= 0x18) return nullptr;
    const uint32_t p = kernel::R32(kCharacterNames + id * kCharacterStride);
    return p ? static_cast<const char*>(kernel::HostPtr(p)) : nullptr;
}

const bool g_installed = [] {
    mk3d::framedata::Install({Practice, Time, Read, Name});
    return true;
}();

}  // namespace
}  // namespace mkda

// The hit test: the strike loops (sub_00054900, sub_00054CF0, sub_00055000 and moves' own) test the attack of the
// fighter whose task runs against the other one here, once a frame while the attack is live and has not made contact.
X86_FUNC(sub_000547B0) {
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
    if (mk3d::framedata::DummyBlock() != mk3d::framedata::Dummy::Stands) {
        const uint32_t dummy = mkda::FighterObject(1), player = mkda::FighterObject(0);
        const uint32_t who = kernel::R32(ctx.esp + 4), action = kernel::R32(ctx.esp + 8);
        if (dummy && player && who == kernel::R32(dummy + mkda::kControls) &&
            who != kernel::R32(player + mkda::kControls) &&
            mk3d::framedata::DummyHolds(int(action), mkda::kActionBlock, mkda::kActionDown)) {
            ctx.eax = 1;
            ctx.esp += 4;  // ret (the caller pops the arguments)
            return;
        }
    }
    __imp__sub_00075B50(ctx, base);
}
