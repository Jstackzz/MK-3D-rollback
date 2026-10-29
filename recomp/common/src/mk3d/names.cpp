// The players' names over an online fight (names.h). The game's adapter says at the top of every frame, on the main
// thread, whether the fight's HUD is up and where its bars are; the overlay draws from that copy on the render path with
// the runtime's UI lock held, in the frame's place in the window (the runtime shows the 640x480 frame centred, 16:9 when
// app::Widescreen(), else 4:3).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/names.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "app/app.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/game.h"

namespace mk3d::names {
namespace {

constexpr float kFontPixels = 64.0f;  // the atlas' size: sharp up to a 4K frame
// the names' line height in the frame's 480 lines: capitals 10 lines tall, as the games' own fighter names in the bars
constexpr float kTextHeight = 19.0f;
// A fight's HUD comes in with its bars growing out from the timer (Deception's wait there a moment first): the names
// show once each has room beside the timer and their place has held still a few frames, then fade in.
constexpr float kMinRoom = 40.0f;  // frame pixels
constexpr int kSettleFrames = 3;
constexpr int kFadeInFrames = 15;

Adapter g_game{};
std::mutex g_lock;
std::string g_match[2], g_test[2];  // the match's (the link's), the test switch's
bool g_up = false;                  // the names show this frame (main thread, copied under g_lock)
Place g_place;
ImFont* g_font = nullptr;
// main thread only
Place g_last;
int g_still = 0;  // frames the place has held still
int g_shown = 0;  // frames since the names came up, 0: not up

bool Near(float a, float b) { return std::fabs(a - b) < 0.25f; }

void FrameStart(uint64_t) {
    Place p;
    bool up = g_game.place && g_game.place(p);
    if (up && (p.p1_end - p.p1_start < kMinRoom || p.p2_end - p.p2_start < kMinRoom)) up = false;
    if (!up) {
        g_still = g_shown = 0;
    } else {
        const bool still = Near(p.p1_start, g_last.p1_start) && Near(p.p1_end, g_last.p1_end) &&
                           Near(p.p2_start, g_last.p2_start) && Near(p.p2_end, g_last.p2_end) && Near(p.y, g_last.y);
        g_still = still ? g_still + 1 : 0;
        if (g_shown || g_still >= kSettleFrames) g_shown = std::min(g_shown + 1, kFadeInFrames);
        up = g_shown > 0;
        p.shade *= float(g_shown) / kFadeInFrames;
    }
    g_last = p;
    std::lock_guard<std::mutex> lock(g_lock);
    g_up = up;
    if (up) g_place = p;
}

float F32(uint32_t address) {
    const uint32_t v = kernel::R32(address);
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

}  // namespace

void Install(const Adapter& adapter) {
    g_game = adapter;
    if (const char* v = Env("TEST_NAMES"); v && *v) {
        const char* bar = std::strchr(v, '|');
        g_test[0] = bar ? std::string(v, bar) : std::string(v);
        g_test[1] = bar ? std::string(bar + 1) : std::string();
    }
    OnFrameStart(&FrameStart);
}

bool PlaceInBars(const BarQuads& bars, Place& out) {
    constexpr float kGap = 8.0f;  // past a bar's middle: clear of the fighter's name in its outer half
    const float width = float(kernel::R32(bars.hud_width));
    if (width < 320 || width > 2048) return false;
    float lo[2], hi[2], middle = 0;
    for (int side = 0; side < 2; side++) {
        const uint32_t obj = kernel::R32(bars.back[side]);
        if (!obj || !mem::IsCommitted(obj) || kernel::R32(obj + 4) != kernel::R32(bars.back[side] + 4)) return false;
        const uint32_t quad = kernel::R32(obj + 0x34);
        if (!quad || !mem::IsCommitted(quad) || !mem::IsCommitted(quad + 0x40)) return false;
        float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX;
        for (uint32_t corner = 0; corner < 4; corner++) {
            const float cx = F32(quad + corner * 0x14), cy = F32(quad + corner * 0x14 + 4);
            x0 = std::min(x0, cx);
            x1 = std::max(x1, cx);
            y0 = std::min(y0, cy);
            y1 = std::max(y1, cy);
        }
        const float ox = float(int32_t(kernel::R32(obj + 0x14))), oy = float(int32_t(kernel::R32(obj + 0x18)));
        lo[side] = x0 + ox;
        hi[side] = x1 + ox;
        middle += (y0 + y1) / 2 + oy;
    }
    const float k = 640.0f / width;
    out.p1_start = ((lo[0] + hi[0]) / 2 + kGap) * k;
    out.p1_end = std::min(hi[0], width / 2 - bars.clear) * k;
    out.p2_start = std::max(lo[1], width / 2 + bars.clear) * k;
    out.p2_end = ((lo[1] + hi[1]) / 2 - kGap) * k;
    out.y = 480.0f - middle / 2;
    return out.y > 0;  // slid off the top with the HUD
}

float ShadeUnder(const FadeBoxes& boxes) {
    constexpr uint32_t kFifty = 0x42480000, kForty = 0x42200000;  // 50.0f, 40.0f
    int darkest = 0;
    for (const uint32_t head : boxes.lists) {
        if (!head) continue;
        uint32_t node = kernel::R32(head);
        for (int n = 0; node && n < 4096; n++) {
            if (!mem::IsCommitted(node) || !mem::IsCommitted(node + 0x13)) break;
            const uint32_t obj = kernel::R32(node);
            if (obj && mem::IsCommitted(obj) && mem::IsCommitted(obj + boxes.scale + 7) &&
                kernel::R32(node + 0x10) == kernel::R32(obj + 4) && int32_t(kernel::R32(obj + 0x14)) == -50 &&
                int32_t(kernel::R32(obj + 0x18)) == -50 && kernel::R32(obj + boxes.scale) == kFifty &&
                kernel::R32(obj + boxes.scale + 4) == kForty) {
                const uint32_t corners = kernel::R32(obj + boxes.corners);
                if (corners && mem::IsCommitted(corners) && mem::IsCommitted(corners + boxes.alpha))
                    darkest = std::max<int>(darkest, kernel::G<uint8_t>(corners + boxes.alpha));
            }
            node = kernel::R32(node + 4);
        }
    }
    return 1.0f - float(darkest) / 255.0f;
}

void SetPlayers(const std::string& p1, const std::string& p2) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_match[0] = p1;
    g_match[1] = p2;
}

void Window(void*) {
    // before the renderer builds the font atlas: Segoe UI Black Italic, heavy and slanted like the games' own HUD
    // lettering, else Bold Italic, else Bold; the runtime's default kept first
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    char dir[MAX_PATH];
    const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) return;
    for (const char* file : {"seguibli.ttf", "segoeuiz.ttf", "segoeuib.ttf"}) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof path, "%s\\Fonts\\%s", dir, file);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        if ((g_font = io.Fonts->AddFontFromFileTTF(path, kFontPixels))) break;
    }
}

void Ui(double) {
    std::string names[2];
    Place p;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const std::string* source = g_match[0].empty() && g_match[1].empty() ? g_test : g_match;
        if (!g_up || (source[0].empty() && source[1].empty())) return;
        names[0] = source[0];
        names[1] = source[1];
        p = g_place;
    }
    const ImGuiIO& io = ImGui::GetIO();
    // the frame's place in the window, as the runtime's blit puts it (gpu/d3d12.cpp BlitFrame)
    const float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float fw = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect), fh = fw / aspect;
    const float fx = (io.DisplaySize.x - fw) / 2, fy = (io.DisplaySize.y - fh) / 2;
    const float sx = fw / 640.0f, sy = fh / 480.0f;
    ImFont* font = g_font ? g_font : ImGui::GetFont();
    const ImFontGlyph* cap = font->FindGlyph('H');
    const int alpha = int(std::lround(255.0f * std::clamp(p.shade, 0.0f, 1.0f)));
    if (alpha == 0) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();  // under any ImGui window (the frame data panel)
    for (int side = 0; side < 2; side++) {
        if (names[side].empty()) continue;
        const char* text = names[side].c_str();
        float size = kTextHeight * sy;
        ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        // a long name (16 wide letters) shrinks to its room rather than reach the fighter's name
        const float room = (side == 0 ? p.p1_end - p.p1_start : p.p2_end - p.p2_start) * sx;
        if (extent.x > room) {
            size *= room / extent.x;
            extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        }
        const float scale = size / font->FontSize;
        const float capital = cap ? (cap->Y0 + cap->Y1) / 2 * scale : size / 2;  // the capitals' middle in the line
        const float outline = std::max(1.0f, size / 12.0f);
        const float x = std::floor(side == 0 ? fx + p.p1_end * sx - extent.x : fx + p.p2_start * sx);
        const float y = std::floor(fy + p.y * sy - capital);
        // a dark outline, then the letters: readable over the bar's green and red alike, like the games' own outlined
        // lettering; both fade with the game's screen
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy)
                    draw->AddText(font, size, ImVec2(x + dx * outline, y + dy * outline),
                                  IM_COL32(0, 0, 0, alpha * 230 / 255), text);
        draw->AddText(font, size, ImVec2(x, y), IM_COL32(245, 245, 245, alpha), text);
    }
}

}  // namespace mk3d::names
