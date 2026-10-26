// XDK 4721 library functions Mortal Kombat: Deadly Alliance calls that the runtime does not implement and whose console
// code cannot run against the runtime's host objects (MK Deadly Alliance Rev 1, XBE sha1 5aba0b06...). Each one is
// replaced here in terms of what the runtime does implement.
//
// D3DDevice_UpdateOverlay 0x1DEAE0: the movie player (sub_001C5D20, the XMV PlayVideo loop) shows each decoded YUY2
//   frame through the video overlay, writing the NV2A's PVIDEO registers through [CDevice + 0x4FC], which is 0 on the
//   host. The frame is converted and scaled into the back buffer here and presented with the runtime's Swap. The
//   vertical blank count is stored at CDevice + 0x2BA8 first, as the library does, so the recompiled
//   D3DDevice_GetOverlayUpdateStatus 0x1DEAC0 (+0x2BA8 against the count +0x2580) keeps pacing the movie loop.
// D3DDevice_DrawIndexedVerticesUP 0x1D4520: pushes the indexed vertices inline (method 0x1818), which the runtime's
//   push buffer walk does not draw. Expanded here into a vertex list for the runtime's DrawVerticesUP 0x1D43C0.
// D3DDevice_GetVertexShaderConstant 0x1D1F10: reads the library's constant shadow at CDevice + 0xC58 (hardware
//   register order), which the runtime's SetVertexShaderConstant* do not write. sub_001C432B saves c-96..c-1 with it.
//   Answered from the runtime's own constants.
// IDirectSoundBuffer_SetConeOrientation 0x1E54E4: the library's code follows the buffer's voice pointer (+0x10 of
//   the object 0x1C below the interface), which the runtime's 0x40 byte buffer objects do not have. The runtime mixes
//   without sound cones; a no-op here.
// CDevice + 0x211C, the device's multisample type: the library's Get2DSurfaceDesc 0x1D71F0 reports it for the back
//   buffer (0x11, D3DMULTISAMPLE_NONE, for any other surface). The runtime's CreateDevice clears the device, so it was
//   0. RenderWare's viewport constants for its own vertex programs (sub_001C3E80: the fighters' skinning, their c-96
//   offset and the projection rows) look the type up in a table of the eleven console types; with 0 the lookup ran
//   past the table and the offset came from the stack, so the fighters were drawn off screen (only their shadows,
//   which go through the 256x256 shadow targets, showed). Kept at 0x11 here: the runtime draws without console
//   multisampling (the game asks for 0x1121 or its configured type, 0x31D070 in sub_0018D0A1).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/frame.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

namespace {

using kernel::HostPtr;
using kernel::R16;
using kernel::R32;
using kernel::W32;

constexpr uint32_t kDevicePtr = 0x1E0930;    // D3D__pDevice
constexpr uint32_t kBackBuffer = 0x21C0;     // CDevice field, xdk/4721/layout.toml
constexpr uint32_t kVBlankCount = 0x2580;    // CDevice field (miniport + 0x1C0)
constexpr uint32_t kOverlayShown = 0x2BA8;   // CDevice field: the vertical blank count of the last UpdateOverlay
constexpr uint32_t kSwap = 0x1DE920;         // D3DDevice_Swap, the runtime's
constexpr uint32_t kDrawVerticesUP = 0x1D43C0;  // D3DDevice_DrawVerticesUP, the runtime's

// one pixel in a linear frame buffer format from 8 bit B, G, R
void Put(uint8_t* p, uint32_t format, uint8_t b, uint8_t g, uint8_t r) {
    switch (format) {
        case 0x11:  // LIN_R5G6B5
        {
            uint16_t v = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            std::memcpy(p, &v, 2);
            break;
        }
        case 0x10:  // LIN_A1R5G5B5
        case 0x1C:  // LIN_X1R5G5B5
        {
            uint16_t v = uint16_t(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
            std::memcpy(p, &v, 2);
            break;
        }
        default:  // LIN_A8R8G8B8, LIN_X8R8G8B8
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 0xFF;
            break;
    }
}

uint32_t BytesPerPixel(uint32_t format) { return format == 0x11 || format == 0x10 || format == 0x1C ? 2 : 4; }

struct Rect {
    int32_t left, top, right, bottom;
};
Rect ReadRect(uint32_t va, int32_t w, int32_t h) {
    if (!va) return {0, 0, w, h};
    return {int32_t(R32(va)), int32_t(R32(va + 4)), int32_t(R32(va + 8)), int32_t(R32(va + 12))};
}

// the overlay surface's src rect scaled onto the back buffer's dst rect, then presented
void ShowOverlay(X86Context& ctx, uint32_t surface, uint32_t src_rect, uint32_t dst_rect) {
    uint32_t dev = R32(kDevicePtr);
    uint32_t bb = dev ? R32(dev + kBackBuffer) : 0;
    gpu::TextureDesc sd, bd;
    if (!surface || !bb || !gpu::Describe(surface, sd) || !gpu::Describe(bb, bd)) {
        LOG_FIRST(4, "overlay: surface %08X or back buffer %08X not described", surface, bb);
        return;
    }
    static std::vector<uint8_t> rgba;
    uint32_t row = 0;
    gpu::DecodeLevel(sd, 0, 0, static_cast<const uint8_t*>(HostPtr(sd.data)), nullptr, rgba, row);
    if (rgba.empty() || !row) return;
    LOG_FIRST(1, "overlay: shown on the frame buffer, %ux%u format %X onto %ux%u format %X", sd.width, sd.height,
              sd.format, bd.width, bd.height, bd.format);
    Rect s = ReadRect(src_rect, int32_t(sd.width), int32_t(sd.height));
    Rect d = ReadRect(dst_rect, int32_t(bd.width), int32_t(bd.height));
    s.left = std::clamp(s.left, 0, int32_t(sd.width));
    s.right = std::clamp(s.right, s.left, int32_t(sd.width));
    s.top = std::clamp(s.top, 0, int32_t(sd.height));
    s.bottom = std::clamp(s.bottom, s.top, int32_t(sd.height));
    int32_t sw = s.right - s.left, sh = s.bottom - s.top, dw = d.right - d.left, dh = d.bottom - d.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    uint8_t* out = static_cast<uint8_t*>(HostPtr(bd.data));
    uint32_t bpp = BytesPerPixel(bd.format);
    for (int32_t y = std::max(d.top, 0); y < std::min(d.bottom, int32_t(bd.height)); y++) {
        int32_t sy = s.top + int32_t(int64_t(y - d.top) * sh / dh);
        const uint8_t* in = &rgba[size_t(sy) * row];
        uint8_t* o = out + size_t(y) * bd.pitch;
        for (int32_t x = std::max(d.left, 0); x < std::min(d.right, int32_t(bd.width)); x++) {
            int32_t sx = s.left + int32_t(int64_t(x - d.left) * sw / dw);
            const uint8_t* p = in + size_t(sx) * 4;
            Put(o + size_t(x) * bpp, bd.format, p[0], p[1], p[2]);
        }
    }
    gpu::MarkDirty(bb);  // the CPU wrote the target: the present uploads it
    kernel::CallGuest(ctx, kSwap, {0});
}

uint32_t g_up_buffer = 0, g_up_size = 0;

}  // namespace

// D3DDevice_UpdateOverlay(surface, src rect, dst rect, enable color key, color key)
X86_FUNC(sub_001DEAE0) {
    (void)base;
    kernel::Args a{ctx};
    uint32_t surface = a[0], src = a[1], dst = a[2];
    if (uint32_t dev = R32(kDevicePtr)) W32(dev + kOverlayShown, R32(dev + kVBlankCount));
    ShowOverlay(ctx, surface, src, dst);
    kernel::Return(ctx, 0, 5);
}

// D3DDevice_DrawIndexedVerticesUP(primitive, vertex count, 16 bit indices, vertices, stride)
X86_FUNC(sub_001D4520) {
    (void)base;
    kernel::Args a{ctx};
    uint32_t prim = a[0], count = a[1], indices = a[2], vertices = a[3], stride = a[4];
    if (count && stride && indices && vertices) {
        uint32_t bytes = count * stride;
        if (bytes > g_up_size) {
            if (g_up_buffer) mem::FreeSystem(g_up_buffer);
            g_up_size = std::max<uint32_t>(bytes, 0x10000);
            g_up_buffer = mem::AllocSystem(g_up_size, 64);
        }
        uint8_t* out = static_cast<uint8_t*>(HostPtr(g_up_buffer));
        for (uint32_t i = 0; i < count; i++)
            std::memcpy(out + size_t(i) * stride, HostPtr(vertices + uint32_t(R16(indices + i * 2)) * stride), stride);
        kernel::CallGuest(ctx, kDrawVerticesUP, {prim, count, g_up_buffer, stride});
    }
    kernel::Return(ctx, 0, 5);
}

// D3DDevice_GetVertexShaderConstant(register, data, count): register -96..95, hardware order is register + 96
X86_FUNC(sub_001D1F10) {
    (void)base;
    kernel::Args a{ctx};
    int32_t reg = int32_t(a[0]);
    uint32_t dst = a[1], count = a[2];
    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        const auto& vsc = d3d::State().vsc;
        for (uint32_t i = 0; i < count; i++) {
            int32_t hw = reg + 96 + int32_t(i);
            if (hw >= 0 && hw < int32_t(vsc.size())) std::memcpy(HostPtr(dst + i * 16), vsc[size_t(hw)].data(), 16);
            else std::memset(HostPtr(dst + i * 16), 0, 16);
        }
    }
    kernel::Return(ctx, 0, 3);
}

// IDirectSoundBuffer_SetConeOrientation(buffer, x, y, z, apply)
X86_FUNC(sub_001E54E4) {
    (void)base;
    kernel::Return(ctx, 0, 5);
}

namespace {
constexpr uint32_t kMultiSampleType = 0x211C;  // CDevice field, XDK 4721
constexpr uint32_t kMultiSampleNone = 0x11;    // D3DMULTISAMPLE_NONE

// at the top of every frame (the device is made before the main loop, and Reset leaves the field alone)
void KeepMultiSampleType(uint64_t) {
    if (uint32_t dev = R32(kDevicePtr); dev && R32(dev + kMultiSampleType) != kMultiSampleNone)
        W32(dev + kMultiSampleType, kMultiSampleNone);
}
const bool g_multisample = [] {
    mkda::OnFrameStart(&KeepMultiSampleType);
    return true;
}();
}  // namespace
