// Deception's side of the practice record (mk3d/record.h; research/mkd/notes.md, "Practice record"): the dummy made a
// player the game reads controls for, on port 1, whose input block this writes from the player's own port or a slot.
//
// A fighter's actions are read from the port at its +0x21C (sub_0008A210(port, action)): the port's block must be open
// (flags bit 1) and drive a player record (+8) that sub_0006A2C0 accepts (state 3, the state practice gives its dummy,
// at once; state 2, picked, with the record's own port at +0 driving it), and the action's bits come from the port's
// button table (+4, the player's own configuration). The practice dummy's port is -1, and port 1's block drives no
// record, so no controls reach it. Driven, the dummy's port is 1, port 1's block is open on the dummy's record with
// port 0's button table, and the record is a picked player on port 1 (state 2, port 1, as a versus player 2's): left
// in practice's dummy state the game took the first hit of a string from its controls but never the next ones (1.0.2:
// Scorpion's 2, 2, 1 came out as a 2). This writes the block's words right after the game's pad update (which only
// touches ports with a controller): previous, current, new presses and the sticks, as the pad update would. Recording
// moves port 0's current buttons over, but START and BACK (the pause), which player 1 keeps.
#include "mk3d/record.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "game/events.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mkd {
namespace {

constexpr uint32_t kGameFrames = 0x61C418;    // the engine's frame counter, still while paused
constexpr uint32_t kPortButtonMap = 0x4;      // in a port's block: its button table (the logical bits of each pad button)
constexpr uint32_t kPortRecord = 0x8;         // ... the player record it drives (0 none)
constexpr uint32_t kDefaultButtons = 0x3E95A0;  // the button table a port starts with
constexpr uint32_t kRecordState = 0x8;        // a player record's select state (3: practice's dummy, driven here)
constexpr uint32_t kRecordFighter = 0x58;     // ... its fighter object
constexpr uint32_t kControls = 0x21C;         // a fighter's port, -1 none
constexpr uint32_t kFighterRecord = 0x18;     // a fighter's player record
constexpr uint32_t kRecordControl = 0x5C;     // a record's control object (its position: x +0xA0, z +0xA8; flags +8)
constexpr uint32_t kFighterOpponentControl = 0x14;  // a fighter's opponent's control object
constexpr uint32_t kCamera = 0x6217F4;        // the camera object (x +0xA0, z +0xA8)
constexpr uint32_t kNeutralSticks = 0x807F807F;
// the button table's entries for the pad's buttons (research/mkd/notes.md: 16 of 12 bytes, the logical bits first)
constexpr uint32_t kEntryBack = 8, kEntryStart = 11;
// the default table's bits for LEFT (its entry 15) and RIGHT (13), read from the xbe (no guest memory at static init)
constexpr uint32_t kLeftBits = 0x8000, kRightBits = 0x2000;

uint32_t Port(int port) { return kPortBlock + uint32_t(port) * kPortStride; }
uint32_t Table(int port) {
    const uint32_t t = kernel::R32(Port(port) + kPortButtonMap);
    return t ? t : kDefaultButtons;
}
uint32_t Entry(uint32_t table, uint32_t entry) { return kernel::R32(table + entry * 12); }
uint32_t Dummy() { return kernel::R32(kP2Record + kRecordFighter); }

constexpr uint32_t kRecordPort = 0x0;   // a player record's port, -1 none
constexpr uint32_t kStatePicked = 2;    // a record's state: picked (practice's dummy is 3)

bool g_driven = false;
struct Saved {
    uint32_t flags, table, record, controls, state, port;
} g_saved;
// The dummy's buttons as this last wrote them: its previous word comes from here, never from port 1's block, which the
// pad update writes too when a controller sits on that port (a second pad, or the player's own seen twice, as
// DS4Windows or Steam Input can show it). Read back from there, the player's own presses were never new for the dummy
// (no attack, no string), or a held button was new every frame.
uint32_t g_word = 0;

void Engage(bool on) {
    const uint32_t dummy = Dummy(), p1 = Port(1);
    if (on == g_driven) return;
    if (on) {
        if (!dummy || !mem::IsCommitted(dummy)) return;
        g_saved = {kernel::R32(p1 + kPortFlags), kernel::R32(p1 + kPortButtonMap), kernel::R32(p1 + kPortRecord),
                   kernel::R32(dummy + kControls), kernel::R32(kP2Record + kRecordState),
                   kernel::R32(kP2Record + kRecordPort)};
        kernel::W32(p1 + kPortFlags, g_saved.flags | 2);
        kernel::W32(p1 + kPortButtonMap, Table(0));
        kernel::W32(p1 + kPortRecord, kP2Record);
        kernel::W32(dummy + kControls, 1);
        kernel::W32(kP2Record + kRecordPort, 1);
        kernel::W32(kP2Record + kRecordState, kStatePicked);
        LOGR_FIRST(1, "record: the dummy a picked player on port 1 (its port was %d, its record's %d, its state %u)",
                   int32_t(g_saved.controls), int32_t(g_saved.port), g_saved.state);
    } else {
        kernel::W32(p1 + kPortFlags, g_saved.flags);
        kernel::W32(p1 + kPortButtonMap, g_saved.table);
        kernel::W32(p1 + kPortRecord, g_saved.record);
        if (dummy && mem::IsCommitted(dummy)) kernel::W32(dummy + kControls, g_saved.controls);
        kernel::W32(kP2Record + kRecordState, g_saved.state);
        kernel::W32(kP2Record + kRecordPort, g_saved.port);
        kernel::W32(p1 + kPortPrevious, 0);
        kernel::W32(p1 + kPortCurrent, 0);
        kernel::W32(p1 + kPortPressed, 0);
        kernel::W32(p1 + kPortSticks, kNeutralSticks);
    }
    g_driven = on;
    g_word = 0;
}

uint32_t Time() { return kernel::R32(kGameFrames); }

bool Paused() { return (kernel::R32(kFlags) >> 13) & 1; }

// Which way the dummy faces, as the game's walk sees it: its stance routine (sub_00098B00) walks a fighter one way or
// the other on LEFT and RIGHT by the sign of sub_000712B0 (the cross product of the camera's lines to the fighter and
// to its opponent), nothing else. Measured: above 0 for the dummy at a practice start, on the right facing left; below
// after player 1 jumps over it. (The sidestep's rule, sub_00075360, also takes the control object's flag bit 17 and the
// fighter's +0x5BC bit 3, which change as a fighter turns round: no use here.) At exactly 0 the last answer stands.
bool DummyFacesLeft() {
    static bool left = true;
    const uint32_t dummy = Dummy();
    if (!dummy || !mem::IsCommitted(dummy)) return left;
    const uint32_t record = kernel::R32(dummy + kFighterRecord);
    const uint32_t me = record && mem::IsCommitted(record) ? kernel::R32(record + kRecordControl) : 0;
    const uint32_t other = kernel::R32(dummy + kFighterOpponentControl), cam = kernel::R32(kCamera);
    if (!me || !other || !cam || !mem::IsCommitted(me) || !mem::IsCommitted(other) || !mem::IsCommitted(cam))
        return left;
    const auto f32 = [](uint32_t va) {
        float f;
        const uint32_t v = kernel::R32(va);
        std::memcpy(&f, &v, 4);
        return f;
    };
    const float mx = f32(me + 0xA0), mz = f32(me + 0xA8), ox = f32(other + 0xA0), oz = f32(other + 0xA8),
                cx = f32(cam + 0xA0), cz = f32(cam + 0xA8);
    const float side = (mx - cx) * -(oz - cz) - (ox - cx) * -(mz - cz);
    if (side > 0) left = true;
    else if (side < 0) left = false;
    return left;
}

// The dummy stands turned away from player 1: the game turns a fighter round (a turn animation, state 0x201) instead of
// walking or striking when the direction to its opponent is more than 2.7 off its yaw; less, it only eases round.
// sub_00074170 for the fighter whose task runs: sub_001E1F60 of the opponent's control object less its own (x +0xA0, z
// +0xA8), against its yaw +0xD4, through sub_00094750 (threshold 0x383478). A dummy with no controls never turns by
// itself, so after player 1 jumps over it it stays turned away.
bool DummyTurnedAway() {
    const uint32_t dummy = Dummy();
    if (!dummy || !mem::IsCommitted(dummy)) return false;
    const uint32_t record = kernel::R32(dummy + kFighterRecord);
    const uint32_t me = record && mem::IsCommitted(record) ? kernel::R32(record + kRecordControl) : 0;
    const uint32_t other = kernel::R32(dummy + kFighterOpponentControl);
    if (!me || !other || !mem::IsCommitted(me) || !mem::IsCommitted(other)) return false;
    const auto f32 = [](uint32_t va) {
        float f;
        const uint32_t v = kernel::R32(va);
        std::memcpy(&f, &v, 4);
        return f;
    };
    const float dx = f32(other + 0xA0) - f32(me + 0xA0), dz = f32(other + 0xA8) - f32(me + 0xA8);
    if (dx == 0 && dz == 0) return false;
    // the game's atan2 takes x over z (0 along z); the difference as the game wraps it, -pi to pi
    const float off = std::remainder(std::atan2(dx, dz) - f32(me + 0xD4), 6.28318531f);
    return std::fabs(off) > 2.7f;
}

// its fighting style (fighter +0x2D8: 0, 1, 2, the weapon last), and the logical bits of the style button (action 2, LT
// by default) in its controls
int DummyStyle() {
    const uint32_t dummy = Dummy();
    return dummy && mem::IsCommitted(dummy) ? int(kernel::R32(dummy + 0x2D8)) : -1;
}
// port 0's table, which the dummy's port gets while it is driven
uint32_t StyleBits() { return Entry(Table(0), 2); }

void DriveDummy(mk3d::record::Drive mode, const mk3d::record::Input& in, mk3d::record::Input& out) {
    using mk3d::record::Drive;
    out = in;
    if (mode == Drive::Off) {
        Engage(false);
        return;
    }
    Engage(true);
    if (!g_driven) return;
    const uint32_t p0 = Port(0), p1 = Port(1);
    uint32_t word = in.buttons, sticks = in.sticks;
    if (mode == Drive::Record) {
        const uint32_t table = Table(0), pause = Entry(table, kEntryStart) | Entry(table, kEntryBack);
        const uint32_t cur = kernel::R32(p0 + kPortCurrent);
        word = cur & ~pause;
        sticks = kernel::R32(p0 + kPortSticks);
        // player 1 keeps its pause buttons, nothing else; its previous word is the pad update's own
        kernel::W32(p0 + kPortCurrent, cur & pause);
        kernel::W32(p0 + kPortPressed, kernel::R32(p0 + kPortPressed) & pause);
    }
    const uint32_t prev = g_word;
    kernel::W32(p1 + kPortPrevious, prev);
    kernel::W32(p1 + kPortCurrent, word);
    kernel::W32(p1 + kPortPressed, (word ^ prev) & word);
    kernel::W32(p1 + kPortSticks, sticks);
    g_word = word;
    out = {word, sticks};
}

const bool g_installed = [] {
    mk3d::record::Install({Time, Paused, DummyFacesLeft, DummyTurnedAway, DummyStyle, StyleBits, DriveDummy,
                           kLeftBits, kRightBits, kNeutralSticks});
    return true;
}();

}  // namespace
}  // namespace mkd
