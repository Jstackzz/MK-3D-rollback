// Deadly Alliance's strings that are made to land whole (combos.cpp).
#pragma once

#include "cpu/x86_ops.h"

namespace mkda::combos {

// a strike's hit test is about to run: sub_000547B0(kind, scale, reach), its arguments on the guest stack
void BeforeHitTest(X86Context& ctx);

}  // namespace mkda::combos
