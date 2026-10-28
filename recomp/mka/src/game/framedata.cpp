// Armageddon's side of the practice frame data (mk3d/framedata.h; research/mka/notes.md, "Frame data"): where its
// fighters are, the game's own rule for when a fighter may start a move, and its hit test reporting live attacks;
// and the practice dummy's block (F8).
#include "mk3d/framedata.h"

#include <cstdint>

#include "cpu/x86_ops.h"
#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_000843D0);
X86_EXTERN_FUNC(__imp__sub_000A0FD0);

namespace mka {
namespace {

constexpr uint32_t kGameFrames = 0x829304;  // the engine's frame counter, still while paused
constexpr uint32_t kSelf = 0x734FFC;        // the fighter whose task runs
constexpr uint32_t kRecordTask = 0x3C;      // a player record's fighter task; a task's function is at +0x30
// The block-hold routines, standing and crouching, let go of block in the frame they see the button up (state 0, or
// 0x101 crouching with down held) and a move can start in the next. After a blocked hit, the standing one is first seen
// at the top of a frame it is about to run in; the crouching one already ran its first frame (the reaction's tail
// waits a frame, then starts it at once), so a crouching blocker who let go there acts in the frame it is first seen
// (as in Deception).
constexpr uint32_t kBlockHold = 0x1CCF60, kCrouchBlockHold = 0x1CD680;
// a fighter object's fields (player record + 0x30)
constexpr uint32_t kContact = 0x304;    // its strike's result: -1 none, 1 hit, 2 blocked, 3 armed again
constexpr uint32_t kBusyUntil = 0x320;  // no new move before this game frame
constexpr uint32_t kPhase = 0x380;      // sub_001D34B0's attack: 1 startup, 2 hit window, 3 recovery
constexpr uint32_t kState = 0x388;      // 0 stance, 0x1200 attack, 0x60x hit reaction, 0xA0x block ...
constexpr uint32_t kControls = 0x390;   // the port its actions are read from (sub_000A0FD0)
constexpr int kActionBlock = 1, kActionDown = 0xE;  // sub_000A0FD0's actions: block, crouch

uint32_t Record(int side) { return side ? kP2Record : kP1Record; }

uint32_t FighterObject(int side) { return kernel::R32(Record(side) + kRecordFighter); }

bool Practice() {
    return (GameMode() == kModePractice || (mk3d::framedata::VersusToo() && GameMode() == kModeVersus)) &&
           !InFrontEnd() && FightSetups() > 0;
}

uint32_t Time() { return kernel::R32(kGameFrames); }

bool Read(int side, mk3d::framedata::Fighter& f) {
    const uint32_t record = Record(side), obj = kernel::R32(record + kRecordFighter);
    if (!obj || !mem::IsCommitted(obj)) return false;
    const uint32_t state = kernel::R32(obj + kState), now = Time(), busy = kernel::R32(obj + kBusyUntil);
    const bool alive = kernel::R32(record + kRecordHealth) != 0;  // a float: 0 is knocked out
    // sub_001D1A90, which the command task asks before it starts a move on the fighter's task in this frame; holding
    // block, as if it let go at the first chance
    const uint32_t task = kernel::R32(record + kRecordTask);
    const uint32_t routine = task && mem::IsCommitted(task) ? kernel::R32(task + 0x30) : 0;
    f.can_act = alive && now >= busy &&
                ((!((state & 0x200) && state != 0x420D) && !(state & 0x800) && state != 0x6000) ||
                 routine == kCrouchBlockHold);
    f.can_act_next = alive && routine == kBlockHold && now + 1 >= busy;
    f.striking = kernel::R32(obj + kPhase) == 2;
    const int32_t contact = int32_t(kernel::R32(obj + kContact));
    f.contact = contact == 1 || contact == 2 ? contact : 0;
    return true;
}

const char* Name(int side) {
    const uint32_t id = kernel::R32(Record(side) + kRecordCharacter);
    if (id >= 0x40) return nullptr;
    const uint32_t p = kernel::R32(kCharacterNames + id * 0x14);
    return p ? static_cast<const char*>(kernel::HostPtr(p)) : nullptr;
}

const bool g_installed = [] {
    mk3d::framedata::Install({Practice, Time, Read, Name});
    return true;
}();

}  // namespace
}  // namespace mka

// The hit test: the strike loops (sub_00084730, sub_00084B80 and moves' own) test the attack of the fighter whose task
// runs against the other one here, once a frame while the attack is live and has not made contact yet.
X86_FUNC(sub_000843D0) {
    if (mk3d::framedata::Following()) {
        const uint32_t self = kernel::R32(mka::kSelf);
        if (self && self == mka::FighterObject(0)) mk3d::framedata::ActiveCheck(0);
        else if (self && self == mka::FighterObject(1)) mk3d::framedata::ActiveCheck(1);
    }
    __imp__sub_000843D0(ctx, base);
}

// Whether a port holds an action (sub_000A0FD0(port, action)), which every block check asks for the blocking fighter
// (sub_00089B50: its own block, the hit's victim's). No controller drives the practice dummy, so with F8 this answers
// for it: block held (and down, crouching).
X86_FUNC(sub_000A0FD0) {
    if (mk3d::framedata::DummyBlock() != mk3d::framedata::Dummy::Stands) {
        const uint32_t dummy = mka::FighterObject(1), player = mka::FighterObject(0);
        const uint32_t port = kernel::R32(ctx.esp + 4), action = kernel::R32(ctx.esp + 8);
        if (dummy && player && port == kernel::R32(dummy + mka::kControls) &&
            port != kernel::R32(player + mka::kControls) &&
            mk3d::framedata::DummyHolds(int(action), mka::kActionBlock, mka::kActionDown)) {
            ctx.eax = 1;
            ctx.esp += 4;  // ret (the caller pops the arguments)
            return;
        }
    }
    __imp__sub_000A0FD0(ctx, base);
}
