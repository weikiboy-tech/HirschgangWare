#include "friends.h"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <mutex>
#include <set>

namespace friends {

namespace {

std::mutex gMx;
std::set<std::string> gNames;  // upper-cased keys for case-insensitive matching

std::string upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string filePath() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n ? n : 0);
    const size_t slash = p.find_last_of("\\/");
    p.resize(slash == std::string::npos ? 0 : slash + 1);
    return p + "friends.txt";
}

void trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
}

}  // namespace

bool contains(const std::string& name) {
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lk(gMx);
    return gNames.count(upper(name)) != 0;
}

bool toggle(const std::string& name) {
    if (name.empty() || name == "?") return false;
    const std::string key = upper(name);
    bool now = false;
    {
        std::lock_guard<std::mutex> lk(gMx);
        const auto it = gNames.find(key);
        if (it != gNames.end()) {
            gNames.erase(it);
        } else {
            gNames.insert(key);
            now = true;
        }
    }
    save();
    return now;
}

void load() {
    std::lock_guard<std::mutex> lk(gMx);
    gNames.clear();
    std::ifstream f(filePath());
    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        gNames.insert(upper(line));
    }
}

void save() {
    std::lock_guard<std::mutex> lk(gMx);
    std::ofstream f(filePath(), std::ios::trunc);
    if (!f) return;
    f << "# HirschgangWare teammates - one player name per line; marked players are green on ESP + radar\n";
    for (const std::string& k : gNames) f << k << "\n";
}

}  // namespace friends
