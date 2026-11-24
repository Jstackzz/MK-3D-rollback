// Costumes (costumes.h).
#include "game/costumes.h"
#include "game/costumes_maps.h"

#include <process.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/events.h"
#include "game/frame.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "mk3d/openhook.h"

X86_EXTERN_FUNC(__imp__sub_00103150);

namespace mkda::costumes {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// the costume table (costumes.h): a row per character, the entries of its two costumes and of their other colours
constexpr uint32_t kTable = 0x243B40, kStride = 0x110, kFirst = 0x00, kSecond = 0x30, kFirstColours = 0x60,
                   kSecondColours = 0x70;
constexpr uint32_t kScorpion = 0, kJax = 1, kRaiden = 10, kBoRaiCho = 20;

// --- the looks: palette colours near a hue (and saturated enough) become another hue, saturation and value ---

struct Rule {
    double center, inner, outer, s_min;  // the colours it takes: hue within inner of center, fading out at outer
    double hue, sat, val, s_floor;       // what they become: that hue, saturation * sat (at least s_floor), value * val
    double v_max = 2.0;                  // and only up to this value, fading out 0.15 above it
};

// Raiden's teal (hue 160 to 200, value about 0.15 in the files: the game's lighting brightens it)
constexpr Rule kDeepBlue{185, 35, 60, 0.2, 228, 1.0, 1.3, 0.85};
constexpr Rule kRed{185, 35, 60, 0.2, 356, 1.0, 1.25, 0.85};
// his picture on the select screen: the cloth's darker colours only, so the lightning in his hand stays as it is
constexpr Rule kDeepBluePicture{185, 35, 60, 0.2, 228, 1.0, 1.3, 0.85, 0.45};
// Scorpion: the gold rims of his gear (sc_armor: shoulder pads, the plates on his thighs, knee guards; sc_pads: shin
// and forearm guards) a brighter, purer yellow, or orange
constexpr Rule kScorpionGear{45, 18, 30, 0.3, 47, 1.15, 1.35, 0.85};
constexpr Rule kScorpionGearOrange{45, 18, 30, 0.3, 30, 1.2, 1.3, 0.9};
// Bo' Rai Cho's second costume: its red pants (hue about 352) black; the yellow belt and the gold stay
constexpr Rule kBoBlackPants{352, 20, 35, 0.3, 0, 0.0, 0.5, 0.0};

// A texture made again by a map of its texels (costumes_maps.h, from its UV layout): each texel of a ramp's class keeps
// its brightness on that ramp of a new palette (64 entries each), each kept texel its colour
struct Ramp {
    uint8_t dark[3], light[3];  // B G R as the palette holds them (the game's light makes them brighter)
};
constexpr Ramp kRampBlack{{4, 4, 5}, {58, 54, 50}}, kRampYellow{{0, 40, 70}, {40, 220, 255}},
               kRampOrange{{0, 24, 70}, {10, 140, 255}}, kRampGloveRed{{6, 6, 105}, {40, 45, 250}};
constexpr uint8_t kKeep = 255;
struct Rebuild {
    const char* texture;
    const char* rename;
    const ClassMap* map;
    const Ramp* ramps[3];
    int ramp_count;
};

double Clamp(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

void RgbToHsv(double r, double g, double b, double& h, double& s, double& v) {
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    v = mx;
    s = mx <= 0 ? 0.0 : (mx - mn) / mx;
    if (mx == mn) h = 0.0;
    else if (mx == r) h = 60.0 * ((g - b) / (mx - mn));
    else if (mx == g) h = 60.0 * ((b - r) / (mx - mn)) + 120.0;
    else h = 60.0 * ((r - g) / (mx - mn)) + 240.0;
    if (h < 0) h += 360.0;
}

void HsvToRgb(double h, double s, double v, double& r, double& g, double& b) {
    h = std::fmod(h, 360.0);
    const double c = v * s, hp = h / 60.0, x = c * (1 - std::fabs(std::fmod(hp, 2.0) - 1)), m = v - c;
    if (hp < 1) r = c, g = x, b = 0;
    else if (hp < 2) r = x, g = c, b = 0;
    else if (hp < 3) r = 0, g = c, b = x;
    else if (hp < 4) r = 0, g = x, b = c;
    else if (hp < 5) r = x, g = 0, b = c;
    else r = c, g = 0, b = x;
    r += m, g += m, b += m;
}

double HueWeight(double h, const Rule& rule) {
    double d = std::fmod(std::fabs(h - rule.center), 360.0);
    d = std::min(d, 360.0 - d);
    if (d <= rule.inner) return 1.0;
    if (d >= rule.outer) return 0.0;
    return (rule.outer - d) / (rule.outer - rule.inner);
}

uint8_t Byte(double x) { return uint8_t(std::floor(Clamp(x) * 255.0 + 0.5)); }

// one palette entry, B G R A
void Recolour(uint8_t* bgra, const Rule& rule) {
    const double r = bgra[2] / 255.0, g = bgra[1] / 255.0, b = bgra[0] / 255.0;
    double h, s, v;
    RgbToHsv(r, g, b, h, s, v);
    const double w = HueWeight(h, rule) * Clamp((s - rule.s_min) / 0.1) * Clamp((rule.v_max + 0.15 - v) / 0.15);
    if (w <= 0) return;
    double nr, ng, nb;
    HsvToRgb(rule.hue, Clamp(std::max(rule.s_floor, s * rule.sat)), Clamp(v * rule.val), nr, ng, nb);
    bgra[2] = Byte(r + (nr - r) * w);
    bgra[1] = Byte(g + (ng - g) * w);
    bgra[0] = Byte(b + (nb - b) * w);
}

// --- the game's files: a .ssf is pieces (" CES" sections among them), a section items (models type 4, textures 3) ---

uint32_t U32(const Bytes& d, size_t at) {
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}
void Put32(Bytes& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }

struct Item {
    uint32_t type, offset, size;
};

// the section at `at` (its items' offsets from there), false when it is not one
bool ReadSection(const Bytes& d, size_t at, size_t size, std::vector<Item>& items) {
    items.clear();
    if (size < 0x18 || at + size > d.size() || std::memcmp(d.data() + at, " CES", 4) != 0) return false;
    const uint32_t n = U32(d, at + 16);
    if (n > 4096 || 0x18 + 12 * size_t(n) > size) return false;
    for (uint32_t i = 0; i < n; i++) {
        const Item it{U32(d, at + 0x18 + 12 * i), U32(d, at + 0x1C + 12 * i), U32(d, at + 0x20 + 12 * i)};
        if (size_t(it.offset) + it.size > size) return false;
        items.push_back(it);
    }
    return true;
}

// a texture: [name length][name][w][h][1][size], then 92 bytes, 256 B G R A palette entries (8 bits a pixel), the
// pixels' size (u32), the pixels (swizzled); no smaller levels
struct Texture {
    size_t palette = 0, pixels = 0;
    uint32_t w = 0, h = 0;
};
bool ReadTexture(const Bytes& d, size_t base, size_t end, Texture& t) {
    const size_t n = d[base];
    const size_t blob = base + 1 + n + 16;
    if (blob + 92 + 1024 + 4 > end || d[blob + 3] != 8) return false;
    t.w = U32(d, base + 1 + n);
    t.h = U32(d, base + 1 + n + 4);
    t.palette = blob + 92;
    t.pixels = t.palette + 1024 + 4;
    return U32(d, t.palette + 1024) == t.w * t.h && t.pixels + size_t(t.w) * t.h <= end;
}
bool FindTexture(const Bytes& d, size_t at, const std::vector<Item>& items, const std::string& name, Texture& t) {
    for (const Item& it : items) {
        if (it.type != 3) continue;
        const size_t base = at + it.offset;
        if (d[base] != name.size() || std::memcmp(d.data() + base + 1, name.data(), name.size()) != 0) continue;
        return ReadTexture(d, base, base + it.size, t);
    }
    return false;
}
// an unnamed texture by its item number (1 the first), as the select screen's pictures have them
bool FindItem(const Bytes& d, size_t at, const std::vector<Item>& items, size_t number, Texture& t) {
    if (number < 1 || number > items.size() || (items[number - 1].type != 2 && items[number - 1].type != 3)) return false;
    const Item& it = items[number - 1];
    return ReadTexture(d, at + it.offset, at + it.offset + it.size, t);
}

// a texture's name and every material of the section's models naming it ([u32 padded length][ff ff 00 08][name]),
// the same length; how many it changed
int Rename(Bytes& d, size_t at, const std::vector<Item>& items, const std::string& from, const std::string& to) {
    if (from.size() != to.size()) return 0;
    int count = 0;
    Bytes pat(8 + from.size() + 1, 0);
    const uint32_t padded = uint32_t((from.size() + 4) & ~size_t(3));
    std::memcpy(pat.data(), &padded, 4);
    const uint8_t mark[4] = {0xFF, 0xFF, 0x00, 0x08};
    std::memcpy(pat.data() + 4, mark, 4);
    std::memcpy(pat.data() + 8, from.data(), from.size());
    for (const Item& it : items) {
        const size_t base = at + it.offset;
        if (it.type == 3 && d[base] == from.size() && std::memcmp(d.data() + base + 1, from.data(), from.size()) == 0) {
            std::memcpy(d.data() + base + 1, to.data(), to.size());
            count++;
        } else if (it.type == 4) {
            const auto first = d.begin() + base, last = d.begin() + base + it.size;
            for (auto p = std::search(first, last, pat.begin(), pat.end()); p != last;
                 p = std::search(p + 1, last, pat.begin(), pat.end())) {
                std::memcpy(&*p + 8, to.data(), to.size());
                count++;
            }
        }
    }
    return count;
}

struct Recolouring {
    const char* texture;
    const Rule* rule;
    const char* rename;  // null: it keeps its name (another change of the texture renames it)
};

// the recolourings in the section at `at`, in order; empty, or what failed
std::string Recolour(Bytes& d, size_t at, size_t size, const std::vector<Recolouring>& changes) {
    std::vector<Item> items;
    if (!ReadSection(d, at, size, items)) return "no section at " + std::to_string(at);
    for (const Recolouring& c : changes) {
        Texture t;
        if (!FindTexture(d, at, items, c.texture, t)) return std::string("no texture ") + c.texture;
        for (int i = 0; i < 256; i++) Recolour(d.data() + t.palette + 4 * i, *c.rule);
        if (c.rename && Rename(d, at, items, c.texture, c.rename) < 2)
            return std::string("no material names ") + c.texture;
    }
    return {};
}

// the pixels' order: the bits of x and y interleaved, x first, the longer side's last bits alone
uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t at = 0, bit = 0;
    for (uint32_t xb = 1, yb = 1; xb < w || yb < h;) {
        if (xb < w) {
            if (x & xb) at |= 1u << bit;
            bit++;
            xb <<= 1;
        }
        if (yb < h) {
            if (y & yb) at |= 1u << bit;
            bit++;
            yb <<= 1;
        }
    }
    return at;
}

double Luminance(const uint8_t* bgra) { return (0.114 * bgra[0] + 0.587 * bgra[1] + 0.299 * bgra[2]) / 255.0; }

// a texture made again by its map: ramp k takes palette 64k to 64k + 63, each class's texels spread over their own
// brightness (the class's 2nd to 98th percentile); the kept texels keep their colours, the most used of them in the
// palette's other entries, the rest the nearest of those; empty, or what failed
std::string RebuildTexture(Bytes& d, size_t at, size_t size, const Rebuild& r) {
    std::vector<Item> items;
    if (!ReadSection(d, at, size, items)) return "no section at " + std::to_string(at);
    Texture t;
    if (!FindTexture(d, at, items, r.texture, t)) return std::string("no texture ") + r.texture;
    if (t.w != r.map->w || t.h != r.map->h) return std::string("the map is not the size of ") + r.texture;
    const size_t n = size_t(t.w) * t.h;
    std::vector<uint8_t> of;
    of.reserve(n);
    for (size_t i = 0; i < r.map->count; i++) of.insert(of.end(), r.map->runs[i][1], uint8_t(r.map->runs[i][0]));
    if (of.size() != n) return std::string("the map does not cover ") + r.texture;
    uint8_t pal[256][4];
    std::memcpy(pal, d.data() + t.palette, sizeof pal);
    double lum[256];
    for (int i = 0; i < 256; i++) lum[i] = Luminance(pal[i]);
    // each texel's colour, in rows as the map has them
    std::vector<uint8_t> idx(n);
    for (uint32_t y = 0; y < t.h; y++)
        for (uint32_t x = 0; x < t.w; x++) idx[size_t(y) * t.w + x] = d[t.pixels + Swizzle(x, y, t.w, t.h)];
    const int k = r.ramp_count;
    double lo[3] = {}, hi[3] = {};
    for (int c = 0; c < k; c++) {
        std::vector<double> vals;
        for (size_t m = 0; m < n; m++)
            if (of[m] == c) vals.push_back(lum[idx[m]]);
        if (vals.empty()) continue;
        std::sort(vals.begin(), vals.end());
        lo[c] = vals[vals.size() / 50];
        hi[c] = vals[vals.size() - 1 - vals.size() / 50];
    }
    // the kept colours: the most used first (then the lower index), as many as the palette has room for
    uint32_t used[256] = {};
    for (size_t m = 0; m < n; m++)
        if (of[m] == kKeep) used[idx[m]]++;
    std::vector<int> order;
    for (int i = 0; i < 256; i++)
        if (used[i]) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return used[a] > used[b]; });
    const size_t room = size_t(256 - 64 * k);
    if (order.size() > room) order.resize(room);
    int new_index[256];
    std::fill(std::begin(new_index), std::end(new_index), -1);
    for (size_t q = 0; q < order.size(); q++) new_index[order[q]] = int(64 * k + q);
    auto nearest = [&](int i) {
        int best = -1, best_d = 0;
        for (int j : order) {
            int dist = 0;
            for (int q = 0; q < 3; q++) dist += (int(pal[i][q]) - int(pal[j][q])) * (int(pal[i][q]) - int(pal[j][q]));
            if (best < 0 || dist < best_d || (dist == best_d && j < best)) best = j, best_d = dist;
        }
        return new_index[best];
    };
    for (uint32_t y = 0; y < t.h; y++)
        for (uint32_t x = 0; x < t.w; x++) {
            const size_t m = size_t(y) * t.w + x;
            uint8_t& out = d[t.pixels + Swizzle(x, y, t.w, t.h)];
            if (of[m] == kKeep) {
                out = uint8_t(new_index[idx[m]] >= 0 ? new_index[idx[m]] : nearest(idx[m]));
            } else {
                const int c = of[m];
                const double v = Clamp((lum[idx[m]] - lo[c]) / std::max(1e-6, hi[c] - lo[c]));
                out = uint8_t(64 * c + int(std::floor(v * 63 + 0.5)));
            }
        }
    for (int c = 0; c < k; c++)
        for (int q = 0; q < 64; q++) {
            uint8_t* e = d.data() + t.palette + 4 * (64 * c + q);
            for (int ch = 0; ch < 3; ch++)
                e[ch] = uint8_t(
                    std::floor(r.ramps[c]->dark[ch] + (r.ramps[c]->light[ch] - r.ramps[c]->dark[ch]) * (q / 63.0) + 0.5));
            e[3] = 255;
        }
    for (size_t q = 0; q < order.size(); q++) std::memcpy(d.data() + t.palette + 4 * (64 * k + q), pal[order[q]], 4);
    if (r.rename && Rename(d, at, items, r.texture, r.rename) < 2) return std::string("no material names ") + r.texture;
    return {};
}

// a section's rebuilt textures, then its recolourings; empty, or what failed
std::string Change(Bytes& d, size_t at, size_t size, const std::vector<Rebuild>& rebuild,
                   const std::vector<Recolouring>& recolour) {
    for (const Rebuild& r : rebuild)
        if (std::string why = RebuildTexture(d, at, size, r); !why.empty()) return why;
    return Recolour(d, at, size, recolour);
}

// --- the fighters ---

// a change of one of the costume's two sections in its file (the same sizes: nothing moves)
struct InPlace {
    bool colours;  // the other colours' section, else the costume's own
    std::vector<Rebuild> rebuild;
    std::vector<Recolouring> recolour;
};

// a look made from one of the costume's sections, put at the end of a copy of its file under the section id of the
// row that shows it (that row's models look for that id)
struct Look {
    bool from_colours;       // the section it starts from: the other colours' (true), else the costume's own
    bool to_colours;         // the row that shows it: the other colours' (true), else the costume's own
    bool mirror;             // only for a player with kMirrorLook (costumes.h), else for everyone
    const char* entry_name;  // its entry's name (the game's entries have one: "RAYDEN.SEC")
    std::vector<Rebuild> rebuild;       // first
    std::vector<Recolouring> recolour;  // then, in order
};

struct Fighter {
    uint32_t character;
    const char* name;  // for the log
    bool second;       // the costume: the first (rows +0x00 and +0x60) or the second (+0x30 and +0x70)
    const char* file;  // its file (artxbox), with both its sections
    std::vector<InPlace> in_place;  // given to the game as `file`
    const char* looks_file;         // the copy of `file` with the looks at its end
    std::vector<Look> looks;
    // from the game's table at the first frame: the costume's two entries (its own, its other colours'), their list
    // (the file's: one for both) and their sections in the file
    uint32_t row = 0, entry[2] = {}, list = 0, off[2] = {}, size[2] = {};
    std::vector<uint32_t> look_entry;  // the looks' entries (guest memory)
    bool ready = false;
    uint32_t RowAt(bool colours) const {
        return row + (second ? (colours ? kSecondColours : kSecond) : (colours ? kFirstColours : kFirst));
    }
};

std::vector<Fighter> g_fighters = {
    // the first costume in a deeper blue; its other colours (LT and A) red, made from it (the game's are purple)
    {kRaiden, "RAIDEN", false, "rayden.ssf",
     {{false,
       {},
       {{"rd_blue", &kDeepBlue, nullptr}, {"rd_cape", &kDeepBlue, nullptr}, {"rd_haircape", &kDeepBlue, nullptr}}}},
     "x_rayden.ssf",
     {{false, true, false, "RAIDEN_RED.SEC", {},
       {{"rd_blue", &kRed, "r3_blue"}, {"rd_cape", &kRed, "r3_cape"}, {"rd_haircape", &kRed, "r3_haircape"}}}}},
    // the first costume's other colours (LT and A) black and yellow, made from it; black and orange for the second of
    // two players who both asked for them
    {kScorpion, "SCORPION", false, "scorpion.ssf", {}, "x_scorpion.ssf",
     {{false, true, false, "SCORPION_BLACK_YELLOW.SEC",
       {{"sc_gold", "s3_gold", &kScorpionGold, {&kRampBlack, &kRampYellow}, 2}},
       {{"sc_armor", &kScorpionGear, "s3_armor"}, {"sc_pads", &kScorpionGear, "s3_pads"}}},
      {false, false, true, "SCORPION_BLACK_ORANGE.SEC",
       {{"sc_gold", "s4_gold", &kScorpionGold, {&kRampBlack, &kRampOrange}, 2}},
       {{"sc_armor", &kScorpionGearOrange, "s4_armor"}, {"sc_pads", &kScorpionGearOrange, "s4_pads"}}}}},
    // the second costume's other colours (LT and START): the second costume with black pants (the game's other colours
    // are purple); x_drunkenfighter2.ssf is alt_drunkenfighter.ssf with it
    {kBoRaiCho, "BO' RAI CHO", true, "alt_drunkenfighter.ssf", {}, "x_drunkenfighter2.ssf",
     {{false, true, false, "BO_RAI_CHO_BLACK_PANTS.SEC", {}, {{"AltBRC_pants", &kBoBlackPants, "BlkBRC_pants"}}}}},
    // the second costume's other colours (LT and START): the game's red jacket, and his gloves red (his own gloves, on
    // his own model: the same collision); renamed, so the second costume itself keeps its black gloves in a mirror
    {kJax, "JAX", true, "alt_jax.ssf",
     {{true, {{"altJx_boot", "redJx_boot", &kJaxGlove, {&kRampGloveRed, &kRampGloveRed}, 2}}, {}}},
     nullptr,
     {}},
};

struct Served {
    std::string suffix;  // "\ARTXBOX\RAYDEN.SSF"
    std::string host;
};
std::vector<Served> g_served;  // set before g_ready, never after
std::atomic<bool> g_ready{false};

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// MKDA_ART_DIR=<folder> (development: textures tried out in the game): an artxbox file there stands in for the game's
// or the one made here
const fs::path g_art_dir = [] {
    const char* s = std::getenv("MKDA_ART_DIR");
    return s && *s ? fs::path(s) : fs::path();
}();

bool OpenHook(const std::string& xbox, std::string& host) {
    if (!g_ready.load(std::memory_order_acquire)) return false;
    if (const size_t at = xbox.rfind("\\ARTXBOX\\"); !g_art_dir.empty() && at != std::string::npos) {
        std::error_code ec;
        const fs::path file = g_art_dir / xbox.substr(at + 9);
        if (fs::is_regular_file(file, ec)) {
            host = file.string();
            return true;
        }
    }
    for (const Served& s : g_served)
        if (xbox.size() >= s.suffix.size() &&
            xbox.compare(xbox.size() - s.suffix.size(), s.suffix.size(), s.suffix) == 0) {
            host = s.host;
            return true;
        }
    return false;
}

// the file in ExeDir()\costumes, written only when it differs (two games on one PC make the same bytes)
bool Keep(const fs::path& dir, const std::string& name, const Bytes& data, fs::path& out) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    out = dir / name;
    auto same = [&] {
        std::ifstream f(out, std::ios::binary);
        if (!f) return false;
        const Bytes old((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return old == data;
    };
    if (same()) return true;
    const fs::path tmp = dir / (name + "." + std::to_string(_getpid()) + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) return false;
    }
    fs::rename(tmp, out, ec);
    if (!ec) return true;
    fs::remove(tmp, ec);
    return same();  // another game wrote it a moment before
}

// the costume's two entries, their list (rayden.ssf's: 0x238C20; [0] the file's own entry {name, list, type}, two
// words, then {entry, offset, size} up to {0, 0, 0}) and their sections' places in the file
bool ReadRows(Fighter& f) {
    f.row = kTable + f.character * kStride;
    for (int c = 0; c < 2; c++) f.entry[c] = kernel::R32(f.RowAt(c != 0));
    f.list = f.entry[0] ? kernel::R32(f.entry[0] + 4) : 0;
    if (!f.list || !f.entry[1] || kernel::R32(f.entry[1] + 4) != f.list) return false;
    for (int i = 0; i < 64; i++) {
        const uint32_t at = f.list + 12 + 12 * i, entry = kernel::R32(at);
        if (!entry) return f.size[0] && f.size[1];
        for (int c = 0; c < 2; c++)
            if (entry == f.entry[c]) f.off[c] = kernel::R32(at + 4), f.size[c] = kernel::R32(at + 8);
    }
    return false;
}

size_t Align(size_t n) { return (n + 0x7FF) & ~size_t(0x7FF); }

// the looks' file: the game's file with each look at its end, a copy of the section it starts from under the section
// id of the row that shows it; the game's own sections stay where they are, for the list's other entries
std::string MakeLooks(const Fighter& f, const Bytes& game, Bytes& out, std::vector<uint32_t>& offsets,
                      std::vector<uint32_t>& sizes) {
    out = game;
    uint32_t ids[2];
    for (int c = 0; c < 2; c++) {
        ids[c] = kernel::R32(f.RowAt(c != 0) + 4) >> 16;
        if (U32(game, f.off[c] + 12) != ids[c]) return "its section ids are not the table's";
    }
    const uint32_t n = U32(out, 16), looks = uint32_t(f.looks.size());
    if (U32(out, 0) != 0x53454320u || 0x18 + 12 * (n + looks) > U32(out, 0x18 + 4)) return "no room in the file's list";
    for (uint32_t k = 0; k < looks; k++) {
        const Look& l = f.looks[k];
        const size_t from = f.off[l.from_colours];
        Bytes sec(game.begin() + from, game.begin() + from + f.size[l.from_colours]);
        if (std::string why = Change(sec, 0, sec.size(), l.rebuild, l.recolour); !why.empty()) return why;
        Put32(sec, 12, ids[l.to_colours]);
        const size_t at = Align(out.size());
        out.resize(at + Align(sec.size()), 0);
        std::memcpy(out.data() + at, sec.data(), sec.size());
        offsets.push_back(uint32_t(at));
        sizes.push_back(uint32_t(sec.size()));
        // the file's own list of its pieces names it too
        Put32(out, 0x18 + 12 * (n + k), 6);
        Put32(out, 0x1C + 12 * (n + k), uint32_t(at));
        Put32(out, 0x20 + 12 * (n + k), uint32_t(sec.size()));
    }
    Put32(out, 16, n + looks);
    Put32(out, 20, uint32_t(out.size()));
    return {};
}

// the looks' entries and file list in guest memory: a copy of the costume's list naming the looks' file, its own
// entries as they are and the looks' after them
bool MakeEntries(Fighter& f, uint32_t& mem, uint32_t end, const std::vector<uint32_t>& offsets,
                 const std::vector<uint32_t>& sizes) {
    auto take = [&](uint32_t n) {
        const uint32_t at = mem;
        mem += (n + 3) & ~3u;
        return mem <= end ? at : 0u;
    };
    auto text = [&](const std::string& s) {
        const uint32_t at = take(uint32_t(s.size() + 1));
        if (at) std::memcpy(kernel::HostPtr(at), s.c_str(), s.size() + 1);
        return at;
    };
    uint32_t count = 0;
    while (count < 64 && kernel::R32(f.list + 12 + 12 * count)) count++;
    const uint32_t looks = uint32_t(f.looks.size());
    const uint32_t list = take(12 + 12 * (count + looks) + 12), own = take(12), file_name = text(f.looks_file);
    if (!list || !own || !file_name) return false;
    kernel::W32(own, file_name);
    kernel::W32(own + 4, list);
    kernel::W32(own + 8, kernel::R32(kernel::R32(f.list) + 8));
    kernel::W32(list, own);
    kernel::W32(list + 4, kernel::R32(f.list + 4));
    kernel::W32(list + 8, kernel::R32(f.list + 8));
    uint32_t i = 0;
    for (; i < count; i++)
        for (int k = 0; k < 3; k++) kernel::W32(list + 12 + 12 * i + 4 * k, kernel::R32(f.list + 12 + 12 * i + 4 * k));
    f.look_entry.clear();
    for (uint32_t k = 0; k < looks; k++, i++) {
        const Look& l = f.looks[k];
        const uint32_t entry = take(12), name = text(l.entry_name);
        if (!entry || !name) return false;
        kernel::W32(entry, name);
        kernel::W32(entry + 4, list);
        kernel::W32(entry + 8, kernel::R32(f.entry[l.to_colours] + 8));
        f.look_entry.push_back(entry);
        kernel::W32(list + 12 + 12 * i, entry);
        kernel::W32(list + 16 + 12 * i, offsets[k]);
        kernel::W32(list + 20 + 12 * i, sizes[k]);
    }
    for (int k = 0; k < 3; k++) kernel::W32(list + 12 + 12 * i + 4 * k, 0);
    return true;
}

// Raiden's picture on the select screen in the deeper blue too: pselart.ssf's second section (the pictures of the
// first costumes), items 5 (his head, 256 x 128) and 6 (his body, 256 x 256); left as it is when they are not there
void MakePicture(const fs::path& dir) {
    constexpr const char* kFile = "pselart.ssf";
    constexpr size_t kItems[2] = {5, 6};
    constexpr uint32_t kSizes[2][2] = {{256, 128}, {256, 256}};
    Bytes d;
    auto make = [&]() -> std::string {
        if (!kernel::ReadGameFile(std::string("\\Device\\CdRom0\\artxbox\\") + kFile, d)) return "no file";
        if (d.size() < 0x18 + 24 || U32(d, 0) != 0x53454320u || U32(d, 16) < 2) return "not the file it was";
        const size_t at = U32(d, 0x18 + 12 + 4), size = U32(d, 0x18 + 12 + 8);
        std::vector<Item> items;
        if (!ReadSection(d, at, size, items)) return "no second section";
        for (int k = 0; k < 2; k++) {
            Texture t;
            if (!FindItem(d, at, items, kItems[k], t) || t.w != kSizes[k][0] || t.h != kSizes[k][1])
                return "his picture is not where it was";
            for (int i = 0; i < 256; i++) Recolour(d.data() + t.palette + 4 * i, kDeepBluePicture);
        }
        fs::path path;
        if (!Keep(dir, kFile, d, path)) return "cannot write " + (dir / kFile).string();
        g_served.push_back({"\\ARTXBOX\\" + Upper(kFile), path.string()});
        return {};
    };
    if (const std::string why = make(); !why.empty())
        LOGE("costumes: RAIDEN's picture as it was (%s: %s)", kFile, why.c_str());
}

// at the first frame after boot, the same moment on both games of a match (guest memory lands where it would on the
// other game): the files, then the entries
void Make(uint64_t) {
    static bool done = false;
    if (done) return;
    done = true;
    const fs::path dir = app::ExeDir() / "costumes";
    constexpr uint32_t kBytes = 0x800;
    uint32_t mem = mem::AllocContiguous(kBytes, 0, 0xFFFFFFFF, 16);
    if (!mem) {
        LOGE("costumes: no guest memory, the game's costumes only");
        return;
    }
    const uint32_t end = mem + kBytes;
    for (uint32_t i = 0; i < kBytes; i += 4) kernel::W32(mem + i, 0);
    for (Fighter& f : g_fighters) {
        Bytes game, changed, looks;
        std::vector<uint32_t> offsets, sizes;
        fs::path changed_path, looks_path;
        auto make = [&]() -> std::string {
            if (!ReadRows(f)) return "the game's table is not as expected";
            if (!kernel::ReadGameFile(std::string("\\Device\\CdRom0\\artxbox\\") + f.file, game)) return "no file";
            if (!f.in_place.empty()) {
                changed = game;
                for (const InPlace& c : f.in_place)
                    if (std::string why = Change(changed, f.off[c.colours], f.size[c.colours], c.rebuild, c.recolour);
                        !why.empty())
                        return why;
                if (!Keep(dir, f.file, changed, changed_path)) return "cannot write " + (dir / f.file).string();
            }
            if (f.looks.empty()) return {};
            if (std::string why = MakeLooks(f, game, looks, offsets, sizes); !why.empty()) return why;
            if (!Keep(dir, f.looks_file, looks, looks_path)) return "cannot write " + (dir / f.looks_file).string();
            if (!MakeEntries(f, mem, end, offsets, sizes)) return "no room for the entries";
            return {};
        };
        if (const std::string why = make(); !why.empty()) {
            LOGE("costumes: %s keeps the game's costumes (%s: %s)", f.name, f.file, why.c_str());
            continue;
        }
        if (!f.in_place.empty()) g_served.push_back({"\\ARTXBOX\\" + Upper(f.file), changed_path.string()});
        if (!f.looks.empty()) g_served.push_back({"\\ARTXBOX\\" + Upper(f.looks_file), looks_path.string()});
        f.ready = true;
        LOGR("costumes: %s's %s costume made from %s (%zu looks)", f.name, f.second ? "second" : "first", f.file,
             f.looks.size());
    }
    MakePicture(dir);
    mk3d::AddOpenHook(OpenHook);
    g_ready.store(true, std::memory_order_release);
}

const Fighter* Find(uint32_t character) {
    if (!g_ready.load(std::memory_order_acquire)) return nullptr;
    for (const Fighter& f : g_fighters)
        if (f.character == character && f.ready) return &f;
    return nullptr;
}

// before each lookup of the table: a row with a look holds its entry (a mirror look only while a player of that
// fighter in that row has kMirrorLook), every other row the game's own
void Apply() {
    if (!g_ready.load(std::memory_order_acquire)) return;
    for (const Fighter& f : g_fighters) {
        if (!f.ready || f.looks.empty()) continue;
        uint32_t want[2] = {f.entry[0], f.entry[1]};
        for (size_t k = 0; k < f.looks.size(); k++) {
            const Look& l = f.looks[k];
            bool shown = !l.mirror;
            for (uint32_t p = 0; p < 2 && !shown; p++) {
                const uint32_t va = kernel::R32(kCostume[p]);
                if (!va || kernel::R32(kCharacter[p]) != f.character) continue;
                const uint32_t w = kernel::R32(va);
                shown = (w & kMirrorLook) && bool(w & 1) == f.second && bool(w & 2) == l.to_colours;
            }
            if (shown) want[l.to_colours] = f.look_entry[k];
        }
        for (int c = 0; c < 2; c++) {
            const uint32_t at = f.RowAt(c != 0);
            if (kernel::R32(at) == want[c]) continue;
            kernel::W32(at, want[c]);
            const char* what = "the game's";
            for (size_t k = 0; k < f.looks.size(); k++)
                if (want[c] == f.look_entry[k]) what = f.looks[k].entry_name;
            LOGR("costumes: %s's %s costume, %s: %s", f.name, f.second ? "second" : "first",
                 c ? "other colours" : "own colours", what);
        }
    }
}

const bool g_registered = (OnFrameStart(Make), true);

}  // namespace

void SelectEnds(uint32_t word[2]) {
    word[0] &= ~kMirrorLook;
    word[1] &= ~kMirrorLook;
    const uint32_t character = kernel::R32(kCharacter[0]);
    const Fighter* f = character == kernel::R32(kCharacter[1]) ? Find(character) : nullptr;
    if (!f) return;
    for (const Look& l : f->looks) {
        if (!l.mirror) continue;
        for (int k = 0; k < 2; k++)
            if (bool(word[k] & 1) != f->second || !(word[k] & kAltPick)) return;
        // both asked for the costume's other colours: the one the mirror rule put back in its own gets the look
        for (int k = 0; k < 2; k++)
            if (bool(word[k] & 2) == l.to_colours) {
                word[k] |= kMirrorLook;
                LOGR("costumes: both players asked for %s's other colours: P%d %s", f->name, k + 1, l.entry_name);
                return;
            }
    }
}

}  // namespace mkda::costumes

// the costume table's lookup (character, colours, costume): the rows of the looks first (costumes.h)
X86_FUNC(sub_00103150) {
    mkda::costumes::Apply();
    __imp__sub_00103150(ctx, base);
}
