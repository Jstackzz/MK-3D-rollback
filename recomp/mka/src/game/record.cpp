// Armageddon's side of the practice record (mk3d/record.h; research/mka/notes.md, "Practice record"): the dummy made a
// player the game reads controls for, on port 1, whose input block this writes from the player's own port or a slot.
//
// A fighter's actions are read from the port at its +0x390 (sub_000A0FD0(port, action)): the port's block must be
// open (flags bit 1), drive a player record (+8) in state 3 (sub_0007ABB0: the state practice gives its dummy), and
// the action's bits come from the port's button table (+4, the player's own configuration). The practice dummy's
// port is -1, and port 1's block drives no record, so no controls reach it. Driven, the dummy's port is 1, port 1's
// block is open on the dummy's record with port 0's button table, and this writes its words right after the game's
// pad update (which only touches ports with a controller): previous, current, new presses and the sticks, as the
// pad update would. Recording moves port 0's current buttons over, but START and BACK (the pause), which player 1
// keeps.
#include "mk3d/record.h"

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "game/events.h"
#include "game/frame.h"
#include "game/netgame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mka {
namespace {

constexpr uint32_t kGameFrames = 0x829304;  // the engine's frame counter, still while paused
constexpr uint32_t kSecondPause = 0x7350B0;  // the Xbox Live session code's own pause
constexpr uint32_t kControls = 0x390;       // a fighter's port, -1 none
constexpr uint32_t kFighterRecord = 0x18;   // a fighter's player record
constexpr uint32_t kRecordControl = 0x34;   // a record's control object (its position: x +0xC0, z +0xC8; flags +0xC)
constexpr uint32_t kFighterOpponentControl = 0x14;  // a fighter's opponent's control object
constexpr uint32_t kFighterMirrored = 0xB54;  // sub_00040990's +0xB1C block, its +0x38: bit 3 turns the walk around
constexpr uint32_t kCameras = 0x5B74BC;     // sub_000573B0(0): the camera object (x +0xC0, z +0xC8)
constexpr uint32_t kNeutralSticks = 0x807F807F;
// the button table's entries for the pad's buttons (research/mka/notes.md: 16 of 12 bytes, the logical bits first)
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

bool g_driven = false;
struct Saved {
    uint32_t flags, table, record, controls, state;
} g_saved;

void Engage(bool on) {
    const uint32_t dummy = Dummy(), p1 = Port(1);
    if (on == g_driven) return;
    if (on) {
        if (!dummy || !mem::IsCommitted(dummy)) return;
        g_saved = {kernel::R32(p1 + kPortFlags), kernel::R32(p1 + kPortButtonMap), kernel::R32(p1 + kPortRecord),
                   kernel::R32(dummy + kControls), kernel::R32(kP2Record + kRecordState)};
        kernel::W32(p1 + kPortFlags, g_saved.flags | 2);
        kernel::W32(p1 + kPortButtonMap, Table(0));
        kernel::W32(p1 + kPortRecord, kP2Record);
        kernel::W32(dummy + kControls, 1);
        kernel::W32(kP2Record + kRecordState, 3);
        LOGR_FIRST(1, "record: the dummy on port 1 (its port was %d, its state %u)", int32_t(g_saved.controls),
                   g_saved.state);
    } else {
        kernel::W32(p1 + kPortFlags, g_saved.flags);
        kernel::W32(p1 + kPortButtonMap, g_saved.table);
        kernel::W32(p1 + kPortRecord, g_saved.record);
        if (dummy && mem::IsCommitted(dummy)) kernel::W32(dummy + kControls, g_saved.controls);
        kernel::W32(kP2Record + kRecordState, g_saved.state);
        kernel::W32(p1 + kPortPrevious, 0);
        kernel::W32(p1 + kPortCurrent, 0);
        kernel::W32(p1 + kPortPressed, 0);
        kernel::W32(p1 + kPortSticks, kNeutralSticks);
    }
    g_driven = on;
}

uint32_t Time() { return kernel::R32(kGameFrames); }

bool Paused() { return ((kernel::R32(kFlags) >> 13) & 1) || kernel::R32(kSecondPause); }

// the game's rule for which way the dummy walks on left (sub_00089890 for the fighter whose task runs): its mirrored
// flag, turned by its mirrored-walk bit, and by which side of the camera's line to it the opponent is on; true when
// left walks it forward, measured: a dummy on the right of player 1 faces left
bool DummyFacesLeft() {
    const uint32_t dummy = Dummy();
    if (!dummy || !mem::IsCommitted(dummy)) return false;
    const uint32_t record = kernel::R32(dummy + kFighterRecord);
    const uint32_t me = record && mem::IsCommitted(record) ? kernel::R32(record + kRecordControl) : 0;
    const uint32_t other = kernel::R32(dummy + kFighterOpponentControl), cam = kernel::R32(kCameras);
    if (!me || !other || !cam || !mem::IsCommitted(me) || !mem::IsCommitted(other) || !mem::IsCommitted(cam))
        return false;
    const auto f32 = [](uint32_t va) {
        float f;
        const uint32_t v = kernel::R32(va);
        std::memcpy(&f, &v, 4);
        return f;
    };
    const float mx = f32(me + 0xC0), mz = f32(me + 0xC8), ox = f32(other + 0xC0), oz = f32(other + 0xC8),
                cx = f32(cam + 0xC0), cz = f32(cam + 0xC8);
    const float side = (mx - cx) * -(oz - cz) - (ox - cx) * -(mz - cz);
    // the game's word is true when right is forward: the dummy then faces right
    bool right = (kernel::R32(me + 0xC) >> 17) & 1;
    if (kernel::R32(dummy + kFighterMirrored) & 8) right = !right;
    if (side > 0) right = !right;
    return !right;
}

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
    const uint32_t prev = kernel::R32(p1 + kPortCurrent);
    kernel::W32(p1 + kPortPrevious, prev);
    kernel::W32(p1 + kPortCurrent, word);
    kernel::W32(p1 + kPortPressed, (word ^ prev) & word);
    kernel::W32(p1 + kPortSticks, sticks);
    out = {word, sticks};
}

const bool g_installed = [] {
    mk3d::record::Install({Time, Paused, DummyFacesLeft, DriveDummy, kLeftBits, kRightBits, kNeutralSticks});
    return true;
}();

}  // namespace
}  // namespace mka
