#pragma once
// The engine's tasks on snapshot-able fibers (task_fibers.cpp) implement the shared interface recomp/common's
// rollback saves them through (mk3d/tasks.h), here under Armageddon's name.
#include "mk3d/tasks.h"

namespace mka {
namespace tasks = mk3d::tasks;
}
