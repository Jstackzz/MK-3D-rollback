// Each player's rank badge beside the timer, above their health bar (ranks.h). The game's adapter says at the top of
// every frame where the bars and the timer are and which textures the HUD's bars draw with; the draw filter
// (d3d::SetDrawFilter) puts both badges into the frame just before the first of those draws, on the title's drawing
// thread, so whatever the HUD draws after goes over them.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/ranks.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "mk3d/game.h"
#include "xdk/d3d8.h"

namespace mk3d::ranks {
namespace {

constexpr uint32_t kQuadList = 8;          // D3DPT_QUADLIST: the HUDs' sprites, four vertices each
constexpr float kBox = 2.4f;               // a badge fits in a box this many times as wide as tall, as seen
constexpr float kGap = 6.0f;               // frame pixels (of 640) between a badge and its player's name
constexpr uint32_t kFirstPicture = 0x100;  // the renderer's picture ids for the badges (0 is the select screen's)
constexpr size_t kLongestRank = 32;

// set once at start (Install, before any frame), then read only
Adapter g_game{};

// a badge's picture: the renderer's id, its width over its height
struct Picture {
    uint32_t id = 0;
    float aspect = 0;
};

std::mutex g_lock;  // everything below, between the main thread's frame start, the drawing thread's filter and the UI
bool g_ready = false;                       // the test switches read and the pictures' folder found
std::string g_match[2], g_test[2];          // the match's ranks (the link's), the test switch's
float g_test_height = 0;                    // the test switch's badge height, 0: the game's
std::filesystem::path g_dir;                // where the badges' pictures are
std::map<std::string, Picture> g_pictures;  // a rank's picture (id 0: none), named to the renderer at first sight
std::string g_logged[2];                    // the ranks the log last named

// what the filter draws this frame
struct Frame {
    bool drawn = false;
    uint32_t anchor[4] = {};
    uint32_t picture[2] = {};  // 0: none for that side
    float rect[2][4] = {};     // x0 y0 x1 y1 in the HUD's units across and lines down
    float hud_width = 640;
};
Frame g_frame;
std::atomic<bool> g_armed{false};  // a badge waits to be drawn this frame (the filter's quick test, without the lock)

// a start line's rank as a picture's name: lower case letters, digits and dashes, else none
std::string Clean(const std::string& rank) {
    if (rank.empty() || rank == "-" || rank.size() > kLongestRank) return {};
    for (const char c : rank)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return {};
    return rank;
}

// the badges' folder: MK3D_RANKS_DIR (tests), the game's own ranks folder, or the package's, two folders up
// (games\<game>\<GAME>.exe beside ranks\ in the launcher's folder)
std::filesystem::path PictureDir() {
    std::error_code ec;
    if (const char* v = std::getenv("MK3D_RANKS_DIR"); v && *v && std::filesystem::is_directory(v, ec)) return v;
    const std::filesystem::path exe = app::ExeDir();
    for (const std::filesystem::path& dir : {exe / "ranks", exe.parent_path().parent_path() / "ranks"})
        if (std::filesystem::is_directory(dir, ec)) return dir;
    return {};
}

// a PNG's width over its height (its IHDR chunk), 0 when it is none
float PngAspect(const std::filesystem::path& file) {
    unsigned char head[24] = {};
    std::ifstream f(file, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || head[1] != 'P' || head[2] != 'N' || head[3] != 'G')
        return 0;
    const auto be = [&](int at) {
        return uint32_t(head[at]) << 24 | uint32_t(head[at + 1]) << 16 | uint32_t(head[at + 2]) << 8 | head[at + 3];
    };
    const uint32_t w = be(16), h = be(20);
    return w && h && w < 16384 && h < 16384 ? float(w) / float(h) : 0;
}

// with g_lock: the test switches, read at the first frame rather than while the program's statics are still being
// made (each game's adapter installs from one of them)
void Ready() {
    if (g_ready) return;
    g_ready = true;
    if (const char* v = Env("TEST_RANK_SIZE"); v && *v) g_test_height = float(std::atof(v));
    if (const char* v = Env("TEST_RANKS"); v && *v) {
        const std::string s(v);
        const size_t bar = s.find('|');
        g_test[0] = Clean(s.substr(0, bar));
        g_test[1] = bar == std::string::npos ? std::string() : Clean(s.substr(bar + 1));
    }
    g_dir = PictureDir();
    LOGR("ranks: badges from %s", g_dir.empty() ? "nowhere (no ranks folder)" : g_dir.string().c_str());
}

// with g_lock: a rank's picture, named to the renderer the first time; id 0 for no rank or no picture of it
Picture PictureOf(const std::string& rank) {
    if (rank.empty()) return {};
    if (auto it = g_pictures.find(rank); it != g_pictures.end()) return it->second;
    Picture pic;
    std::error_code ec;
    const std::filesystem::path file = g_dir / (rank + ".png");
    if (!g_dir.empty() && std::filesystem::is_regular_file(file, ec) && (pic.aspect = PngAspect(file)) > 0) {
        pic.id = kFirstPicture + uint32_t(g_pictures.size());
        gpu::SetPicture(pic.id, file);
    } else {
        LOGR("ranks: no badge for %s", rank.c_str());
    }
    g_pictures[rank] = pic;
    return pic;
}

void FrameStart(uint64_t) {
    Place place;
    const bool up = g_game.place && g_game.place(place) && place.hud_width >= 320;
    // the screen's width over its height (16:9 when app::Widescreen, else 4:3) spread over the HUD's width: a HUD unit
    // as wide as a line is tall on screen times this
    const float unit = place.hud_width / ((app::Widescreen() ? 16.0f / 9 : 4.0f / 3) * 480.0f);
    std::lock_guard<std::mutex> lock(g_lock);
    Ready();
    const std::string* source = g_match[0].empty() && g_match[1].empty() ? g_test : g_match;
    Frame f;
    const float box = g_test_height > 0 ? g_test_height : place.height;
    if (up && box > 0) {
        std::copy(std::begin(place.anchor), std::end(place.anchor), f.anchor);
        f.hud_width = place.hud_width;
        for (int side = 0; side < 2; side++) {
            const Picture pic = PictureOf(source[side]);
            if (!pic.id) continue;
            // as large as fits in the box, its own shape kept, against the timer and standing on the bar
            const float h = std::min(box, box * kBox / pic.aspect), w = h * pic.aspect * unit;
            const float x0 = side == 0 ? place.inner[0] - w : place.inner[1];
            f.picture[side] = pic.id;
            f.rect[side][0] = x0;
            f.rect[side][1] = place.bottom - h;
            f.rect[side][2] = x0 + w;
            f.rect[side][3] = place.bottom;
        }
        if ((f.picture[0] || f.picture[1]) && (source[0] != g_logged[0] || source[1] != g_logged[1])) {
            g_logged[0] = source[0];
            g_logged[1] = source[1];
            LOGR("ranks: P1 %s, P2 %s beside the timer", source[0].empty() ? "-" : source[0].c_str(),
                 source[1].empty() ? "-" : source[1].c_str());
        }
    }
    g_frame = f;
    g_armed = f.picture[0] || f.picture[1];
}

int Filter(const gpu::State& s, uint32_t prim, uint32_t count, const uint8_t*, uint32_t) {
    if (!g_armed.load(std::memory_order_relaxed) || prim != kQuadList || count != 4 || !s.textures[0])
        return d3d::kDrawAsIs;
    const uint32_t texture = s.textures[0];
    Frame f;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_frame.drawn || std::find(std::begin(g_frame.anchor), std::end(g_frame.anchor), texture) ==
                                 std::end(g_frame.anchor))
            return d3d::kDrawAsIs;
        g_frame.drawn = true;
        g_armed = false;
        f = g_frame;
    }
    // into the target's own pixels: its width spans the HUD's, its height the 480 lines
    uint32_t w = 0, h = 0;
    gpu::SurfaceSize(s.rt, w, h);
    const float sx = float(w) / f.hud_width, sy = float(h) / 480.0f;
    for (int side = 0; side < 2; side++)
        if (f.picture[side])
            d3d::DrawPicture(f.picture[side], f.rect[side][0] * sx, f.rect[side][1] * sy, f.rect[side][2] * sx,
                             f.rect[side][3] * sy);
    return d3d::kDrawAsIs;
}

}  // namespace

void Install(const Adapter& adapter) {
    g_game = adapter;
    OnFrameStart(&FrameStart);
    d3d::SetDrawFilter(&Filter);
}

void SetPlayers(const std::string& p1, const std::string& p2) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_match[0] = Clean(p1);
    g_match[1] = Clean(p2);
}

void KeepClear(float& p1_end, float& p2_start) {
    std::lock_guard<std::mutex> lock(g_lock);
    const float k = 640.0f / g_frame.hud_width;  // the names' room is in the frame's 640 pixels across
    if (g_frame.picture[0]) p1_end = std::min(p1_end, g_frame.rect[0][0] * k - kGap);
    if (g_frame.picture[1]) p2_start = std::max(p2_start, g_frame.rect[1][2] * k + kGap);
}

}  // namespace mk3d::ranks
