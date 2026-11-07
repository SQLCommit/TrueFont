// Which XIPivot overlay serves a DAT.
#pragma once
#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace tf {

struct PivotIni {
    std::string root;                    // "" = default DATs
    std::vector<std::string> overlays;   // first wins
};

inline std::string pivotTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(uint8_t(s[a]))) a++;
    while (b > a && std::isspace(uint8_t(s[b - 1]))) b--;
    return s.substr(a, b - a);
}
inline bool pivotSame(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
    return true;
}

// Parsed as XIPivot does.
inline PivotIni parsePivotIni(const std::string& text) {
    PivotIni r;
    std::vector<std::pair<std::string, std::string>> ov;
    std::string section;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = pivotTrim(text.substr(at, nl - at));
        at = nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { section = pivotTrim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = pivotTrim(line.substr(0, eq)), v = pivotTrim(line.substr(eq + 1));
        if (pivotSame(section, "settings") && pivotSame(k, "root_path")) r.root = v;
        else if (pivotSame(section, "overlays")) ov.push_back({k, v});
    }
    for (int i = 0;; i++) {
        const std::string key = std::to_string(i);
        const std::string* v = nullptr;
        for (const auto& kv : ov)
            if (kv.first == key) { v = &kv.second; break; }
        if (!v) break;
        if (!v->empty()) r.overlays.push_back(*v);
    }
    return r;
}

// Overlay holding rel, or "".
inline std::string pivotOverlayFor(const PivotIni& ini, const std::vector<std::string>& roots, const std::string& rel,
                                   const std::function<bool(const std::string&)>& exists) {
    for (const auto& name : ini.overlays)
        for (std::string root : roots) {
            if (root.empty()) continue;
            if (root.back() != '\\' && root.back() != '/') root += '\\';
            if (exists(root + name + "\\" + rel)) return name;
        }
    return std::string();
}

// Live: needs pivot.dll loaded.
inline std::string pivotOverlayLive(const std::string& ashitaRoot, const std::string& rel) {
    if (!GetModuleHandleA("pivot.dll")) return std::string();
    const std::string path = ashitaRoot + "config\\pivot\\pivot.ini";
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string text(64 * 1024, '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, &text[0], DWORD(text.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok) return std::string();
    text.resize(got);
    const PivotIni ini = parsePivotIni(text);
    char cwd[MAX_PATH] = {};
    const std::string here = GetCurrentDirectoryA(MAX_PATH, cwd) ? std::string(cwd) + "\\" : ashitaRoot;
    std::vector<std::string> roots;
    if (ini.root.empty()) roots = {here + "DATs", ashitaRoot + "polplugins\\DATs"};
    else if (ini.root.size() > 1 && (ini.root[1] == ':' || ini.root[0] == '\\' || ini.root[0] == '/')) roots = {ini.root};
    else roots = {here + ini.root, ashitaRoot + ini.root};
    return pivotOverlayFor(ini, roots, rel, [](const std::string& p) {
        const DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    });
}

}   // namespace tf
