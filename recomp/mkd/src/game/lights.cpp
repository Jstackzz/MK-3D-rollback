// An arena area's lights without a crash when one of them is gone.
//
// sub_0002D210 makes an area of the arena's data (0x6C bytes each) the current one and sets its lights with
// sub_000DEFF0(list, group): the list's three entries are each empty or a light setting, whose kind (1 or 3) names a
// light of the scene's light group to set, found by sub_000DF200(group, kind, n) (the group's n-th light of the kind,
// 0 when there is none; the group holds weak references and drops the ones whose light is gone). sub_000DEFF0 reads
// the light's +8 without asking whether there was one, the only caller that does not (sub_000DFA60 asks), so a group
// without the light read guest address 8: a crash on a player's PC in 0.1.20 (MKD.dll+20508E, outside a match).
//
// Here the same walk is made first with lookups only, and when a light it would set is not there the area's lights
// stay as they are.
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_000DEFF0);

namespace mkd::lights {
namespace {

constexpr uint32_t kFindLight = 0xDF200;  // sub_000DF200(group, kind, n)

bool Readable(uint32_t va) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + 3); }

// whether every light sub_000DEFF0(list, group) sets is there: its own walk (three steps; a setting it cannot apply
// takes a step without moving on, kinds 2, 4 and 5 always), the lookups only
bool LightsThere(X86Context& ctx, uint32_t list, uint32_t group) {
    uint32_t entry = list, nth = 0;  // nth: kind 3 settings applied so far, the next one's light
    for (int step = 0; step < 3; step++) {
        if (!Readable(entry)) return false;
        const uint32_t setting = kernel::R32(entry);
        if (!setting) {
            entry += 4;
            continue;
        }
        if (!Readable(setting)) return false;
        const uint32_t kind = kernel::R32(setting);
        if (kind == 1 || kind == 3) {
            const uint32_t light = kernel::CallGuest(ctx, kFindLight, {group, kind, kind == 3 ? nth : 0});
            if (!light || !Readable(light + 8)) return false;
            if (kernel::R32(light + 8)) {
                if (kind == 3) nth++;
                entry += 4;
            }
        } else if (kind < 2 || kind > 5) {
            entry += 4;
        }
    }
    return true;
}

}  // namespace
}  // namespace mkd::lights

X86_FUNC(sub_000DEFF0) {
    const uint32_t list = kernel::R32(ctx.esp + 4), group = kernel::R32(ctx.esp + 8);
    if (!mkd::lights::LightsThere(ctx, list, group)) {
        LOGR_FIRST(4, "lights: an arena area's lights left as they were (list %08X, group %08X): a light is gone",
                   list, group);
        ctx.esp += 4;  // ret (cdecl: the caller takes its arguments off)
        return;
    }
    __imp__sub_000DEFF0(ctx, base);
}
