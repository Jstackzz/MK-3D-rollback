// The shared helpers on top of what each game provides (mk3d/game.h).
#include "mk3d/game.h"

#include <cstdlib>
#include <string>

namespace mk3d {

const char* Env(const char* name) {
    std::string key = std::string(EnvPrefix()) + "_" + name;
    return std::getenv(key.c_str());
}

}  // namespace mk3d
