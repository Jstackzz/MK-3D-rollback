// Deception as the shared core sees it (recomp/common/src/mk3d/game.h): its name for environment variables, and the
// bytes rollback leaves to other threads from the start.
#include "mk3d/game.h"
#include "mk3d/rollback.h"

namespace mk3d {

const char* EnvPrefix() { return "MKD"; }

}  // namespace mk3d

namespace mkd {
namespace {

const bool g_registered = [] {
    // the D3D device (0x2F8070): what the vertical blank thread keeps, VBlankCallback (+0x1DB8), the VBlankEvent
    // (+0x1DBC) the main thread also clears, DisplayFlags, VBlankCount (+0x1DE8), Field (+0x1DF8); the palette slots
    // (+0xF98) and the render targets (+0x1A04) share the page
    mk3d::rb::AddFixedVolatile(0x2F8070 + 0x1DB8, 0x1E04 - 0x1DB8);
    return true;
}();

}  // namespace
}  // namespace mkd
