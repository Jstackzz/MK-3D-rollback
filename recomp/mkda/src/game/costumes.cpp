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
// the game's logical WHITE (frame.h): L1 on a PlayStation pad (the runtime's left shoulder); A and X (cross and square)
constexpr uint32_t kButtonWhite = 0x1, kButtonA = 0x40, kButtonSquare = 0x80;
// the select screen: each player's cursor slot (P1, P2), the grid of slots (a character index each, 0x18 its end)
constexpr uint32_t kSelectCursor[2] = {0x2EA660, 0x2EA65C};
constexpr uint32_t kGrid = 0x273E38, kGridStride = 0x2C, kGridSlots = 32;
// a player's own look of a costume: costume 3 or a Square look
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
// his picture on the select screen: the cloth's darker colours only, so the lightning in his hand stays as it is
constexpr Rule kDeepBluePicture{185, 35, 60, 0.2, 228, 1.0, 1.3, 0.85, 0.45};
// Scorpion: the gold rims of his gear (sc_armor: shoulder pads, the plates on his thighs, knee guards; sc_pads: shin
// and forearm guards) orange
constexpr Rule kScorpionGearOrange{45, 18, 30, 0.3, 30, 1.2, 1.3, 0.9};
// Bo' Rai Cho's second costume: its red pants (hue about 352) black; the yellow belt and the gold stay
constexpr Rule kBoBlackPants{352, 20, 35, 0.3, 0, 0.0, 0.5, 0.0};
// every colour a dark grey, a quarter of its value (Jax's hood in his costume 3: light grey cloth, his cap black already)
constexpr Rule kAllDark{0, 180, 180, -0.1, 0, 0.0, 0.25, 0.0};
// Drahmin: the green of his mask's flat parts (Dflat) ivory (Crimson); his banner's red glyphs and border a grey green
// (Bronze)
constexpr Rule kIvoryFlat{110, 30, 50, 0.2, 42, 0.3, 1.2, 0.17};
constexpr Rule kGreyGreenGlyphs{355, 25, 40, 0.25, 140, 0.22, 0.8, 0.14};


// every colour OUTSIDE a kept band (hue within inner of center, fading out at outer; saturation and value above s_min
// and v_min, fading over 0.08) becomes that hue and saturation, value * val
struct Except {
    double center, inner, outer, s_min, v_min;
    double hue, sat, val;
};
// Drahmin's cloth wraps maroon (Crimson: all of them, the red glyph on his chest a darker maroon) or charcoal (Bronze:
// the red glyph on his chest kept; the arms' wraps keep nothing, s_min past 1)
constexpr Except kCrimsonWraps{355, 18, 30, 2.0, 0.12, 352, 0.72, 0.5};
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
// fighter there with kMirrorLook, kThird or kFourth (a Square look, costumes.h)
enum class Shown { Always, Mirror, Third, Square };
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
    bool HasThird() const {
        return std::any_of(looks.begin(), looks.end(), [](const Look& l) { return l.shown == Shown::Third; });
    }
    bool HasSquare() const {
        return std::any_of(looks.begin(), looks.end(), [](const Look& l) { return l.shown == Shown::Square; });
    }
    uint32_t RowAt(bool colours) const {
        return row + (second ? (colours ? kSecondColours : kSecond) : (colours ? kFirstColours : kFirst));
    }
};

// Jax's costume 3: his alternate costume 2 (below) with red gloves; a copy of the game's section (the red jacket) with
// the gloves red, the belt white, the undershirt and its hood black (his cap black already), under both rows' section
// ids, for whichever row it takes
const std::vector<Rebuild> kJaxThirdRebuild = {
    {"altJx_boot", "redJx_belt", &kJaxGloveBelt, {&kRampGloveRed, &kRampGloveRed, &kRampWhite}, 3},
    {"ap-alt_vest", "c3-alt_vest", &kJaxShirt, {&kRampBlack}, 1}};
const std::vector<Recolouring> kJaxThirdRecolour = {{"altJX_hoodie", &kAllDark, "blkJX_hoodie"}};

// Drahmin's Bronze (his second costume's other colours, LT and START): those colours (bronze metal already) with the
// wraps charcoal and the banner's glyphs grey green
const std::vector<Recolouring> kDrahminBronze = {{"Dchestwrap", nullptr, "Bchestwrap", &kBronzeCloth},
                                                 {"Darmwrap", nullptr, "Barmwrap", &kBronzeWraps},
                                                 {"Dloincloth", &kGreyGreenGlyphs, "Bloincloth"}};
// Drahmin's Crimson (his costume 3, L1 and START): his second costume itself (its grey metal) with maroon wraps, gold
// glyphs on his banner (its red border kept), and its own copy of his shared section for an ivory mask with red markings
const std::vector<Rebuild> kDrahminCrimsonRebuild = {
    {"Dloincloth", "Cloincloth", &kDrahminBannerGlyphs, {&kRampGold}, 1}};
const std::vector<Recolouring> kDrahminCrimson = {{"Dchestwrap", nullptr, "Cchestwrap", &kCrimsonWraps},
                                                  {"Darmwrap", nullptr, "Carmwrap", &kCrimsonWraps},
                                                  {"Dflat", &kIvoryFlat, "Cflat"}};
const std::vector<Recolouring> kDrahminCrimsonMask = {
    {"Dmask", &kCrimsonMarkings, "Cmask"}, {"Cmask", nullptr, nullptr, &kCrimsonMaskBase}, {"Dflat", &kIvoryFlat, "Cflat"}};
// Shang Tsung's black and gold (his costume 3, L1 and A): everything of his outfit that is not gold black, his pants and
// his brown leather gloves all black (kShangHands), made from his first costume; his face (ShangTsungTexture3) untouched
const std::vector<Rebuild> kShangBlackGoldRebuild = {
    {"ShangTsungTexture1", "ShangTsungTexturY1", &kShangHands, {&kRampDeepBlack, &kRampDeepBlack}, 2, &kBlackButGold, 1}};
const std::vector<Recolouring> kShangBlackGold = {{"ShangTsungTexture2", nullptr, "ShangTsungTexturY2", &kBlackButGold}};

// Frost's All Black (Jet Black, SQUARE) and Deep Blue (costume 3, L1 and A), the concept's, both made on her first
// costume: her gi (the straps, the front panel,
// the mask) one deep sapphire blue, its marks gone (made of the nearest cloth, smoothed), the medallion on her strap
// bronze; her top, sleeves, gloves, pants and boots jet black; Jet Black's gloves and boots all black (their lines and
// the forearms' light panels gone) and the belt around her leg and its tie black, Deep Blue's gloves and boots with
// their lines in a darker blue and the belt and its tie the gi's blue
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
    // the first costume in a deeper blue; its other colours (LT and A) red, made from it (the game's are purple)
    {kRaiden, "RAIDEN", false, "rayden.ssf",
     {{false,
       {},
       {{"rd_blue", &kDeepBlue, nullptr}, {"rd_cape", &kDeepBlue, nullptr}, {"rd_haircape", &kDeepBlue, nullptr}}}},
     "x_rayden.ssf",
     {{false, true, Shown::Always, "RAIDEN_RED.SEC", {},
       {{"rd_blue", &kRed, "r3_blue"}, {"rd_cape", &kRed, "r3_cape"}, {"rd_haircape", &kRed, "r3_haircape"}}}}},
    // the first costume's other colours (LT and A) black with the first costume's own gold (its mask, trims and studs
    // kept as they are, his gear as the game's), made from it; black and orange for the second of two players who
    // both asked for them
    {kScorpion, "SCORPION", false, "scorpion.ssf", {}, "x_scorpion.ssf",
     {{false, true, Shown::Always, "SCORPION_BLACK_GOLD.SEC", {{"sc_gold", "s3_gold", &kScorpionGold, {&kRampBlack}, 1}},
       {}},
      {false, false, Shown::Mirror, "SCORPION_BLACK_ORANGE.SEC",
       {{"sc_gold", "s4_gold", &kScorpionGold, {&kRampBlack, &kRampOrange}, 2}},
       {{"sc_armor", &kScorpionGearOrange, "s4_armor"}, {"sc_pads", &kScorpionGearOrange, "s4_pads"}}}}},
    // the second costume's other colours (LT and START): the second costume with black pants (the game's other colours
    // are purple); x_drunkenfighter2.ssf is alt_drunkenfighter.ssf with it
    {kBoRaiCho, "BO' RAI CHO", true, "alt_drunkenfighter.ssf", {}, "x_drunkenfighter2.ssf",
     {{false, true, Shown::Always, "BO_RAI_CHO_BLACK_PANTS.SEC", {}, {{"AltBRC_pants", &kBoBlackPants, "BlkBRC_pants"}}}}},
    // the second costume's other colours (LT and START): the game's red jacket with the belt white and the undershirt
    // and its hood black, his own black gloves (the boot and hood textures renamed, so the second costume itself keeps
    // its belt and hood in a mirror). Costume 3 (L1 and START): that look with red gloves (his own gloves, on his own
    // model: the same collision)
    {kJax, "JAX", true, "alt_jax.ssf",
     {{true,
       {{"altJx_boot", "altJx_belt", &kJaxBelt, {&kRampWhite}, 1}, {"ap-alt_vest", nullptr, &kJaxShirt, {&kRampBlack}, 1}},
       {{"altJX_hoodie", &kAllDark, "blkJX_hoodie"}}}},
     "x_jax.ssf",
     {{true, false, Shown::Third, "JAX_COSTUME_3.SEC", kJaxThirdRebuild, kJaxThirdRecolour},
      {true, true, Shown::Third, "JAX_COSTUME_3_IN_COLOURS.SEC", kJaxThirdRebuild, kJaxThirdRecolour}}},
    // the concept's Bronze and Crimson, on his second costume: its other colours (LT and START) Bronze (charcoal wraps,
    // his green mask, bronze metal), made from those colours; costume 3 (L1 and START) Crimson, under both rows' section
    // ids, each with its own copy of his shared section (the mask). His skin (Dface, Dtorso) is never touched
    {kDrahmin, "DRAHMIN", true, "alt_drahmin.ssf", {}, "x_drahmin2.ssf",
     {{true, true, Shown::Always, "DRAHMIN_BRONZE.SEC", {}, kDrahminBronze},
      {false, false, Shown::Third, "DRAHMIN_CRIMSON.SEC", kDrahminCrimsonRebuild, kDrahminCrimson,
       "DRAHMIN_CRIMSON_MASK.SEC", kDrahminCrimsonMask},
      {false, true, Shown::Third, "DRAHMIN_CRIMSON_IN_COLOURS.SEC", kDrahminCrimsonRebuild, kDrahminCrimson,
       "DRAHMIN_CRIMSON_MASK_2.SEC", kDrahminCrimsonMask}}},
    // costume 3 (L1 and A) black and gold, under both rows' section ids; his first costume's other colours the game's
    {kShangTsung, "SHANG TSUNG", false, "shangtsung.ssf", {}, "x_shangtsung.ssf",
     {{false, false, Shown::Third, "SHANG_TSUNG_BLACK_GOLD.SEC", kShangBlackGoldRebuild, kShangBlackGold},
      {false, true, Shown::Third, "SHANG_TSUNG_BLACK_GOLD_IN_COLOURS.SEC", kShangBlackGoldRebuild, kShangBlackGold}}},
    // costume 3 (L1 and A) Deep Blue; SQUARE All Black (Jet Black); each under both rows' section ids; her first
    // costume's other colours the game's
    {kFrost, "FROST", false, "subzerette.ssf", {}, "x_subzerette.ssf",
     {{false, false, Shown::Third, "FROST_DEEP_BLUE.SEC", kFrostDeepBlue, {}},
      {false, true, Shown::Third, "FROST_DEEP_BLUE_IN_COLOURS.SEC", kFrostDeepBlue, {}},
      {false, false, Shown::Square, "FROST_JET_BLACK.SEC", kFrostJetBlack, {}},
      {false, true, Shown::Square, "FROST_JET_BLACK_IN_COLOURS.SEC", kFrostJetBlack, {}}}},
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

// before each lookup of the table (character, colours, costume, as sub_00103150 has them): a row with a look holds its
// entry (a mirror look, costume 3 or a Square look only while a player of that fighter in that row has kMirrorLook,
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

// WHITE held (L1 on a PlayStation pad) by the player who picks: their own controller; practice picks both fighters
// with P1's
bool ThirdHeld(uint32_t player) {
    auto held = [](uint32_t p) {
        return (kernel::R32(kPlayerBlock + p * kPlayerStride + kPlayerCurrent) & kButtonWhite) != 0;
    };
    if (kernel::R32(kGameMode) == kModePractice) return held(0) || held(1);
    return held(player);
}

const bool g_registered = (OnFrameStart(Make), true);

// both players of one fighter in the costume a look is made on: one in a look (costume 3, a Square look), the other
// plain: the plain one keeps the row it asked for (the other colours only with LT held; the game's own rule at a pick
// gives them to the second of two players of one fighter whatever their costumes) and the look takes the row it leaves;
// in two different looks: a row each, P1 the costume's own; true when it did (the same look: Settle)
bool Share(uint32_t word[2]) {
    if ((word[0] & 1) != (word[1] & 1)) return false;
    const uint32_t a = word[0] & kLooks, b = word[1] & kLooks;
    if (a && b) {
        if (a == b) return false;
        word[0] &= ~2u;
        word[1] |= 2u;
        return true;
    }
    for (int k = 0; k < 2; k++) {
        const int o = 1 - k;
        if (!(word[k] & kLooks) || (word[o] & kLooks)) continue;
        word[o] = (word[o] & ~2u) | ((word[o] & kAltPick) ? 2u : 0u);
        word[k] = (word[k] & ~2u) | ((word[o] & 2u) ^ 2u);
        return true;
    }
    return false;
}

// SQUARE (the pad's X) picks a fighter's Square look. The game picks with A and START only: a player choosing (state 2)
// on a fighter with a Square look who presses Square gets an A press that frame instead, right after the pad update and
// before the select screen reads the buttons, and the pick that follows (Picked) takes the look. Practice picks both
// fighters with P1's controller
uint64_t g_square_until[2] = {};
void SquarePicks(uint64_t) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    const bool practice = kernel::R32(kGameMode) == kModePractice;
    for (uint32_t p = 0; p < 2; p++) {
        if (kernel::R32(kPlayerState[p]) != 2) continue;
        const uint32_t block = kPlayerBlock + (practice ? 0 : p) * kPlayerStride;
        if (!(kernel::R32(block + kPlayerPressed) & kButtonSquare)) continue;
        const uint32_t slot = kernel::R32(kSelectCursor[p]);
        if (slot >= kGridSlots) continue;
        const Fighter* f = Find(kernel::R32(kGrid + slot * kGridStride));
        if (!f || !f->HasSquare()) continue;
        kernel::W32(block + kPlayerCurrent, kernel::R32(block + kPlayerCurrent) | kButtonA);
        kernel::W32(block + kPlayerPressed, kernel::R32(block + kPlayerPressed) | kButtonA);
        g_square_until[p] = Frame() + 1;
    }
}
const bool g_square_registered = (OnPadsRead(SquarePicks), true);

}  // namespace

void Picked(uint32_t player) {
    if (player > 1) return;
    const uint32_t va = kernel::R32(kCostume[player]);
    if (!va) return;
    uint32_t w = kernel::R32(va) & ~kLooks;
    const Fighter* f = Find(kernel::R32(kCharacter[player]));
    const bool square = g_square_until[player] && Frame() <= g_square_until[player];
    g_square_until[player] = 0;
    // a look of the costume picked (not the other colours, should LT be held too); the row it goes in settles at the
    // select screen's end: SQUARE the Square look; WHITE held costume 3 (A for one made on the first costume, START on
    // the second)
    if (f && bool(w & 1) == f->second && square && f->HasSquare()) {
        w = (w & ~kAltPick) | kFourth;
        LOGR("costumes: P%u picked %s with SQUARE: its Square look (costume word %X)", player + 1, f->name, w);
    } else if (f && bool(w & 1) == f->second && f->HasThird() && ThirdHeld(player)) {
        w = (w & ~kAltPick) | kThird;
        LOGR("costumes: P%u picked %s holding WHITE: costume 3 (costume word %X)", player + 1, f->name, w);
    }
    kernel::W32(va, w);
    // the other player already picked this fighter: the rows as the select screen's end will have them, before this
    // pick's model loads (a task on the next frames)
    const uint32_t other = 1 - player, ova = kernel::R32(kCostume[other]);
    if (!f || !ova || kernel::R32(kPlayerState[other]) != 3 ||
        kernel::R32(kCharacter[other]) != kernel::R32(kCharacter[player]))
        return;
    uint32_t word[2];
    word[player] = w;
    word[other] = kernel::R32(ova);
    if (!Share(word)) return;
    kernel::W32(va, word[player]);
    kernel::W32(ova, word[other]);
    LOGR("costumes: %s's looks and the costume they are made on: P1 %s, P2 %s", f->name,
         word[0] & 2 ? "other colours" : "own colours", word[1] & 2 ? "other colours" : "own colours");
}

int Settle(uint32_t word[2]) {
    for (int k = 0; k < 2; k++) {
        const Fighter* f = Find(kernel::R32(kCharacter[k]));
        if (!f || bool(word[k] & 1) != f->second) word[k] &= ~kLooks;
        if (!f || !f->HasThird()) word[k] &= ~kThird;
        if (!f || !f->HasSquare()) word[k] &= ~kFourth;
    }
    const uint32_t character = kernel::R32(kCharacter[0]), a = word[0] & kLooks, b = word[1] & kLooks;
    if (character == kernel::R32(kCharacter[1]) && a && a == b) {
        // both in one look of a fighter's costume: P2 the costume it is made on, in its own colours; P1 the other row
        word[1] &= ~(kLooks | 2u);
        word[0] |= 2u;
        LOGR("costumes: both players picked the same look of %s: P2 the costume itself", Find(character)->name);
    } else if (character == kernel::R32(kCharacter[1]) && Share(word)) {
        LOGR("costumes: %s's looks and the costume they are made on: P1 %s, P2 %s", Find(character)->name,
             word[0] & 2 ? "other colours" : "own colours", word[1] & 2 ? "other colours" : "own colours");
    }
    return a && !b ? 0 : 1;
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
