// Deception's side of the practice frame data (mk3d/framedata.h; research/mkd/notes.md, "Frame data"): where its
// fighters are, the game's own rule for when a fighter may start a move, and its hit test reporting live attacks;
// and the practice dummy's block (F8).
#include "mk3d/framedata.h"

#include <cstdint>

#include "cpu/x86_ops.h"
#include "game/events.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_00071730);
X86_EXTERN_FUNC(__imp__sub_0008A210);

namespace mkd {
namespace {

constexpr uint32_t kModeVersus = 1, kModePractice = 4;
constexpr uint32_t kGameFrames = 0x61C418;  // the engine's frame counter, still while paused
constexpr uint32_t kSelf = 0x5FE9E0;        // the fighter whose task runs
// The block-hold routines, standing and crouching, let go of block in the frame they see the button up (state 0, or
// 0x101 crouching with down held) and a move can start in the next. After a blocked hit, the standing one is first seen
// at the top of a frame it is about to run in; the crouching one already ran its first frame (the reaction's tail
// waits a frame, then starts it at once), so a crouching blocker who let go there acts in the frame it is first seen.
// Measured in versus against both players pressing a new button every frame.
constexpr uint32_t kBlockHold = 0x1455F0, kCrouchBlockHold = 0x145C60;
constexpr uint32_t kNames = 0x3F89E0;       // character names, 16 bytes per character, name pointer first
// a fighter object's fields (player record + 0x58)
constexpr uint32_t kTask = 0x20, kTaskId = 0x24;  // its task (pointer, id); a task's function is at +0x30
constexpr uint32_t kContact = 0x1BC;              // its strike's result: -1 none, 1 hit, 2 blocked, 3 armed again
constexpr uint32_t kBusyUntil = 0x1D8;            // no new move before this game frame
constexpr uint32_t kPhase = 0x20C;                // sub_0014AE00's attack: 1 startup, 2 hit window, 3 recovery
constexpr uint32_t kState = 0x214;                // 0 stance, 0x1200 attack, 0x60x hit reaction, 0xA0x block ...
constexpr uint32_t kControls = 0x21C;             // the port its actions are read from (sub_0008A210)
constexpr int kActionBlock = 1, kActionDown = 0xE;  // sub_0008A210's actions: block, crouch

uint32_t FighterObject(int side) { return kernel::R32((side ? kP2Record : kP1Record) + 0x58); }

bool Practice() {
    return (GameMode() == kModePractice || (mk3d::framedata::VersusToo() && GameMode() == kModeVersus)) &&
           !InFrontEnd() && FightSetups() > 0;
}

uint32_t Time() { return kernel::R32(kGameFrames); }

bool Read(int side, mk3d::framedata::Fighter& f) {
    const uint32_t record = side ? kP2Record : kP1Record;
    const uint32_t obj = kernel::R32(record + 0x58);
    const uint32_t select = kernel::R32(record + 8);
    if (!obj || !mem::IsCommitted(obj) || (select != 2 && select != 3)) return false;
    const uint32_t state = kernel::R32(obj + kState), now = Time(), busy = kernel::R32(obj + kBusyUntil);
    const bool alive = kernel::R32(record + 0xC) != 0;  // health, a float: 0 is knocked out
    // sub_0014A070, which the command task asks before it starts a move on the fighter's task in this frame; holding
    // block, as if it let go at the first chance
    const uint32_t task = kernel::R32(obj + kTask);
    const uint32_t routine = task && mem::IsCommitted(task) && kernel::R32(task + 4) == kernel::R32(obj + kTaskId)
                                 ? kernel::R32(task + 0x30)
                                 : 0;
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
    const uint32_t id = kernel::R32(side ? kP2Character : kP1Character);
    if (id >= 40) return nullptr;
    const uint32_t p = kernel::R32(kNames + id * 16);
    return p ? static_cast<const char*>(kernel::HostPtr(p)) : nullptr;
}

const bool g_installed = [] {
    mk3d::framedata::Install({Practice, Time, Read, Name});
    return true;
}();

}  // namespace
}  // namespace mkd

// The hit test: every strike loop (sub_00071880, sub_00071E90, the special moves' sub_00146400..sub_001472C0) tests the
// attack of the fighter whose task runs against the other one here, once a frame while the attack is live and has not
// made contact yet.
X86_FUNC(sub_00071730) {
    if (mk3d::framedata::Following()) {
        const uint32_t self = kernel::R32(mkd::kSelf);
        if (self && self == mkd::FighterObject(0)) mk3d::framedata::ActiveCheck(0);
        else if (self && self == mkd::FighterObject(1)) mk3d::framedata::ActiveCheck(1);
    }
    __imp__sub_00071730(ctx, base);
}

// Whether a port holds an action (sub_0008A210(port, action)), which every block check asks for the blocking fighter
// (sub_00075610: its own block, the hit's victim's). No controller drives the practice dummy, so with F8 this answers
// for it: block held (and down, crouching).
X86_FUNC(sub_0008A210) {
    if (mk3d::framedata::DummyBlock() != mk3d::framedata::Dummy::Stands) {
        const uint32_t dummy = mkd::FighterObject(1), player = mkd::FighterObject(0);
        const uint32_t port = kernel::R32(ctx.esp + 4), action = kernel::R32(ctx.esp + 8);
        if (dummy && player && port == kernel::R32(dummy + mkd::kControls) &&
            port != kernel::R32(player + mkd::kControls) &&
            mk3d::framedata::DummyHolds(int(action), mkd::kActionBlock, mkd::kActionDown)) {
            ctx.eax = 1;
            ctx.esp += 4;  // ret (the caller pops the arguments)
            return;
        }
    }
    __imp__sub_0008A210(ctx, base);
}
