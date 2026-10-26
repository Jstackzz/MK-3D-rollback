// Armageddon as the shared core sees it (recomp/common/src/mk3d/game.h): its name for environment variables, and the
// bytes rollback leaves to other threads from the start.
#include "mk3d/game.h"
#include "mk3d/rollback.h"

namespace mk3d {

const char* EnvPrefix() { return "MKA"; }

}  // namespace mk3d

namespace mka {
namespace {

const bool g_registered = [] {
    // the D3D device (0x427AF0, XDK 5849 like Deception's): what the vertical blank thread keeps, VBlankCallback
    // (+0x1DB8), the VBlankEvent (+0x1DBC) the main thread also clears, DisplayFlags, VBlankCount (+0x1DE8), Field
    // (+0x1DF8); the palette slots and the render targets share the page
    mk3d::rb::AddFixedVolatile(0x427AF0 + 0x1DB8, 0x1E04 - 0x1DB8);
    // the texture header 0x71F700 that sub_0027C910 builds over the current back buffer every frame
    // (XGSetTextureHeader): its data address (+4) follows the swap chain, which a run again does not put back
    mk3d::rb::AddFixedVolatile(0x71F700, 0x14);
    return true;
}();

}  // namespace
}  // namespace mka
