// The costumes' other colours (colours.h).
#include "game/colours.h"

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/events.h"
#include "game/pads.h"
#include "kernel/kernel.h"

namespace mka::colours {
namespace {

constexpr uint32_t kAlternate[2] = {0x24, 0x58}, kColours[2] = {0x28, 0x5C};  // the select block's flags

}  // namespace

void SelectEnds() {
    const uint32_t block = SelectBlock();
    if (!block) return;
    const uint32_t c0 = kernel::R32(kP1Character), c1 = kernel::R32(kP2Character);
    uint32_t costume[2];
    for (int s = 0; s < 2; s++)
        costume[s] = (kernel::R32(block + kAlternate[s]) ? 1u : 0u) | (kernel::R32(block + kColours[s]) ? 2u : 0u);
    if (c0 != c1 || costume[0] != costume[1]) return;
    kernel::W32(block + kColours[1], (costume[1] & 2u) ? 0u : 1u);
    LOGR("colours: both players have character %u in the same costume and colours: P2 the other colours", c0);
}

}  // namespace mka::colours

// A pick (sub_00207930(record), the record whose player confirmed; in practice the second pick goes to the dummy's):
// the second costume with START held, the mirror rule's colours for the second of two players picking the same
// fighter. LT held as it is taken: the costume's other colours.
X86_EXTERN_FUNC(__imp__sub_00207930);
X86_FUNC(sub_00207930) {
    using namespace mka;
    const uint32_t record = kernel::R32(ctx.esp + 4);
    const uint32_t before[2] = {kernel::R32(kP1Record + kRecordState), kernel::R32(kP2Record + kRecordState)};
    __imp__sub_00207930(ctx, base);
    const uint32_t block = SelectBlock();
    if (!block || !record) return;
    for (int s = 0; s < 2; s++) {
        const uint32_t rec = s ? kP2Record : kP1Record;
        if (before[s] >= 2 || kernel::R32(rec + kRecordState) < 2) continue;  // not picked now
        uint32_t port = kernel::R32(record);
        if (port > 3) port = kernel::R32(rec);
        if (port > 3 || !SelectLT(int(port))) continue;  // LT, kept from the screen (pads.h)
        kernel::W32(block + colours::kColours[s], 1);
        LOGR("colours: P%d picked holding LT: the costume's other colours", s + 1);
    }
}
