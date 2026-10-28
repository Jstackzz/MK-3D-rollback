// Shujinko's moves.
//
// Shujinko learns most of his special moves and both fatalities in Konquest, and a fight asks for each one whether
// the player has (sub_00170240(fighter, move): only for characters 0x19 and 0x1A, the two Shujinkos; the eleven moves
// sub_00170150 numbers, 0x4219.. specials and the fatalities 0x1203 and 0x1205, are looked up in the side's record
// 0x5FDF60 / 0x5FD980 by sub_00173140(record, 4, index); every other move and character passes). The game's six
// callers are the move inputs, the move list and the fatality checks. Online matches and practice open every move,
// in both games alike (autopilot::MovesOpen): there is no Konquest progress to read there, and a player whose profile
// had some would have had moves the other game refused.
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/autopilot.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00170240);

namespace {

// each move opened that this profile never learned, once: what a player's log shows when they ask
void Opened(uint32_t move) {
    static uint32_t logged[16];
    static size_t n = 0;
    for (size_t i = 0; i < n; i++)
        if (logged[i] == move) return;
    if (n < 16) logged[n++] = move;
    LOGR("moves: Shujinko's move %04X open (this profile has not learned it in Konquest)", move);
}

}  // namespace

X86_FUNC(sub_00170240) {
    if (!mkd::autopilot::MovesOpen()) return __imp__sub_00170240(ctx, base);
    static bool told = false;
    if (!told) {
        told = true;
        LOGR("moves: every move open, Shujinko's Konquest specials and fatalities included");
    }
    const uint32_t move = kernel::R32(ctx.esp + 8);
    // the profile's answer, for the log alone: a lookup that writes nothing but its own stack, so the two games of a
    // match (two profiles) stay alike; it returns, the return address off the stack
    __imp__sub_00170240(ctx, base);
    if (!ctx.eax) Opened(move);
    ctx.eax = 1;
}
