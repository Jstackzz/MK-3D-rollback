#pragma once
// The engine's tasks on snapshot-able fibers (task_fibers.cpp) implement the shared interface recomp/common's
// rollback saves them through (mk3d/tasks.h), here under Deception's name.
#include "mk3d/tasks.h"

namespace mkd {
namespace tasks = mk3d::tasks;
}
