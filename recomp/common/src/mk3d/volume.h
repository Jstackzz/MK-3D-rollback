#pragma once
// The launcher's sound levels (Settings, SOUND; MK3D Online 1.0.7), each a percent of the game's own level (100: as
// the game plays it). The master scales everything the speakers get, movies too (the runtime's output voice, patch
// 0049); the others are the game's own sound categories, five floats of its options (music, environment, announcer,
// effects, fighter voices) that every sound the game starts is multiplied by: written at the top of every frame while
// they differ from the levels, so they hold over the game's own writes (its defaults, a profile's options). A sound
// that already plays keeps its level; the next one has the new level (a fight's music starts again every round).
//
// Online the two players' floats may differ (each has their own levels, as each had their own options before): no
// game decision hangs on them, and 0 % is written as a whisper above 0, so no "silent: skip it" test can take another
// path on one game than on the other.
#include <cstdint>

namespace mk3d::volume {

enum Level { kMaster, kMusic, kEnvironment, kAnnouncer, kEffects, kVoices, kLevels };

// the level's key in the launcher's display file and settings ("master", "music"...)
const char* Name(Level level);
// a level from the launcher (any thread): percent, 100 the game's own
void Set(Level level, int percent);

// the game's category floats: music, environment, announcer, effects and fighter voices at va (4 bytes apart), the
// game's own level for all five (its defaults); once, before the first frame
void Install(uint32_t va, float base);

}  // namespace mk3d::volume
