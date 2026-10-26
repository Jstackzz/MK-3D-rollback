#pragma once
// The engine's task system on snapshot-able fibers (each game's task_fibers.cpp, on mk3d/sfiber.h): the parked state
// of every task can be saved and put back at the top of the main loop, where no task runs. Rollback (rollback.h)
// keeps one per frame.
#include <cstdint>
#include <utility>
#include <vector>

#include "mk3d/sfiber.h"

namespace mk3d::tasks {

struct Snapshot {
    sfiber::Snapshot fibers;
    std::vector<std::pair<uint32_t, int>> tasks;  // task -> fiber slot
    std::vector<uint8_t> records;                  // the fixed per-slot task records
};
void Save(Snapshot& snap);
void Load(const Snapshot& snap);
int Alive();

}  // namespace mk3d::tasks
