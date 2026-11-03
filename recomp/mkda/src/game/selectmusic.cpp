// The character select screen's music: Konquest's second battle theme, in place of the select screen's own.
//
// Deadly Alliance plays its sounds by number, sub_00123A20(n): a table at 0x275140, 0x1C bytes a number, gives each
// its bank (an index into the bank names at 0x27F5F8), its cue in the bank, its volume and its volume group, and the
// bank has to be loaded (sub_00124090(bank, slot) loads one into a slot, sub_001242E0(slot) empties a slot; 0x2EABF0
// holds each loaded bank's handle, 0x2EAD80 each slot's). The select screen (sub_0010C640) starts its theme with
// sound 0x469, the menu bank's (shell) cue 1 in the menus' music group. Konquest's music is bank 8 (music_konquest):
// sounds 0x4A9, 0x4AA and 0x4AB, its cues 0x0F, 0x10 and 0x11, are its second battle theme.
//
// Here the select screen's theme first loads bank 8 into the music slot (0: a fight's music, unloaded by the next
// fight before it loads its own, sub_00124760), then sound 0x469 plays bank 8's cue with 0x469's own volume and
// group (the menus' music volume, the select screen's own fades). Without bank 8, 0x469 stays the select screen's
// own theme.
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00123A20);

namespace mkda::selectmusic {
namespace {

constexpr uint32_t kSelectTheme = 0x469;
constexpr uint32_t kSounds = 0x275140, kSoundStride = 0x1C;  // a sound number's bank at +0, its cue at +4
constexpr uint32_t kBankHandles = 0x2EABF0, kSlotHandles = 0x2EAD80;
constexpr uint32_t kLoadBank = 0x124090, kEmptySlot = 0x1242E0;
constexpr uint32_t kShellBank = 0, kShellTheme = 1;       // the select screen's own theme
constexpr uint32_t kKonquestBank = 8, kBattleTwo = 0x11;  // Konquest's second battle theme
constexpr uint32_t kMusicSlot = 0;

}  // namespace
}  // namespace mkda::selectmusic

// a sound by number: the select screen's theme becomes Konquest's second battle theme
X86_FUNC(sub_00123A20) {
    using namespace mkda::selectmusic;
    if (kernel::R32(ctx.esp + 4) == kSelectTheme) {
        if (!kernel::R32(kBankHandles + kKonquestBank * 4)) {
            if (kernel::R32(kSlotHandles + kMusicSlot * 4)) kernel::CallGuest(ctx, kEmptySlot, {kMusicSlot});
            kernel::CallGuest(ctx, kLoadBank, {kKonquestBank, kMusicSlot});
        }
        const bool konquest = kernel::R32(kBankHandles + kKonquestBank * 4) != 0;
        const uint32_t entry = kSounds + kSelectTheme * kSoundStride;
        kernel::W32(entry, konquest ? kKonquestBank : kShellBank);
        kernel::W32(entry + 4, konquest ? kBattleTwo : kShellTheme);
        LOGR_FIRST(4, "select music: %s", konquest ? "Konquest's second battle theme"
                                                   : "its own (Konquest's music bank did not load)");
    }
    __imp__sub_00123A20(ctx, base);
}
