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
// the fighter's shared section's entry (sh_drahmin.sec: his mask's model, Dmask), every costume's; the loader reads it
// right after the fighter's costume lookup (sub_00103250 -> sub_00103230)
constexpr uint32_t kShared = 0x80;
constexpr uint32_t kScorpion = 0, kJax = 1, kShangTsung = 2, kRaiden = 10, kFrost = 11, kDrahmin = 18,
                   kBoRaiCho = 20;
// the game's logical buttons (frame.h): A and START (the picks), X and Y (square and triangle)
constexpr uint32_t kButtonA = 0x40, kButtonStart = 0x800, kButtonSquare = 0x80, kButtonTriangle = 0x10;
// the select screen: each player's cursor slot (P1, P2), the grid of slots (a character index each, 0x18 its end)
constexpr uint32_t kSelectCursor[2] = {0x2EA660, 0x2EA65C};
constexpr uint32_t kGrid = 0x273E38, kGridStride = 0x2C, kGridSlots = 32;
// a player's own look of a costume: a Triangle or a Square look
constexpr uint32_t kLooks = kThird | kFourth;

// --- the looks: palette colours near a hue (and saturated enough) become another hue, saturation and value ---

struct Rule {
    double center, inner, outer, s_min;  // the colours it takes: hue within inner of center, fading out at outer
    double hue, sat, val, s_floor;       // what they become: that hue, saturation * sat (at least s_floor), value * val
    double v_max = 2.0;                  // and only up to this value, fading out 0.15 above it
};

// Raiden's teal (hue 160 to 200, value about 0.15 in the files: the game's lighting brightens it)
constexpr Rule kDeepBlue{185, 35, 60, 0.2, 228, 1.0, 1.3, 0.85};
constexpr Rule kRed{185, 35, 60, 0.2, 356, 1.0, 1.25, 0.85};
// Scorpion: the gold rims of his gear (sc_armor: shoulder pads, the plates on his thighs, knee guards; sc_pads: shin
// and forearm guards) orange
constexpr Rule kScorpionGearOrange{45, 18, 30, 0.3, 30, 1.2, 1.3, 0.9};
// Bo' Rai Cho's second costume: its red pants (hue about 352) black; the yellow belt and the gold stay
constexpr Rule kBoBlackPants{352, 20, 35, 0.3, 0, 0.0, 0.5, 0.0};
// every colour a dark grey, a quarter of its value (Jax's hood in his two looks: light grey cloth, his cap black already)
constexpr Rule kAllDark{0, 180, 180, -0.1, 0, 0.0, 0.25, 0.0};
// Drahmin: the green of his mask's flat parts (Dflat) ivory (Crimson)
constexpr Rule kIvoryFlat{110, 30, 50, 0.2, 42, 0.3, 1.2, 0.17};


// every colour OUTSIDE a kept band (hue within inner of center, fading out at outer; saturation and value above s_min
// and v_min, fading over 0.08) becomes that hue and saturation, value * val
struct Except {
    double center, inner, outer, s_min, v_min;
    double hue, sat, val;
};
// Drahmin's cloth wraps maroon (Crimson: all of them; his chest's but its logo, gold by then: kCrimsonWrapsKeepGold
// keeps kRampGold's hues) or charcoal (Bronze: the red logo on his chest kept; the arms' wraps keep nothing, s_min
// past 1)
constexpr Except kCrimsonWraps{355, 18, 30, 2.0, 0.12, 352, 0.72, 0.5};
constexpr Except kCrimsonWrapsKeepGold{40, 8, 14, 0.35, 0.12, 352, 0.72, 0.5};
constexpr Except kBronzeCloth{355, 18, 30, 0.35, 0.12, 30, 0.08, 0.28};
constexpr Except kBronzeWraps{355, 18, 30, 2.0, 0.12, 30, 0.08, 0.28};
// Drahmin's mask in Crimson (sh_drahmin.sec's Dmask): its painted markings (every saturated colour: cyan, green,
// yellow, orange, the blue around its teeth, the red of its lips) red; then its pale green ground (little saturated)
// ivory
constexpr Rule kCrimsonMarkings{0, 180, 180, 0.42, 356, 1.0, 0.8, 0.75};
constexpr Except kCrimsonMaskBase{0, 180, 180, 0.5, 0.0, 42, 0.17, 1.05};
// Shang Tsung's robe, sashes, belt, boots (and hair) black, his gold (hue 23 to 69, saturated, not too dark) kept
constexpr Except kBlackButGold{46, 23, 26, 0.45, 0.2, 0, 0.0, 0.22};

// A texture made again by a map of its texels (costumes_maps.h, from its UV layout): each texel of a ramp's class keeps
// its brightness on that ramp of a new palette (64 entries each, 48 with four ramps), each kept texel (255, or a class
// past the ramps) its colour
struct Ramp {
    uint8_t dark[3], light[3];  // B G R as the palette holds them (the game's light makes them brighter)
};
constexpr Ramp kRampBlack{{4, 4, 5}, {58, 54, 50}}, kRampOrange{{0, 24, 70}, {10, 140, 255}},
               kRampGloveRed{{6, 6, 105}, {40, 45, 250}}, kRampWhite{{150, 153, 156}, {246, 248, 250}},
               kRampDeepBlack{{3, 3, 3}, {34, 32, 30}}, kRampGold{{5, 45, 70}, {80, 200, 250}};
// Frost: her gi's deep sapphire (narrow: one even colour), the darker blue of Deep Blue's lines, jet black, bronze
constexpr Ramp kRampSapphire{{88, 26, 14}, {122, 40, 21}}, kRampFrostLines{{50, 12, 6}, {160, 48, 22}},
               kRampJet{{4, 4, 4}, {44, 42, 40}}, kRampBronze{{14, 24, 30}, {100, 150, 175}};
constexpr uint8_t kKeep = 255;
struct Rebuild {
    const char* texture;
    const char* rename;
    const ClassMap* map;
    const Ramp* ramps[4];
    int ramp_count;
    const Except* keep = nullptr;  // texels of class keep_class whose colour is inside this band keep it
    int keep_class = 0;
    // classes (bit 1 << class) whose texels take the colour of the nearest texel of class fill_from (spreading out from
    // it): their marks gone
    uint32_t fill = 0;
    int fill_from = 0;
    // a ramp class whose texels' brightness is smoothed over a box of its own texels (2 * blur + 1 wide): one even
    // colour with soft shading
    int blur_class = -1;
    int blur = 0;
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

double HueWeight(double h, double center, double inner, double outer) {
    double d = std::fmod(std::fabs(h - center), 360.0);
    d = std::min(d, 360.0 - d);
    if (d <= inner) return 1.0;
    if (d >= outer) return 0.0;
    return (outer - d) / (outer - inner);
}

uint8_t Byte(double x) { return uint8_t(std::floor(Clamp(x) * 255.0 + 0.5)); }

// one palette entry, B G R A
void Recolour(uint8_t* bgra, const Rule& rule) {
    const double r = bgra[2] / 255.0, g = bgra[1] / 255.0, b = bgra[0] / 255.0;
    double h, s, v;
    RgbToHsv(r, g, b, h, s, v);
    const double w = HueWeight(h, rule.center, rule.inner, rule.outer) * Clamp((s - rule.s_min) / 0.1) *
                     Clamp((rule.v_max + 0.15 - v) / 0.15);
    if (w <= 0) return;
    double nr, ng, nb;
    HsvToRgb(rule.hue, Clamp(std::max(rule.s_floor, s * rule.sat)), Clamp(v * rule.val), nr, ng, nb);
    bgra[2] = Byte(r + (nr - r) * w);
    bgra[1] = Byte(g + (ng - g) * w);
    bgra[0] = Byte(b + (nb - b) * w);
}

// how far a colour is inside an Except's kept band, 0 to 1
double Kept(const uint8_t* bgra, const Except& e) {
    double h, s, v;
    RgbToHsv(bgra[2] / 255.0, bgra[1] / 255.0, bgra[0] / 255.0, h, s, v);
    return HueWeight(h, e.center, e.inner, e.outer) * Clamp((s - e.s_min) / 0.08 + 0.5) * Clamp((v - e.v_min) / 0.08 + 0.5);
}

void Recolour(uint8_t* bgra, const Except& e) {
    const double r = bgra[2] / 255.0, g = bgra[1] / 255.0, b = bgra[0] / 255.0;
    double h, s, v;
    RgbToHsv(r, g, b, h, s, v);
    const double w = 1.0 - Kept(bgra, e);
    if (w <= 0) return;
    double nr, ng, nb;
    HsvToRgb(e.hue, e.sat, Clamp(v * e.val), nr, ng, nb);
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
    const Rule* rule;               // a hue shift, or (null)
    const char* rename;             // null: it keeps its name (another change of the texture renames it)
    const Except* except = nullptr;  // every colour outside a band
};

// the recolourings in the section at `at`, in order; empty, or what failed
std::string Recolour(Bytes& d, size_t at, size_t size, const std::vector<Recolouring>& changes) {
    std::vector<Item> items;
    if (!ReadSection(d, at, size, items)) return "no section at " + std::to_string(at);
    for (const Recolouring& c : changes) {
        Texture t;
        if (!FindTexture(d, at, items, c.texture, t)) return std::string("no texture ") + c.texture;
        for (int i = 0; i < 256; i++) {
            if (c.rule) Recolour(d.data() + t.palette + 4 * i, *c.rule);
            if (c.except) Recolour(d.data() + t.palette + 4 * i, *c.except);
        }
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

// a texture made again by its map: ramp k takes palette per * k to per * k + per - 1 (per: 64, 48 with four ramps),
// each class's texels spread over their own
// brightness (the class's 2nd to 98th percentile; filled and smoothed first, should the Rebuild ask); the kept texels
// (255, or a class with no ramp) keep their colours, the most used of them in the palette's other entries, the rest the
// nearest of those; empty, or what failed
std::string RebuildTexture(Bytes& d, size_t at, size_t size, const Rebuild& r) {
    std::vector<Item> items;
    if (!ReadSection(d, at, size, items)) return "no section at " + std::to_string(at);
    Texture t;
    if (!FindTexture(d, at, items, r.texture, t)) return std::string("no texture ") + r.texture;
    if (t.w != r.map->w || t.h != r.map->h) return std::string("the map is not the size of ") + r.texture;
    if (r.ramp_count < 1 || r.ramp_count > 4) return std::string("no ramps for ") + r.texture;
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
    const int k = r.ramp_count, per = k <= 3 ? 64 : 48;
    if (r.fill) {
        // outward from class fill_from, one ring of neighbours at a time
        std::vector<uint32_t> queue;
        for (size_t m = 0; m < n; m++)
            if (of[m] == r.fill_from) queue.push_back(uint32_t(m));
        for (size_t q = 0; q < queue.size(); q++) {
            const uint32_t m = queue[q], x = m % t.w, y = m / t.w;
            const uint32_t next[4] = {x > 0 ? m - 1 : m, x + 1 < t.w ? m + 1 : m, y > 0 ? m - t.w : m,
                                      y + 1 < t.h ? m + t.w : m};
            for (const uint32_t j : next)
                if (j != m && of[j] < 32 && (r.fill >> of[j] & 1)) {
                    of[j] = uint8_t(r.fill_from);
                    idx[j] = idx[m];
                    queue.push_back(j);
                }
        }
    }
    if (r.keep)
        for (size_t m = 0; m < n; m++)
            if (of[m] == r.keep_class && Kept(pal[idx[m]], *r.keep) >= 0.5) of[m] = kKeep;
    // each texel's brightness; a smoothed class's the mean of its own texels around it, rows then columns
    std::vector<double> bright(n);
    for (size_t m = 0; m < n; m++) bright[m] = lum[idx[m]];
    if (r.blur > 0 && r.blur_class >= 0 && r.blur_class < k) {
        const int w = int(t.w), h = int(t.h), b = r.blur;
        std::vector<double> sum(n, 0.0), count(n, 0.0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                double s = 0, c = 0;
                for (int i = std::max(0, x - b); i <= std::min(w - 1, x + b); i++)
                    if (const size_t j = size_t(y) * w + i; of[j] == r.blur_class) s += bright[j], c += 1;
                sum[size_t(y) * w + x] = s;
                count[size_t(y) * w + x] = c;
            }
        std::vector<double> smooth(bright);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t m = size_t(y) * w + x;
                if (of[m] != r.blur_class) continue;
                double s = 0, c = 0;
                for (int i = std::max(0, y - b); i <= std::min(h - 1, y + b); i++)
                    s += sum[size_t(i) * w + x], c += count[size_t(i) * w + x];
                smooth[m] = s / c;  // the texel itself counts
            }
        bright.swap(smooth);
    }
    double lo[4] = {}, hi[4] = {};
    for (int c = 0; c < k; c++) {
        std::vector<double> vals;
        for (size_t m = 0; m < n; m++)
            if (of[m] == c) vals.push_back(bright[m]);
        if (vals.empty()) continue;
        std::sort(vals.begin(), vals.end());
        lo[c] = vals[vals.size() / 50];
        hi[c] = vals[vals.size() - 1 - vals.size() / 50];
    }
    // the kept colours: the most used first (then the lower index), as many as the palette has room for
    uint32_t used[256] = {};
    for (size_t m = 0; m < n; m++)
        if (of[m] >= k) used[idx[m]]++;
    std::vector<int> order;
    for (int i = 0; i < 256; i++)
        if (used[i]) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return used[a] > used[b]; });
    const size_t room = size_t(256 - per * k);
    if (order.size() > room) order.resize(room);
    int new_index[256];
    std::fill(std::begin(new_index), std::end(new_index), -1);
    for (size_t q = 0; q < order.size(); q++) new_index[order[q]] = int(per * k + q);
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
            if (of[m] >= k) {
                out = uint8_t(new_index[idx[m]] >= 0 ? new_index[idx[m]] : nearest(idx[m]));
            } else {
                const int c = of[m];
                const double v = Clamp((bright[m] - lo[c]) / std::max(1e-6, hi[c] - lo[c]));
                out = uint8_t(per * c + int(std::floor(v * (per - 1) + 0.5)));
            }
        }
    for (int c = 0; c < k; c++)
        for (int q = 0; q < per; q++) {
            uint8_t* e = d.data() + t.palette + 4 * (per * c + q);
            for (int ch = 0; ch < 3; ch++)
                e[ch] = uint8_t(std::floor(r.ramps[c]->dark[ch] +
                                           (r.ramps[c]->light[ch] - r.ramps[c]->dark[ch]) * (q / double(per - 1)) + 0.5));
            e[3] = 255;
        }
    for (size_t q = 0; q < order.size(); q++) std::memcpy(d.data() + t.palette + 4 * (per * k + q), pal[order[q]], 4);
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
// row that shows it (that row's models look for that id): for everyone in that row, or only for a player of the
// fighter there with kMirrorLook, kThird (a Triangle look) or kFourth (a Square look, costumes.h)
enum class Shown { Always, Mirror, Triangle, Square };
struct Look {
    bool from_colours;       // the section it starts from: the other colours' (true), else the costume's own
    bool to_colours;         // the row that shows it: the other colours' (true), else the costume's own
    Shown shown;
    const char* entry_name;  // its entry's name (the game's entries have one: "RAYDEN.SEC")
    std::vector<Rebuild> rebuild;       // first
    std::vector<Recolouring> recolour;  // then, in order
    // its own copy of the fighter's shared section (kShared), recoloured, for whoever's costume lookup lands on this
    // look (the copy keeps its section id: the models find it in that player's sections); none when empty
    const char* shared_name = nullptr;
    std::vector<Recolouring> shared;
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
    // the shared section (when a look has its own copy): the game's entry, its place in the file; the looks' copies'
    // entries (0: none)
    uint32_t shared_entry = 0, shared_off = 0, shared_size = 0;
    std::vector<uint32_t> look_shared_entry;
    bool ready = false;
    bool HasShared() const {
        return std::any_of(looks.begin(), looks.end(), [](const Look& l) { return !l.shared.empty(); });
    }
    bool HasTriangle() const {
        return std::any_of(looks.begin(), looks.end(), [](const Look& l) { return l.shown == Shown::Triangle; });
    }
    bool HasSquare() const {
        return std::any_of(looks.begin(), looks.end(), [](const Look& l) { return l.shown == Shown::Square; });
    }
    uint32_t RowAt(bool colours) const {
        return row + (second ? (colours ? kSecondColours : kSecond) : (colours ? kFirstColours : kFirst));
    }
};

// Raiden's first costume, its teal (rd_blue, the cape, the hair's cape) a deeper blue (SQUARE) or red (TRIANGLE)
const std::vector<Recolouring> kRaidenDeepBlue = {
    {"rd_blue", &kDeepBlue, "rb_blue"}, {"rd_cape", &kDeepBlue, "rb_cape"}, {"rd_haircape", &kDeepBlue, "rb_haircape"}};
const std::vector<Recolouring> kRaidenRed = {
    {"rd_blue", &kRed, "r3_blue"}, {"rd_cape", &kRed, "r3_cape"}, {"rd_haircape", &kRed, "r3_haircape"}};
// Scorpion's first costume black with its own gold (SQUARE), or black and orange, his gear's gold rims orange too
// (TRIANGLE)
const std::vector<Rebuild> kScorpionBlackGold = {{"sc_gold", "s3_gold", &kScorpionGold, {&kRampBlack}, 1}};
const std::vector<Rebuild> kScorpionBlackOrange = {
    {"sc_gold", "s4_gold", &kScorpionGold, {&kRampBlack, &kRampOrange}, 2}};
const std::vector<Recolouring> kScorpionGearToOrange = {{"sc_armor", &kScorpionGearOrange, "s4_armor"},
                                                        {"sc_pads", &kScorpionGearOrange, "s4_pads"}};
// Bo' Rai Cho's second costume with black pants (SQUARE)
const std::vector<Recolouring> kBoRaiChoBlackPants = {{"AltBRC_pants", &kBoBlackPants, "BlkBRC_pants"}};

// Jax, on his second costume's other colours (the game's red jacket): SQUARE the belt white, the undershirt and its
// hood black (his cap black already), his own black gloves; TRIANGLE the same with the gloves red. Every texture a look
// changes is renamed (the game's own sections of that costume keep the names)
const std::vector<Rebuild> kJaxBlackGlovesRebuild = {{"altJx_boot", "blgJx_belt", &kJaxBelt, {&kRampWhite}, 1},
                                                     {"ap-alt_vest", "bg-alt_vest", &kJaxShirt, {&kRampBlack}, 1}};
const std::vector<Recolouring> kJaxBlackGlovesRecolour = {{"altJX_hoodie", &kAllDark, "bglJX_hoodie"}};
const std::vector<Rebuild> kJaxRedGlovesRebuild = {
    {"altJx_boot", "redJx_belt", &kJaxGloveBelt, {&kRampGloveRed, &kRampGloveRed, &kRampWhite}, 3},
    {"ap-alt_vest", "c3-alt_vest", &kJaxShirt, {&kRampBlack}, 1}};
const std::vector<Recolouring> kJaxRedGlovesRecolour = {{"altJX_hoodie", &kAllDark, "blkJX_hoodie"}};

// Drahmin's Bronze (SQUARE): his second costume's other colours (bronze metal already) with the wraps charcoal; his
// banner's border and glyphs keep those colours' red (grey green in 1.0.17 and 1.0.18)
const std::vector<Recolouring> kDrahminBronze = {{"Dchestwrap", nullptr, "Bchestwrap", &kBronzeCloth},
                                                 {"Darmwrap", nullptr, "Barmwrap", &kBronzeWraps}};
// Drahmin's Crimson (TRIANGLE): his second costume itself (its grey metal) with maroon wraps, the logo on his chest
// gold (the red of its own was lost in the maroon), gold glyphs on his banner (its red border kept), and its own copy of
// his shared section for an ivory mask with red markings
const std::vector<Rebuild> kDrahminCrimsonRebuild = {
    {"Dloincloth", "Cloincloth", &kDrahminBannerGlyphs, {&kRampGold}, 1},
    {"Dchestwrap", "Cchestwrap", &kDrahminChestLogo, {&kRampGold}, 1}};
const std::vector<Recolouring> kDrahminCrimson = {{"Cchestwrap", nullptr, nullptr, &kCrimsonWrapsKeepGold},
                                                  {"Darmwrap", nullptr, "Carmwrap", &kCrimsonWraps},
                                                  {"Dflat", &kIvoryFlat, "Cflat"}};
const std::vector<Recolouring> kDrahminCrimsonMask = {
    {"Dmask", &kCrimsonMarkings, "Cmask"}, {"Cmask", nullptr, nullptr, &kCrimsonMaskBase}, {"Dflat", &kIvoryFlat, "Cflat"}};
// Shang Tsung's black and gold (SQUARE): everything of his outfit that is not gold black, his pants and
// his brown leather gloves all black (kShangHands), made from his first costume; his face (ShangTsungTexture3) untouched
const std::vector<Rebuild> kShangBlackGoldRebuild = {
    {"ShangTsungTexture1", "ShangTsungTexturY1", &kShangHands, {&kRampDeepBlack, &kRampDeepBlack}, 2, &kBlackButGold, 1}};
const std::vector<Recolouring> kShangBlackGold = {{"ShangTsungTexture2", nullptr, "ShangTsungTexturY2", &kBlackButGold}};

// Frost's All Black (Jet Black, SQUARE) and Deep Blue (TRIANGLE), the concept's, both made on her first costume: her
// gi (the straps, the front panel, the mask) one deep sapphire blue, its marks gone (made of the nearest cloth,
// smoothed), the medallion on her strap bronze; her top, sleeves, gloves, pants and boots jet black; Jet Black's gloves
// and boots all black (their lines and the forearms' light panels gone) and the belt around her leg and its tie black,
// Deep Blue's gloves and boots with their lines in a darker blue and the belt and its tie the gi's blue
const std::vector<Rebuild> kFrostJetBlack = {
    {.texture = "FR_belt", .rename = "FJ_belt", .map = &kFrostBelt,
     .ramps = {&kRampSapphire, &kRampJet, &kRampBronze, &kRampJet}, .ramp_count = 4, .fill = 1u << 4, .fill_from = 0,
     .blur_class = 0, .blur = 4},
    {.texture = "FR_chest", .rename = "FJ_chest", .map = &kFrostChest, .ramps = {&kRampJet, &kRampSapphire},
     .ramp_count = 2, .fill = 1u << 2, .fill_from = 1, .blur_class = 1, .blur = 4},
    {.texture = "FR_arms", .rename = "FJ_arms", .map = &kFrostArms, .ramps = {&kRampJet}, .ramp_count = 1,
     .fill = 1u << 1 | 1u << 2, .fill_from = 0},
    {.texture = "FR_boot", .rename = "FJ_boot", .map = &kFrostBoot, .ramps = {&kRampJet}, .ramp_count = 1,
     .fill = 1u << 1, .fill_from = 0},
    {.texture = "FR_pants", .rename = "FJ_pants", .map = &kFrostPants, .ramps = {&kRampJet}, .ramp_count = 1}};
const std::vector<Rebuild> kFrostDeepBlue = {
    {.texture = "FR_belt", .rename = "FD_belt", .map = &kFrostBelt,
     .ramps = {&kRampSapphire, &kRampSapphire, &kRampBronze, &kRampSapphire}, .ramp_count = 4, .fill = 1u << 4,
     .fill_from = 0, .blur_class = 0, .blur = 4},
    {.texture = "FR_chest", .rename = "FD_chest", .map = &kFrostChest, .ramps = {&kRampJet, &kRampSapphire},
     .ramp_count = 2, .fill = 1u << 2, .fill_from = 1, .blur_class = 1, .blur = 4},
    {.texture = "FR_arms", .rename = "FD_arms", .map = &kFrostArms, .ramps = {&kRampJet, &kRampFrostLines},
     .ramp_count = 2, .fill = 1u << 2, .fill_from = 0},
    {.texture = "FR_boot", .rename = "FD_boot", .map = &kFrostBoot, .ramps = {&kRampJet, &kRampFrostLines},
     .ramp_count = 2},
    {.texture = "FR_pants", .rename = "FD_pants", .map = &kFrostPants, .ramps = {&kRampJet}, .ramp_count = 1}};

std::vector<Fighter> g_fighters = {
    // SQUARE his first costume in a deeper blue; TRIANGLE red, made from it (his own colours, A, and other colours,
    // LT and A, the game's teal and purple)
    {kRaiden, "RAIDEN", false, "rayden.ssf", {}, "x_rayden.ssf",
     {{false, false, Shown::Square, "RAIDEN_DEEP_BLUE.SEC", {}, kRaidenDeepBlue},
      {false, true, Shown::Square, "RAIDEN_DEEP_BLUE_IN_COLOURS.SEC", {}, kRaidenDeepBlue},
      {false, false, Shown::Triangle, "RAIDEN_RED.SEC", {}, kRaidenRed},
      {false, true, Shown::Triangle, "RAIDEN_RED_IN_COLOURS.SEC", {}, kRaidenRed}}},
    // SQUARE his first costume black with its own gold (his mask, trims and studs as they are, his gear as the game's);
    // TRIANGLE black and orange
    {kScorpion, "SCORPION", false, "scorpion.ssf", {}, "x_scorpion.ssf",
     {{false, false, Shown::Square, "SCORPION_BLACK_GOLD.SEC", kScorpionBlackGold, {}},
      {false, true, Shown::Square, "SCORPION_BLACK_GOLD_IN_COLOURS.SEC", kScorpionBlackGold, {}},
      {false, false, Shown::Triangle, "SCORPION_BLACK_ORANGE.SEC", kScorpionBlackOrange, kScorpionGearToOrange},
      {false, true, Shown::Triangle, "SCORPION_BLACK_ORANGE_IN_COLOURS.SEC", kScorpionBlackOrange,
       kScorpionGearToOrange}}},
    // SQUARE his second costume with black pants (its other colours, LT and START, the game's purple)
    {kBoRaiCho, "BO' RAI CHO", true, "alt_drunkenfighter.ssf", {}, "x_drunkenfighter2.ssf",
     {{false, false, Shown::Square, "BO_RAI_CHO_BLACK_PANTS.SEC", {}, kBoRaiChoBlackPants},
      {false, true, Shown::Square, "BO_RAI_CHO_BLACK_PANTS_IN_COLOURS.SEC", {}, kBoRaiChoBlackPants}}},
    // on his second costume's other colours (the game's red jacket): SQUARE with the belt white and the undershirt and
    // its hood black, his own black gloves; TRIANGLE that look with red gloves (his own gloves, on his own model: the
    // same collision)
    {kJax, "JAX", true, "alt_jax.ssf", {}, "x_jax.ssf",
     {{true, false, Shown::Square, "JAX_BLACK_GLOVES.SEC", kJaxBlackGlovesRebuild, kJaxBlackGlovesRecolour},
      {true, true, Shown::Square, "JAX_BLACK_GLOVES_IN_COLOURS.SEC", kJaxBlackGlovesRebuild, kJaxBlackGlovesRecolour},
      {true, false, Shown::Triangle, "JAX_RED_GLOVES.SEC", kJaxRedGlovesRebuild, kJaxRedGlovesRecolour},
      {true, true, Shown::Triangle, "JAX_RED_GLOVES_IN_COLOURS.SEC", kJaxRedGlovesRebuild, kJaxRedGlovesRecolour}}},
    // the concept's Bronze and Crimson, on his second costume: SQUARE Bronze (charcoal wraps, his green mask, bronze
    // metal, a red banner), made from its other colours; TRIANGLE Crimson, made from the costume itself, each with its
    // own copy of his shared section (the mask). His skin (Dface, Dtorso) is never touched
    {kDrahmin, "DRAHMIN", true, "alt_drahmin.ssf", {}, "x_drahmin2.ssf",
     {{true, false, Shown::Square, "DRAHMIN_BRONZE.SEC", {}, kDrahminBronze},
      {true, true, Shown::Square, "DRAHMIN_BRONZE_IN_COLOURS.SEC", {}, kDrahminBronze},
      {false, false, Shown::Triangle, "DRAHMIN_CRIMSON.SEC", kDrahminCrimsonRebuild, kDrahminCrimson,
       "DRAHMIN_CRIMSON_MASK.SEC", kDrahminCrimsonMask},
      {false, true, Shown::Triangle, "DRAHMIN_CRIMSON_IN_COLOURS.SEC", kDrahminCrimsonRebuild, kDrahminCrimson,
       "DRAHMIN_CRIMSON_MASK_2.SEC", kDrahminCrimsonMask}}},
    // SQUARE black and gold on his first costume
    {kShangTsung, "SHANG TSUNG", false, "shangtsung.ssf", {}, "x_shangtsung.ssf",
     {{false, false, Shown::Square, "SHANG_TSUNG_BLACK_GOLD.SEC", kShangBlackGoldRebuild, kShangBlackGold},
      {false, true, Shown::Square, "SHANG_TSUNG_BLACK_GOLD_IN_COLOURS.SEC", kShangBlackGoldRebuild, kShangBlackGold}}},
    // on her first costume: SQUARE All Black (Jet Black), TRIANGLE Deep Blue
    {kFrost, "FROST", false, "subzerette.ssf", {}, "x_subzerette.ssf",
     {{false, false, Shown::Square, "FROST_JET_BLACK.SEC", kFrostJetBlack, {}},
      {false, true, Shown::Square, "FROST_JET_BLACK_IN_COLOURS.SEC", kFrostJetBlack, {}},
      {false, false, Shown::Triangle, "FROST_DEEP_BLUE.SEC", kFrostDeepBlue, {}},
      {false, true, Shown::Triangle, "FROST_DEEP_BLUE_IN_COLOURS.SEC", kFrostDeepBlue, {}}}},
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
    if (f.HasShared()) f.shared_entry = kernel::R32(f.row + kShared);
    for (int i = 0; i < 64; i++) {
        const uint32_t at = f.list + 12 + 12 * i, entry = kernel::R32(at);
        if (!entry) return f.size[0] && f.size[1] && (!f.HasShared() || f.shared_size);
        for (int c = 0; c < 2; c++)
            if (entry == f.entry[c]) f.off[c] = kernel::R32(at + 4), f.size[c] = kernel::R32(at + 8);
        if (f.shared_entry && entry == f.shared_entry) f.shared_off = kernel::R32(at + 4), f.shared_size = kernel::R32(at + 8);
    }
    return false;
}

size_t Align(size_t n) { return (n + 0x7FF) & ~size_t(0x7FF); }

// the looks' file: the game's file with each look at its end, a copy of the section it starts from under the section
// id of the row that shows it; the game's own sections stay where they are, for the list's other entries
std::string MakeLooks(const Fighter& f, const Bytes& game, Bytes& out, std::vector<uint32_t>& offsets,
                      std::vector<uint32_t>& sizes, std::vector<uint32_t>& shared_offsets,
                      std::vector<uint32_t>& shared_sizes) {
    out = game;
    uint32_t ids[2];
    for (int c = 0; c < 2; c++) {
        ids[c] = kernel::R32(f.RowAt(c != 0) + 4) >> 16;
        if (U32(game, f.off[c] + 12) != ids[c]) return "its section ids are not the table's";
    }
    const uint32_t n = U32(out, 16), looks = uint32_t(f.looks.size());
    uint32_t shared = 0;
    for (const Look& l : f.looks) shared += l.shared.empty() ? 0 : 1;
    if (U32(out, 0) != 0x53454320u || 0x18 + 12 * (n + looks + shared) > U32(out, 0x18 + 4))
        return "no room in the file's list";
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
    uint32_t added = looks;
    for (uint32_t k = 0; k < looks; k++) {
        const Look& l = f.looks[k];
        shared_offsets.push_back(0);
        shared_sizes.push_back(0);
        if (l.shared.empty()) continue;
        Bytes sec(game.begin() + f.shared_off, game.begin() + f.shared_off + f.shared_size);
        if (std::string why = Recolour(sec, 0, sec.size(), l.shared); !why.empty()) return "shared section: " + why;
        const size_t at = Align(out.size());
        out.resize(at + Align(sec.size()), 0);
        std::memcpy(out.data() + at, sec.data(), sec.size());
        shared_offsets.back() = uint32_t(at);
        shared_sizes.back() = uint32_t(sec.size());
        Put32(out, 0x18 + 12 * (n + added), 6);
        Put32(out, 0x1C + 12 * (n + added), uint32_t(at));
        Put32(out, 0x20 + 12 * (n + added), uint32_t(sec.size()));
        added++;
    }
    Put32(out, 16, n + added);
    Put32(out, 20, uint32_t(out.size()));
    return {};
}

// the looks' entries and file list in guest memory: a copy of the costume's list naming the looks' file, its own
// entries as they are and the looks' after them
bool MakeEntries(Fighter& f, uint32_t& mem, uint32_t end, const std::vector<uint32_t>& offsets,
                 const std::vector<uint32_t>& sizes, const std::vector<uint32_t>& shared_offsets,
                 const std::vector<uint32_t>& shared_sizes) {
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
    uint32_t shared = 0;
    for (const Look& l : f.looks) shared += l.shared.empty() ? 0 : 1;
    const uint32_t list = take(12 + 12 * (count + looks + shared) + 12), own = take(12), file_name = text(f.looks_file);
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
    f.look_shared_entry.assign(looks, 0);
    for (uint32_t k = 0; k < looks; k++) {
        const Look& l = f.looks[k];
        if (l.shared.empty()) continue;
        const uint32_t entry = take(12), name = text(l.shared_name);
        if (!entry || !name) return false;
        kernel::W32(entry, name);
        kernel::W32(entry + 4, list);
        kernel::W32(entry + 8, kernel::R32(f.shared_entry + 8));
        f.look_shared_entry[k] = entry;
        kernel::W32(list + 12 + 12 * i, entry);
        kernel::W32(list + 16 + 12 * i, shared_offsets[k]);
        kernel::W32(list + 20 + 12 * i, shared_sizes[k]);
        i++;
    }
    for (int k = 0; k < 3; k++) kernel::W32(list + 12 + 12 * i + 4 * k, 0);
    return true;
}


// at the first frame after boot, the same moment on both games of a match (guest memory lands where it would on the
// other game): the files, then the entries
void Make(uint64_t) {
    static bool done = false;
    if (done) return;
    done = true;
    const fs::path dir = app::ExeDir() / "costumes";
    constexpr uint32_t kBytes = 0x2000;
    uint32_t mem = mem::AllocContiguous(kBytes, 0, 0xFFFFFFFF, 16);
    if (!mem) {
        LOGE("costumes: no guest memory, the game's costumes only");
        return;
    }
    const uint32_t end = mem + kBytes;
    for (uint32_t i = 0; i < kBytes; i += 4) kernel::W32(mem + i, 0);
    for (Fighter& f : g_fighters) {
        Bytes game, changed, looks;
        std::vector<uint32_t> offsets, sizes, shared_offsets, shared_sizes;
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
            if (std::string why = MakeLooks(f, game, looks, offsets, sizes, shared_offsets, shared_sizes); !why.empty())
                return why;
            if (!Keep(dir, f.looks_file, looks, looks_path)) return "cannot write " + (dir / f.looks_file).string();
            if (!MakeEntries(f, mem, end, offsets, sizes, shared_offsets, shared_sizes)) return "no room for the entries";
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
    // a file of the folder this game does not give the game any more goes: up to 1.0.18 rayden.ssf, alt_jax.ssf and
    // pselart.ssf were the game's own changed in place
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        const std::string name = Upper(e.path().filename().string());
        if (!e.is_regular_file(ec) || name.size() < 4 || name.compare(name.size() - 4, 4, ".SSF") != 0) continue;
        const std::string suffix = "\\ARTXBOX\\" + name;
        if (std::none_of(g_served.begin(), g_served.end(), [&](const Served& s) { return s.suffix == suffix; }) &&
            fs::remove(e.path(), ec))
            LOGR("costumes: %s removed (not used any more)", name.c_str());
    }
    mk3d::AddOpenHook(OpenHook);
    g_ready.store(true, std::memory_order_release);
}

const Fighter* Find(uint32_t character) {
    if (!g_ready.load(std::memory_order_acquire)) return nullptr;
    for (const Fighter& f : g_fighters)
        if (f.character == character && f.ready) return &f;
    return nullptr;
}

// before each lookup of the table (character, colours, costume, as sub_00103150 has them): a row with a look holds its
// entry (a mirror look, a Triangle or a Square look only while a player of that fighter in that row has kMirrorLook,
// kThird or kFourth), every
// other row the game's own; the looked-up fighter's shared entry is its look's copy when the row it lands on has one
void Apply(uint32_t character, uint32_t colours, uint32_t costume) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    for (const Fighter& f : g_fighters) {
        if (!f.ready || f.looks.empty()) continue;
        uint32_t want[2] = {f.entry[0], f.entry[1]};
        for (size_t k = 0; k < f.looks.size(); k++) {
            const Look& l = f.looks[k];
            bool shown = l.shown == Shown::Always;
            const uint32_t bit = l.shown == Shown::Mirror ? kMirrorLook : l.shown == Shown::Square ? kFourth : kThird;
            for (uint32_t p = 0; p < 2 && !shown; p++) {
                const uint32_t va = kernel::R32(kCostume[p]);
                if (!va || kernel::R32(kCharacter[p]) != f.character) continue;
                const uint32_t w = kernel::R32(va);
                shown = (w & bit) && bool(w & 1) == f.second && bool(w & 2) == l.to_colours;
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
        if (!f.HasShared() || character != f.character) continue;
        uint32_t shared = f.shared_entry;
        if ((costume != 0) == f.second)
            for (size_t k = 0; k < f.looks.size(); k++)
                if (want[colours != 0] == f.look_entry[k] && f.look_shared_entry[k]) shared = f.look_shared_entry[k];
        if (kernel::R32(f.row + kShared) != shared) {
            kernel::W32(f.row + kShared, shared);
            LOGR("costumes: %s's shared section: %s", f.name, shared == f.shared_entry ? "the game's" : "the look's own");
        }
    }
}


const bool g_registered = (OnFrameStart(Make), true);

// A player's look (a Triangle or a Square look) has the row of their side from the pick on: P1 the costume's own, P2 its
// other colours (each look is under both rows' section ids, the same either way), and never moves. A look that took
// another row after its player's game had shown it on the select screen parted the two games of a match at once (the
// fighter's render-path body point +0xA28, from the fight's first frame: linked tests, 2026-11-25), whereas a plain
// costume moving to the other row is the game's own mirror rule and keeps them alike.
uint32_t LookRow(uint32_t player) { return player == 1 ? 2u : 0u; }

// both players of one fighter in the costume a look is made on, one in a look and the other plain: the plain one takes
// the row the look leaves (whatever it asked for); two looks have their sides' rows already. True when a word changed
bool Share(uint32_t word[2]) {
    if ((word[0] & 1) != (word[1] & 1)) return false;
    for (int k = 0; k < 2; k++) {
        const int o = 1 - k;
        if (!(word[k] & kLooks) || (word[o] & kLooks)) continue;
        const uint32_t row = (word[k] & 2u) ^ 2u;
        if ((word[o] & 2u) == row) return false;
        word[o] = (word[o] & ~2u) | row;
        return true;
    }
    return false;
}

// SQUARE (the pad's X) picks a fighter's Square look, TRIANGLE (the pad's Y) its Triangle look. The game picks with A
// and START only: a player choosing (state 2) on such a fighter who presses one of them gets, that frame, the press
// that picks the costume the look is made on (A for the first costume, START for the second), right after the pad
// update and before the select screen reads the buttons, and the pick that follows (Picked) takes the look. Neither
// does anything else on the select screen, online too (WHITE, L1, which picked costume 3 in 1.0.17 and 1.0.18, opens
// the handicap there outside practice, and nobody picks while it is open). Practice picks both fighters with P1's
// controller
uint64_t g_square_until[2] = {}, g_triangle_until[2] = {};
void LookPicks(uint64_t) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    const bool practice = kernel::R32(kGameMode) == kModePractice;
    for (uint32_t p = 0; p < 2; p++) {
        if (kernel::R32(kPlayerState[p]) != 2) continue;
        const uint32_t block = kPlayerBlock + (practice ? 0 : p) * kPlayerStride;
        const uint32_t pressed = kernel::R32(block + kPlayerPressed);
        if (!(pressed & (kButtonSquare | kButtonTriangle))) continue;
        const uint32_t slot = kernel::R32(kSelectCursor[p]);
        if (slot >= kGridSlots) continue;
        const Fighter* f = Find(kernel::R32(kGrid + slot * kGridStride));
        if (!f) continue;
        uint64_t* until = (pressed & kButtonSquare) && f->HasSquare()     ? &g_square_until[p]
                          : (pressed & kButtonTriangle) && f->HasTriangle() ? &g_triangle_until[p]
                                                                         : nullptr;
        if (!until) continue;
        const uint32_t pick = f->second ? kButtonStart : kButtonA;
        kernel::W32(block + kPlayerCurrent, kernel::R32(block + kPlayerCurrent) | pick);
        kernel::W32(block + kPlayerPressed, kernel::R32(block + kPlayerPressed) | pick);
        *until = Frame() + 1;
        // practice: P1's controller picks the opponent too, a pick the game may take as P2's
        if (practice) {
            uint64_t* both = until == &g_square_until[p] ? g_square_until : g_triangle_until;
            both[1 - p] = *until;
        }
    }
}
const bool g_looks_registered = (OnPadsRead(LookPicks), true);

}  // namespace

void Picked(uint32_t player) {
    if (player > 1) return;
    const uint32_t va = kernel::R32(kCostume[player]);
    if (!va) return;
    uint32_t w = kernel::R32(va) & ~kLooks;
    const Fighter* f = Find(kernel::R32(kCharacter[player]));
    const bool square = g_square_until[player] && Frame() <= g_square_until[player];
    const bool triangle = g_triangle_until[player] && Frame() <= g_triangle_until[player];
    g_square_until[player] = g_triangle_until[player] = 0;
    // a look of the costume the button picked (LookPicks), in its side's row (LookRow; not the other colours, should
    // LT be held too): SQUARE its Square look, TRIANGLE its Triangle look; any other pick the game's own costume
    const bool on_looks = f && bool(w & 1) == f->second;
    if (on_looks && square && f->HasSquare()) {
        w = (w & ~(kAltPick | 2u)) | LookRow(player) | kFourth;
        LOGR("costumes: P%u picked %s with SQUARE: its Square look (costume word %X)", player + 1, f->name, w);
    } else if (on_looks && triangle && f->HasTriangle()) {
        w = (w & ~(kAltPick | 2u)) | LookRow(player) | kThird;
        LOGR("costumes: P%u picked %s with TRIANGLE: its Triangle look (costume word %X)", player + 1, f->name, w);
    } else if (on_looks) {
        LOGR("costumes: P%u picked %s's %s costume, the game's (costume word %X)", player + 1, f->name,
             f->second ? "second" : "first", w);
    }
    kernel::W32(va, w);
    // the other player already picked this fighter: the plain one in the row the look leaves, before this pick's model
    // loads (a task on the next frames). Practice's dummy pick comes before its caller sets the states (events.h
    // kPracticePicker): there the other side's fighter being in is its pick.
    const uint32_t other = 1 - player, ova = kernel::R32(kCostume[other]);
    const bool other_picked = kernel::R32(kPlayerState[other]) == 3 ||
                              (kernel::R32(kGameMode) == kModePractice &&
                               kernel::R32(kCharacter[other]) != kNoCharacter);
    if (!f || !ova || !other_picked || kernel::R32(kCharacter[other]) != kernel::R32(kCharacter[player])) return;
    uint32_t word[2];
    word[player] = w;
    word[other] = kernel::R32(ova);
    if (!Share(word)) return;
    kernel::W32(va, word[player]);
    kernel::W32(ova, word[other]);
    LOGR("costumes: %s's look and the plain costume beside it: P1 %s, P2 %s", f->name,
         word[0] & 2 ? "other colours" : "own colours", word[1] & 2 ? "other colours" : "own colours");
}

int Settle(uint32_t word[2]) {
    for (int k = 0; k < 2; k++) {
        const Fighter* f = Find(kernel::R32(kCharacter[k]));
        if (!f || bool(word[k] & 1) != f->second) word[k] &= ~kLooks;
        if (!f || !f->HasTriangle()) word[k] &= ~kThird;
        if (!f || !f->HasSquare()) word[k] &= ~kFourth;
        // a look in its side's row: Picked put it there (a word from anywhere else as well)
        if (word[k] & kLooks) word[k] = (word[k] & ~2u) | LookRow(uint32_t(k));
    }
    const uint32_t character = kernel::R32(kCharacter[0]), a = word[0] & kLooks, b = word[1] & kLooks;
    if (character == kernel::R32(kCharacter[1]) && Share(word))
        LOGR("costumes: %s's look and the plain costume beside it: P1 %s, P2 %s", Find(character)->name,
             word[0] & 2 ? "other colours" : "own colours", word[1] & 2 ? "other colours" : "own colours");
    // the side the mirror rule may turn: never a look (two looks, or a look and a plain costume, are rows apart by now)
    return b && !a ? 0 : 1;
}

void SelectEnds(uint32_t word[2]) {
    word[0] &= ~kMirrorLook;
    word[1] &= ~kMirrorLook;
    const uint32_t character = kernel::R32(kCharacter[0]);
    const Fighter* f = character == kernel::R32(kCharacter[1]) ? Find(character) : nullptr;
    if (!f) return;
    for (const Look& l : f->looks) {
        if (l.shown != Shown::Mirror) continue;
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
    mkda::costumes::Apply(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8), kernel::R32(ctx.esp + 12));
    __imp__sub_00103150(ctx, base);
}
