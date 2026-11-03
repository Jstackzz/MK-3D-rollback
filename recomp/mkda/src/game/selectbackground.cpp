// Deadly Alliance's character select screen: a picture as its background in place of its orange clouds
// (select_background.png beside the game, from recomp/mkda/data), and no fire along the bottom of the screen.
//
// The select screen (main task sub_0010C640) draws its background first: two layers of a 128x128 P8 cloud texture on
// a 63-vertex strip that the game tints orange, the second blended over the first. The first becomes the picture
// (the renderer draws it over the whole target, runtime patch 0041), the second is not drawn. The fire is a row of
// quads along the bottom edge, each a frame of a 128x128 P8 animation; those are not drawn. The portraits, the grid
// and the text draw as before. Without the picture the clouds stay.
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "app/app.h"
#include "core/log.h"
#include "game/events.h"
#include "game/frame.h"
#include "gpu/state.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace mkda {
namespace {

constexpr uint32_t kCloudPrim = 6, kCloudCount = 63;
constexpr uint32_t kQuadPrim = 8;  // D3DPT_QUADLIST
// a texture header's format word: the colour format in bits 8-15, log2 of the width and height in 20-23 and 24-27
constexpr uint32_t kTextureFormat = 0xC, kFmtP8 = 0x0B, kLog2Size = 7;
// the fire's quads reach the bottom of the 640x480 screen from below its middle
constexpr float kScreenBottom = 470, kBelowMiddle = 300;

bool g_picture = false;  // select_background.png is there
uint32_t g_layers = 0;   // the select screen's cloud layers drawn this frame

void EachFrame(uint64_t) { g_layers = 0; }

// a 128x128 P8 texture: the clouds', the fire's frames, one tile of a portrait
bool Texture128P8(uint32_t header) {
    if (!header) return false;
    const uint32_t f = kernel::R32(header + kTextureFormat);
    return (f >> 8 & 0xFF) == kFmtP8 && (f >> 20 & 0xF) == kLog2Size && (f >> 24 & 0xF) == kLog2Size;
}

// a quad of the fire, by its vertices' screen positions (the select screen's quads come pretransformed, in pixels):
// 128x128 at the bottom (0,352 to 640,480 in five), where the screen's other 128x128 quad is a piece of the grid's
// frame at its top
bool Fire(const gpu::State& s, const uint8_t* vertices, uint32_t stride) {
    const uint32_t at = (s.vs_handle & 1) && s.vs && s.vs->inputs[0].stream == 0 ? s.vs->inputs[0].offset : 0;
    if (!vertices || at + 8 > stride) return false;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t v = 0; v < 4; v++) {
        float xy[2];
        std::memcpy(xy, vertices + v * stride + at, sizeof(xy));
        x0 = std::min(x0, xy[0]);
        x1 = std::max(x1, xy[0]);
        y0 = std::min(y0, xy[1]);
        y1 = std::max(y1, xy[1]);
    }
    const bool fire = y1 >= kScreenBottom && y0 >= kBelowMiddle;
    if (fire)
        LOGR_FIRST(5, "select screen: its fire is not drawn (%08X at %.0f,%.0f to %.0f,%.0f)", s.textures[0], x0, y0, x1,
                   y1);
    return fire;
}

int Filter(const gpu::State& s, uint32_t prim, uint32_t count, const uint8_t* vertices, uint32_t stride) {
    const bool cloud = prim == kCloudPrim && count == kCloudCount, quad = prim == kQuadPrim && count == 4;
    if (!cloud && !quad) return d3d::kDrawAsIs;
    if (kernel::R32(kMainTask) != kSelectTask || !Texture128P8(s.textures[0])) return d3d::kDrawAsIs;
    if (cloud) return !g_picture ? d3d::kDrawAsIs : g_layers++ == 0 ? d3d::kDrawImage : d3d::kDrawSkip;
    return Fire(s, vertices, stride) ? d3d::kDrawSkip : d3d::kDrawAsIs;
}

const bool g_installed = [] {
    std::error_code ec;
    g_picture = std::filesystem::exists(app::ExeDir() / "select_background.png", ec);
    mk3d::OnFrameStart(&EachFrame);
    d3d::SetDrawFilter(&Filter);
    return true;
}();

}  // namespace
}  // namespace mkda
