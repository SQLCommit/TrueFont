// Presets and shared settings, accessed on the disk writer.
// Presets omit enabled. Every Character shares other settings but keeps enabled in the character's file.
// Shared saves write changed keys only, preserving unrelated changes from other clients.
#pragma once
#include "settings.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace tf {

inline constexpr size_t kPresetNameMax = 40;   // bytes of UTF-8

// The name as typed, trimmed, at most 40 bytes without cutting a code point.
inline std::string presetName(const std::string& typed) {
    size_t a = 0, b = typed.size();
    while (a < b && std::isspace(static_cast<unsigned char>(typed[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(typed[b - 1]))) b--;
    std::string s = typed.substr(a, b - a);
    if (s.size() > kPresetNameMax) {
        size_t cut = kPresetNameMax;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;   // s[cut] starts the first code point left out
        s.resize(cut);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    }
    return s;
}

inline std::string presetFileBase(const std::string& name) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (const unsigned char c : name) {
        if (c >= 0x80 || c == '%' || c < 0x20 || c == 0x7F || std::strchr("\\/:*?\"<>|", c)) { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
        else o += char(c);
    }
    // A reserved device name opens the device, not a file, whatever the extension.
    std::string stem = o.substr(0, o.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& ch : stem) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    const bool device = stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                        (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '1' && stem[3] <= '9');
    if (device && !o.empty()) {
        const unsigned char c = static_cast<unsigned char>(o[0]);
        o = std::string("%") + hex[c >> 4] + hex[c & 15] + o.substr(1);
    }
    return o;
}
inline std::string presetNameFromFile(const std::string& base) { return iniUnescape(base); }
inline std::string presetPath(const std::string& dir, const std::string& name) { return dir + presetFileBase(name) + ".ini"; }

inline std::string presetText(const Settings& s) {
    std::string t = std::string("[") + Settings::kSection + "]\r\n";
    for (const IniValue& v : s.snapshot(false))
        if (!v.remove) t += v.key + "=" + v.value + "\r\n";   // a removal (the first font's <g>_font) is no line in a whole file
    return t;
}

// The whole file, written to a temp file and renamed over `path`. False with the error (GetLastError's).
inline bool writeWholeFile(const std::string& path, const std::string& text, DWORD& error) {
    const std::string tmp = path + ".tmp";
    error = 0;
    const HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == DWORD(text.size());
    if (!ok) error = GetLastError();
    CloseHandle(h);
    if (ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    if (!error) error = GetLastError();
    DeleteFileA(tmp.c_str());
    return false;
}

inline bool savePreset(const std::string& dir, const std::string& name, const Settings& s, DWORD& error) {
    if (name.empty()) { error = ERROR_INVALID_NAME; return false; }
    return writeWholeFile(presetPath(dir, name), presetText(s), error);
}
// The presets in `dir`, by name, sorted ignoring case. A folder that does not exist has none.
inline std::vector<std::string> listPresets(const std::string& dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd{};
    const HANDLE h = FindFirstFileA((dir + "*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string f = fd.cFileName;
        if (f.size() <= 4 || _stricmp(f.c_str() + f.size() - 4, ".ini") != 0) continue;   // "*.ini" also matches longer extensions by their 8.3 names
        names.push_back(presetNameFromFile(f.substr(0, f.size() - 4)));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const int c = _stricmp(a.c_str(), b.c_str());
        return c != 0 ? c < 0 : a < b;
    });
    return names;
}
// Read presets over defaults; the caller preserves its enabled flag.
inline bool loadPreset(const std::string& dir, const std::string& name, Settings& out) {
    Settings p;
    if (!p.load(presetPath(dir, name))) return false;
    out = p;
    return true;
}
inline bool deletePreset(const std::string& dir, const std::string& name, DWORD& error) {
    error = 0;
    if (DeleteFileA(presetPath(dir, name).c_str())) return true;
    error = GetLastError();
    return false;
}

// A settings file's values as this client last read or wrote them, by key (a key the file does not hold is absent).
using IniBase = std::map<std::string, std::string>;
inline IniBase readIniBase(const std::string& path, const IniSnapshot& keys) {
    IniBase b;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return b;
    for (const IniValue& k : keys) {
        char v[512] = {};
        if (GetPrivateProfileStringA(k.section.c_str(), k.key.c_str(), "\x01", v, sizeof v, path.c_str()) > 0 && v[0] == '\x01' && !v[1]) continue;   // absent
        b[k.key] = v;
    }
    return b;
}
// The entries of `want` that change the file as `base` knows it: a value it does not hold, or a removal of a key it has.
inline IniSnapshot changedValues(const IniSnapshot& want, const IniBase& base) {
    IniSnapshot out;
    for (const IniValue& v : want) {
        const auto it = base.find(v.key);
        if (v.remove ? it != base.end() : (it == base.end() || it->second != v.value)) out.push_back(v);
    }
    return out;
}
// Track implied defaults for keys missing from older files, so their first save preserves existing behavior.
inline IniBase impliedValues(const IniSnapshot& loaded, const IniBase& base) {
    IniBase out;
    for (const IniValue& v : loaded)
        if (!v.remove && !v.last && !base.count(v.key)) out[v.key] = v.value;
    return out;
}
// For unchanged implied defaults, preserve keys another client has since written to the shared file.
inline void keepOthersWrites(const std::string& path, IniSnapshot& want, IniBase& base, const IniBase& implied) {
    if (implied.empty()) return;
    IniSnapshot out;
    for (IniValue& v : want) {
        const auto im = implied.find(v.key);
        if (!v.remove && im != implied.end() && im->second == v.value) {
            if (base.count(v.key)) continue;
            char b[512] = {};
            const DWORD n = GetPrivateProfileStringA(v.section.c_str(), v.key.c_str(), "\x01", b, sizeof b, path.c_str());
            if (!(n > 0 && b[0] == '\x01' && !b[1])) {   // there now (as readIniBase decides)
                base[v.key] = b;
                continue;
            }
        }
        out.push_back(std::move(v));
    }
    want = std::move(out);
}
// Write changed entries and update the baseline per successful write. Commit last entries only after
// all ordinary entries succeed; preserve the first failure while attempting the remaining writes.
inline bool writeChanged(const std::string& path, const IniSnapshot& want, IniBase& base, DWORD& error, int* written = nullptr, IniBase* implied = nullptr,
                         IniWriteFn write = &WritePrivateProfileStringA) {
    int n = 0;
    const bool ok = writeIniEntries(
        path, changedValues(want, base), error,
        [&](const IniValue& v) {
            ++n;
            if (v.remove) base.erase(v.key);
            else base[v.key] = v.value;
            if (implied) implied->erase(v.key);
        },
        write);
    if (written) *written = n;
    return ok;
}

inline bool readShareAll(const std::string& ownPath) { return GetPrivateProfileIntA(Settings::kSection, "share_all", 0, ownPath.c_str()) == 1; }
inline bool writeShareAll(const std::string& ownPath, bool on) { return WritePrivateProfileStringA(Settings::kSection, "share_all", on ? "1" : "0", ownPath.c_str()) != 0; }

// Read shared settings when enabled; fall back to the character's file if the shared file is missing.
struct CharacterRead { bool exists = false, share = false, sharedMissing = false; };
inline CharacterRead readCharacter(const std::string& ownPath, const std::string& sharedPath, Settings& s) {
    CharacterRead r;
    r.share = readShareAll(ownPath);
    if (r.share && GetFileAttributesA(sharedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        r.exists = s.load(sharedPath);
        // The TrueFont switch is the character's own (an older shared file may hold one; the own file's wins).
        const int own = int(GetPrivateProfileIntA(Settings::kSection, "enabled", -1, ownPath.c_str()));
        if (own == 0 || own == 1) s.enabled = own == 1;
    } else {
        r.sharedMissing = r.share;
        r.exists = s.load(ownPath);
    }
    return r;
}
// On: save the local enabled flag, copy other settings to shared, then commit share_all.
// Off: copy the current settings to the character file, then clear share_all. Never change the flag before the copy succeeds.
inline bool shareOn(const std::string& ownPath, const std::string& sharedPath, const Settings& s, DWORD* error = nullptr) {
    DWORD e = 0;
    bool ok = WritePrivateProfileStringA(Settings::kSection, "enabled", s.enabled ? "1" : "0", ownPath.c_str()) != 0;
    if (!ok) e = GetLastError();
    if (ok) ok = s.save(sharedPath, false, &e);
    if (ok && !writeShareAll(ownPath, true)) { e = GetLastError(); ok = false; }
    if (error) *error = ok ? 0 : e;
    return ok;
}
inline bool shareOff(const std::string& ownPath, const Settings& s, DWORD* error = nullptr) {
    DWORD e = 0;
    bool ok = s.save(ownPath, true, &e);
    if (ok && !writeShareAll(ownPath, false)) { e = GetLastError(); ok = false; }
    if (error) *error = ok ? 0 : e;
    return ok;
}

// A save job's outcome: while Every Character is on it writes two files, and the first failure is the one said.
struct SaveOutcome {
    bool ok = true;
    DWORD error = 0;
    std::string shown;   // the failed file, as the chat names it
    void add(bool fileOk, DWORD fileError, const std::string& fileShown) {
        if (fileOk || !ok) return;
        ok = false;
        error = fileError;
        shown = fileShown;
    }
};
// Report the first failure in a run and its recovery; an empty string means no notice.
struct SaveNotice {
    bool failSaid = false;
    std::string next(const SaveOutcome& o, unsigned char& color) {
        if (!o.ok) {
            if (failSaid) return std::string();
            failSaid = true;
            color = 0x44;
            return "could not save the settings to " + o.shown + " (error " + std::to_string(o.error) + "); they apply for this session.";
        }
        if (!failSaid) return std::string();
        failSaid = false;
        color = 0x6A;
        return "settings saved again.";
    }
};

}  // namespace tf
