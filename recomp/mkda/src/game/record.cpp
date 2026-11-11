// Deadly Alliance's side of the practice record (mk3d/record.h; research/mkda-rev1/notes.md, "Practice record"): the
// dummy made a player the game reads controls for, whose input block this writes from the player's own or a slot.
//
// The pad update keeps logical buttons per player, and a fighter's actions are read for its player number
// (sub_00075B50(player, action), new presses sub_00075CA0): player 2's current word 0x2CE780 against its button
// configuration 0x42C560, unless 0x2CE7BC says the player has no controls, which practice's fight setup sets for the
// dummy along with its player state 0, and the move gate sub_000EA250 starts no move for a player whose state is
// not 3 (picked) or 4 (a demo). Port 1 would also write the block, but only once a controller on it joins. Driven,
// the dummy is a picked player with controls, reading player 1's configuration (so a remapped button means the same
// to both), and this writes its block right after the game's pad update: previous, current, new presses and the
// sticks, as the pad update would. Recording moves player 1's current buttons over, but START and BACK (the pause),
// which player 1 keeps.
#include "mk3d/record.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "game/events.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace mkda {
namespace {

constexpr uint32_t kNoControls[2] = {0x2CE7B8, 0x2CE7BC};  // sub_00075B50 reads nothing for the player while set
constexpr uint32_t kConfig[2] = {0x42C640, 0x42C560};      // each player's button configuration: 17 actions of 12 bytes
constexpr uint32_t kConfigWords = 17 * 3;
constexpr uint32_t kControlObject = 0x28, kControlObjectId = 0x2C;  // a fighter's control object (x +0x84, z +0x8C,
                                                                    // flags +8), its id after the pointer
constexpr uint32_t kOpponentControl = 0x14;  // a fighter's opponent's control object
constexpr uint32_t kCamera = 0x42CBF4;       // the camera object (x +0x84, z +0x8C)
constexpr uint32_t kNeutralSticks = 0x807F807F;
// the logical bits of the button map (frame.h): START and BACK pause, LEFT and RIGHT walk
constexpr uint32_t kStart = 0x800, kBack = 0x100, kLeft = 0x8000, kRight = 0x2000;
constexpr uint32_t kStatePicked = 3;

uint32_t Block(int player) { return kPlayerBlock + uint32_t(player) * kPlayerStride; }
uint32_t Sticks(int player) { return kPlayerSticks + uint32_t(player) * 4; }

bool g_driven = false;
struct Saved {
    uint32_t state, no_controls;
    uint32_t config[kConfigWords];
} g_saved;

void Engage(bool on) {
    if (on == g_driven) return;
    if (on) {
        if (!kernel::R32(kFighter[1])) return;
        g_saved.state = kernel::R32(kPlayerState[1]);
        g_saved.no_controls = kernel::R32(kNoControls[1]);
        for (uint32_t i = 0; i < kConfigWords; i++) {
            g_saved.config[i] = kernel::R32(kConfig[1] + i * 4);
            kernel::W32(kConfig[1] + i * 4, kernel::R32(kConfig[0] + i * 4));
        }
        kernel::W32(kPlayerState[1], kStatePicked);
        kernel::W32(kNoControls[1], 0);
        LOGR_FIRST(1, "record: the dummy reads player 2's controls (its state was %u, no controls %u)", g_saved.state,
                   g_saved.no_controls);
    } else {
        kernel::W32(kPlayerState[1], g_saved.state);
        kernel::W32(kNoControls[1], g_saved.no_controls);
        for (uint32_t i = 0; i < kConfigWords; i++) kernel::W32(kConfig[1] + i * 4, g_saved.config[i]);
        kernel::W32(Block(1) + kPlayerPrevious, 0);
        kernel::W32(Block(1) + kPlayerCurrent, 0);
        kernel::W32(Block(1) + kPlayerPressed, 0);
        kernel::W32(Sticks(1), kNeutralSticks);
    }
    g_driven = on;
}

uint32_t Time() { return kernel::R32(kGameCounter); }

bool Paused() { return kernel::R32(kPaused) != 0; }

// Which way the dummy faces, as the game's walk sees it: its stance routine (sub_000BFCC0) walks a fighter forward on
// LEFT when sub_000543F0 (the cross product of the camera's lines to the fighter and to its opponent) is above 0 and on
// RIGHT below it, nothing else. Measured: above 0 for the dummy at a practice start, on the right facing left; below
// after player 1 jumps over it. (The sidestep's rule, sub_00057B60, also takes the control object's flag bit 17, which
// changes as a fighter turns round: no use here.) At exactly 0 the last answer stands.
bool DummyFacesLeft() {
    static bool left = true;
    const uint32_t dummy = kernel::R32(kFighter[1]);
    if (!dummy || !mem::IsCommitted(dummy)) return left;
    const uint32_t me = kernel::R32(dummy + kControlObject), other = kernel::R32(dummy + kOpponentControl),
                   cam = kernel::R32(kCamera);
    if (!me || !other || !cam || !mem::IsCommitted(me) || !mem::IsCommitted(other) || !mem::IsCommitted(cam) ||
        kernel::R32(me + 4) != kernel::R32(dummy + kControlObjectId))
        return left;
    const auto f32 = [](uint32_t va) {
        float f;
        const uint32_t v = kernel::R32(va);
        std::memcpy(&f, &v, 4);
        return f;
    };
    const float mx = f32(me + 0x84), mz = f32(me + 0x8C), ox = f32(other + 0x84), oz = f32(other + 0x8C),
                cx = f32(cam + 0x84), cz = f32(cam + 0x8C);
    const float side = (mx - cx) * -(oz - cz) - (ox - cx) * -(mz - cz);
    if (side > 0) left = true;
    else if (side < 0) left = false;
    return left;
}

// The dummy stands turned away from player 1: the game turns a fighter round (a turn animation, state 0x201) instead of
// walking or striking when the direction to its opponent is more than 2.7 off its yaw; less, it only eases round.
// sub_00056B40 for the fighter whose task runs: sub_00158500 of the opponent's control object less its own (x +0x84, z
// +0x8C), against its yaw +0xA8; the threshold is the double at 0x2326C8. A dummy with no controls never turns by
// itself, so after player 1 jumps over it it stays turned away.
bool DummyTurnedAway() {
    const uint32_t dummy = kernel::R32(kFighter[1]);
    if (!dummy || !mem::IsCommitted(dummy)) return false;
    const uint32_t me = kernel::R32(dummy + kControlObject);
    const uint32_t other = kernel::R32(dummy + kOpponentControl);
    if (!me || !other || !mem::IsCommitted(me) || !mem::IsCommitted(other) ||
        kernel::R32(me + 4) != kernel::R32(dummy + kControlObjectId)) return false;
    const auto f32 = [](uint32_t va) {
        float f;
        const uint32_t v = kernel::R32(va);
        std::memcpy(&f, &v, 4);
        return f;
    };
    const float dx = f32(other + 0x84) - f32(me + 0x84), dz = f32(other + 0x8C) - f32(me + 0x8C);
    if (dx == 0 && dz == 0) return false;
    // the game's atan2 takes x over z (0 along z); the difference as the game wraps it, -pi to pi
    const float off = std::remainder(std::atan2(dx, dz) - f32(me + 0xA8), 6.28318531f);
    return std::fabs(off) > 2.7f;
}

// its fighting style (fighter +0x8A0: 0, 1, 2, the weapon last), and the logical bits of the style button (action 2, LT
// by default) in its controls
int DummyStyle() {
    const uint32_t dummy = kernel::R32(kFighter[1]);
    return dummy && mem::IsCommitted(dummy) ? int(kernel::R32(dummy + 0x8A0)) : -1;
}
// player 1's configuration, which the dummy gets a copy of while it is driven
uint32_t StyleBits() { return kernel::R32(kConfig[0] + 2 * 12); }

void DriveDummy(mk3d::record::Drive mode, const mk3d::record::Input& in, mk3d::record::Input& out) {
    using mk3d::record::Drive;
    out = in;
    if (mode == Drive::Off) {
        Engage(false);
        return;
    }
    Engage(true);
    if (!g_driven) return;
    const uint32_t p1 = Block(0), p2 = Block(1);
    uint32_t word = in.buttons, sticks = in.sticks;
    if (mode == Drive::Record) {
        const uint32_t cur = kernel::R32(p1 + kPlayerCurrent), pause = kStart | kBack;
        word = cur & ~pause;
        sticks = kernel::R32(Sticks(0));
        // player 1 keeps its pause buttons, nothing else; its previous word is the pad update's own
        kernel::W32(p1 + kPlayerCurrent, cur & pause);
        kernel::W32(p1 + kPlayerPressed, kernel::R32(p1 + kPlayerPressed) & pause);
    }
    const uint32_t prev = kernel::R32(p2 + kPlayerCurrent);
    kernel::W32(p2 + kPlayerPrevious, prev);
    kernel::W32(p2 + kPlayerCurrent, word);
    kernel::W32(p2 + kPlayerPressed, (word ^ prev) & word);
    kernel::W32(Sticks(1), sticks);
    out = {word, sticks};
}

const bool g_installed = [] {
    mk3d::record::Install({Time, Paused, DummyFacesLeft, DummyTurnedAway, DummyStyle, StyleBits, DriveDummy, kLeft,
                           kRight, kNeutralSticks});
    return true;
}();

}  // namespace
}  // namespace mkda
