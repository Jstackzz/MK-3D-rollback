// Record and playback for the practice dummy (record.h). The state runs on the main thread, right after the game's
// pad update each main loop frame (OnPadsRead): a recording takes one frame of the dummy's input per game frame (the
// game's own counter, so a paused game records nothing), playback hands one back per game frame. The keys come from
// the window thread as counted requests, the overlay reads a copy the main thread publishes.
//
// A slot plays back as it was recorded, but for left and right: each frame remembers which way the dummy faced, and
// a frame played while it faces the other way has them swapped, so a string recorded on the left plays the same
// from the right. Repeat and random start the next sequence a moment after the last one ended, once the dummy can
// act again (the game's own move gate, as the frame data reads it). Between sequences the dummy is the game's own:
// standing, or blocking with F8.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk3d/record.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "mk3d/framedata.h"
#include "mk3d/game.h"

namespace mk3d::record {
namespace {

constexpr int kSlots = 8;
constexpr uint32_t kMaxFrames = 30 * 60;   // a slot holds 30 seconds
constexpr uint32_t kRepeatGap = 20;        // frames from a sequence's end to the next, once the dummy can act again
constexpr uint64_t kHintFrames = 10 * 60;  // the keys are shown this long after a fight starts ...
constexpr uint64_t kFlashFrames = 3 * 60;  // ... and after a key, while nothing records or plays
constexpr char kFileMagic[8] = {'M', 'K', '3', 'D', 'R', 'E', 'C', '1'};

struct Sample {
    uint32_t buttons = 0, sticks = 0;
    bool faces_left = false;
};

enum class State { Idle, Recording, Playing };
enum class Mode { Once, Repeat, Random };

const char* ModeText(Mode m) { return m == Mode::Once ? "once" : m == Mode::Repeat ? "repeat" : "random slot"; }

Adapter g_game{};
bool g_installed = false;

// the keys, counted on the window thread and taken on the main thread
std::atomic<int> g_key_record{0}, g_key_play{0}, g_key_slot{0}, g_key_mode{0};

// main thread
State g_state = State::Idle;
Mode g_mode = Mode::Once;
int g_slot = 0;  // the slot F11 chose: F9 records into it, F10 plays it
std::vector<Sample> g_slots[kSlots];
int g_play_slot = 0;       // the slot playing
size_t g_pos = 0;          // its frame
bool g_between = false;    // the gap between sequences (repeat, random)
uint32_t g_gap = 0;        // frames of it left
uint32_t g_last_time = 0;  // the game frame the last input was taken or given for
bool g_first = true;       // ... none yet since the start
bool g_in_fight = false;
uint64_t g_show_until = 0;  // main loop frame the key hints are shown until
std::string g_loaded_for;   // the dummy fighter the slots were loaded for
std::mt19937 g_random{std::random_device{}()};
std::atomic<bool> g_drives{false};

// what the overlay shows
struct Shown {
    bool fight = false, hint = false, between = false;
    State state = State::Idle;
    Mode mode = Mode::Once;
    int slot = 0, play_slot = 0;
    uint32_t frames = 0;  // recorded so far, or the playing sequence's frame
    uint32_t length[kSlots] = {};
    char note[64] = {};  // a short word about the last key ("slot 3: empty")
};
std::mutex g_shown_lock;
Shown g_shown;
char g_note[64] = {};
uint64_t g_note_until = 0;

std::string Sanitized(const char* name) {
    std::string s;
    for (const char* p = name; p && *p && s.size() < 32; p++) {
        const unsigned char c = static_cast<unsigned char>(*p);
        s += std::isalnum(c) ? char(std::tolower(c)) : '_';
    }
    return s.empty() ? std::string("dummy") : s;
}

// the slots of a dummy fighter, beside the game: practice/<GAME>-<fighter>.rec
std::filesystem::path SlotFile(const std::string& fighter) {
    return app::ExeDir() / "practice" / (std::string(EnvPrefix()) + "-" + fighter + ".rec");
}

void Save() {
    if (g_loaded_for.empty()) return;
    const std::filesystem::path path = SlotFile(g_loaded_for);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        LOGE("record: cannot write %s", path.string().c_str());
        return;
    }
    f.write(kFileMagic, sizeof kFileMagic);
    const uint32_t slots = kSlots;
    f.write(reinterpret_cast<const char*>(&slots), 4);
    for (const auto& slot : g_slots) {
        const uint32_t n = uint32_t(slot.size());
        f.write(reinterpret_cast<const char*>(&n), 4);
        for (const Sample& fr : slot) {
            f.write(reinterpret_cast<const char*>(&fr.buttons), 4);
            f.write(reinterpret_cast<const char*>(&fr.sticks), 4);
            const uint8_t faces = fr.faces_left ? 1 : 0;
            f.write(reinterpret_cast<const char*>(&faces), 1);
        }
    }
}

void Load(const std::string& fighter) {
    for (auto& slot : g_slots) slot.clear();
    g_loaded_for = fighter;
    const std::filesystem::path path = SlotFile(fighter);
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char magic[8];
    uint32_t slots = 0;
    if (!f.read(magic, 8) || std::memcmp(magic, kFileMagic, 8) != 0 || !f.read(reinterpret_cast<char*>(&slots), 4)) {
        LOGE("record: %s is not a recordings file", path.string().c_str());
        return;
    }
    int loaded = 0;
    for (uint32_t i = 0; i < slots; i++) {
        uint32_t n = 0;
        if (!f.read(reinterpret_cast<char*>(&n), 4) || n > kMaxFrames) break;
        std::vector<Sample> frames(n);
        bool ok = true;
        for (Sample& fr : frames) {
            uint8_t faces = 0;
            if (!f.read(reinterpret_cast<char*>(&fr.buttons), 4) || !f.read(reinterpret_cast<char*>(&fr.sticks), 4) ||
                !f.read(reinterpret_cast<char*>(&faces), 1)) {
                ok = false;
                break;
            }
            fr.faces_left = faces != 0;
        }
        if (!ok) break;
        if (i < kSlots && n) {
            g_slots[i] = std::move(frames);
            loaded++;
        }
    }
    if (loaded)
        LOGR("record: %d slot%s for %s from %s", loaded, loaded == 1 ? "" : "s", fighter.c_str(),
             path.string().c_str());
}

void Note(const char* text) {
    std::snprintf(g_note, sizeof g_note, "%s", text);
    g_note_until = Frame() + kFlashFrames;
    g_show_until = std::max(g_show_until, g_note_until);
}

void Publish() {
    Shown s;
    s.fight = g_in_fight;
    s.hint = Frame() < g_show_until;
    s.between = g_between;
    s.state = g_state;
    s.mode = g_mode;
    s.slot = g_slot;
    s.play_slot = g_play_slot;
    s.frames = g_state == State::Recording ? uint32_t(g_slots[g_slot].size())
               : g_state == State::Playing ? uint32_t(std::min(g_pos, g_slots[g_play_slot].size()))
                                           : 0;
    for (int i = 0; i < kSlots; i++) s.length[i] = uint32_t(g_slots[i].size());
    if (Frame() < g_note_until) std::snprintf(s.note, sizeof s.note, "%s", g_note);
    std::lock_guard<std::mutex> lock(g_shown_lock);
    g_shown = s;
}

void Mirror(uint32_t& buttons) {
    const uint32_t left = g_game.left_bits, right = g_game.right_bits;
    const bool l = left && (buttons & left) == left, r = right && (buttons & right) == right;
    buttons &= ~(left | right);
    if (l) buttons |= right;
    if (r) buttons |= left;
}

void Off() {
    Input out;
    g_game.drive(Drive::Off, Input{0, g_game.neutral_sticks}, out);
}

void StopRecording(const char* why) {
    const auto& slot = g_slots[g_slot];
    LOGR("record: slot %d holds %zu frames (%.1f s)%s", g_slot + 1, slot.size(), slot.size() / 60.0, why);
    g_state = State::Idle;
    Off();
    Save();
    char note[64];
    std::snprintf(note, sizeof note, "slot %d: %.1f s recorded", g_slot + 1, slot.size() / 60.0);
    Note(note);
}

void StopPlaying(const char* why) {
    LOGR("record: playback stopped%s", why);
    g_state = State::Idle;
    g_between = false;
    Off();
}

int RecordedSlots() {
    int n = 0;
    for (const auto& s : g_slots) n += !s.empty();
    return n;
}

// the next sequence: the chosen slot, or any recorded one at random; false with nothing to play
bool BeginSequence() {
    int slot = g_slot;
    if (g_mode == Mode::Random) {
        const int n = RecordedSlots();
        if (!n) return false;
        int pick = int(g_random() % uint32_t(n));
        for (int i = 0; i < kSlots; i++)
            if (!g_slots[i].empty() && pick-- == 0) slot = i;
    }
    if (g_slots[slot].empty()) return false;
    g_play_slot = slot;
    g_pos = 0;
    g_first = true;
    g_between = false;
    return true;
}

void StartRecording() {
    if (g_state == State::Playing) StopPlaying(" (recording)");
    g_slots[g_slot].clear();
    g_state = State::Recording;
    g_first = true;
    LOGR("record: recording slot %d, the dummy faces %s", g_slot + 1, g_game.dummy_faces_left() ? "left" : "right");
}

void StartPlaying() {
    if (g_state == State::Recording) StopRecording(" (playing)");
    if (!BeginSequence()) {
        char note[64];
        if (g_mode == Mode::Random) std::snprintf(note, sizeof note, "nothing recorded yet");
        else std::snprintf(note, sizeof note, "slot %d: empty", g_slot + 1);
        Note(note);
        return;
    }
    g_state = State::Playing;
    const auto& slot = g_slots[g_play_slot];
    LOGR("record: playing slot %d, %s; the dummy faces %s, the recording started facing %s", g_play_slot + 1,
         ModeText(g_mode), g_game.dummy_faces_left() ? "left" : "right", slot[0].faces_left ? "left" : "right");
}

void TakeKeys() {
    if (g_key_record.exchange(0)) {
        if (g_state == State::Recording) StopRecording("");
        else StartRecording();
        g_show_until = Frame() + kFlashFrames;
    }
    if (g_key_play.exchange(0)) {
        if (g_state == State::Playing) StopPlaying("");
        else StartPlaying();
        g_show_until = Frame() + kFlashFrames;
    }
    if (const int n = g_key_slot.exchange(0)) {
        if (g_state == State::Recording) StopRecording(" (slot changed)");
        g_slot = (g_slot + n) % kSlots;
        char note[64];
        if (g_slots[g_slot].empty()) std::snprintf(note, sizeof note, "slot %d: empty", g_slot + 1);
        else std::snprintf(note, sizeof note, "slot %d: %.1f s", g_slot + 1, g_slots[g_slot].size() / 60.0);
        Note(note);
    }
    if (const int n = g_key_mode.exchange(0)) {
        g_mode = Mode((int(g_mode) + n) % 3);
        char note[64];
        std::snprintf(note, sizeof note, "playback: %s", ModeText(g_mode));
        Note(note);
        LOGR("record: playback %s", ModeText(g_mode));
    }
}

void OnPads(uint64_t frame) {
    const bool fight = framedata::PracticeFight();
    if (!fight) {
        if (g_state == State::Recording) StopRecording(" (the fight ended)");
        else if (g_state == State::Playing) StopPlaying(" (the fight ended)");
        else Off();
        g_in_fight = false;
        g_drives = false;
        // a key pressed outside a practice fight means nothing once one starts
        g_key_record = g_key_play = g_key_slot = g_key_mode = 0;
        Publish();
        return;
    }
    if (!g_in_fight) {
        g_in_fight = true;
        g_show_until = frame + kHintFrames;
        const std::string fighter = Sanitized(framedata::FighterName(1));
        if (fighter != g_loaded_for) Load(fighter);
    }
    TakeKeys();
    const uint32_t t = g_game.time();
    const bool paused = g_game.paused();
    const Input none{0, g_game.neutral_sticks};
    Input out;
    bool drives = false;
    if (g_state == State::Idle || paused) {
        // paused: the dummy is the game's own again, so the menu has player 1's controls, and a state switch from
        // it (PLAYER SELECT, MAIN MENU) finds the players as the game left them
        Off();
    } else if (g_state == State::Recording) {
        g_game.drive(Drive::Record, none, out);
        drives = true;
        auto& slot = g_slots[g_slot];
        const Sample fr{out.buttons, out.sticks, g_game.dummy_faces_left()};
        if (g_first || t != g_last_time || slot.empty()) slot.push_back(fr);
        else slot.back() = fr;  // the same game frame again (a pad update outside the main loop): the last word
        g_first = false;
        g_last_time = t;
        if (slot.size() >= kMaxFrames) StopRecording(" (30 seconds)");
    } else {
        const bool next = !g_first && t != g_last_time;
        g_first = false;
        g_last_time = t;
        bool ready = true;
        if (g_between) {
            if (next && g_gap) g_gap--;
            framedata::Fighter dummy;
            const bool can_act = framedata::TopFighter(1, dummy) && dummy.can_act;
            ready = !g_gap && can_act && BeginSequence();
        } else if (next) {
            g_pos++;
        }
        if (!ready) {
            g_game.drive(Drive::Play, none, out);
        } else {
            const auto& slot = g_slots[g_play_slot];
            if (g_pos >= slot.size()) {
                if (g_mode == Mode::Once) {
                    StopPlaying(" (the sequence is over)");
                } else {
                    g_between = true;
                    g_gap = kRepeatGap;
                    g_game.drive(Drive::Play, none, out);
                }
            } else {
                const Sample& fr = slot[g_pos];
                Input in{fr.buttons, fr.sticks};
                if (fr.faces_left != g_game.dummy_faces_left()) Mirror(in.buttons);
                g_game.drive(Drive::Play, in, out);
                drives = true;
            }
        }
    }
    g_drives = drives;
    Publish();
}

}  // namespace

void Install(const Adapter& adapter) {
    g_game = adapter;
    g_installed = true;
    OnPadsRead(&OnPads);
}

bool DrivesDummy() { return g_drives.load(std::memory_order_relaxed); }

void Ui(double) {
    Shown s;
    {
        std::lock_guard<std::mutex> lock(g_shown_lock);
        s = g_shown;
    }
    if (!s.fight || (s.state == State::Idle && !s.hint)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float px = std::max(13.0f, io.DisplaySize.y / 40.0f);  // text height
    // a line (and a note under it) at the top of the screen, in the middle, under the health bars and the portraits
    // with the fighters' names beside them (Deadly Alliance's reach 21 % of the height)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.225f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings;
    ImFont* font = framedata::OverlayFont();
    if (font) ImGui::PushFont(font);
    if (ImGui::Begin("##mk3d_record", nullptr, flags)) {
        ImGui::SetWindowFontScale(px / (font ? framedata::OverlayFontPixels() : 13.0f));
        const ImVec4 dim(0.75f, 0.75f, 0.75f, 1.0f), red(1.0f, 0.3f, 0.3f, 1.0f), green(0.45f, 0.9f, 0.45f, 1.0f);
        const float gap = px;
        // a red dot for recording, a green triangle for playback, in front of the word
        const auto mark = [&](bool circle, ImVec4 colour) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight(), r = h * 0.3f;
            const ImU32 c = ImGui::GetColorU32(colour);
            if (circle) draw->AddCircleFilled(ImVec2(at.x + r, at.y + h * 0.5f), r, c);
            else
                draw->AddTriangleFilled(ImVec2(at.x, at.y + h * 0.5f - r), ImVec2(at.x + r * 1.8f, at.y + h * 0.5f),
                                        ImVec2(at.x, at.y + h * 0.5f + r), c);
            ImGui::Dummy(ImVec2(r * 2.2f, h));
            ImGui::SameLine(0.0f, px * 0.3f);
        };
        if (s.state == State::Recording) {
            mark(true, red);
            ImGui::TextColored(red, "REC");
            ImGui::SameLine(0.0f, gap);
            ImGui::Text("slot %d   %d:%04.1f / 0:30", s.slot + 1, int(s.frames / 3600), (s.frames % 3600) / 60.0);
            ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(dim, "F9 stops");
        } else if (s.state == State::Playing) {
            mark(false, green);
            ImGui::TextColored(green, "PLAY");
            ImGui::SameLine(0.0f, gap);
            if (s.between) ImGui::Text("slot %d   %s, the next in a moment", s.play_slot + 1, ModeText(s.mode));
            else
                ImGui::Text("slot %d   %d:%04.1f / %d:%04.1f   %s", s.play_slot + 1, int(s.frames / 3600),
                            (s.frames % 3600) / 60.0, int(s.length[s.play_slot] / 3600),
                            (s.length[s.play_slot] % 3600) / 60.0, ModeText(s.mode));
            ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(dim, "F10 stops");
        } else {
            char slot[48];
            if (s.length[s.slot])
                std::snprintf(slot, sizeof slot, "slot %d/%d (%.1f s)", s.slot + 1, kSlots, s.length[s.slot] / 60.0);
            else std::snprintf(slot, sizeof slot, "slot %d/%d (empty)", s.slot + 1, kSlots);
            ImGui::TextColored(dim, "Record the dummy: F9");
            ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(dim, "Play: F10");
            ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(dim, "F11: %s", slot);
            ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(dim, "F12: %s", ModeText(s.mode));
        }
        if (s.note[0]) ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s", s.note);
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    if (font) ImGui::PopFont();
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    // F10 is a system key on Windows (it would activate a menu the window has not got): taken whole, down and up
    if (wp == VK_F10 && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_KEYDOWN || msg == WM_KEYUP)) {
        if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && !(lp & (1 << 30))) g_key_play++;
        if (result) *result = 0;
        return true;
    }
    if (msg != WM_KEYDOWN || (lp & (1 << 30))) return false;  // not the key's autorepeat
    if (wp == VK_F9) g_key_record++;
    else if (wp == VK_F11) g_key_slot++;
    else if (wp == VK_F12) g_key_mode++;
    return false;  // the game and the runtime see the key as before
}

}  // namespace mk3d::record
