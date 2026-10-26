// Deadly Alliance as the shared core sees it (recomp/common/src/mk3d/game.h): its name for environment variables, and
// the bytes rollback leaves to other threads from the start.
#include "mk3d/game.h"
#include "mk3d/rollback.h"

namespace mk3d {

const char* EnvPrefix() { return "MKDA"; }

}  // namespace mk3d

namespace mkda {
namespace {

const bool g_registered = [] {
    // the D3D device (XDK 4721, static at 0x1E10E0): what the vertical blank thread keeps, VBlankCallback (+0x2550),
    // the VBlankEvent (+0x2554) the main thread also clears, DisplayFlags (+0x2574), VBlankCount (+0x2580), Field
    // (+0x2590), up to the gamma ramps (+0x259C); xdk/4721/layout.toml
    mk3d::rb::AddFixedVolatile(0x1E10E0 + 0x2550, 0x259C - 0x2550);
    return true;
}();

}  // namespace
}  // namespace mkda
