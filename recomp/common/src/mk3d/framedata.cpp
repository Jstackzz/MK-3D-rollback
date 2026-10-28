// Frame data in practice (framedata.h). The tracker runs at the top of every main loop frame on the main thread and
// only reads: the game's adapter reads both fighters as the game's own rules see them, the game's hit test reports
// each frame an attack is live (ActiveCheck, also on the main thread: the fighters' tasks are fibers of it). The
// overlay is drawn on the render path with the runtime's UI lock held, from a copy the tracker publishes.
//
// Counting, in the game's frames (its frame counter, which stands still while the game is paused), the press being
// move frame 1: a move starts in the frame at whose top player 1 could act and after which it no longer can, without
// having been hit; its startup is the first frame its attack was tested against player 2, its active frames run from
// there to the last frame it was tested or its hit window stood open, its recovery from there to the first frame
// player 1 can act again. On hit or block, the advantage is the first frame player 2 can act again after the last
// contact (holding block: had it let go at its first chance) less player 1's. A move that never tested an attack (a
// walk, a jump, blocking) is not shown.
//
// The practice dummy's block (F8) is the one thing that changes the game: each game's "is this action held" hook asks
// DummyHolds, which answers only in a practice fight on this PC alone.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/framedata.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "mk3d/game.h"
#include "mk3d/link.h"
#include "mk3d/netplay.h"

namespace mk3d::framedata {
namespace {

constexpr UINT kToggleKey = VK_F7, kDummyKey = VK_F8;
constexpr float kFontPixels = 48.0f;
constexpr uint32_t kGiveUp = 300;  // frames: a move not over by then (a throw, a scripted sequence) shows nothing

Adapter g_game{};
bool g_installed = false;
std::atomic<bool> g_on{true};          // MK3D_FRAMEDATA, F7
std::atomic<bool> g_practice{false};   // a practice fight on this PC alone (the dummy may block)
std::atomic<bool> g_following{false};  // ... and the overlay follows it
std::atomic<int> g_dummy{int(Dummy::Stands)};  // F8
ImFont* g_font = nullptr;              // a larger font than the runtime's 13 px default, when Windows has one

// player 1's attack being followed (main thread)
struct Move {
    bool on = false;
    uint32_t start = 0;  // its first frame
    // its active frames: the frames its attack was tested or its hit window stood open, as runs (a hit each, mostly)
    static constexpr int kWindows = 6;
    uint32_t window_start[kWindows] = {}, window_end[kWindows] = {};
    int windows = 0;
    int contact = 0;          // 1 hit (any of its contacts), 2 blocked (all of them)
    uint32_t contact_at = 0;  // the frame of its last contact
    int hits = 0;
    uint32_t p1_free = 0;  // the first frame player 1 can act again
    uint32_t p2_free = 0;  // the first frame player 2 can act again after the last contact

    void Active(uint32_t frame) {
        if (windows && frame <= window_end[windows - 1] + 1) {
            window_end[windows - 1] = std::max(window_end[windows - 1], frame);
        } else if (windows < kWindows) {
            window_start[windows] = window_end[windows] = frame;
            windows++;
        } else {
            window_end[windows - 1] = frame;  // more hits than kept: the last run takes them
        }
    }
};

// what the overlay shows
struct Shown {
    bool valid = false;
    int startup = 0, recovery = 0, total = 0;
    char active[48] = {};  // "6", or each hit's run with the gaps between: "6 (7) 2"
    int contact = 0, hits = 0;
    int contact_frame = 0;  // the move frame of its last contact
    bool advantage_known = false;
    bool advantage_at_least = false;  // player 2 had not recovered yet when player 1 attacked again
    int advantage = 0;
    char name[32] = {};
};

Move g_move;
Fighter g_prev[2];
bool g_prev_valid = false;
uint32_t g_prev_time = 0;
uint32_t g_tested[2] = {~0u, ~0u};  // the frame each side's attack was last tested
std::mutex g_shown_lock;
Shown g_shown;  // under g_shown_lock

const char* ContactText(int contact) { return contact == 1 ? "On hit" : contact == 2 ? "On block" : "Whiff"; }

const char* DummyText(Dummy d) {
    return d == Dummy::Blocks ? "blocks" : d == Dummy::CrouchBlocks ? "crouch blocks" : "stands";
}

bool Allowed() {
    // never in a game the launcher drives or with netplay configured: only a practice fight on this PC alone
    return g_installed && !link::Active() && !net::Enabled() && g_game.practice && g_game.practice();
}

void Publish(const Move& m, bool interrupted, uint32_t now) {
    if (!m.windows && !m.contact) return;  // no attack in it
    Shown s;
    s.valid = true;
    const uint32_t first = m.windows ? m.window_start[0] : m.contact_at;
    const uint32_t last = m.windows ? std::max(m.window_end[m.windows - 1], first) : first;
    s.startup = int(first - m.start) + 1;
    if (!m.windows) {
        std::snprintf(s.active, sizeof s.active, "1");
    } else {
        int at = 0;
        for (int i = 0; i < m.windows && at < int(sizeof s.active) - 12; i++) {
            if (i) at += std::snprintf(s.active + at, sizeof s.active - at, " (%u) ",
                                       m.window_start[i] - m.window_end[i - 1] - 1);
            at += std::snprintf(s.active + at, sizeof s.active - at, "%u", m.window_end[i] - m.window_start[i] + 1);
        }
    }
    if (m.p1_free) {
        s.total = int(m.p1_free - m.start);
        s.recovery = std::max(0, int(m.p1_free - (last + 1)));
    }
    s.contact = m.contact;
    s.hits = m.hits;
    s.contact_frame = m.contact ? int(m.contact_at - m.start) + 1 : 0;
    if (m.contact && m.p1_free && m.p2_free) {
        s.advantage_known = true;
        s.advantage = int(m.p2_free) - int(m.p1_free);
    } else if (m.contact && m.p1_free && interrupted) {
        // player 1 attacked again in frame now, before player 2 could act
        s.advantage_known = s.advantage_at_least = true;
        s.advantage = int(now + 1) - int(m.p1_free);
    }
    if (g_game.name)
        if (const char* n = g_game.name(0)) std::snprintf(s.name, sizeof s.name, "%s", n);
    if (s.advantage_known)
        LOGR("framedata: %s startup %d active %s recovery %d total %d, %s %s%+d (%d hits, the last in frame %d)", s.name,
             s.startup, s.active, s.recovery, s.total, ContactText(s.contact), s.advantage_at_least ? ">= " : "",
             s.advantage, s.hits, s.contact_frame);
    else
        LOGR("framedata: %s startup %d active %s recovery %d total %d, %s", s.name, s.startup, s.active, s.recovery,
             s.total, ContactText(s.contact));
    std::lock_guard<std::mutex> lock(g_shown_lock);
    g_shown = s;
}

void Reset() {
    g_move = {};
    g_prev_valid = false;
}

void OnFrame(uint64_t) {
    const bool practice = Allowed();
    g_practice = practice;
    if (!g_on.load(std::memory_order_relaxed) || !practice) {
        g_following = false;
        Reset();
        return;
    }
    const uint32_t t = g_game.time();
    if (g_prev_valid && t == g_prev_time) return;  // paused: nothing ran
    Fighter p[2];
    if (!g_game.read(0, p[0]) || !g_game.read(1, p[1])) {
        g_following = false;
        Reset();
        return;
    }
    g_following = true;
    if (!g_prev_valid || t != g_prev_time + 1) {  // the first frame of a fight, or time jumped
        g_move = {};
        g_prev[0] = p[0];
        g_prev[1] = p[1];
        g_prev_valid = true;
        g_prev_time = t;
        return;
    }
    const uint32_t ran = t - 1;  // the frame that just ran; this is the top of frame t
    const bool p2_hit_p1 = p[1].contact && p[1].contact != g_prev[1].contact;

    // player 1 started something in the frame that ran: it could act at its top and cannot now
    if (g_prev[0].can_act && !p[0].can_act && !p2_hit_p1) {
        if (g_move.on && g_move.contact && !g_move.p2_free) Publish(g_move, true, ran);
        g_move = {};
        g_move.on = true;
        g_move.start = ran;
    }
    if (g_move.on) {
        Move& m = g_move;
        if (p2_hit_p1) {
            m = {};  // a trade, or player 2 struck first: nothing to show
        } else {
            if (g_tested[0] == ran) m.Active(ran);
            // its hit window stands open for this frame: tested in it unless it made contact already, or the game
            // skips the test (a high attack over a crouching fighter)
            if (p[0].striking) m.Active(t);
            if (p[0].contact && p[0].contact != g_prev[0].contact) {
                m.contact = (p[0].contact == 1 || m.contact == 1) ? 1 : 2;
                m.contact_at = ran;
                m.hits++;
                m.p2_free = 0;
            }
            if (!m.p1_free && t > m.start && p[0].can_act) m.p1_free = t;
            if (m.contact && !m.p2_free && t > m.contact_at) {
                if (p[1].can_act) m.p2_free = t;
                else if (p[1].can_act_next) m.p2_free = t + 1;
            }
            // done once player 1 can act again and, after a contact, player 2 too; a move without an attack is dropped
            if (m.p1_free && (!m.contact || m.p2_free)) {
                Publish(m, false, ran);
                m = {};
            } else if ((m.p1_free && !m.windows && !m.contact) || t - m.start > kGiveUp) {
                m = {};
            }
        }
    }
    g_prev[0] = p[0];
    g_prev[1] = p[1];
    g_prev_time = t;
}

}  // namespace

void Install(const Adapter& adapter) {
    g_game = adapter;
    g_installed = true;
    if (const char* v = std::getenv("MK3D_FRAMEDATA")) g_on = std::atoi(v) != 0;
    if (const char* v = Env("FRAMEDATA_DUMMY")) g_dummy = std::clamp(std::atoi(v), 0, 2);  // tests: F8 from the start
    OnFrameStart(&OnFrame);
}

bool Following() { return g_following.load(std::memory_order_relaxed); }

bool VersusToo() {
    static const bool on = Env("FRAMEDATA_VERSUS") != nullptr;
    return on;
}

Dummy DummyBlock() {
    return g_practice.load(std::memory_order_relaxed) ? Dummy(g_dummy.load(std::memory_order_relaxed)) : Dummy::Stands;
}

bool DummyHolds(int action, int block_action, int down_action) {
    const Dummy d = DummyBlock();
    return (d != Dummy::Stands && action == block_action) || (d == Dummy::CrouchBlocks && action == down_action);
}

void ActiveCheck(int side) {
    if (!Following() || (side != 0 && side != 1)) return;
    g_tested[side] = g_game.time();
}

void Window(void*) {
    // before the renderer builds the font atlas: a Windows font for the overlay, the runtime's default kept first
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    char path[MAX_PATH];
    const UINT n = GetWindowsDirectoryA(path, MAX_PATH);
    if (n && n < MAX_PATH - 32) {
        std::strcat(path, "\\Fonts\\segoeuib.ttf");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) g_font = io.Fonts->AddFontFromFileTTF(path, kFontPixels);
    }
}

void Ui(double) {
    if (!g_on.load(std::memory_order_relaxed) || !g_following.load(std::memory_order_relaxed)) return;
    Shown s;
    {
        std::lock_guard<std::mutex> lock(g_shown_lock);
        s = g_shown;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float px = std::max(14.0f, io.DisplaySize.y / 34.0f);  // text height
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.02f, io.DisplaySize.y * 0.17f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (g_font) ImGui::PushFont(g_font);
    if (ImGui::Begin("##mk3d_framedata", nullptr, flags)) {
        ImGui::SetWindowFontScale(px / (g_font ? kFontPixels : 13.0f));
        const ImVec4 dim(0.75f, 0.75f, 0.75f, 1.0f);
        const Dummy dummy = Dummy(g_dummy.load(std::memory_order_relaxed));
        if (!s.valid) {
            ImGui::TextColored(dim, "Frame data: attack with player 1");
            ImGui::TextColored(dim, "F8: the dummy %s", DummyText(dummy));
        } else {
            if (s.name[0]) ImGui::TextColored(dim, "%s", s.name);
            ImGui::Text("Startup %d   Active %s", s.startup, s.active);
            if (s.total) ImGui::Text("Recovery %d   Total %d", s.recovery, s.total);
            if (!s.contact) {
                ImGui::TextColored(dim, "Whiff");
            } else {
                const ImVec4 color = !s.advantage_known ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                     : s.advantage > 0  ? ImVec4(0.45f, 0.9f, 0.45f, 1.0f)
                                     : s.advantage < 0  ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                                        : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                if (!s.advantage_known)
                    ImGui::TextColored(color, "%s ?", ContactText(s.contact));
                else
                    ImGui::TextColored(color, "%s %s%+d", ContactText(s.contact), s.advantage_at_least ? ">= " : "",
                                       s.advantage);
                // a hit that did not land on the first active frame changes the advantage: name its frame
                if (s.hits > 1 || s.contact_frame != s.startup) {
                    ImGui::SameLine();
                    if (s.hits > 1) ImGui::TextColored(dim, "(%d hits, last on frame %d)", s.hits, s.contact_frame);
                    else ImGui::TextColored(dim, "(on frame %d)", s.contact_frame);
                }
            }
            if (dummy != Dummy::Stands) ImGui::TextColored(dim, "The dummy %s (F8)", DummyText(dummy));
        }
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    if (g_font) ImGui::PopFont();
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t*) {
    if (msg != WM_KEYDOWN || (lp & (1 << 30))) return false;  // not the key's autorepeat
    if (wp == kToggleKey) {
        const bool on = !g_on.load();
        g_on = on;
        LOGR("framedata: %s", on ? "shown" : "hidden");
    } else if (wp == kDummyKey) {
        const int d = (g_dummy.load() + 1) % 3;
        g_dummy = d;
        LOGR("framedata: the practice dummy %s", DummyText(Dummy(d)));
    }
    return false;  // the game and the runtime see the key as before
}

}  // namespace mk3d::framedata
