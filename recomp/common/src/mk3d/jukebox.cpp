// The jukebox's bank substitution (jukebox.h): the map file, and the open hook that swaps a music bank's files.
#include "mk3d/jukebox.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"

namespace mk3d::jukebox {
namespace {

namespace fs = std::filesystem;

struct Entry {
    bool select = false;  // else an arena's
    int arena = -1;       // -1: any arena
    std::string bank;     // upper case, "MUSIC_TUNE4"
    std::string folder;   // holds <bank>.msx (and .mbx) as the launcher wrote them
};

std::mutex g_lock;
Adapter g_adapter{};
fs::path g_file;
fs::file_time_type g_file_time{};
std::chrono::steady_clock::time_point g_checked{};
bool g_said_missing = false;
std::vector<Entry> g_entries;
std::string g_last_logged;  // one line per bank and arena, not per file of it

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// g_lock held: the map file again when it changed (looked at once a second at most)
void Refresh() {
    const auto now = std::chrono::steady_clock::now();
    if (g_checked != std::chrono::steady_clock::time_point{} && now - g_checked < std::chrono::seconds(1)) return;
    g_checked = now;
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(g_file, ec);
    if (ec) {
        if (!g_entries.empty() || !g_said_missing) {
            g_said_missing = true;
            LOGR("jukebox: %s is not there, the game's own music", g_file.string().c_str());
        }
        g_entries.clear();
        g_file_time = {};
        return;
    }
    if (time == g_file_time) return;
    std::ifstream in(g_file, std::ios::binary);
    if (!in) return;  // being replaced: the next look reads it
    g_file_time = time;
    std::vector<Entry> entries;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string kind;
        Entry e;
        words >> kind;
        if (kind == "select") {
            e.select = true;
        } else if (kind == "arena") {
            std::string id;
            words >> id;
            if (id == "*") {
                e.arena = -1;
            } else {
                char* end = nullptr;
                const long v = std::strtol(id.c_str(), &end, 10);
                if (!end || *end || v < 0) {
                    LOGR("jukebox: %s line %d: arena %s?", g_file.string().c_str(), n, id.c_str());
                    continue;
                }
                e.arena = int(v);
            }
        } else {
            LOGR("jukebox: %s line %d: %s?", g_file.string().c_str(), n, kind.c_str());
            continue;
        }
        words >> e.bank;
        std::getline(words, e.folder);
        while (!e.folder.empty() && std::isspace(static_cast<unsigned char>(e.folder.front()))) e.folder.erase(0, 1);
        while (!e.folder.empty() && std::isspace(static_cast<unsigned char>(e.folder.back()))) e.folder.pop_back();
        if (e.bank.empty() || e.folder.empty()) {
            LOGR("jukebox: %s line %d: a bank and a folder are needed", g_file.string().c_str(), n);
            continue;
        }
        e.bank = Upper(e.bank);
        entries.push_back(std::move(e));
    }
    g_entries = std::move(entries);
    g_said_missing = false;
    LOGR("jukebox: %s: %zu substitute%s", g_file.string().c_str(), g_entries.size(), g_entries.size() == 1 ? "" : "s");
}

// "\DEVICE\CDROM0\SNDSXBOX\MUSIC_TUNE4.MSX" -> bank "MUSIC_TUNE4", ext "MSX"; false for any other path
bool SoundBank(const std::string& xbox, std::string& bank, std::string& ext) {
    const size_t dir = xbox.rfind("\\SNDSXBOX\\");
    if (dir == std::string::npos) return false;
    const std::string name = xbox.substr(dir + 10);
    if (name.find('\\') != std::string::npos) return false;
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    ext = name.substr(dot + 1);
    if (ext != "MSX" && ext != "MBX") return false;
    bank = name.substr(0, dot);
    return true;
}

bool Hook(const std::string& xbox, std::string& host) {
    std::string bank, ext;
    if (!SoundBank(xbox, bank, ext)) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    Refresh();
    const int arena = g_adapter.arena ? g_adapter.arena() : -1;
    // what a fight loads, for the mapping of arenas to banks (the launcher's table) and for a player's report
    if (ext == "MSX") {
        const std::string key = bank + "@" + std::to_string(arena);
        if (key != g_last_logged) {
            g_last_logged = key;
            LOGR("jukebox: bank %s opened, arena %d", bank.c_str(), arena);
        }
    }
    const Entry* found = nullptr;
    for (const Entry& e : g_entries) {
        if (e.bank != bank) continue;
        if (e.select) {
            found = &e;
            break;
        }
        if (e.arena == arena) {
            found = &e;
            break;
        }
        if (e.arena < 0 && !found) found = &e;
    }
    if (!found) return false;
    std::string lower = bank;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    const fs::path file = fs::path(found->folder) / (lower + (ext == "MSX" ? ".msx" : ".mbx"));
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        if (ext == "MSX")
            LOGR("jukebox: %s has no %s, the game's own", found->folder.c_str(), file.filename().string().c_str());
        return false;
    }
    if (ext == "MSX") {
        if (found->select) LOGR("jukebox: %s for the front end from %s", file.filename().string().c_str(), found->folder.c_str());
        else LOGR("jukebox: %s for arena %d from %s", file.filename().string().c_str(), arena, found->folder.c_str());
    }
    host = file.string();
    return true;
}

}  // namespace

void Install(const Adapter& adapter) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_adapter = adapter;
    const char* file = std::getenv("MK3D_JUKEBOX");
    if (!file || !*file) return;
    g_file = fs::path(file);
    kernel::SetOpenHook(Hook);
    LOGR("jukebox: map %s", file);
}

}  // namespace mk3d::jukebox
