// The costumes' other colours (colours.h).
#include "game/colours.h"

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/events.h"
#include "game/pads.h"
#include "kernel/kernel.h"

namespace mkd::colours {
namespace {

constexpr uint32_t kRecordState = 0x8, kRecordCostume = 0x14, kRecordCharacter = 0x54;
constexpr uint32_t kRecordStride = 0x6C;
constexpr uint32_t kPracticePlayer = 0x46CDB4;  // practice: the side whose player picks both fighters
constexpr uint32_t kModePractice = 4;
constexpr uint32_t kColoursBit = 2;

}  // namespace

void SelectEnds() {
    const uint32_t c0 = kernel::R32(kP1Record + kRecordCharacter), c1 = kernel::R32(kP2Record + kRecordCharacter);
    const uint32_t w0 = kernel::R32(kP1Record + kRecordCostume), w1 = kernel::R32(kP2Record + kRecordCostume);
    if (c0 != c1 || (w0 & 3u) != (w1 & 3u)) return;
    kernel::W32(kP2Record + kRecordCostume, w1 ^ kColoursBit);
    LOGR("colours: both players have character %u in the same costume and colours: P2 the other colours", c0);
}

}  // namespace mkd::colours

using namespace mkd::colours;

// A pick (sub_0017AFA0(record)): the character under the cursor, the second costume with START held (bit 0), the
// mirror rule (bit 1 for the second of two players picking the same fighter). LT held as it is taken: the costume's
// other colours. The port that confirmed: the record's own, in practice the picking player's (as the pick reads START).
X86_EXTERN_FUNC(__imp__sub_0017AFA0);
X86_FUNC(sub_0017AFA0) {
    const uint32_t record = kernel::R32(ctx.esp + 4);
    const uint32_t before = record ? kernel::R32(record + kRecordState) : 0;
    __imp__sub_0017AFA0(ctx, base);
    if (!record || before >= 2 || kernel::R32(record + kRecordState) < 2) return;  // no new pick
    uint32_t port = kernel::R32(record);
    if (kernel::R32(mkd::kGameMode) == kModePractice)
        port = kernel::R32(mkd::kP1Record + kernel::R32(kPracticePlayer) * kRecordStride);
    if (port > 3 || !mkd::SelectLT(int(port))) return;  // LT, kept from the screen (pads.h)
    kernel::W32(record + kRecordCostume, kernel::R32(record + kRecordCostume) | kColoursBit);
    LOGR("colours: P%u picked holding LT: the costume's other colours (costume flags %X)",
         record == mkd::kP2Record ? 2u : 1u, kernel::R32(record + kRecordCostume));
}
