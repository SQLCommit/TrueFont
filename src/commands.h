// Command metadata and shared status/diagnostic text.
#pragma once
#include "tf_mem.h"
#include <cstdint>
#include <string>

namespace tf {

// Read-only commands leave selftest running. Bare applies only without arguments.
enum class Reads : uint8_t { Never, Always, Bare };
struct CommandWord {
    const char* word;
    const char* usage;   // as the usage line shows it
    Reads reads;
    bool needsPart;      // with nothing resolved it says why instead
    bool devOnly;
};
inline constexpr CommandWord kCommandWords[] = {
    {"on", "on", Reads::Never, true, false},
    {"off", "off", Reads::Never, true, false},
    {"font", "font \"<family>\"", Reads::Never, true, false},
    {"fonts", "fonts [rescan]", Reads::Bare, false, false},
    {"status", "status", Reads::Always, true, false},
    {"diag", "diag", Reads::Always, false, false},   // the report is most wanted when nothing resolved
    {"rebuild", "rebuild", Reads::Never, true, false},
    {"original", "original [on | off]", Reads::Never, true, false},
    {"reset", "reset", Reads::Never, false, false},
    {"selftest", "selftest [stop]", Reads::Never, false, true},
};
inline const CommandWord* commandWord(const std::string& sub) {
    for (const CommandWord& c : kCommandWords)
        if (sub == c.word) return &c;
    return nullptr;
}
// `words`: the command's words, /tfont included.
inline bool commandOnlyReads(const std::string& sub, size_t words) {
    const CommandWord* c = commandWord(sub);
    return c && (c->reads == Reads::Always || (c->reads == Reads::Bare && words == 2));
}
inline bool commandNeedsPart(const std::string& sub) {
    const CommandWord* c = commandWord(sub);
    return c && c->needsPart;
}
inline std::string commandUsage(bool dev) {
    std::string t = "/truefont opens the settings panel; /tfont ";
    bool first = true;
    for (const CommandWord& c : kCommandWords) {
        if (c.devOnly && !dev) continue;
        t += std::string(first ? "" : " | ") + c.usage;
        first = false;
    }
    return t + ".";
}

// Highlight command names with the chat encoder's markers.
inline constexpr const char* kLoadUsage = "\x11" "/truefont" "\x12" " [" "\x11" "status" "\x12" "|" "\x11" "diag" "\x12" "]   (or " "\x11" "/tfont" "\x12" ")";
// Empty settingsShown means no character has loaded yet.
inline std::string settingsFooterText(const std::string& settingsShown, bool everyCharacter, const std::string& logShown, uint32_t build) {
    const std::string settings = settingsShown.empty() ? std::string("none yet (not logged in)") : settingsShown + (everyCharacter ? " (Every Character)" : "");
    return "Settings: " + settings + ". Log: " + logShown + fmt(". Build %08X.", unsigned(build));
}

inline constexpr const char* kDiagUsage = "/tfont diag: writes a diagnostic report to the log.";
inline std::string diagDoneText(const std::string& logPath) { return "diagnostics written to " + logPath + "."; }
// The end of /tfont status's first chat line.
inline std::string statusChatTail() { return " /tfont diag writes a full report to the log."; }
inline std::string diagWho(double version, uint32_t build) { return fmt("TrueFont %.1f build %08X", version, build); }
// A diagnostic block has no level column; mark warnings and errors inline.
inline std::string diagLine(const char* level, const std::string& text) {
    const std::string lv = level ? level : "info";
    return "      " + (lv == "info" ? std::string() : "[" + lv + "] ") + text + "\n";
}

struct GroupDiag {
    bool enabled = true;     // TrueFont's own switch
    bool on = false;         // the group's box
    bool available = true;
    bool down = false;
    bool failed = false;
    bool restart = false;
    bool replaced = false;
    std::string language;    // the game's language when it has no menu layouts; "" = none
    int shape = -1;          // Aspect and Size: -1 none for this group, 0 available, 1 another plugin's, 2 not on this game version
};
// A group's state as the panel reads it, in the panel's precedence.
inline std::string groupDiagText(const char* key, const GroupDiag& g) {
    std::string t = std::string("group ") + key + ": ";
    if (!g.available) t += g.language.empty() ? "not available on this game version" : "not available (the game's language, " + g.language + ", has no menu layouts in TrueFont)";
    else if (!g.on) t += "switched off";
    else if (!g.enabled) t += "switched on; TrueFont is off";
    else if (g.down) t += "down (its side could not turn on or turned itself off)";
    else if (g.failed) t += g.restart ? "failed (its record still holds an earlier texture of TrueFont's: restart the game)" : "failed (it could not be built or put in place)";
    else if (g.replaced) t += "the game's own (its record holds other art since load; /tfont on takes a fresh look)";
    else t += "on";
    if (g.shape >= 0) t += std::string("; Aspect and Size: ") + (g.shape == 1 ? "another plugin's" : g.shape == 2 ? "not on this game version" : "available");
    return t;
}

struct DiagMemory {
    int textures = 0;
    uint64_t textureBytes = 0, keptBytes = 0;   // the Status panel's Memory is their sum
    uintptr_t shapePage = 0;
    bool processRead = false;
    uint64_t workingSet = 0, privateBytes = 0, availVirtual = 0, totalVirtual = 0;
};
inline std::string diagMemoryText(const DiagMemory& m) {
    const auto mb = [](uint64_t b) { return double(b) / double(1u << 20); };
    std::string t = fmt("memory: TrueFont's %d textures %.1f MB, kept copies %.1f MB; Aspect and Size's page ", m.textures, mb(m.textureBytes), mb(m.keptBytes));
    t += m.shapePage ? fmt("4 KB at %08X", unsigned(m.shapePage)) : std::string("none");
    if (!m.processRead) return t + "; the game: could not be read";
    return t + fmt("; the game: working set %.1f MB, private %.1f MB, address space free %.1f MB of %.1f MB", mb(m.workingSet), mb(m.privateBytes), mb(m.availVirtual), mb(m.totalVirtual));
}

}  // namespace tf
