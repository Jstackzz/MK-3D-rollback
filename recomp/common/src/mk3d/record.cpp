// Record and playback for the practice dummy (record.h). The state runs on the main thread, right after the game's
// pad update each main loop frame (OnPadsRead): a recording takes one frame of the dummy's input per game frame (the
// game's own counter, so a paused game records nothing), playback hands one back per game frame. The keys come from
// the window thread as counted requests, the overlay reads a copy the main thread publishes.
//
// A slot plays back as it was recorded, but for left and right: each frame remembers which way the dummy faced, and
// a frame played while it faces the other way has them swapped, so a string recorded on the left plays the same
// from the right (each game's adapter says which way the dummy faces by the side its walk takes left and right by).
// A sequence starts once the dummy can act (the game's own move gate, as the frame data reads it): the first one when
// F10 is pressed, the next a moment after the last ended (repeat, random). Between sequences the dummy is the game's
// own: standing, or blocking with F8; after the last, or a recording, it keeps its controls with nothing held until a
// move it started has played out.
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
constexpr uint32_t kFinishCap = 3 * 60;    // frames the dummy's move may take to end after a recording or playback
constexpr uint64_t kHintFrames = 10 * 60;  // the keys are shown this long after a fight starts ...
constexpr uint64_t kFlashFrames = 3 * 60;  // ... and after a key, while nothing records or plays
constexpr char kFileMagic[8] = {'M', 'K', '3', 'D', 'R', 'E', 'C', '2'};    // per slot: style, frames, samples
constexpr char kFileMagicV1[8] = {'M', 'K', '3', 'D', 'R', 'E', 'C', '1'};  // 0.1.39 and 1.0.0: no style

struct Sample {
    uint32_t buttons = 0, sticks = 0;
    bool faces_left = false;
};

enum class State { Idle, Recording, Playing, Finishing };
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
int g_slot_style[kSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};  // the dummy's fighting style when each was recorded
int g_play_slot = 0;          // the slot playing
size_t g_pos = 0;             // its frame
uint32_t g_seq_start = 0;     // the game frame its first input was given for
uint32_t g_rec_start = 0;     // the game frame a recording's first input was taken for
bool g_between = false;       // waiting to start a sequence: the first, or the next (repeat, random)
uint32_t g_gap_until = 0;     // ... not before this game frame
uint32_t g_finish_until = 0;  // Finishing: the dummy is the game's own again by this game frame at the latest
bool g_rec_waiting = false;   // Recording: the dummy is not ready to take the first input yet
int g_turns = 0;              // turns asked for since a recording or sequence was asked for
int g_style_presses = 0;      // ... and style changes
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
    for (int i = 0; i < kSlots; i++) {
        const auto& slot = g_slots[i];
        const int32_t style = g_slot_style[i];
        f.write(reinterpret_cast<const char*>(&style), 4);
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
    for (int& style : g_slot_style) style = -1;
    g_loaded_for = fighter;
    const std::filesystem::path path = SlotFile(fighter);
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char magic[8];
    uint32_t slots = 0;
    if (!f.read(magic, 8) || (std::memcmp(magic, kFileMagic, 8) != 0 && std::memcmp(magic, kFileMagicV1, 8) != 0) ||
        !f.read(reinterpret_cast<char*>(&slots), 4)) {
        LOGE("record: %s is not a recordings file", path.string().c_str());
        return;
    }
    const bool styled = std::memcmp(magic, kFileMagic, 8) == 0;
    int loaded = 0;
    for (uint32_t i = 0; i < slots; i++) {
        int32_t style = -1;
        if (styled && !f.read(reinterpret_cast<char*>(&style), 4)) break;
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
            g_slot_style[i] = style;
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
    s.state = g_state == State::Finishing ? State::Idle : g_state;
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

// the dummy stands in its stance and can start a move now (the game's own gate, as the frame data reads it at the top of
// the frame): not still walking (a turn round ends in a step), not in a move, a reaction or a style change
bool DummyReady() {
    framedata::Fighter f;
    return framedata::TopFighter(1, f) && f.can_act && f.neutral;
}

// A dummy turned away from player 1 turns round at its first input, a direction without a step: one frame of
// towards for it, then the caller waits for the turn to end (the dummy cannot act while it turns). Three at most.
bool TurnedAway() { return g_game.dummy_turned_away && g_turns < 3 && g_game.dummy_turned_away(); }

void Turn(uint32_t t, Input& out) {
    // towards player 1 (away walks it back after the turn, an attack button attacks after it, block does not turn it)
    const Input towards{g_game.dummy_faces_left() ? g_game.left_bits : g_game.right_bits, g_game.neutral_sticks};
    g_game.drive(Drive::Play, towards, out);
    g_gap_until = t + 2;
    g_turns++;
    LOGR("record: the dummy faces away from player 1: turned round first");
}

int Style() { return g_game.dummy_style ? g_game.dummy_style() : -1; }

// A sequence plays from the fighting style it was recorded in (its style changes, and every move, are the style's): the
// dummy's style button, a frame at a time, until it is in that style. Three at most.
bool WrongStyle() {
    const int want = g_slot_style[g_play_slot], now = Style();
    return want >= 0 && now >= 0 && now != want && g_style_presses < 3 && g_game.style_bits;
}

void ChangeStyle(uint32_t t, Input& out) {
    g_game.drive(Drive::Play, Input{g_game.style_bits(), g_game.neutral_sticks}, out);
    g_gap_until = t + 2;
    g_style_presses++;
    LOGR("record: the dummy is in style %d, slot %d was recorded in %d: its style button", Style(), g_play_slot + 1,
         g_slot_style[g_play_slot]);
}

// After a recording or a playback the dummy keeps its controls a moment with nothing held, until it can act again: a
// move it started, or the next hit of a string whose button it took, plays out as the game would play it. Then it is
// the game's own (Off). at_once: the fight is over, nothing to wait for.
void Finish(bool at_once) {
    if (at_once) {
        g_state = State::Idle;
        Off();
        return;
    }
    g_state = State::Finishing;
    g_finish_until = g_game.time() + kFinishCap;
}

void StopRecording(const char* why, bool at_once = false) {
    const auto& slot = g_slots[g_slot];
    LOGR("record: slot %d holds %zu frames (%.1f s)%s", g_slot + 1, slot.size(), slot.size() / 60.0, why);
    Save();
    char note[64];
    std::snprintf(note, sizeof note, "slot %d: %.1f s recorded", g_slot + 1, slot.size() / 60.0);
    Note(note);
    Finish(at_once);
}

void StopPlaying(const char* why, bool at_once = false) {
    LOGR("record: playback stopped%s", why);
    g_between = false;
    Finish(at_once);
}

int RecordedSlots() {
    int n = 0;
    for (const auto& s : g_slots) n += !s.empty();
    return n;
}

// the slot the next sequence plays: the chosen one, or any recorded one at random; -1 with nothing to play
int PickSlot() {
    if (g_mode != Mode::Random) return g_slots[g_slot].empty() ? -1 : g_slot;
    const int n = RecordedSlots();
    if (!n) return -1;
    int pick = int(g_random() % uint32_t(n));
    for (int i = 0; i < kSlots; i++)
        if (!g_slots[i].empty() && pick-- == 0) return i;
    return -1;
}

// the wait for the next sequence: its slot is chosen now, so the dummy can be made ready for it
bool AwaitSequence(uint32_t from) {
    const int slot = PickSlot();
    if (slot < 0) return false;
    g_play_slot = slot;
    g_between = true;
    g_gap_until = from;
    g_turns = g_style_presses = 0;
    return true;
}

// a sequence starts in game frame t: its first input is given for it
void BeginSequence(uint32_t t) {
    g_seq_start = t;
    g_pos = 0;
    g_between = false;
}

void StartRecording() {
    if (g_state == State::Playing) StopPlaying(" (recording)", true);
    g_slots[g_slot].clear();
    g_slot_style[g_slot] = -1;
    g_state = State::Recording;
    g_rec_waiting = true;
    g_gap_until = 0;
    g_turns = 0;
    LOGR("record: recording slot %d, the dummy faces %s", g_slot + 1, g_game.dummy_faces_left() ? "left" : "right");
}

// Playback starts once the dummy can act, as in today's fighting games (a playback asked for while it is still in a
// move, a reaction or turning round would lose the inputs at its start): the first sequence waits as the next one does.
void StartPlaying() {
    if (g_state == State::Recording) StopRecording(" (playing)");
    // the dummy gets its controls first, and whether it can act is asked from two frames on: a move, a reaction or a
    // turn it starts as it gets them shows by then
    if (!AwaitSequence(g_game.time() + 2)) {
        char note[64];
        if (g_mode == Mode::Random) std::snprintf(note, sizeof note, "nothing recorded yet");
        else std::snprintf(note, sizeof note, "slot %d: empty", g_slot + 1);
        Note(note);
        return;
    }
    g_state = State::Playing;
    LOGR("record: playing slot %d, %s, once the dummy can act; it faces %s and is in style %d, the recording started "
         "facing %s in style %d",
         g_play_slot + 1, ModeText(g_mode), g_game.dummy_faces_left() ? "left" : "right", Style(),
         g_slots[g_play_slot][0].faces_left ? "left" : "right", g_slot_style[g_play_slot]);
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
        if (g_state == State::Recording) StopRecording(" (the fight ended)", true);
        else if (g_state == State::Playing) StopPlaying(" (the fight ended)", true);
        else Finish(true);
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
    // Inputs are kept and given per game frame (the game's own counter, which stands still while it is paused): a
    // sample's place is its game frame counted from the first one, so a frame the pad update runs in twice gives or
    // takes the same input, and the playback stays frame for frame with the recording.
    const uint32_t t = g_game.time();
    const bool paused = g_game.paused();
    const Input none{0, g_game.neutral_sticks};
    Input out;
    bool drives = false;
    if (g_state == State::Idle || paused) {
        // paused: the dummy is the game's own again, so the menu has player 1's controls, and a state switch from
        // it (PLAYER SELECT, MAIN MENU) finds the players as the game left them
        Off();
    } else if (g_state == State::Recording && g_rec_waiting &&
               (int32_t(t - g_gap_until) < 0 || !DummyReady() || TurnedAway())) {
        // The recording starts once the dummy can act and faces player 1: inputs before would do nothing (or turn it
        // round) here, and moves in a playback that starts from neutral. Meanwhile player 1 keeps the controller.
        if (int32_t(t - g_gap_until) >= 0 && DummyReady()) Turn(t, out);
        else g_game.drive(Drive::Play, none, out);
    } else if (g_state == State::Recording) {
        if (g_rec_waiting) {
            g_rec_waiting = false;
            g_slot_style[g_slot] = Style();
            LOGR("record: the recording starts, the dummy in style %d", g_slot_style[g_slot]);
        }
        g_game.drive(Drive::Record, none, out);
        drives = true;
        auto& slot = g_slots[g_slot];
        const Sample fr{out.buttons, out.sticks, g_game.dummy_faces_left()};
        if (slot.empty()) {
            g_rec_start = t;
            slot.push_back(fr);
        } else {
            const uint32_t at = t - g_rec_start;
            while (slot.size() < at) slot.push_back(slot.back());  // a game frame no pad update ran in: none seen
            if (slot.size() == at) slot.push_back(fr);
            else slot.back() = fr;  // the same game frame again: its last word
        }
        if (slot.size() >= kMaxFrames) StopRecording(" (30 seconds)");
    } else if (g_state == State::Finishing) {
        if (DummyReady() || int32_t(t - g_finish_until) >= 0) {
            g_state = State::Idle;
            Off();
        } else {
            g_game.drive(Drive::Play, none, out);
        }
    } else {
        const bool waited = g_between && int32_t(t - g_gap_until) >= 0 && DummyReady();
        if (waited && TurnedAway()) {
            Turn(t, out);
        } else if (waited && WrongStyle()) {
            ChangeStyle(t, out);
        } else if (g_between && !waited) {
            g_game.drive(Drive::Play, none, out);
        } else {
            if (g_between) BeginSequence(t);
            const auto& slot = g_slots[g_play_slot];
            g_pos = t - g_seq_start;
            if (g_pos >= slot.size()) {
                if (g_mode == Mode::Once || !AwaitSequence(t + kRepeatGap)) StopPlaying(" (the sequence is over)");
                g_game.drive(Drive::Play, none, out);
            } else {
                const Sample& fr = slot[g_pos];
                Input in{fr.buttons, fr.sticks};
                // left and right as the dummy faced when it was recorded: back is still back from the other side
                if (fr.faces_left != g_game.dummy_faces_left()) Mirror(in.buttons);
                g_game.drive(Drive::Play, in, out);
                drives = true;
            }
        }
    }
    // the dummy's block (F8) stands aside while a recording or a playback has the dummy, waits included: a blocking dummy
    // would never be standing ready for the next sequence
    g_drives = drives || g_state == State::Playing || g_state == State::Recording;
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
            if (s.between) ImGui::Text("slot %d   %s, when the dummy is free", s.play_slot + 1, ModeText(s.mode));
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
