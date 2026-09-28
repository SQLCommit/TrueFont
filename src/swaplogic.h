// Installer state decisions, independent of Direct3D and game-memory access.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace tf {

class KeepMonitor {
public:
    static constexpr int kChatAfter = 3;                 // one chat line once this many re-applies were needed
    static constexpr int kAutoOffCount = 20;             // this many within kAutoOffWindowMs => automatic off (sticky)
    static constexpr uint64_t kAutoOffWindowMs = 60000;
    struct Event {
        bool counted = false;   // this call was counted (false: a second re-apply in the same frame)
        bool chat = false;      // say the one chat line now
        bool autoOff = false;   // turn TrueFont off, sticky
        int total = 0;          // re-applies since load
        int inWindow = 0;       // re-applies within the last kAutoOffWindowMs
    };
    // At most one re-apply is counted a frame (a frame that re-applies both records counts once).
    bool mayReapply(uint64_t frame) const { return !any_ || frame != lastFrame_; }
    Event reapplied(uint64_t frame, uint64_t nowMs) {
        Event e;
        e.total = total_;
        if (!mayReapply(frame)) { e.inWindow = int(times_.size()); return e; }
        any_ = true;
        lastFrame_ = frame;
        ++total_;
        times_.push_back(nowMs);
        while (!times_.empty() && nowMs - times_.front() >= kAutoOffWindowMs) times_.pop_front();
        e.counted = true;
        e.total = total_;
        e.inWindow = int(times_.size());
        if (total_ >= kChatAfter && !chatSaid_) { chatSaid_ = true; e.chat = true; }
        if (e.inWindow >= kAutoOffCount) e.autoOff = true;
        return e;
    }
    // /tfont on after an automatic off: the 60 s window starts empty (the running total and the chat-once stay).
    void clearWindow() { times_.clear(); }
    int total() const { return total_; }
    bool chatSaid() const { return chatSaid_; }

private:
    std::deque<uint64_t> times_;
    uint64_t lastFrame_ = 0;
    bool any_ = false, chatSaid_ = false;
    int total_ = 0;
};

enum class SlotRestore : uint8_t {
    WriteOriginal,   // +40 holds ours: write the original back, then drop the reference the record held on ours
    AlreadyOriginal, // +40 holds the original already (someone put it back): nothing to write
    Foreign,         // +40 holds another texture: another writer's business; nothing written, ours not released for it
    Unreadable,      // +40 could not be read: nothing written; ours may still be there (a leftover)
    RecordChanged,   // the renderer now names another record (or none): the old one is not touched
    GameGone,        // the renderer global is 0 (game teardown): nothing is touched
    UnknownOriginal, // +40 holds ours but the original is not known (0): nothing written, ours stays
};
inline const char* slotRestoreText(SlotRestore r) {
    switch (r) {
    case SlotRestore::WriteOriginal: return "ours: the original goes back";
    case SlotRestore::AlreadyOriginal: return "already the original";
    case SlotRestore::Foreign: return "another texture (another writer's): left alone";
    case SlotRestore::Unreadable: return "unreadable: left";
    case SlotRestore::RecordChanged: return "the record changed: the old one is not touched";
    case SlotRestore::GameGone: return "the renderer is gone (game teardown): nothing touched";
    case SlotRestore::UnknownOriginal: return "ours, but the original is unknown: nothing written, ours left";
    }
    return "?";
}
inline SlotRestore decideSlotRestore(bool rendererAlive, bool recordSame, bool readOk, uintptr_t now, uintptr_t ours, uintptr_t original) {
    if (!rendererAlive) return SlotRestore::GameGone;
    if (!recordSame) return SlotRestore::RecordChanged;
    if (!readOk) return SlotRestore::Unreadable;
    if (now == ours && ours) return original ? SlotRestore::WriteOriginal : SlotRestore::UnknownOriginal;   // never write a null original
    if (now == original) return SlotRestore::AlreadyOriginal;
    return SlotRestore::Foreign;
}
// Whether the record may still reference our texture after this decision (then our texture must never be released).
inline bool slotMayHoldOurs(SlotRestore r, bool writeOk) {
    switch (r) {
    case SlotRestore::WriteOriginal: return !writeOk;
    case SlotRestore::Unreadable: return true;
    case SlotRestore::UnknownOriginal: return true;
    default: return false;
    }
}

enum class ReleaseMode : uint8_t {
    Nothing,    // Initialize refused: touch nothing at all
    Full,       // render thread, outside TrueFont's frame callback: pointers back, our texture released
    DataOnly,   // inside the frame callback, or off the render thread: pointers back only (no Direct3D call), DLL pinned
};
inline const char* releaseModeText(ReleaseMode m) {
    switch (m) {
    case ReleaseMode::Nothing: return "nothing (Initialize refused)";
    case ReleaseMode::Full: return "full restore (render thread, outside a frame)";
    case ReleaseMode::DataOnly: return "data only (inside a frame or off the render thread)";
    }
    return "?";
}
// `renderThread` 0: no frame was seen, nothing was installed: the full path is safe from any thread.
inline ReleaseMode decideReleaseMode(bool refused, unsigned long renderThread, unsigned long here, bool insideFrame) {
    if (refused) return ReleaseMode::Nothing;
    if (!renderThread) return ReleaseMode::Full;
    if (here == renderThread && !insideFrame) return ReleaseMode::Full;
    return ReleaseMode::DataOnly;
}

// What an off or an unload could not put back.
struct OffReport {
    bool wasOn = false;
    bool gameGone = false;          // the renderer was gone: nothing touched
    std::vector<std::string> left;  // leftovers of TrueFont's, in words
    bool clean() const { return left.empty(); }
    std::string leftText() const {
        std::string s;
        for (size_t i = 0; i < left.size(); i++) s += (i ? "; " : "") + left[i];
        return s;
    }
};
// Unload warning, empty during game teardown or when no warning is needed.
inline std::string releaseLine(const OffReport& rep, ReleaseMode mode, bool onRender, bool mustPin) {
    if (rep.gameGone) return std::string();
    const char* how = mode == ReleaseMode::DataOnly ? (onRender ? "inside a frame" : "off the render thread") : "";
    if (!rep.clean()) return std::string("unloaded") + (*how ? " " : "") + how + ", leaving: " + rep.leftText() + ". Restart the game before loading TrueFont again.";
    if (mode == ReleaseMode::DataOnly && rep.wasOn)
        return std::string("unloaded ") + how + ": the game's own fonts are back, but TrueFont's textures stay in memory, so a reload needs a game restart.";
    if (mustPin && rep.wasOn) return "unloaded: the game's own fonts are back, but part of TrueFont stays in memory, so a reload needs a game restart.";
    return std::string();
}

// A section's [lo, hi) RVA range from a PE image laid out as in memory. False when absent.
inline bool peSection(const uint8_t* image, size_t size, const char* name, uint32_t& lo, uint32_t& hi) {
    lo = hi = 0;
    if (!image || size < 0x40) return false;
    uint32_t pe = 0;
    std::memcpy(&pe, image + 0x3C, 4);
    if (uint64_t(pe) + 24 > size || std::memcmp(image + pe, "PE\0\0", 4) != 0) return false;
    uint16_t sections = 0, optSize = 0;
    std::memcpy(&sections, image + pe + 6, 2);
    std::memcpy(&optSize, image + pe + 20, 2);
    const uint64_t table = uint64_t(pe) + 24 + optSize;
    for (uint16_t i = 0; i < sections; i++) {
        const uint64_t at = table + uint64_t(i) * 40;
        if (at + 40 > size) return false;
        char nm[9] = {};
        std::memcpy(nm, image + at, 8);
        if (std::strcmp(nm, name) != 0) continue;
        uint32_t vsize = 0, va = 0;
        std::memcpy(&vsize, image + at + 8, 4);
        std::memcpy(&va, image + at + 12, 4);
        if (!vsize || !va) return false;
        lo = va;
        hi = va + vsize;
        return true;
    }
    return false;
}

// Zone-out 0x00B, +04: 1 logout to character select, 2 zone change, 3 Mog House, 4 cancelled logout, 0 nothing,
// 5-9 other exits (a logout).
inline bool zoneOutIsLogout(const uint8_t* data, uint32_t size) { return data && size >= 5 && data[4] != 0 && data[4] != 2 && data[4] != 3 && data[4] != 4; }
// The zone-in packet 0x00A names a character: server id at +04, the name at +84.
inline bool zoneInNamesCharacter(const uint8_t* data, uint32_t size) {
    if (!data || size < 0x94) return false;
    uint32_t server = 0;
    std::memcpy(&server, data + 0x04, 4);
    return server != 0 && data[0x84] != 0;
}
inline constexpr uint64_t kLogoutMs = 10000;       // profile.h kEmptyPollsForLogout (10 one-second polls)
inline constexpr uint64_t kDisconnectMs = 60000;   // profile.h kEmptyPollsForDisconnect
// Why the absence is a logout, or null while it is not (yet). `outMs`: out of the game and not zoning this long.
// `packetKnown`: the character came with a zone-in packet (then only the logout packet or 60 s count).
inline const char* logoutReason(bool byPacket, bool packetKnown, uint64_t outMs) {
    if (byPacket) return "the game's logout packet";
    if (packetKnown) return outMs >= kDisconnectMs ? "60 s out of the game: the connection was lost" : nullptr;
    return outMs >= kLogoutMs ? "10 s out of the game" : nullptr;
}

// "<folder of FFXiMain.dll>\ROM\272\120.DAT", the game's PNG copy of the moji atlas; "" without a folder.
inline std::wstring datPathFor(const std::wstring& modulePath) {
    const size_t slash = modulePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return modulePath.substr(0, slash + 1) + L"ROM\\272\\120.DAT";
}

// Copies `rows` rows of `rowBytes` bytes between buffers with their own pitches.
inline void copyRows(uint8_t* dst, size_t dstPitch, const uint8_t* src, size_t srcPitch, size_t rowBytes, size_t rows) {
    for (size_t y = 0; y < rows; y++) std::memcpy(dst + y * dstPitch, src + y * srcPitch, rowBytes);
}

// The atlas grid a texture of w x h holds: the cell size, or 0 when it is not a 64 x 128 grid of square cells.
inline int gridCell(unsigned w, unsigned h) {
    if (!w || w % 64 || h != 2 * w) return 0;
    return int(w / 64);
}

}  // namespace tf
