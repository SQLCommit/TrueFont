// Plugin callbacks, settings and UI. Chat and sprite-font installers resolve independently and run on Present.
// File I/O runs on the disk writer; neither installer starts before the character's settings are loaded.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Ashita.h"
#include "plugin_log.h"
#include "tf_mem.h"
#include "sigs.h"
#include "charmap.h"
#include "atlas.h"
#include "swap.h"
#include "spritesigs.h"
#include "spriteswap.h"
#include "screen.h"
#include "shape.h"
#include "lifecycle.h"
#include "settings.h"
#include "presets.h"
#include "fonts.h"
#include "fontsdir.h"
#include "panel.h"
#include "commands.h"
#ifdef TF_DEV
#include "selftest.h"
#endif
#include <psapi.h>
#include <cctype>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr double kVersion = 1.1;
constexpr const char* kName = "truefont";

IAshitaCore* core = nullptr;
plog::FileLog fileLog;
plog::Run run;
std::string root;              // the Ashita folder (above plugins\)
std::string charName;          // "" before login
bool langLogPending = false;   // Log the language once per character load.
uint32_t charServer = 0;
tf::SoleInstance soleInstance;   // lifecycle.h: one TrueFont per process
bool refused = false;   // Initialization refused; release must not touch shared state.
bool logStarted = false;
std::string resolveWhy = "the game addresses did not resolve";
std::vector<std::string> resolveLines;   // the load's resolve summary lines, for /tfont diag
uint32_t clientStamp = 0;      // FFXiMain.dll's PE TimeDateStamp (0: not read)
std::string startFailed;   // Initialization exception; takes precedence over unsupported-client messages.

tf::Module client;             // FFXiMain.dll
tf::Resolved R;                // its addresses (RVAs)
tf::ScreenResolved screenR;    // the sizes Sharpness Auto reads (screen.h)
tf::AutoKMonitor autoKPlate;   // Nameplates: poll each second; debounce scale changes for 3 s.
tf::AutoKMonitor autoKDamage;   // Damage: independent scale using its Aspect and Size.
// Aspect/Size patch state; only the render thread writes it.
tf::ShapeResolved shapeR;
tf::LiveShapeMem shapeMem;
tf::ShapePatch shape;
tf::ShapeWant shapeWant[tf::kShapeGroupCount];   // Retained across logout until settings reload.
bool shapeWantKnown = false;   // the character's settings were read at least once
bool shapeAsked[tf::kShapeGroupCount] = {};   // Explicit requests report conflicts in chat.
bool shapeChatSaid = false;    // that line, once since load
double shapeRhoNow = 0;        // H/W of the window the floats follow
uint32_t shapeLastVp = 0;      // the viewport words (app+10) last seen: a window resize can rewrite them
uint64_t shapeNextPoll = 0;    // the once-a-second re-read of the sites and the window
#ifdef TF_DEV
bool shapeHeld = false;   // Previous selftest geometry hold, for transition logging.
#endif
bool screenSaid = false;       // "the sizes cannot be read" logged once
DWORD renderThread = 0;
uint64_t frame = 0;
volatile long releasing = 0;   // Release has begun: the frame callback does nothing more
volatile long inFrame = 0;     // TrueFont's Present callback is running
bool pinned = false;
bool loggedIn = false;
IDirect3DDevice8* device = nullptr;
// Retain installers and pin the DLL if a worker cannot stop; destroying a joinable thread would terminate.
tf::Installer* installer = nullptr;
// Sprite and chat installers resolve independently and share the same release rules.
tf::SpriteInstaller* sprites = nullptr;
tf::DiskWriter disk;           // settings reads and saves and the font enumeration, off the render thread
tf::Settings settings;         // the live values (the defaults until the character's file is read)
tf::Panel panel;               // /truefont (no argument) shows or hides it
std::mutex fontsMutex;
std::shared_ptr<const tf::FontList> fonts;   // set by each fonts scan (the disk writer), swapped whole
tf::FolderFonts folderFonts;   // config\truefont\fonts\ (fontsdir.h): loaded and scanned on the disk writer only
std::atomic<bool> fontsScanning{false};   // Scan pending/running; cleared by the writer on every exit.
std::mutex fontScanMutex;
struct FontScanDone { bool rescan = false; tf::FontScan r; };
std::shared_ptr<FontScanDone> fontScanDone;   // the writer's finished scan, for the render thread
std::set<std::string> missingSaid;   // the missing fonts said in chat (once per name)
bool chatQuiet = false;        // the DEV self-check runs: chat lines go to the log only
bool retryRefusal = false;     // a login-status change retries a refusal once the settings are in
bool keepOriginalOomSaid = false;   // Keep Original put back for want of memory: said in chat once since load
#ifdef TF_DEV
tf::Selftest selftest(installer, sprites);   // DEV builds only
bool selftestActive() { return selftest.active(); }
bool selftestHoldsShape() { return selftest.holdsShape(); }
void selftestAbort(const std::string& why) { selftest.abort(why); }
#else
bool selftestActive() { return false; }
bool selftestHoldsShape() { return false; }
void selftestAbort(const std::string&) {}
#endif

using tf::fmt;

// /tfont diag: while set, the render thread's log lines go into the report instead (other threads' lines still go to the log).
std::string* logCapture = nullptr;
DWORD logCaptureThread = 0;
void logl(const char* level, const std::string& s) {
    if (!logStarted) return;
    try {
        if (logCapture && GetCurrentThreadId() == logCaptureThread) logCapture->append(tf::diagLine(level, s));
        else fileLog.write(level, s);
    } catch (const std::exception&) {}   // out of memory: the line is lost; an AV is not swallowed
}
void info(const std::string& s) { logl("info", s); }
void warn(const std::string& s) { logl("warn", s); }
void err(const std::string& s) { logl("error", s); }

void sendChat(const std::string& text, unsigned char color) {
    if (!core || !core->GetChatManager()) return;
    char buf[512];
    const std::string sjis = tf::chatBody(text, color, 480);   // Convert font names to the chat encoding.
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "\x1E\x51" "[" "\x1E\x06" "truefont" "\x1E\x51" "]" "\x1E\x01" " " "\x1E" "%c%s" "\x1E\x01", color, sjis.c_str());
    core->GetChatManager()->AddChatMessage(1, false, buf);
}
// Allocation-free reporting for release failures, including out-of-memory errors.
void sendChatFixed(const char* text) {
    if (!core || !core->GetChatManager()) return;
    char buf[320];
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "\x1E\x51" "[" "\x1E\x06" "truefont" "\x1E\x51" "]" "\x1E\x01" " " "\x1E\x44" "%s" "\x1E\x01", text);
    core->GetChatManager()->AddChatMessage(1, false, buf);
}
// Defer Initialize messages to Present to avoid racing the chat store.
bool holdChat = false;
std::vector<std::pair<std::string, unsigned char>> heldChat;
void flushHeldChat() {
    holdChat = false;
    const auto lines = std::move(heldChat);
    heldChat.clear();
    for (const auto& l : lines) sendChat(l.first, l.second);
}
// One short line per command or outcome: info 0x6A, warn 0x68, fail 0x44.
void chat(const std::string& text, unsigned char color = 0x6A) {
#ifdef TF_DEV
    logl(color == 0x44 ? "error" : color == 0x68 ? "warn" : "info", std::string(chatQuiet ? "chat (quiet: selftest): " : "chat: ") + tf::plainText(text));
    if (chatQuiet) return;
#else
    logl(color == 0x44 ? "error" : color == 0x68 ? "warn" : "info", "chat: " + tf::plainText(text));   // chatQuiet is the DEV self-check's alone
#endif
    if (holdChat) {
        try { heldChat.emplace_back(text, color); } catch (const std::exception&) {}
        return;
    }
    sendChat(text, color);
}
void chatSink(const std::string& text, unsigned char color) { chat(text, color); }
void usage() { chat(tf::kLoadUsage); }
std::string logPathText() { return plog::underRoot(root, fileLog.path()) + (fileLog.atStartupFile() ? ", which moves into your character's log at login" : ""); }
void anchor() {}   // an address inside this DLL (ashitaRoot, ownImageStamp)
uint32_t buildStamp() { return plog::ownImageStamp(reinterpret_cast<const void*>(&anchor)); }   // this DLL's PE TimeDateStamp

// Distinguish known unsupported builds from newer or unrecognized client builds by PE timestamp.
bool clientUnknown() { return clientStamp && !tf::knownBuild(clientStamp); }
std::string clientVersionText() {
    return fmt("this game version (FFXiMain %08X) is %s TrueFont %.1f knows", clientStamp, tf::newerThanKnown(clientStamp) ? "newer than" : "not one", kVersion);
}
// `part`: "TrueFont", "Chat and menus" or "the menu and HUD fonts"; `plural`: "stay" rather than "stays".
std::string partOffText(const char* part, bool plural, const std::string& why) {
    if (clientUnknown()) return clientVersionText() + ", so " + part + (plural ? " stay" : " stays") + " off";
    std::string p = part;
    return p + (plural ? " are" : " is") + " not supported on this game version" +
           (clientStamp ? fmt(" (FFXiMain %08X, %s%s%s)", clientStamp, tf::knownBuild(clientStamp) ? tf::knownBuild(clientStamp) : "unknown", why.empty() ? "" : ": ", why.c_str())
                        : why.empty() ? std::string() : " (" + why + ")");
}
std::string capFirst(std::string s) {
    if (!s.empty()) s[0] = char(toupper(static_cast<unsigned char>(s[0])));
    return s;
}

void pinSelf() {
    if (pinned) return;
    HMODULE self = nullptr;
    pinned = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCSTR>(&anchor), &self) && self;
    if (pinned) warn("the DLL is pinned: it stays mapped until the game closes");
    else err(fmt("could not pin the DLL (error %lu)", GetLastError()));
}

HWND gameHwnd() { return core && core->GetProperties() ? core->GetProperties()->GetFinalFantasyHwnd() : nullptr; }
uint16_t zoneId() {
    try {
        IMemoryManager* m = core ? core->GetMemoryManager() : nullptr;
        IParty* p = m ? m->GetParty() : nullptr;
        return p ? p->GetMemberZone(0) : 0;
    } catch (const std::exception&) { return 0; }
}
bool isZoning() {
    try {
        IMemoryManager* m = core ? core->GetMemoryManager() : nullptr;
        IPlayer* p = m ? m->GetPlayer() : nullptr;
        return p && p->GetIsZoning() != 0;
    } catch (const std::exception&) { return false; }
}
bool inGame() {
    try {
        IMemoryManager* m = core ? core->GetMemoryManager() : nullptr;
        IPlayer* p = m ? m->GetPlayer() : nullptr;
        return p && p->GetLoginStatus() == 2;
    } catch (const std::exception&) { return false; }
}

// Log installer ticks exceeding 1 ms, using each 10 s window's maximum.
// Panel timings are reported when it closes.
struct FrameCosts {
    uint64_t windowStart = 0;
    double chatMax = 0, spriteMax = 0;
    uint64_t panelFrames = 0;
    double panelSum = 0, panelMax = 0;
    void add(uint64_t nowMs, double chatMs, double spriteMs, bool panelWasOpen, double panelMs, bool panelOpen) {
        if (!windowStart) windowStart = nowMs;
        chatMax = (std::max)(chatMax, chatMs);
        spriteMax = (std::max)(spriteMax, spriteMs);
        if (nowMs - windowStart >= 10000) {
            if (chatMax > 1.0 || spriteMax > 1.0)
                info(fmt("frame: TrueFont's longest work on one frame in the last %llu s: Chat/Items %.2f ms, the menu and HUD fonts %.2f ms (QPC; said when above 1 ms)",
                         static_cast<unsigned long long>((nowMs - windowStart) / 1000), chatMax, spriteMax));
            windowStart = nowMs;
            chatMax = spriteMax = 0;
        }
        if (panelWasOpen) {
            ++panelFrames;
            panelSum += panelMs;
            panelMax = (std::max)(panelMax, panelMs);
        }
        if (!panelOpen && panelFrames) {
            info(fmt("panel: closed after %llu frames: %.0f us a frame on average, the longest %.0f us (QPC, the panel's own drawing)", static_cast<unsigned long long>(panelFrames),
                     1000.0 * panelSum / double(panelFrames), 1000.0 * panelMax));
            panelFrames = 0;
            panelSum = panelMax = 0;
        }
    }
};
FrameCosts frameCosts;

// A resolve summary line: logged, and kept for /tfont diag.
void resolveNote(void (*to)(const std::string&), const std::string& line) {
    resolveLines.push_back(line);
    to(line);
}
// Validate chat signatures, the SJIS decoder, character map and widths before creating the installer.
bool resolveChat(const uint8_t* image, size_t size, const HMODULE h, std::string& why, std::string& first) {
    const double t0 = tf::qpcMs();
    std::vector<std::string> lines;
    const bool all = tf::resolveAll(image, size, client.base, R, lines);
    resolveNote(info, fmt("resolve: FFXiMain base %08X size %08X stamp %08X, %.1f ms, %s", unsigned(client.base), unsigned(client.size), plog::imageStamp(h), tf::qpcMs() - t0,
                          all ? (R.infos.empty() ? "everything resolved" : "everything required resolved (a supporting check did not: log only)") : "NOT everything required resolved"));
    for (const auto& l : lines)
        (l.find("NOTE") != std::string::npos || ((l.find("MISSING") != std::string::npos || l.find("UNRESOLVED") != std::string::npos) && l.find("(supporting)") == std::string::npos) ? warn : info)(l);
    if (!all) {
        why = "a game address did not resolve or a cross-check disagreed";
        first = R.failure;
        return false;
    }
    // The SJIS map port against the client's own function, for every byte pair (pure code: no state, no allocation).
    const auto live = reinterpret_cast<int(__cdecl*)(const uint8_t*)>(client.base + R.sjisMap);
    tf::MapCheck mc;
    const bool called = tf::seh([&] { mc = tf::checkSjisMap([&](const uint8_t* p) { return live(p); }); });
    if (!called || mc.mismatches) {
        why = called ? fmt("the SJIS map port differs from the client's own for %u of %u byte pairs (first %04X)", mc.mismatches, mc.checked, mc.first)
                     : std::string("calling the client's SJIS map faulted");
        return false;
    }
    info(fmt("charmap: the SJIS map port matches the client's own function for all %u byte pairs", mc.checked));
    auto map = std::make_shared<tf::CharMap>();
    std::string mapWhy;
    if (!map->build(&mapWhy)) { why = "the character map could not be built (" + mapWhy + ")"; return false; }
    info(fmt("charmap: %d cells mapped (%d glyphs reached by more than one pair)", map->mapped(), map->ambiguous()));
    std::vector<int8_t> widths(tf::kWidthTableLen);
    if (!tf::readRaw(client.base + R.widthTable, widths.data(), widths.size())) { why = "the width table could not be read"; return false; }
    wchar_t modulePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(h, modulePath, MAX_PATH * 2);
    installer = new tf::Installer();
    installer->setClient(client, R, std::move(map), std::move(widths), tf::datPathFor(modulePath));
    installer->setDevice(device);
    return true;   // it turns on once the character's settings say so (syncEnabled)
}
// Resolve sprites independently, reusing chat vtable votes when available.
bool resolveSpriteFonts(const uint8_t* image, size_t size, std::string& why, std::string& first) {
    const double t0 = tf::qpcMs();
    tf::SpriteResolved SR;
    std::vector<std::string> lines;
    const bool ok = tf::resolveSprites(image, size, client.base, R, SR, lines);
    resolveNote(info, fmt("resolve: the menu and HUD fonts (the sprite-font signatures): %.1f ms, %s", tf::qpcMs() - t0,
                          ok ? (SR.infos.empty() ? "everything resolved" : "everything required resolved (a supporting check did not: log only)") : "NOT everything required resolved"));
    for (const auto& l : lines)
        (l.find("NOTE") != std::string::npos || ((l.find("MISSING") != std::string::npos || l.find("UNRESOLVED") != std::string::npos) && l.find("(supporting)") == std::string::npos) ? warn : info)(l);
    // The Chat and menus records must carry CYyTex's exact vtable once YC and YD agree on it.
    if (installer) installer->setRecordVtable(SR.texVtable);
    if (!ok) { why = "a menu or HUD font address did not resolve or a cross-check disagreed"; first = SR.failure; return false; }
    sprites = new tf::SpriteInstaller();
    sprites->setClient(client, SR, R.renderer);
    sprites->setDevice(device);
    return true;
}
// Resolve addresses by signature; success requires at least one installer.
bool resolveClient(std::string& why) {
    const HMODULE h = GetModuleHandleA("FFXiMain.dll");
    MODULEINFO mi{};
    if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof mi)) { why = "FFXiMain.dll is not loaded"; return false; }
    client.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    client.size = mi.SizeOfImage;
    std::vector<uint8_t> image(client.size);
    size_t readable = 0;
    for (size_t off = 0; off < image.size(); off += 0x1000) {
        const size_t n = (std::min<size_t>)(0x1000, image.size() - off);
        if (tf::readRaw(client.base + off, image.data() + off, n)) readable += n;
    }
    if (!readable) { why = "the client image could not be read"; return false; }
    clientStamp = plog::imageStamp(h);
    const char* known = tf::knownBuild(clientStamp);
    resolveNote(info, fmt("resolve: FFXiMain build %08X: ", clientStamp) +
         (known ? std::string(known) + (tf::knownBuildLive(clientStamp) ? ", a build TrueFont knows (run live)" : ", a build TrueFont knows (resolved offline only)")
                : fmt("not a build TrueFont %.1f knows (%s its newest, %08X)", kVersion, tf::newerThanKnown(clientStamp) ? "newer than" : "not", tf::kKnownBuilds[0].stamp)));
    std::string chatWhy, sWhy, chatFirst, spriteFirst;
    const bool chatOk = resolveChat(image.data(), image.size(), h, chatWhy, chatFirst);
    if (!chatOk) {
        resolveWhy = chatWhy;
        resolveNote(err, "resolve: Chat/Items stays off: " + chatWhy + (chatFirst.empty() ? std::string() : "; the first failing check: " + chatFirst));
    }
    const bool spriteOk = resolveSpriteFonts(image.data(), image.size(), sWhy, spriteFirst);
    {   // Sharpness Auto's sizes (screen.h): supporting; without them Auto is 2x
        std::vector<std::string> lines;
        screenR = tf::resolveScreen(image.data(), image.size(), client.base, R.deviceSlot, lines);
        for (const auto& l : lines) (l.find("NOT RESOLVED") != std::string::npos ? warn : info)(l);
    }
    {   // Aspect and Size's three sites (shape.h): nothing is written here, only on the render thread once the settings ask
        std::vector<std::string> lines;
        shapeR = tf::resolveShape(image.data(), image.size(), client.base, screenR.appSlot, lines);
        for (const auto& l : lines)
            ((l.find(": stock") != std::string::npos || l.find("the constant 0.00234375 at") != std::string::npos) && l.find("NOT RESOLVED") == std::string::npos ? info : warn)(l);
        shape.setup(shapeR, &shapeMem, [](const char* level, const std::string& line) { logl(level, line); });
    }
    if (!spriteOk) resolveNote(err, "resolve: the menu and HUD fonts stay off: " + sWhy + (spriteFirst.empty() ? std::string() : "; the first failing check: " + spriteFirst));
    if (!chatOk && !spriteOk) {
        why = chatWhy;
        chat(partOffText("TrueFont", false, chatWhy) + " (see " + logPathText() + ").", 0x44);
        return false;
    }
    if (!chatOk) chat(partOffText("Chat/Items", false, chatWhy) + "; the menu and HUD fonts still work (see " + logPathText() + ").", 0x68);
    else if (!spriteOk) chat(partOffText("the menu and HUD fonts", true, sWhy) + "; Chat/Items still works (see " + logPathText() + ").", 0x68);
    return true;
}

// The font list: scanned on the disk writer at load, before any settings read (the writer is FIFO), and at each Rescan.
std::shared_ptr<const tf::FontList> fontList() {
    std::lock_guard<std::mutex> lock(fontsMutex);
    return fonts;
}
// Follow the family chain, falling back to Segoe UI when missing. Style remains local to the group.
// Before enumeration, use the saved name; first-font sentinels resolve when the list arrives.
std::wstring groupFamily(tf::Group g, bool& missing) {
    const tf::Group end = settings.followEnd(g);
    const std::string& f = settings.g[int(end)].font;
    missing = false;
    const auto list = fontList();
    if (f.empty()) return list && !list->empty() ? list->first() : tf::fromUtf8(tf::kDefaultFont);
    const std::wstring want = tf::fromUtf8(f);
    if (!list || list->empty()) return want;
    const std::wstring found = list->find(want);
    if (!found.empty()) return found;
    missing = end == g;
    const std::wstring fallback = list->find(tf::fromUtf8(tf::kDefaultFont));
    return fallback.empty() ? tf::fromUtf8(tf::kDefaultFont) : fallback;
}
std::wstring effectiveFamily(bool& missing) { return groupFamily(tf::Group::Chat, missing); }
// The family Chat and menus' kana and kanji draw with: the Japanese Font when installed, else Chat and menus' own.
std::wstring jpFamily(bool& missing) {
    missing = false;
    bool m = false;
    const std::wstring latin = effectiveFamily(m);
    if (settings.jpSame() || settings.jpOff()) return latin;
    const std::wstring want = tf::fromUtf8(settings.chatJp);
    const auto list = fontList();
    if (!list || list->empty()) return want;
    const std::wstring found = list->find(want);
    if (!found.empty()) return found;
    missing = true;
    return latin;
}
constexpr const char* kJpGroups = "Chat/Items' kana and kanji";
// A group's own font is drawn: a group that is on ends its chain there, even while the group itself is off.
bool fontDrawn(tf::Group g) { return settings.ownFontDrawn(g); }
// A family as the player reads it: when not even the fallback is installed, GDI picks the face.
std::string familyText(const std::wstring& f) {
    const auto list = fontList();
    return list && !list->empty() && list->find(f).empty() ? std::string("the system's default font") : tf::toUtf8(f);
}

struct LoadedSettings { uint64_t seq = 0; bool exists = true, failed = false, share = false, sharedMissing = false; tf::Settings set; };
std::mutex loadedMutex;
std::shared_ptr<LoadedSettings> loadedReady;
uint64_t loadSeq = 0, appliedSeq = 0;
bool settingsReadFailed = false;
bool identityPending = false;   // the character left the game: a save is held until identityTick knows who is in game
// With packet identity, use explicit logout or a 60 s absence; mid-game loads use a 10 s absence.
// Zoning pauses the 10 s timer only. Saves remain held until identity is confirmed.
uint64_t notInGameSince = 0;    // GetTickCount64 when the login status last left "in game", or zoning was last seen (0: in game)
uint64_t leftGameAt = 0;        // GetTickCount64 when the login status last left "in game" (the log's absence length)
bool zoningSeen = false;        // the game said it was zoning during this absence (for the log)
uint64_t zoningAt = 0;          // GetTickCount64 when the game last said it was zoning during this absence (the log)
bool logoutSaid = false;        // "character: none (logged out)" was logged for this absence
volatile long zoneInPacket = 0; // the packet thread: a zone-in (0x00A) naming a character arrived
volatile long logoutPacket = 0; // the packet thread: a zone-out (0x00B) that is a logout arrived
bool packetKnown = false;       // render thread: the character in game came with a zone-in packet
bool logoutByPacket = false;    // render thread: a logout packet came; said as soon as the character is out of the game
int pendingEnabled = -1;        // /tfont on|off typed before the character's file was read
bool shareAll = false;          // Every character: this character's settings are the shared file's

std::string ownSettingsPath() {
    if (charName.empty()) return std::string();
    return root + "config\\" + kName + "\\" + plog::characterKey(charName, charServer) + "\\settings.ini";
}
std::string sharedSettingsPath() { return root + "config\\" + kName + "\\shared\\settings.ini"; }
std::string presetsDir() { return root + "config\\" + kName + "\\presets\\"; }
std::string settingsPath() {
    if (charName.empty()) return std::string();
    return shareAll ? sharedSettingsPath() : ownSettingsPath();
}
std::string settingsPathText() { return plog::underRoot(root, settingsPath()); }
bool settingsReady() { return !charName.empty() && !identityPending && loadSeq != 0 && loadSeq == appliedSeq; }
// Apply edits while the known character is absent; defer saving until that character returns.
bool heldForIdentity() { return identityPending && !charName.empty() && loadSeq != 0 && loadSeq == appliedSeq && !settingsReadFailed; }
bool heldSave = false;          // a save was held by heldForIdentity(): identityTick saves it for the same character
std::string notSavedWhy() {
    return heldSave ? "kept; saved when the character is back in game" : "applies for this session only (the settings file could not be read)";
}

// Write only changed keys. Baselines are keyed by path and maintained on the disk writer.
std::mutex iniBaseMutex;
std::map<std::string, tf::IniBase> iniBases;
std::map<std::string, tf::IniBase> iniImplied;   // per file, the values the loaded settings implied for the keys it lacked
tf::SaveNotice saveNotice;   // Writer-owned failure/recovery notice state.
void presetNotice(const std::string& text, unsigned char color);
std::string baseKey(std::string path) { for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c))); return path; }
void setIniBase(const std::string& path, tf::IniBase base, tf::IniBase implied = tf::IniBase()) {
    std::lock_guard<std::mutex> lock(iniBaseMutex);
    iniBases[baseKey(path)] = std::move(base);
    iniImplied[baseKey(path)] = std::move(implied);
}
// Write changed keys against the file baseline and accumulate the job's result.
void writeSettingsChanges(const std::string& path, const tf::IniSnapshot& values, const std::string& shown, tf::SaveOutcome& out) {
    DWORD error = 0;
    bool ok = true;
    {
        std::lock_guard<std::mutex> lock(iniBaseMutex);
        auto it = iniBases.find(baseKey(path));
        if (it == iniBases.end()) it = iniBases.emplace(baseKey(path), tf::readIniBase(path, values)).first;
        tf::IniBase& implied = iniImplied[baseKey(path)];
        tf::IniSnapshot want = values;
        tf::keepOthersWrites(path, want, it->second, implied);   // another window's newer writes stay
        ok = tf::writeChanged(path, want, it->second, error, nullptr, &implied);
    }
    out.add(ok, error, shown);
}
bool saveSettings() {
    if (!settingsReady() || settingsReadFailed) {
        if (heldForIdentity()) heldSave = true;
        return false;
    }
    heldSave = false;
    const std::string path = settingsPath(), own = ownSettingsPath(), shown = plog::underRoot(root, path), ownShown = plog::underRoot(root, own);
    // While Every Character is on, the TrueFont switch is saved in the character's own file, the rest in the shared one.
    const bool share = shareAll;
    const tf::IniSnapshot values = settings.snapshot(!share);
    tf::IniSnapshot enabled;
    if (share) enabled.push_back(tf::IniValue{tf::Settings::kSection, "enabled", settings.enabled ? "1" : "0"});
    const auto job = [path, own, shown, ownShown, values, enabled] {
        tf::SaveOutcome out;   // one outcome for the job's files: one chat line at most
        writeSettingsChanges(path, values, shown, out);
        if (!enabled.empty()) writeSettingsChanges(own, enabled, ownShown, out);
        unsigned char color = 0x6A;
        const std::string line = saveNotice.next(out, color);
        if (!line.empty()) presetNotice(line, color);
    };
    if (!disk.runJob(job)) info("settings: not saved: the disk writer is not running");
    return true;
}

tf::SpriteOptions spriteOptions() {
    tf::SpriteOptions o = tf::SpriteOptions::defaults();
    for (int g = int(tf::Group::Labels); g < tf::kGroupCount; g++) {
        bool missing = false;
        o.style[g] = settings.style(tf::Group(g), groupFamily(tf::Group(g), missing));
        o.on[g] = settings.g[g].on;   // Exclude Chat so toggling it does not rebuild sprites.
    }
    settings.spriteQuality(o);   // Hinting, Compress, Redraw "Miss!", each group's Sharpness
    o.autoKPlate = autoKPlate.k;     // Sharpness Auto's k for Nameplates and Damage numbers (screen.h), each its own
    o.autoKDamage = autoKDamage.k;
    return o;
}

// Warn once per missing family, only when an enabled group actually uses it.
void sayMissingFonts() {
    bool missing = false;
    const std::wstring family = effectiveFamily(missing);
    std::vector<tf::MissingFont> news;
    for (const tf::Group grp : tf::kGroupOrder) {   // in tab order
        const int g = int(grp);
        bool gm = false;
        const std::wstring used = groupFamily(tf::Group(g), gm);
        const std::string& name = settings.g[g].font;
        if (!tf::sayMissingFont(settings, tf::Group(g), gm, missingSaid.count(name) != 0)) continue;
        const std::string groups = settings.drawnWithText(tf::Group(g));
        warn("font: \"" + name + "\" (the own font of " + tf::groupName(tf::Group(g)) + ") is not installed; " + groups + " " + tf::useVerb(groups) + " " + tf::toUtf8(used));
        const std::string usedText = familyText(used);
        auto it = std::find_if(news.begin(), news.end(), [&](const tf::MissingFont& m) { return m.name == name && m.used == usedText; });
        if (it == news.end()) news.push_back(tf::MissingFont{name, groups, usedText});
        else it->groups += ", " + groups;
    }
    bool jpMissing = false;
    jpFamily(jpMissing);
    if (tf::sayMissingJp(settings, jpMissing, missingSaid.count(settings.chatJp) != 0)) {
        warn("font: \"" + settings.chatJp + "\" (the Japanese font of Chat/Items) is not installed; its kana and kanji use " + tf::toUtf8(family));
        const std::string usedText = familyText(family);
        auto it = std::find_if(news.begin(), news.end(), [&](const tf::MissingFont& m) { return m.name == settings.chatJp && m.used == usedText; });
        if (it == news.end()) news.push_back(tf::MissingFont{settings.chatJp, kJpGroups, usedText});
        else it->groups += std::string(", ") + kJpGroups;
    }
    for (const auto& m : news) missingSaid.insert(m.name);
    if (!news.empty()) chat(tf::missingFontsText(news), 0x68);
}

// Push options and debounce rebuilds. Revert Keep Original on allocation failure only for an explicit
// panel toggle; loads, presets and resets retain the saved preference.
bool pushOptions(bool* chatScheduled = nullptr, bool* spriteScheduled = nullptr, bool keepTicked = false) {   // true when a rebuild was scheduled
    if (!installer && !sprites) return false;
    sayMissingFonts();
    bool missing = false, jpMissing = false;
    const std::wstring family = effectiveFamily(missing), jp = jpFamily(jpMissing);
    const bool chatNow = installer && installer->setOptions(settings.options(family, jp));
    // Keep Original set while Chat and menus goes off in the same change (a Reset): nothing copied now.
    if (installer && !installer->setKeepOriginal(settings.keepOriginal, settings.enabled && settings.chat().on)) {   // out of memory for the copy: nothing half-applied
        if (keepTicked) {
            settings.keepOriginal = false;   // Persist the reverted checkbox.
            info("settings (panel): Keep Original is put back off: there was not enough memory to copy the game's font");
        } else info("settings: Keep Original stays on in the settings but keeps nothing for now: there was not enough memory to copy the game's font");
        if (!keepOriginalOomSaid) {
            keepOriginalOomSaid = true;
            chat(keepTicked ? "Keep Original stays off: there was not enough memory to copy the game's font."
                            : "Keep Original keeps nothing for now: there was not enough memory to copy the game's font (the setting stays on).",
                 0x68);
        }
    }
    const bool spriteNow = sprites && sprites->setOptions(spriteOptions());
    if (chatScheduled) *chatScheduled = chatNow;
    if (spriteScheduled) *spriteScheduled = spriteNow;
    return chatNow || spriteNow;
}
// Mark explicit Aspect/Size requests for one chat warning on conflict; automatic changes only log.
void markShapeAsked(const tf::Settings& was, const tf::Settings& now) {
    for (const tf::Group grp : {tf::Group::Nameplates, tf::Group::Damage}) {
        const tf::ShapeWant a = tf::shapeWantOf(was, grp), b = tf::shapeWantOf(now, grp);
        if (b.active() && (!a.active() || a.aspect != b.aspect || a.size != b.size)) shapeAsked[int(tf::shapeGroupOf(grp))] = true;
    }
}
bool applySettings(const tf::Settings& next, const char* why, bool* chatScheduled = nullptr, bool* spriteScheduled = nullptr) {
    const tf::Settings was = settings;
    settings = next;
    settings.clamp();
    if (!settings.followBroken.empty()) {   // The panel prevents cycles; manual settings may contain them.
        warn(std::string("settings (") + why + "): Same Font As looped, so the group that closed each loop follows nothing now: " + settings.followBroken);
        settings.followBroken.clear();
    }
    if (was != settings) info(std::string("settings (") + why + "): " + settings.text());
    if (std::strcmp(why, "loaded") != 0 && std::strncmp(why, "defaults", 8) != 0) markShapeAsked(was, settings);
    return pushOptions(chatScheduled, spriteScheduled, std::strcmp(why, "panel") == 0 && !was.keepOriginal && settings.keepOriginal);
}

// Rebuild Auto nameplate/damage composites after k stays stable for 3 s. Include active Aspect/Size.
// Unreadable screen dimensions fall back to 2x.
void screenTick(uint64_t now) {
    // Keep the player's Auto scale during selftest; temporary native geometry must not trigger a rebuild.
    if (!sprites || selftestHoldsShape() || !autoKPlate.due(now)) return;
    const tf::ScreenSizes sz = tf::readScreen(client.base, screenR, gameHwnd());
    if (!sz.ok()) {
        if (!screenSaid) {
            screenSaid = true;
            warn(std::string("screen: the window's sizes cannot be read (") + (screenR.ok() ? "the app object is not set or its sizes are not sizes" : "the app getters did not resolve") +
                 "); Sharpness Auto for Nameplates and Damage numbers stays 2x");
        }
        return;
    }
    const double rho = tf::shapeRho(sz);
    double wf[tf::kShapeGroupCount] = {1.0, 1.0}, sc[tf::kShapeGroupCount] = {1.0, 1.0};
    for (int g = 0; g < tf::kShapeGroupCount; g++)
        if (tf::autoKTakesShape(shapeWant[g], shape.inEffect(tf::ShapeGroup(g)))) {
            wf[g] = tf::aspectFactor(shapeWant[g].aspect, rho);
            sc[g] = shapeWant[g].size / 100.0;
        }
    const int wasP = autoKPlate.k, wasD = autoKDamage.k;
    const bool first = !autoKPlate.seen;
    const bool cP = autoKPlate.offer(sz, now, wf[0], sc[0]), cD = autoKDamage.offer(sz, now, wf[1], sc[1]);
    if (!cP && !cD && !first) return;
    info(fmt("screen: Sharpness Auto: Nameplates k %d (was %d; width x%.3f, size x%.2f), Damage numbers k %d (was %d; width x%.3f, size x%.2f) (%s) from %s", autoKPlate.k, wasP,
             wf[0], sc[0], autoKDamage.k, wasD, wf[1], sc[1], first ? "first read" : "held 3 s", sz.text().c_str()));
    if ((autoKPlate.k != wasP || autoKDamage.k != wasD) && settingsReady()) pushOptions();
}

// Poll sites and window ratio each second, or immediately on viewport changes, then apply saved Aspect/Size.
// Retain the last values while character settings are unavailable; never write before the first load.
bool showingOriginalNow() { return (installer && installer->showingOriginal()) || (sprites && sprites->showingOriginal()); }
void shapeTick(uint64_t now) {
    if (!shape.ready() || (!installer && !sprites)) return;
    const bool recheck = now >= shapeNextPoll;
    if (recheck) shapeNextPoll = now + tf::AutoKMonitor::kPollMs;
    uint32_t vp = 0;
    if (screenR.ok()) {
        const uintptr_t app = tf::rd<uint32_t>(client.base + screenR.appSlot);
        if (app >= 0x10000) vp = tf::rd<uint32_t>(app + tf::kAppViewport);
    }
    if (recheck || vp != shapeLastVp) {
        shapeLastVp = vp;
        const double rho = tf::shapeRho(tf::readScreen(client.base, screenR, gameHwnd()));
        if (rho > 0 && std::fabs(rho - shapeRhoNow) > 1e-9) {
            if (recheck) info(fmt("shape: the window's H/W is %.4f (was %.4f)", rho, shapeRhoNow));
            shapeRhoNow = rho;
        }
    }
    if (settingsReady()) {
        shapeWantKnown = true;
        for (const tf::Group grp : {tf::Group::Nameplates, tf::Group::Damage}) shapeWant[int(tf::shapeGroupOf(grp))] = tf::shapeWantOf(settings, grp);
    }
    if (!shapeWantKnown) {
        for (auto& w : shapeWant) w = tf::ShapeWant{};
        std::fill(std::begin(shapeAsked), std::end(shapeAsked), false);
    }
#ifdef TF_DEV
    // Use native geometry for identity shots; retain the player's requested values for Auto and the panel.
    const bool hold = selftestHoldsShape();
    if (hold != shapeHeld) {
        shapeHeld = hold;
        info(tf::shapeHoldLine(hold, shapeWant));
    }
    tf::ShapeWant sent[tf::kShapeGroupCount];
    for (int g = 0; g < tf::kShapeGroupCount; g++) sent[g] = tf::shapeWantForRun(shapeWant[g], hold);
    shape.sync(sent, shapeRhoNow, showingOriginalNow(), recheck);
#else
    shape.sync(shapeWant, shapeRhoNow, showingOriginalNow(), recheck);
#endif
    for (int g = 0; g < tf::kShapeGroupCount; g++) {
        const bool shapeRefused = shape.takeRefusal(tf::ShapeGroup(g));
        if (shapeRefused && shapeAsked[g] && !shapeChatSaid && shape.groupState(tf::ShapeGroup(g)) == tf::ShapePatch::GroupState::Taken) {
            shapeChatSaid = true;
            chat(tf::shapeTakenChat(tf::ShapeGroup(g), tf::nameplateLoaded()), 0x68);   // the plugin is named only while it is loaded
        }
        shapeAsked[g] = false;
    }
}

tf::OffReport mergeReports(tf::OffReport a, const tf::OffReport& b) {
    a.wasOn = a.wasOn || b.wasOn;
    a.gameGone = a.gameGone || b.gameGone;
    a.left.insert(a.left.end(), b.left.begin(), b.left.end());
    return a;
}
bool anyEnabled() { return (installer && installer->enabled()) || (sprites && sprites->enabled()); }

// Propagate Show Original when enabling either installer. announce repeats refusals for typed commands.
void chatTurnOn(bool announce = false) {
    installer->turnOn(announce);
    if (sprites && sprites->showingOriginal()) {
        std::string why;
        installer->setShowOriginal(true, why);
    }
}
void spritesTurnOn(bool announce = false) {
    sprites->turnOn(announce);
    if (installer && installer->showingOriginal()) {
        std::string why;
        if (!sprites->setShowOriginal(true, why)) info("sprites: Show Original not passed on: " + why);
    }
}

// Follow loaded settings independently, preserving automatic-off and refusal latches.
void syncEnabled() {
    if ((!installer && !sprites) || !settingsReady()) return;
    const bool retry = retryRefusal;
    retryRefusal = false;
    tf::OffReport rep;
    if (installer && !selftestActive()) {   // Selftest controls the installer while active.
        const bool chatOn = settings.chat().on;
        if (retry && settings.enabled && chatOn && installer->failureRetriableAtLogin() && !installer->autoOff()) {
            info("login changed: the earlier refusal is retried: " + installer->failure());
            installer->clearLatches();
        }
        if (settings.enabled && chatOn && !installer->enabled() && !installer->autoOff()) chatTurnOn();
        else if (!settings.enabled && installer->enabled()) rep = installer->turnOff("this character's settings say off");
        else if (!chatOn && installer->enabled()) {   // quiet (the panel shows it); leftovers to the log
            const tf::OffReport r = installer->turnOff("Chat/Items is switched off");
            if (!r.clean()) warn("Chat/Items switched off, but these were left: " + r.leftText());
        }
        if (!chatOn && !installer->enabled()) installer->freeOriginal();   // Release cached readback while Chat is disabled.
    }
    if (sprites && !selftestActive()) {
        if (retry && settings.enabled && sprites->failureRetriableAtLogin() && !sprites->autoOff()) {
            info("login changed: the menu and HUD fonts' earlier refusal is retried: " + sprites->failure());
            sprites->clearLatches();
        }
        if (settings.enabled && !sprites->enabled() && !sprites->autoOff()) spritesTurnOn();
        else if (!settings.enabled && sprites->enabled()) rep = mergeReports(rep, sprites->turnOff("this character's settings say off"));
    }
    if (rep.wasOn) chat(rep.clean() ? "off (this character's settings say off). /tfont on turns it on." : "off, but these were left: " + rep.leftText() + ". See the log.",
                        rep.clean() ? 0x6A : 0x68);
}

// Re-enabling the Chat group retries its installer, like the master switch.
void chatGroupChanged(const tf::Settings& was) {
    if (!installer || selftestActive() || !settingsReady() || !settings.enabled || was.chat().on || !settings.chat().on) return;
    info("Chat/Items switched on: it is built and put in place again");
    chatTurnOn();
}

void loadSettingsForCharacter() {
    const uint64_t seq = ++loadSeq;
    const std::string path = ownSettingsPath(), shared = sharedSettingsPath(), rootCopy = root;
    std::shared_ptr<LoadedSettings> r;
    try { r = std::make_shared<LoadedSettings>(); } catch (const std::exception&) { r = nullptr; }
    if (!r) {
        settingsReadFailed = true;
        appliedSeq = seq;
        applySettings(tf::Settings{}, "defaults: the settings could not be read");
        err("settings: could not be read (out of memory); the defaults are in use and nothing is saved for this character");
        chat("could not read the settings (out of memory); the defaults are in use and nothing is saved for this character.", 0x44);
        return;
    }
    r->seq = seq;
    const auto read = [r, path, shared, rootCopy]() {
        saveNotice = tf::SaveNotice{};   // Do not carry recovery notices across characters.
        try {
            plog::ensureDirs(rootCopy, path);
            if (tf::readShareAll(path)) plog::ensureDirs(rootCopy, shared);
            const tf::CharacterRead cr = tf::readCharacter(path, shared, r->set);
            r->exists = cr.exists;
            r->share = cr.share;
            r->sharedMissing = cr.sharedMissing;
            const tf::IniSnapshot keys = tf::Settings{}.snapshot();   // Include legacy key removals.
            tf::IniBase own = tf::readIniBase(path, keys);
            const bool fromShared = cr.share && !cr.sharedMissing;   // the file the settings were read from implies its missing keys
            tf::IniBase ownImplied = fromShared ? tf::IniBase() : tf::impliedValues(r->set.snapshot(), own);
            setIniBase(path, std::move(own), std::move(ownImplied));
            if (cr.share) {
                tf::IniBase sh = tf::readIniBase(shared, keys);
                tf::IniBase shImplied = fromShared ? tf::impliedValues(r->set.snapshot(false), sh) : tf::IniBase();
                setIniBase(shared, std::move(sh), std::move(shImplied));
            }
        } catch (const std::exception&) {
            r->failed = true;
        }
        std::lock_guard<std::mutex> lock(loadedMutex);
        loadedReady = r;
    };
    if (!disk.runJob(read)) read();
}
void applySettingsWhenReady() {
    std::shared_ptr<LoadedSettings> r;
    {
        std::lock_guard<std::mutex> lock(loadedMutex);
        r = std::move(loadedReady);
    }
    if (!r || r->seq != loadSeq) return;
    settingsReadFailed = r->failed;
    appliedSeq = r->seq;
    shareAll = !r->failed && r->share;
    tf::Settings next = r->failed ? tf::Settings{} : r->set;
    const bool override = pendingEnabled >= 0 && (pendingEnabled != 0) != next.enabled;
    if (pendingEnabled >= 0) {
        if (override) info(fmt("settings: /tfont %s was typed before the file was read; it replaces the file's enabled=%d", pendingEnabled ? "on" : "off", next.enabled));
        next.enabled = pendingEnabled != 0;
        pendingEnabled = -1;
    }
    applySettings(next, r->failed ? "defaults: the settings file could not be read" : "loaded");
    if (r->failed) {
        err("settings: " + settingsPathText() + " could not be read; the defaults are in use and nothing is saved for this character");
        chat("could not read the settings (" + settingsPathText() + "); the defaults are in use and nothing is saved for this character.", 0x44);
        return;
    }
    if (r->sharedMissing) info("settings: Every Character is on but " + settingsPathText() + " does not exist: this character's own settings, written there now");
    else if (r->exists) info("settings: " + settingsPathText() + (shareAll ? " (Every Character)" : "") + ": " + settings.text());
    else info("settings: " + settingsPathText() + " does not exist yet: the defaults, written there now");
    if (r->set.migratedFromV1) info("settings: an older settings file: its keys are the Chat/Items group; saved again with the per-group keys");
    if (r->set.migratedJp) info("settings: an older Scripts / Japanese font pair is the Japanese font now; saved again with chat_jp");
    if (r->set.migratedSame) info("settings: an older Same as chat / Same as menu headings is Same Font As now; saved again with <group>_same and <group>_follow");
    if (r->set.migratedSplit)
        info("settings: a file from before Nameplates and Compass were their own groups: they take HUD text's and Menu headings' settings and Same Font As them (nothing looks "
             "different); saved again with their keys");
    if (r->set.migratedCells) info(fmt("settings: an older Quality Cells is Chat/Items' Sharpness now (%s); saved again with chat_sharp", tf::sharpKey(r->set.chat().sharp)));
    if (r->exists && r->set.olderFile) info("settings: a file from before the 2026-09-25 defaults: a key it lacks keeps its old default (saved with every key at the next change)");
    if (!r->exists) info("settings: a new character: every font group starts switched off, on the first font in the list (tick a group's TrueFont box in /truefont)");
    if (!r->exists || r->sharedMissing || override || r->set.migratedFromV1 || r->set.migratedJp || r->set.migratedSame || r->set.migratedSplit || r->set.migratedCells ||
        !r->set.followBroken.empty())
        saveSettings();   // Persist corrected links.
}

// /tfont on|off and the panel's box: saved; before the character's settings are read, kept and applied over them.
void rememberEnabled(bool on) {
    if (!settingsReady()) {
        pendingEnabled = on ? 1 : 0;
        settings.enabled = on;
        info(std::string("settings: /tfont ") + (on ? "on" : "off") + (heldForIdentity() ? " is kept until the character is back in game, then saved" : " is kept until the character's settings are read, then saved"));
        return;
    }
    if (settings.enabled == on) return;
    const tf::Settings was = settings;
    settings.enabled = on;
    markShapeAsked(was, settings);
    if (!saveSettings()) info(std::string("settings: /tfont ") + (on ? "on" : "off") + " " + notSavedWhy());
}

struct LoadedPreset { uint64_t seq = 0, character = 0; std::string name; bool ok = false; tf::Settings set; };
std::mutex presetMutex;                     // guards everything below it that the writer sets
std::vector<std::string> presetNames;       // the last list read
bool presetsKnown = false;
uint64_t presetsGen = 0;   // Completed list generation.
uint64_t presetsQueued = 0;   // Requested generation; writer jobs are FIFO.
std::shared_ptr<LoadedPreset> presetLoaded;
std::vector<std::pair<std::string, unsigned char>> presetNotices;   // a file that could not be written or read
uint64_t shareRevertCharacter = 0;   // Failed sharing copy's character generation; 0=none.
bool shareRevertTo = false;   // Sharing flag to restore.
uint64_t presetLoadSeq = 0;                 // the newest Load asked for (render thread)

void presetNotice(const std::string& text, unsigned char color) {   // the writer
    std::lock_guard<std::mutex> lock(presetMutex);
    if (presetNotices.size() < 16) presetNotices.emplace_back(text, color);
}
void queuePresetList() {
    const std::string dir = presetsDir();
    ++presetsQueued;
    if (!disk.runJob([dir] {
            std::vector<std::string> names;
            try { names = tf::listPresets(dir); } catch (const std::exception&) {}
            std::lock_guard<std::mutex> lock(presetMutex);
            presetNames = std::move(names);
            presetsKnown = true;
            ++presetsGen;
        }))
    {
        --presetsQueued;
        warn("presets: the disk writer is not running; the list is not read");
    }
}
void savePresetNow(const std::string& name) {
    const std::string dir = presetsDir(), rootCopy = root, shown = plog::underRoot(root, tf::presetPath(dir, name));
    const tf::Settings copy = settings;
    info("presets: save \"" + name + "\" to " + shown + ": " + copy.text());
    if (!disk.runJob([dir, rootCopy, name, copy, shown] {
            plog::ensureDirs(rootCopy, tf::presetPath(dir, name));
            DWORD error = 0;
            if (!tf::savePreset(dir, name, copy, error)) presetNotice("could not save the preset '" + name + "' to " + shown + " (error " + std::to_string(error) + ").", 0x44);
        }))
        chat("could not save the preset '" + name + "': the disk writer is not running.", 0x44);
    queuePresetList();
}
void deletePresetNow(const std::string& name) {
    const std::string dir = presetsDir(), shown = plog::underRoot(root, tf::presetPath(dir, name));
    info("presets: delete \"" + name + "\" (" + shown + ")");
    if (!disk.runJob([dir, name, shown] {
            DWORD error = 0;
            if (!tf::deletePreset(dir, name, error) && error != ERROR_FILE_NOT_FOUND)
                presetNotice("could not delete the preset '" + name + "' (" + shown + ", error " + std::to_string(error) + ").", 0x44);
        }))
        chat("could not delete the preset '" + name + "': the disk writer is not running.", 0x44);
    queuePresetList();
}
void loadPresetNow(const std::string& name) {
    const std::string dir = presetsDir();
    auto r = std::make_shared<LoadedPreset>();
    r->seq = ++presetLoadSeq;
    r->character = loadSeq;
    r->name = name;
    info("presets: load \"" + name + "\"");
    if (!disk.runJob([dir, r] {
            try { r->ok = tf::loadPreset(dir, r->name, r->set); } catch (const std::exception&) { r->ok = false; }
            std::lock_guard<std::mutex> lock(presetMutex);
            presetLoaded = r;
        }))
        chat("could not read the preset '" + name + "': the disk writer is not running.", 0x68);
}
// A Load's file applied as a panel change is (TrueFont's on/off kept), once the same character's settings are in.
void applyPresetWhenReady() {
    std::shared_ptr<LoadedPreset> r;
    {
        std::lock_guard<std::mutex> lock(presetMutex);
        r = std::move(presetLoaded);
    }
    if (!r || r->seq != presetLoadSeq) return;
    if (r->character == loadSeq && heldForIdentity()) {   // Defer until this character returns.
        std::lock_guard<std::mutex> lock(presetMutex);
        if (!presetLoaded) presetLoaded = std::move(r);
        return;
    }
    if (r->character != loadSeq || !settingsReady()) { info("presets: \"" + r->name + "\" is not applied: the character changed"); return; }
    if (!r->ok) { chat("could not read the preset '" + r->name + "'; nothing changed.", 0x68); queuePresetList(); return; }
    selftestAbort("a preset was loaded");
    tf::Settings next = r->set;
    next.enabled = settings.enabled;
    const tf::Settings was = settings;
    applySettings(next, "preset");
    info("presets: \"" + r->name + "\" loaded: " + settings.text());
    if (!saveSettings()) info("settings: the preset " + notSavedWhy());
    chatGroupChanged(was);
}
// Every Character: the path switches now; the copy runs on the writer ahead of any save queued after it.
void setShareAll(bool on) {
    if (!settingsReady() || settingsReadFailed) {
        info(std::string("settings: Every Character is not changed: ") + (heldForIdentity() ? "the character is not in game" : "the character's settings are not read"));
        return;
    }
    if (on == shareAll) return;
    const std::string own = ownSettingsPath(), shared = sharedSettingsPath(), rootCopy = root;
    const std::string shown = plog::underRoot(root, on ? shared : own);
    const tf::Settings copy = settings;
    const uint64_t character = loadSeq;
    shareAll = on;
    info(std::string("settings: Every Character ") + (on ? "on: the settings are copied to " : "off: the shared settings are copied back to ") + shown + " and read and saved there");
    if (!disk.runJob([on, own, shared, rootCopy, copy, character, shown] {
            // Revert the sharing flag if copying fails, so future saves and the next login use the same file.
            std::string failed;
            bool ok = false;   // Do not revert a successful copy if baseline reads fail.
            try {
                plog::ensureDirs(rootCopy, on ? shared : own);
                DWORD error = 0;   // Preserve the write error before subsequent reads.
                ok = on ? tf::shareOn(own, shared, copy, &error) : tf::shareOff(own, copy, &error);
                const tf::IniSnapshot keys = tf::Settings{}.snapshot();
                setIniBase(own, tf::readIniBase(own, keys));
                setIniBase(shared, tf::readIniBase(shared, keys));
                if (ok) return;
                failed = shown + " could not be written (error " + std::to_string(error) + ")";
            } catch (const std::exception& e) {
                if (ok) {
                    // The copy succeeded. Drop stale baselines and reread them on the next save without reverting sharing.
                    {
                        std::lock_guard<std::mutex> lock(iniBaseMutex);
                        iniBases.erase(baseKey(own));
                        iniBases.erase(baseKey(shared));
                        iniImplied.erase(baseKey(own));
                        iniImplied.erase(baseKey(shared));
                    }
                    try { info(std::string("settings: Every Character is ") + (on ? "on" : "off") + "; its file bases are read again at the next save (" + e.what() + ")"); }
                    catch (const std::exception&) {}
                    return;
                }
                try { failed = std::string("the copy failed (") + e.what() + ")"; } catch (const std::exception&) {}
            }
            {
                std::lock_guard<std::mutex> lock(presetMutex);
                shareRevertCharacter = character;
                shareRevertTo = !on;
            }
            try {
                presetNotice(std::string("could not turn Every Character ") + (on ? "on" : "off") + ": " + (failed.empty() ? std::string("out of memory") : failed) + "; " +
                                 (on ? "this character keeps its own settings." : "this character keeps the shared settings."),
                             0x44);
            } catch (const std::exception&) {}
        })) {
        shareAll = !on;
        chat(std::string("could not turn Every Character ") + (on ? "on" : "off") + ": the disk writer is not running.", 0x44);
    }
}
void takePresetOutcomes() {
    std::vector<std::pair<std::string, unsigned char>> notes;
    uint64_t revertFor = 0;
    bool revertTo = false;
    {
        std::lock_guard<std::mutex> lock(presetMutex);
        notes.swap(presetNotices);
        revertFor = shareRevertCharacter;
        revertTo = shareRevertTo;
        shareRevertCharacter = 0;
    }
    for (const auto& n : notes) chat(n.first, n.second);
    if (revertFor && revertFor == loadSeq && shareAll != revertTo) {
        shareAll = revertTo;
        warn(std::string("settings: Every Character is back ") + (revertTo ? "on" : "off") + " (its copy failed)");
    }
}

std::string fontsDir() { return root + "config\\" + kName + "\\fonts\\"; }
std::wstring widen(const std::string& ansi) {   // the Ashita folder's path (GetModuleFileNameA)
    if (ansi.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), int(ansi.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), int(ansi.size()), w.data(), n);
    return w;
}
void scanFonts(bool rescan) {
    // Declare cleanup first so scanning clears only after the outcome is posted, including exceptional exits.
    struct Clear { bool rescan; ~Clear() { if (rescan) fontsScanning = false; } } clear{rescan};
    std::shared_ptr<FontScanDone> done;
    struct Post {
        std::shared_ptr<FontScanDone>& d;
        ~Post() {
            if (!d) return;
            std::lock_guard<std::mutex> lock(fontScanMutex);
            fontScanDone = std::move(d);
        }
    } post{done};
    done = std::make_shared<FontScanDone>();
    done->rescan = rescan;
    tf::FontScan& r = done->r;
    const std::string dir = fontsDir();
    try {
        plog::ensureDirs(root, dir);
        r = folderFonts.scan(widen(dir), tf::fontGate());
    } catch (const std::exception& e) {
        r = tf::FontScan{};
        r.why = e.what();
    }
    if (r.list) {   // Existing readers retain the previous list.
        std::lock_guard<std::mutex> lock(fontsMutex);
        fonts = r.list;
    }
    try {   // Post the outcome even if logging allocates unsuccessfully.
        for (const auto& w : r.warnings) warn(w);
        for (const auto& f : r.newFailures) warn("fonts: " + f + "; skipped");
        for (const auto& d : r.newDuplicates) warn("fonts: " + d);
        if (r.list) {
            info(fmt("fonts: %zu installed TrueType/OpenType families, %zu from the folder, %zu duplicates (%s: %zu of %zu font files loaded%s) in %llu ms", r.list->installedCount(),
                     r.list->folderCount(), r.list->duplicates, plog::underRoot(root, dir).c_str(), r.loaded, r.files, r.failed ? ", the others not" : "", r.ms) +
                 (r.waitMs >= 50 ? fmt(", %llu of them waiting for a font build to finish", r.waitMs) : std::string()));
            for (const auto& fam : r.list->folder) info("fonts: from the folder: " + tf::toUtf8(fam) + (r.list->isJapanese(fam) ? " (Japanese)" : ""));
        } else warn("fonts: " + std::string(rescan ? "the rescan" : "the scan") + (r.reloaded ? ": " : " changed nothing: ") + r.why + "; the previous font list stays");
    } catch (const std::exception&) {}
}
void rescanFonts() {
    if (fontsScanning) { chat("fonts: a rescan is already running."); return; }
    fontsScanning = true;
    info("fonts: rescan asked for");
    if (!disk.runJob([] { scanFonts(true); })) {
        fontsScanning = false;
        chat("fonts: cannot rescan: the disk writer is not running.", 0x68);
    }
}
// Re-resolve against the new font list; rebuild only groups whose effective family changed.
void takeFontScan() {
    std::shared_ptr<FontScanDone> d;
    {
        std::lock_guard<std::mutex> lock(fontScanMutex);
        d = std::move(fontScanDone);
    }
    if (!d) return;
    if (!d->rescan && !d->r.list) chat("the font list could not be read; Rescan Fonts tries again.", 0x68);
    if (d->rescan) {
        if (d->r.list) chat("fonts: " + tf::fontsFoundLine(*d->r.list) + ".");
        else if (d->r.reloaded) chat("fonts: " + d->r.why + "; the previous list stays. Try Rescan Fonts again in a moment.", 0x68);
        else chat("fonts: the rescan changed nothing (" + d->r.why + "); try again in a moment.", 0x68);
    }
    if (d->r.list && settingsReady()) pushOptions();
}

void identityTick() {
    std::string name;
    uint32_t server = 0;
    try {
        IMemoryManager* m = core ? core->GetMemoryManager() : nullptr;
        IPlayer* p = m ? m->GetPlayer() : nullptr;
        IParty* party = m ? m->GetParty() : nullptr;
        if (p && party && p->GetLoginStatus() == 2) {
            const char* n = party->GetMemberName(0);
            server = party->GetMemberServerId(0);
            if (n && *n && server) name = n;
        }
    } catch (const std::exception&) {}
    if (name.empty()) return;
    const bool same = name == charName && server == charServer;
    if (identityPending) {   // back in game: after a zone change nothing is said; after a logout, who it is
        identityPending = false;
        if (logoutSaid || !same) {
            info(same ? "character: " + name + " again (its settings stay)" : std::string("character: a different character"));
            langLogPending = true;
        }
        logoutSaid = false;
    }
    if (same) {
        bool save = heldSave;
        if (pendingEnabled >= 0 && settingsReady()) {
            settings.enabled = pendingEnabled != 0;
            pendingEnabled = -1;
            save = true;
        }
        if (save) {
            if (heldSave) info("settings: the change made while out of the game is saved now");
            saveSettings();
        }
        return;
    }
    if (heldSave) info("settings: a change made while out of the game is not saved: another character is in game now (its own settings are read)");
    heldSave = false;
    charName = name;
    charServer = server;
    langLogPending = true;
    fileLog.moveToCharacter(plog::characterLogPath(root, kName, plog::characterKey(name, server)), name);   // the log writer writes the "character:" line there
    loadSettingsForCharacter();
}

std::string settingsStatusText() {
    if (charName.empty()) return "settings: none yet (they are read when a character is in game).";
    std::string t = "settings: " + charName + " (id " + std::to_string(charServer) + "): " + settingsPathText();
    if (shareAll) t += " (Every Character; TrueFont's on/off stays in " + plog::underRoot(root, ownSettingsPath()) + ")";
    if (identityPending) t += logoutSaid ? "; logged out: they stay in use, and a change is saved when this character is back in game" : "; not in game: a change is saved when the character is back";
    else if (!settingsReady()) t += "; still being read";
    else if (settingsReadFailed) t += "; not saved: the file could not be read, so the defaults are in use";
    return t + ".";
}

std::mutex cmdMutex;
std::deque<std::vector<std::string>> cmdQueue;

// Count only enabled, installed groups whose installer is active.
tf::GlyphCounts countsNow() {
    const bool chatOn = settings.chat().on, spriteOn = settings.anySpriteGroupOn();
    const bool chatDown = installer && chatOn && (installer->autoOff() || !installer->failure().empty());
    const bool spriteDown = sprites && spriteOn && (sprites->autoOff() || !sprites->failure().empty());
    const bool chatDrawn = installer && chatOn && !chatDown && installer->installed() && installer->builtCell() > 0;
    return tf::statusCounts(settings.enabled, chatDrawn, installer ? installer->stats().rendered() : 0, installer ? installer->stats().kept() : 0, spriteDown,
                            sprites ? sprites->glyphCounts() : tf::GlyphCounts{});
}

// Omit disabled Chat status and append disabled group names.
std::string statusAll() {
    std::string s;
    if (!installer) s = capFirst(partOffText("Chat/Items", false, std::string())) + ".";
    else if (!settings.enabled || settings.chat().on) s = installer->statusLine();
    if (sprites) {
        std::string t = sprites->statusText();
        if (!t.empty()) t[0] = char(toupper(static_cast<unsigned char>(t[0])));
        s += (s.empty() ? "" : " ") + t + ".";
    } else s += std::string(s.empty() ? "" : " ") + capFirst(partOffText("the menu and HUD fonts", true, std::string())) + ".";
    const std::string off = settings.groupsOffText();
    if (settings.enabled && !off.empty()) s += " Switched off: " + off + ".";
    return s;
}
void clearLatchesAll() {
    if (installer) installer->clearLatches();
    if (sprites) sprites->clearLatches();
}
void turnOnNow(bool announce) {
    if (installer && settings.chat().on) {
        if (installer->autoOff()) info("on: clears the automatic off: " + installer->autoOffReason());
        chatTurnOn(announce);
    }
    if (sprites) spritesTurnOn(announce);
}
void turnOffNow(const char* why, bool quiet) {
    tf::OffReport rep;
    bool any = false;
    if (installer && installer->enabled()) { rep = installer->turnOff(why); any = true; }
    if (sprites && sprites->enabled()) { rep = mergeReports(rep, sprites->turnOff(why)); any = true; }
    if (!any) return;
    if (quiet) { if (!rep.clean()) warn("off left: " + rep.leftText()); return; }
    if (!rep.wasOn) chat("off.");
    else if (rep.gameGone) chat("off (the game is closing; nothing was touched).");
    else if (rep.clean()) chat("off: the game's own fonts are back.");
    else chat("off, but these were left: " + rep.leftText() + ". See the log.", 0x68);
}

tf::PanelView panelView();
// Capture render-thread diagnostics as one log block.
void runDiag() {
    std::string body;
    logCapture = &body;
    logCaptureThread = GetCurrentThreadId();
    struct CaptureScope { ~CaptureScope() { logCapture = nullptr; } } captureScope;   // Clear the stack pointer on every exit.
    info("character " + (charName.empty() ? std::string("none") : plog::characterKey(charName, charServer)) + (identityPending ? " (not in game)" : ""));
    if (!startFailed.empty()) info("start failed: " + startFailed);
    for (const auto& l : resolveLines) info(l);
    {   // each group's state, as the panel reads it
        const tf::PanelView v = panelView();
        for (const tf::Group grp : tf::kGroupOrder) {
            const int g = int(grp);
            const tf::GroupView& gv = v.groups[g];
            tf::GroupDiag d;
            d.enabled = settings.enabled;
            d.on = settings.g[g].on;
            d.available = gv.available;
            d.down = gv.down;
            d.failed = gv.failed;
            d.restart = gv.restart;
            d.replaced = gv.replaced;
            d.language = gv.language;
            d.shape = tf::hasShape(grp) ? gv.shape : -1;
            info(tf::groupDiagText(tf::groupKey(grp), d));
        }
    }
    if (installer) installer->logStatus();
    if (sprites) sprites->logStatus();
    {
        const tf::GlyphCounts n = countsNow();
        const tf::GlyphCounts sp = sprites ? sprites->glyphCounts() : tf::GlyphCounts{};
        info(fmt("counts (the Status panel's, every group that is on and drawn): %s; the menu and HUD textures in place: %d from the fonts, %d native%s",
                 n.any ? fmt("From Font %d, Native %d", n.rendered, n.kept).c_str() : "\"-\" (no group that is on is drawn)", sp.rendered, sp.kept, sp.any ? "" : " (none drawn)"));
    }
    info("settings: " + (settingsReady() ? settingsPathText() : std::string("(not read yet)")) + (shareAll ? " (Every Character)" : "") + ": " + settings.text() +
         (settingsReadFailed ? " (NOT SAVED: the read failed)" : ""));
    {
        bool jm = false;
        const std::wstring jf = jpFamily(jm);
        info("japanese font: " + (settings.jpOff() ? std::string("off (kana and kanji stay the game's own)")
                                                    : (settings.jpSame() ? std::string("same as Font") : settings.chatJp) + ", drawn with " + tf::toUtf8(jf) + (jm ? " (not installed)" : "")));
    }
    std::string fam;   // what each group draws with, and the Same font as chain it came through (tab order)
    for (const tf::Group grp : tf::kGroupOrder) {
        const int g = int(grp);
        bool m = false;
        fam += std::string(fam.empty() ? "" : "; ") + tf::groupKey(tf::Group(g)) + " " + tf::toUtf8(groupFamily(tf::Group(g), m));
        tf::Group at = settings.follows(tf::Group(g));
        for (int n = 0; at != tf::Group::None && n < tf::kGroupCount; at = settings.follows(at), n++) fam += std::string(n ? " -> " : " via ") + tf::groupKey(at);
        if (m) fam += " (own font missing)";
    }
    info("families: " + fam);
    {   // the engine each group was last built with (ftface.h)
        std::string en = std::string("engine: ") + tf::engineKey(settings.engine) + (tf::underWine() ? " (under Wine)" : "") + "; last built with:";
        bool any = false;
        if (installer && installer->enabled() && !installer->stats().face.empty()) {   // not while Chat/Items is off
            const tf::AtlasStats& st = installer->stats();
            en += std::string(" chat ") + tf::engineName(st.engine) + (st.fullFace.empty() ? "" : std::string(" (kana and kanji ") + tf::engineName(st.fullEngine) + ")");
            any = true;
        }
        for (const tf::Group grp : tf::kGroupOrder) {
            tf::Engine e = tf::Engine::Windows;
            bool mixed = false;
            if (grp == tf::Group::Chat || !sprites || !sprites->groupEngine(grp, e, &mixed)) continue;
            en += std::string(any ? "; " : " ") + tf::groupKey(grp) + " " + tf::groupEngineText(e, mixed);
            any = true;
        }
        info(en + (any ? "" : " nothing yet"));
    }
    if (const auto list = fontList()) info("fonts: " + tf::fontsFoundText(*list) + fmt("; %zu font files loaded from ", folderFonts.held()) + plog::underRoot(root, fontsDir()));
    for (const auto& l : shape.diagLines()) info(l);
    {
        tf::DiagMemory m;
        if (installer) {
            if (installer->textureBytes()) { m.textures += 1; m.textureBytes += installer->textureBytes(); }
            m.keptBytes += installer->keptBytes() + installer->shapesBytes();
        }
        if (sprites) {
            m.textures += sprites->texturesLoaded();
            m.textureBytes += sprites->textureBytes();
            m.keptBytes += sprites->keptBytes();
        }
        m.shapePage = shape.page();
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof pmc;
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        m.processRead = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc) && GlobalMemoryStatusEx(&ms);
        if (m.processRead) {
            m.workingSet = pmc.WorkingSetSize;
            m.privateBytes = pmc.PrivateUsage;
            m.availVirtual = ms.ullAvailVirtual;
            m.totalVirtual = ms.ullTotalVirtual;
        }
        info(tf::diagMemoryText(m));
    }
    logCapture = nullptr;
    fileLog.writeDiag(tf::diagWho(kVersion, buildStamp()), body);
    chat(tf::diagDoneText(logPathText()));
}

void runCommand(const std::vector<std::string>& a) {
    const std::string sub = a.size() > 1 ? a[1] : "";
    info("command: /tfont " + sub + fmt(" (thread %lu, frame %llu)", GetCurrentThreadId(), static_cast<unsigned long long>(frame)));
    if (sub.empty()) {
        panel.open = !panel.open;
        info(std::string("panel ") + (panel.open ? "shown" : "hidden"));
        return;
    }
    if (sub == "selftest") {
#ifdef TF_DEV
        if (!installer) { chat("the selftest needs Chat/Items, which is not supported on this game version.", 0x68); return; }
        if (a.size() > 2 && a[2] == "stop") selftest.abort("/tfont selftest stop");
        else if (selftest.active()) {   // said aloud while the run keeps chat quiet
            chatQuiet = false;
            chat("a selftest is already running; /tfont selftest stop ends it.");
            chatQuiet = true;
        } else selftest.start();
#else
        chat("the selftest is in the DEV build only.");
#endif
        return;
    }
    // Any other command ends a running self-check; status, diag and a bare /tfont fonts only read.
    if (!tf::commandOnlyReads(sub, a.size())) selftestAbort("/tfont " + sub);
    if (!installer && !sprites && tf::commandNeedsPart(sub)) {
        if (!startFailed.empty()) chat("TrueFont failed to start (" + startFailed + "); reload it or restart the game (see " + logPathText() + ").", 0x44);
        else chat(partOffText("TrueFont", false, resolveWhy) + " (see " + logPathText() + ").", 0x44);
        return;
    }
    if (sub == "on") {
        rememberEnabled(true);
        if (!settingsReady()) {   // nothing turns on before the character's settings are read
            clearLatchesAll();
            chat("on: it applies once the character is in game.");
            return;
        }
        const bool chatOn = !installer || !settings.chat().on || installer->onAsAsked();
        // Revalidate sprite records that reverted to native or may now be installable.
        const bool freshLook = sprites && sprites->installed() && sprites->wantsFreshLook();
        const bool spriteOn = !sprites || (sprites->enabled() && sprites->failure().empty() && !freshLook);
        if (chatOn && spriteOn) { chat(statusAll()); return; }
        if (freshLook) {
            const tf::OffReport rep = sprites->turnOff("/tfont on: a fresh look at every sprite-font record");
            if (!rep.clean()) warn("/tfont on: the fresh look left: " + rep.leftText());
        }
        // Report unsupported menu language once; Chat reports separately when installed.
        const std::string langNo = sprites ? sprites->languageRefusal() : std::string();
        turnOnNow(true);
        sayMissingFonts();
        if (settings.groupsOn() == 0) chat("on; every font group is switched off in the panel, so the game's own fonts stay.");
        else if (!langNo.empty()) chat("on: " + langNo + ", so the menu and HUD fonts keep the game's own.", 0x68);
        else chat("on: the fonts are built and put in place in a moment.");
    } else if (sub == "off") {
        rememberEnabled(false);
        if (!anyEnabled()) { chat(statusAll()); return; }
        turnOffNow("/tfont off", false);
    } else if (sub == "font") {
        std::string want;
        for (size_t i = 2; i < a.size(); i++) want += (i > 2 ? " " : "") + a[i];   // the family keeps its case
        if (want.size() >= 2 && want.front() == '"' && want.back() == '"') want = want.substr(1, want.size() - 2);
        const auto list = fontList();
        const std::wstring found = list ? list->find(tf::fromUtf8(want)) : std::wstring();
        const tf::Group chatFollows = settings.follows(tf::Group::Chat);
        const std::string followText = chatFollows != tf::Group::None ? std::string(" (Same Font As ") + tf::groupTitle(chatFollows) + ")" : std::string();
        if (want.empty()) {   // the font in use, and the saved one when it is not installed
            bool missing = false;
            const std::string used = familyText(effectiveFamily(missing));
            chat("/tfont font \"<family>\": Chat/Items uses " + used + followText + (missing ? " (the saved " + settings.chat().font + " is not installed)" : std::string()) +
                 ". The panel lists every installed font: /truefont.");
            return;
        }
        if (!settingsReady()) { chat("the character's settings are not read yet; try again in game.", 0x68); return; }
        if (!list) { chat("the font list is not read (Rescan Fonts on the Settings tab, or /tfont fonts rescan, reads it again).", 0x68); return; }
        if (found.empty()) { chat("no installed font is called '" + want + "'. The panel lists every installed font: /truefont.", 0x68); return; }
        tf::Settings next = settings;
        next.chat().font = tf::toUtf8(found);
        if (chatFollows != tf::Group::None) {   // Save its own family without altering the followed font.
            const bool changed = next != settings;
            if (changed) {
                applySettings(next, "/tfont font");
                if (!saveSettings()) info("settings: the font applies for this session only");
            }
            bool m = false;
            chat("font: " + next.chat().font + (changed ? " is saved as" : " is already") + " Chat/Items' own font; Chat/Items follows " + tf::groupTitle(chatFollows) +
                 " (Same Font As), so it draws with " + familyText(effectiveFamily(m)) + ". Untick Same Font As on its tab to draw with it.");
            return;
        }
        bool missing = false;
        if (effectiveFamily(missing) == found) {   // Effective family unchanged; skip the rebuild.
            if (tf::fontPickSaves(missing, settings.chat().firstFont())) {
                applySettings(next, "/tfont font");
                if (!saveSettings()) info("settings: the font applies for this session only");
            }
            chat("font: " + next.chat().font + " is already in use.");
            return;
        }
        const bool wasRefused = installer && !installer->failure().empty();
        bool chatScheduled = false, spriteScheduled = false;
        applySettings(next, "/tfont font", &chatScheduled, &spriteScheduled);
        if (!saveSettings()) info("settings: the font applies for this session only");
        const std::string& f = next.chat().font;
        const std::string followers = spriteScheduled ? "; the groups that follow it are rebuilt" : "";
        if (!settings.enabled) chat("font: " + f + " (TrueFont is off; /tfont on to draw with it).");
        else if (!installer) chat("font: " + f + " is saved, but Chat/Items is not supported on this game version" + followers + ".", 0x68);
        else if (!settings.chat().on) chat("font: " + f + " is saved; Chat/Items is switched off, so it is not built" + followers + ".");
        else if (installer->autoOff())
            chat("font: " + f + " is saved; Chat/Items turned itself off, so " + (spriteScheduled ? "it is not rebuilt" + followers : std::string("nothing is built")) +
                     ". /tfont on tries again.",
                 0x68);
        else if (wasRefused) {   // Explicit retries repeat their outcome.
            chatTurnOn(true);
            chat("font: " + f + "; Chat/Items tries again now.");
        } else {
            if (chatScheduled) installer->announceNextRebuild();   // Announce command-triggered rebuilds.
            chat("font: " + f + "; building it now.");
        }
        if (spriteScheduled && sprites) sprites->announceNextRebuild();   // Announce affected followers after building.
    } else if (sub == "fonts") {
        if (a.size() > 2 && a[2] == "rescan") { rescanFonts(); return; }
        if (a.size() > 2) { chat("/tfont fonts [rescan]: the fonts found, or read them again.", 0x68); return; }
        const auto list = fontList();
        chat(std::string("fonts: ") + (list ? tf::fontsFoundLine(*list) : std::string("not read yet")) + (fontsScanning ? " (a rescan is running)" : "") +
             ". Your own fonts go in " + plog::underRoot(root, fontsDir()) + "; /tfont fonts rescan reads them again.");
    } else if (sub == "reset") {
        if (!settingsReady()) { chat("the character's settings are not read yet; try again in game.", 0x68); return; }
        if (settings.allDefault()) { chat("reset: every setting is already at its default."); return; }
        tf::Settings next = settings;
        next.resetAll();   // Disable all groups; syncEnabled restores native Chat next frame.
        applySettings(next, "/tfont reset");
        if (!saveSettings()) info("settings: the reset applies for this session only");
        chat(tf::resetReplyText(settings.enabled));
    } else if (sub == "rebuild") {
        std::string why, why2;
        bool chatRebuilds = false, spriteRebuilds = false;
        if (installer && settings.enabled && !settings.chat().on) why = "Chat/Items is switched off";
        else if (installer) {
            installer->announceNextRebuild();
            chatRebuilds = installer->rebuild(why);
        }
        if (sprites) {
            sprites->announceNextRebuild();
            spriteRebuilds = sprites->rebuild(why2);
        }
        if (chatRebuilds || spriteRebuilds) chat("rebuilding the fonts...");
        else chat("cannot rebuild: " + (why.empty() ? why2 : why2.empty() || why2 == why ? why : why + "; " + why2) + ".", 0x68);
    } else if (sub == "original") {
        if (a.size() > 2 && a[2] != "on" && a[2] != "off") { chat("/tfont original [on | off]: shows the game's own fonts for a comparison.", 0x68); return; }
        const bool chatWas = installer && installer->showingOriginal(), spriteWas = sprites && sprites->showingOriginal();
        const bool want = a.size() > 2 ? (a[2] == "on") : !(chatWas || spriteWas);
        std::string chatWhy, spriteWhy;
        const bool chatSwitchedOff = installer && settings.enabled && !settings.chat().on;
        if (chatSwitchedOff) chatWhy = "Chat/Items is switched off";
        const bool chatOk = installer && !chatSwitchedOff && installer->setShowOriginal(want, chatWhy);
        const bool spriteOk = sprites && sprites->setShowOriginal(want, spriteWhy);
        if (!chatOk && !spriteOk) chat("cannot switch: " + (chatWhy.empty() ? spriteWhy : chatWhy) + ".", 0x68);
        else if (want) chat("showing the game's own fonts; /tfont original again for TrueFont's.");
        else if (chatWas && !chatOk)
            chat((spriteWas ? "the menu and HUD fonts are TrueFont's again; Chat/Items is not: " : "Chat/Items could not switch back: ") + chatWhy + ".", 0x68);
        else if (spriteWas && !spriteOk)
            chat((chatWas ? "Chat/Items is TrueFont's again; the menu and HUD fonts are not: " : "the menu and HUD fonts could not switch back: ") + spriteWhy + ".", 0x68);
        else chat("TrueFont's fonts are back.");
    } else if (sub == "status") {
        const bool q = chatQuiet;
        chatQuiet = false;   // a typed status answers even while the self-check keeps chat quiet
        const std::string lang = sprites && sprites->clientLangRead() ? std::string(" The game's language: ") + tf::clientLangName(sprites->clientLang()) + "." : std::string();
        chat(statusAll() + lang + (selftestActive() ? " A selftest is running." : "") + tf::statusChatTail());
        chat(settingsStatusText());
        chatQuiet = q;
    } else if (sub == "diag") {
        if (a.size() > 2) { chat(tf::kDiagUsage, 0x68); return; }
        const bool q = chatQuiet;
        chatQuiet = false;   // answers even while the self-check keeps chat quiet
        runDiag();
        chatQuiet = q;
    } else {
#ifdef TF_DEV
        chat(tf::commandUsage(true), 0x68);
#else
        chat(tf::commandUsage(false), 0x68);
#endif
    }
}

// Panel changes announce only the first install or a new refusal.
tf::PanelView panelView() {
    tf::PanelView v;
    v.version = kVersion;
    v.s = settings;
    v.ready = settingsReady();
    v.supported = installer != nullptr || sprites != nullptr;
    v.fonts = fontList();
    v.firstFont = v.fonts && !v.fonts->empty() ? tf::toUtf8(v.fonts->first()) : std::string(tf::kDefaultFont);   // Resolved default for reset tooltips and selection.
    v.fontsScanning = fontsScanning;
    {
        bool jm = false;
        jpFamily(jm);
        v.jpMissing = jm && settings.jpDrawn();
    }
    v.shareAll = shareAll;
    v.footer = tf::settingsFooterText(charName.empty() ? std::string() : settingsPathText(), shareAll, plog::underRoot(root, fileLog.path()), buildStamp());
    {
        std::lock_guard<std::mutex> lock(presetMutex);
        v.presets = presetNames;
        v.presetsKnown = presetsKnown;
        v.presetsGen = presetsGen;
        v.presetsQueued = presetsQueued;
    }
    for (int g = 0; g < tf::kGroupCount; g++) {
        bool missing = false;
        v.groups[g].family = familyText(groupFamily(tf::Group(g), missing));
        v.groups[g].missing = missing && fontDrawn(tf::Group(g));   // Warn only for used families.
        // Show the client-version reason for unresolved groups.
        const int unknown = !clientUnknown() ? 0 : tf::newerThanKnown(clientStamp) ? 1 : 2;
        if (g == int(tf::Group::Chat)) {
            v.groups[g].available = installer != nullptr;
            v.groups[g].unknownClient = installer ? 0 : unknown;
        } else {
            const auto st = sprites ? sprites->groupState(tf::Group(g)) : tf::SpriteInstaller::GroupState::Unavailable;
            v.groups[g].available = st != tf::SpriteInstaller::GroupState::Unavailable;
            v.groups[g].replaced = sprites && sprites->groupArtReplaced(tf::Group(g));   // a record re-created since load holds other art
            v.groups[g].modLayout = sprites && sprites->groupLayoutUnknown(tf::Group(g));   // a font mod's letter layout it does not recognise
            v.groups[g].failed = st == tf::SpriteInstaller::GroupState::Failed;   // could not be built or put in place
            v.groups[g].restart = sprites && sprites->groupNeedsRestart(tf::Group(g));
            v.groups[g].unknownClient = tf::spriteGroupUnknownClient(sprites != nullptr, v.groups[g].modLayout, unknown);
            if (sprites && sprites->languageRefused()) v.groups[g].language = tf::clientLangName(sprites->clientLang());
            // The nameplate page read resolves independently from sprite fonts.
            if (sprites && g == int(tf::Group::Nameplates) && !sprites->plateReport().available) v.groups[g].unknownClient = unknown;
            if (tf::hasShape(tf::Group(g))) {   // Aspect and Size: 1 another plugin's bytes at a site, 2 not on this game version
                const auto ss = shape.ready() ? shape.groupState(tf::shapeGroupOf(tf::Group(g))) : tf::ShapePatch::GroupState::Unavailable;
                v.groups[g].shape = ss == tf::ShapePatch::GroupState::Taken ? 1 : ss == tf::ShapePatch::GroupState::Unavailable ? 2 : 0;
                if (v.groups[g].shape == 1) v.nameplateLoaded = tf::nameplateLoaded();   // the note names the plugin only while it is loaded
            }
        }
    }
    if (installer) {
        // A rebuild waiting for its copy's buffer is building too.
        v.chatBuilding = installer->building() || installer->rebuildPending() || installer->phase() == tf::Installer::Phase::Building;
        v.progress = installer->progress();
        v.installed = installer->installed();
        v.showOriginal = installer->showingOriginal();
        v.reapplies = installer->reapplies();
        v.highActive = installer->highActive();
        if (installer->textureBytes()) { v.textures += 1; v.textureBytes += installer->textureBytes(); }
        v.keptBytes = installer->keptBytes() + installer->shapesBytes();
        v.originalBytes = installer->originalBytes();
        // Derive Sharpness availability from the same size budget as the installer.
        const tf::ChatBudget b = installer->budget();
        for (int i = 0; i < 4; i++) v.chatSharpDenied[i] = tf::chatSharpDenied(b, tf::PanelView::kSharps[i]);
        v.chatSharpNote = tf::chatSharpNote(b, settings.chat().sharp);
        v.chatSharpTip = tf::chatSharpTip(b);
    } else v.chatSharpTip = tf::chatSharpTip(tf::ChatBudget{});
    if (sprites) {
        v.installed = v.installed || sprites->installed();
        v.showOriginal = v.showOriginal || sprites->showingOriginal();
        v.reapplies += sprites->reapplies();
        v.textures += sprites->texturesLoaded();
        v.textureBytes += sprites->textureBytes();
        v.keptBytes += sprites->keptBytes();   // the A8L8 composites kept for a Compress change
        if (sprites->building()) v.building = true;
        v.clientRead = sprites->clientLangRead();
        v.client = v.clientRead ? tf::clientLangName(sprites->clientLang()) : "-";   // "-": not read yet
    }
    // Disable the whole panel only when all existing installers are down; disabled groups do not count as failures.
    const bool chatOn = settings.chat().on, spriteOn = settings.anySpriteGroupOn();
    const bool chatDown = installer && chatOn && (installer->autoOff() || !installer->failure().empty());
    const bool spriteDown = sprites && spriteOn && (sprites->autoOff() || !sprites->failure().empty());
    v.sideOff = settings.enabled && (!installer || chatDown) && (!sprites || spriteDown);
    v.chatDown = settings.enabled && chatDown;
    for (int g = 0; g < tf::kGroupCount; g++) v.groups[g].down = settings.enabled && (g == int(tf::Group::Chat) ? chatDown : spriteDown);
    {
        const tf::GlyphCounts n = countsNow();
        v.hasStats = n.any;
        v.rendered = n.rendered;
        v.kept = n.kept;
    }
    v.building = v.building || v.chatBuilding;
    // Show the highest-priority warning: unsupported, failed, auto-off, missing font, then re-applied.
    std::string& w = v.warning;
    const std::string retry = tf::kRetryText;
    if (!installer && !sprites)
        w = !startFailed.empty() ? "TrueFont failed to start (" + startFailed + "); reload it or restart the game."
            : clientUnknown()    ? capFirst(clientVersionText()) + ", so TrueFont stays off. See the log."
                                 : std::string("This game version is not supported, so TrueFont stays off. See the log.");
    else if (!settings.enabled) w.clear();
    else if (installer && chatOn && !installer->failure().empty()) w = tf::chatDownWarning(v.sideOff, installer->failure(), installer->failureNeedsRestart());
    else if (installer && chatOn && installer->autoOff())
        w = v.sideOff ? std::string("TrueFont turned itself off: something else kept replacing the font textures") + retry
                      : std::string("Chat/Items turned itself off: something else kept replacing its texture") + retry;
    else if (sprites && spriteOn && sprites->languageRefused()) w = sprites->languageRefusal() + (installer ? "; Chat/Items works." : ".");   // no retry can change it
    else if (sprites && spriteOn && !sprites->failure().empty()) w = "The menu and HUD fonts could not turn on: " + sprites->failure() + retry;
    else if (sprites && spriteOn && sprites->autoOff()) w = "The menu and HUD fonts turned themselves off: something else kept replacing their textures" + retry;
    else {
        for (const tf::Group grp : tf::kGroupOrder) {
            const int g = int(grp);
            if (w.empty() && v.groups[g].missing) {
                const std::string groups = settings.drawnWithText(grp);
                w = "The font '" + settings.g[g].font + "' is not installed; " + groups + " " + tf::useVerb(groups) + " " + v.groups[g].family + ".";
            }
        }
        if (w.empty() && v.jpMissing) w = "The font '" + settings.chatJp + "' is not installed; " + kJpGroups + " use " + v.groups[int(tf::Group::Chat)].family + ".";
        if (w.empty() && v.reapplies) w = "Something else replaced a font texture " + std::to_string(v.reapplies) + " times; TrueFont put it back. See the log.";
    }
    return v;
}
void panelTick() {
    IGuiManager* gui = core ? core->GetGuiManager() : nullptr;
    if (!gui || !panel.open) return;
    const tf::PanelResult r = panel.draw(gui, panelView());
    if (r.toggledOn || r.committed || r.showOriginalClicked || r.rescanFonts) selftestAbort("a change in the panel");
    if (r.toggledOn) {
        info(std::string("panel: TrueFont ") + (r.s.enabled ? "on" : "off"));
        rememberEnabled(r.s.enabled);
        if (r.s.enabled) {
            turnOnNow(false);
            if (settingsReady() && (installer || sprites)) sayMissingFonts();
        } else turnOffNow("the panel's TrueFont box", true);
    }
    if (r.committed) {   // Commit each completed edit once.
        tf::Settings next = r.s;
        next.enabled = settings.enabled;
        info("panel: " + next.text());
        const tf::Settings was = settings;
        applySettings(next, "panel");
        // A click rebuilds on the next frame; a slider let go keeps the quiet time.
        if (!r.slider) {
            if (installer) installer->hurryRebuild();
            if (sprites) sprites->hurryRebuild();
        }
        if (!saveSettings()) info("settings: the panel's change " + notSavedWhy());
        chatGroupChanged(was);   // off: syncEnabled turns it off on the next frame
    }
    if (r.shareAllClicked) setShareAll(r.shareAll);
    if (r.rescanFonts) rescanFonts();
    switch (r.presetOp) {
    case tf::PresetOp::List: queuePresetList(); break;
    case tf::PresetOp::Load: loadPresetNow(r.presetName); break;
    case tf::PresetOp::Save: savePresetNow(r.presetName); break;
    case tf::PresetOp::Delete: deletePresetNow(r.presetName); break;
    default: break;
    }
    if (r.showOriginalClicked) {
        std::string why;
        if (installer && installer->enabled() && !installer->setShowOriginal(r.showOriginal, why)) warn("panel: Show Original " + std::string(r.showOriginal ? "on" : "off") + " failed: " + why);
        if (sprites && !sprites->setShowOriginal(r.showOriginal, why)) info("panel: Show Original " + std::string(r.showOriginal ? "on" : "off") + " (menu and HUD fonts): " + why);
    }
}

}  // namespace

class TrueFont final : public IPlugin {
public:
    const char* GetName() const override { return kName; }
    const char* GetAuthor() const override { return "SQLCommit"; }
    const char* GetDescription() const override { return "Draws the game's text with TrueType fonts of your choice, in eight groups, with no change to the layout unless you ask for it (Aspect and Size)."; }
    const char* GetLink() const override { return "https://github.com/SQLCommit/TrueFont"; }
    double GetVersion() const override { return kVersion; }
    uint32_t GetFlags() const override {
        return uint32_t(Ashita::PluginFlags::UseCommands) | uint32_t(Ashita::PluginFlags::UseDirect3D) | uint32_t(Ashita::PluginFlags::UsePackets);   // packets: the logout rule
    }

    bool Initialize(IAshitaCore* c, ILogManager*, uint32_t) override {
        if (!soleInstance.acquire(tf::soleInstanceName(GetCurrentProcessId()))) {
            refused = true;
            core = c;
            // The existing instance may use another filename. Send immediately: a pinned image may have no frame
            // callback to flush queued chat.
            logl("error", std::string("chat: ") + tf::kAlreadyLoadedText);
            sendChat(tf::kAlreadyLoadedText, 0x44);
            core = nullptr;
            return false;
        }
        core = c;
        holdChat = true;   // until the first frame
        try {
            tf::sink.info = &info;
            tf::sink.warn = &warn;
            tf::sink.err = &err;
            tf::sink.chat = &chatSink;
            root = plog::ashitaRoot(reinterpret_cast<const void*>(&anchor));
            run = plog::thisRun();
            fileLog.open(root, plog::startupLogPath(root, kName, run));
            char ver[16], iface[16];
            _snprintf_s(ver, sizeof ver, _TRUNCATE, "%.1f", kVersion);
            _snprintf_s(iface, sizeof iface, _TRUNCATE, "%.2f", ASHITA_INTERFACE_VERSION);
            const uint32_t build = buildStamp();
            const std::string session = plog::sessionText(kName, ver, build, plog::imageStamp(GetModuleHandleA("FFXiMain.dll")), iface, run);
            fileLog.setSession(session, run);
            logStarted = true;
            fileLog.write("info", session);
            fileLog.start();
            {
                const std::string rootCopy = root;
                const plog::Run runCopy = run;
                fileLog.post([rootCopy, runCopy] { plog::cleanupStartupFiles(rootCopy, kName, runCopy); });
            }
#ifdef TF_DEV
            const char* flavour = " (DEV build)";
#else
            const char* flavour = "";
#endif
            info(fmt("truefont %s loading, build %08X%s, Initialize on thread %lu", ver, build, flavour, GetCurrentThreadId()));
            disk.start();
#ifdef TF_DEV
            selftest.host.root = root;
            selftest.host.inGame = [] { return inGame(); };
            selftest.host.zoning = [] { return isZoning(); };
            selftest.host.zone = [] { return zoneId(); };
            selftest.host.hwnd = [] { return gameHwnd(); };
            selftest.host.post = [](std::function<void()> job) { fileLog.post(std::move(job)); };
            selftest.host.shotsDir = [] {
                return plog::logsDir(root, kName) + (charName.empty() ? std::string("startup") : plog::characterKey(charName, charServer)) + "\\selftest\\";
            };
            selftest.host.quiet = [](bool q) { chatQuiet = q; };
            selftest.host.ready = [] { return settingsReady(); };
            selftest.host.panelOpen = [](int set) { const bool was = panel.open; if (set >= 0) panel.open = set != 0; return was; };
            selftest.host.savedSpriteOptions = [] { return spriteOptions(); };
            selftest.host.savedOptions = [] {
                bool missing = false, jpMissing = false;
                return settings.options(effectiveFamily(missing), jpFamily(jpMissing));
            };
            selftest.host.keepOriginal = [] { return settings.keepOriginal; };
            selftest.host.shapeText = [] {
                std::string t = "Aspect and Size as saved: " + tf::shapeWantsText(shapeWant);
                for (int g = 0; g < tf::kShapeGroupCount; g++) {
                    const tf::ShapeGroup sg = tf::ShapeGroup(g);
                    const auto gs = shape.ready() ? shape.groupState(sg) : tf::ShapePatch::GroupState::Unavailable;
                    t += std::string("; ") + tf::shapeGroupName(sg) + ": " +
                         (!shapeWant[g].active() ? "the game's (nothing written)" : shape.inEffect(sg) ? "in place"
                          : gs == tf::ShapePatch::GroupState::Taken ? "NOT in place (another plugin's)" : "NOT in place (not on this game version)");
                }
                return t;
            };
            selftest.host.screenText = [] {
                return fmt("Auto's k %d (Nameplates), %d (Damage numbers) from %s (the newest read: %s)", autoKPlate.k, autoKDamage.k,
                           autoKPlate.seen ? autoKPlate.last.text().c_str() : "no read yet", autoKPlate.latest.ok() ? autoKPlate.latest.text().c_str() : "none");
            };
#endif
            if (!disk.runJob([] { scanFonts(false); })) scanFonts(false);
            std::string why;
            if (!resolveClient(why)) {   // resolveClient says it in chat when the image was read
                resolveWhy = why;
                resolveNote(err, "resolve failed: " + why + "; TrueFont stays off");
                if (!clientStamp) chat("this game version is not supported (" + why + "), so TrueFont stays off (see " + logPathText() + ").", 0x44);
            }
            usage();
            return true;
        } catch (const std::exception& e) {   // only C++ exceptions: an access violation must reach the process
            // Nothing is installed before the first frame, so a half-made set-up is dropped whole.
            delete installer;
            installer = nullptr;
            delete sprites;
            sprites = nullptr;
            err(std::string("Initialize failed: ") + e.what());
            try { startFailed = e.what(); } catch (const std::exception&) {}   // the later lines say this, not "not supported"
            if (startFailed.empty()) startFailed = "out of memory";
            chat(std::string("TrueFont failed to start (") + e.what() + "); it stays loaded and does nothing.", 0x44);
        }
        return true;   // stay loaded but passive: nothing was changed, so Release has nothing to restore
    }

    // Full restoration requires the render thread outside Present. Otherwise restore pointers without D3D
    // and pin the DLL to keep in-flight draws valid. Retained state or workers also require pinning.
    void Release() override {
        const unsigned caller = unsigned(reinterpret_cast<uintptr_t>(_ReturnAddress()));
        try {
            ReleaseSteps steps(caller);
            tf::runRelease(steps, refused);
        } catch (const std::exception& e) {
            try { pinSelf(); } catch (const std::exception&) {}
            try { err(std::string("Release failed: ") + e.what() + "; the DLL is pinned and what was not put back stays until the game closes"); } catch (const std::exception&) {}
            if (renderThread && GetCurrentThreadId() == renderThread) sendChatFixed("could not finish unloading; its fonts stay until you close the game.");
        }
        core = nullptr;
    }
    // ReleaseHost adapter for plugin-owned state.
    struct ReleaseSteps final : tf::ReleaseHost {
        explicit ReleaseSteps(unsigned caller) : caller_(caller) {}
        void markReleasing() override { _InterlockedExchange(&releasing, 1); }
        void abortFontScans() override { tf::fontGate().abort(); }
        bool stopDiskWriter() override { return disk.stop(); }
        unsigned long renderThread() const override { return ::renderThread; }
        unsigned long thisThread() const override { return GetCurrentThreadId(); }
        bool waitFrameLeft() override {
            const ULONGLONG deadline = GetTickCount64() + 1000;
            while (::inFrame > 0 && GetTickCount64() < deadline) Sleep(1);
            return ::inFrame == 0;
        }
        bool inFrame() const override { return ::inFrame > 0; }
        void releaseDev() override {
#ifdef TF_DEV
            selftest.release();
            chatQuiet = false;
#endif
        }
        // Restore native values and only operands still owned by this plugin. Free the page only after a full
        // restore; leave it during game shutdown. No DLL pin is needed because these operands point outside it.
        void releaseShape(tf::ReleaseMode mode, bool frameLeft, bool mayWriteCode) override {
            const bool gone = R.renderer && tf::rd<uint32_t>(client.base + R.renderer, 1) == 0;
            const tf::ShapeReleaseReport sr = shape.release(mode, frameLeft, gone, mayWriteCode);
            if (gone && shape.page()) ::info("Release: the game is closing: Aspect and Size's operands and page are left as they are (they go with the game)");
            else if (sr.touched && !sr.left.empty()) ::warn("Release: Aspect and Size: these sites are not stock (another writer's, or a CAS failed): " + [&] {
                std::string t;
                for (const auto& l : sr.left) t += (t.empty() ? "" : "; ") + l;
                return t;
            }() + "; the page stays with the game's floats");
        }
        bool hasInstallers() const override { return installer || sprites; }
        tf::ReleaseInstallers releaseInstallers(tf::ReleaseMode mode) override {
            tf::ReleaseInstallers r;
            if (installer) r.rep = installer->release(mode, r.workerStopped);
            if (sprites) r.rep = mergeReports(r.rep, sprites->release(mode, r.spriteStopped));
            return r;
        }
        void deleteInstallers(bool chatStopped, bool spritesStopped) override {
            if (installer && chatStopped) { delete installer; installer = nullptr; }
            if (sprites && spritesStopped) { delete sprites; sprites = nullptr; }
        }
        void unloadFolderFonts() override {
            const size_t had = folderFonts.held(), left = folderFonts.unloadAll(tf::fontGate());
            if (left) ::warn(fmt("Release: %zu of the fonts folder's fonts stay loaded (a font build is still running); they go when the game closes", left));
            else if (had) ::info(fmt("Release: the fonts folder's %zu fonts are unloaded", had));
        }
        void say(const std::string& line, bool toChat) override {
            if (toChat) chat(line, 0x44);
            else ::err("chat (not sent: off the render thread): " + line);
        }
        void pin() override { pinSelf(); }
        bool pinned() const override { return ::pinned; }
        bool stopLog() override {
            ::info("unloaded" + plog::runSuffix(run));
            return !logStarted || fileLog.stop();
        }
        void endLog() override { logStarted = false; }
        void releaseSoleInstance() override { soleInstance.release(false); }
        unsigned callerAddress() const override { return caller_; }
        void info(const std::string& s) override { ::info(s); }
        void warn(const std::string& s) override { ::warn(s); }
        void err(const std::string& s) override { ::err(s); }

    private:
        unsigned caller_;
    };

    bool HandleCommand(int32_t, const char* command, bool) override {
        if (!command) return false;
        std::vector<std::string> args;
        Ashita::Commands::GetCommandArgs(command, &args);
        if (args.empty() || !Ashita::Commands::CommandCheck(args[0], {"/truefont", "/tfont"})) return false;
        for (auto& a : args) a = tf::fromChatText(a);   // CP932 input to UTF-8.
        // Only the command and its verb are lowercased: `font "<family>"` keeps its case.
        for (size_t i = 0; i < args.size() && i < 2; i++) std::transform(args[i].begin(), args[i].end(), args[i].begin(), [](unsigned char ch) { return char(tolower(ch)); });
        if (args.size() > 2 && (args[1] == "original" || args[1] == "fonts")) std::transform(args[2].begin(), args[2].end(), args[2].begin(), [](unsigned char ch) { return char(tolower(ch)); });
        std::lock_guard<std::mutex> lock(cmdMutex);
        cmdQueue.push_back(args);
        return true;
    }

    // Publish zone/login identity for the render thread without allocation: 0x00A server +04, name +84;
    // 0x00B logout reason +04. Pass packets through unchanged.
    bool HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t*, uint32_t, const uint8_t*, bool injected, bool) override {
        if (injected || !data || releasing) return false;
        if (id == 0x000A && tf::zoneInNamesCharacter(data, size)) _InterlockedExchange(&zoneInPacket, 1);
        else if (id == 0x000B && tf::zoneOutIsLogout(data, size)) _InterlockedExchange(&logoutPacket, 1);
        return false;
    }

    bool Direct3DInitialize(IDirect3DDevice8* d) override {
        device = d;
        if (installer) installer->setDevice(d);
        if (sprites) sprites->setDevice(d);
        info(fmt("Direct3DInitialize: device %08X on thread %lu", unsigned(uintptr_t(d)), GetCurrentThreadId()));
        return true;
    }

    // Catch C++ exceptions only; access violations must propagate.
    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override {
        _InterlockedIncrement(&inFrame);
        if (!renderThread) renderThread = GetCurrentThreadId();   // Record the thread before present() can trigger release.
        if (!releasing) {
            try {
                present();
            } catch (const std::exception& e) {
                if (++presentFailures <= 20 || presentFailures % 1000 == 0) err(fmt("frame callback failed (%s), failure %u", e.what(), presentFailures));
            }
        }
        _InterlockedDecrement(&inFrame);
    }

private:
    unsigned presentFailures = 0;
    void present() {
        if (holdChat) flushHeldChat();
        ++frame;
        if (frame == 1) info(fmt("first frame: render thread %lu", renderThread));
        const bool nowIn = inGame();
        if (frame % 60 == 1 || (nowIn && (charName.empty() || identityPending))) identityTick();
        if (langLogPending && sprites && sprites->clientLangRead()) {   // once per character load, in its log
            langLogPending = false;
            tf::TableSet ts = tf::TableSet::English;
            const bool known = tf::tableSetFor(sprites->clientLang(), ts);
            info(std::string("client: the game's language is ") + tf::clientLangName(sprites->clientLang()) + " (read from the font sheets it loaded); " +
                 (known ? std::string("TrueFont uses its ") + tf::tableSetName(ts) + " letter layouts" : std::string("TrueFont has no menu letter layouts for it (Chat/Items is not affected)")));
        }
        applySettingsWhenReady();
        takeFontScan();
        if (_InterlockedExchange(&zoneInPacket, 0)) packetKnown = true;
        if (_InterlockedExchange(&logoutPacket, 0)) { packetKnown = false; logoutByPacket = true; }
        if (nowIn != loggedIn) {
            loggedIn = nowIn;
            const uint64_t t = GetTickCount64();
            // Record absence duration and whether it included zoning.
            const std::string away = nowIn && leftGameAt ? fmt(" (out of the game %llu ms; %s)", t - leftGameAt,
                                                               zoningSeen ? fmt("the game said zoning, the last %llu ms not", t - zoningAt).c_str() : "the game never said zoning")
                                                         : std::string();
            info(std::string("login status: ") + (nowIn ? "in game" : "not in game") + away +
                 ((installer && installer->installed()) || (sprites && sprites->installed()) ? " (the fonts stay installed, I10)" : ""));
            if (!nowIn) identityPending = true;   // the next login may be another character
            notInGameSince = leftGameAt = nowIn ? 0 : t;
            zoningSeen = false;
            if (nowIn) logoutByPacket = false;
            retryRefusal = true;
        }
        // Retain settings and fonts across logout; save pending changes only after the same character returns.
        if (!nowIn && notInGameSince && !logoutSaid) {
            const uint64_t t = GetTickCount64();
            if (isZoning()) {   // a zone, however slow, is not a logout on the 10 s clock; the 60 s one counts on
                zoningSeen = true;
                zoningAt = t;
                if (!packetKnown) notInGameSince = t;
            }
            const char* how = tf::logoutReason(logoutByPacket, packetKnown, t - (packetKnown ? leftGameAt : notInGameSince));
            if (!charName.empty() && how) {
                logoutSaid = true;
                packetKnown = false;
                logoutByPacket = false;
                info("character: none (logged out: " + std::string(how) + "); " + charName + "'s settings stay in use, and a change is saved when this character is back in game");
            }
        }
        syncEnabled();
        for (;;) {
            std::vector<std::string> cmd;
            {
                std::lock_guard<std::mutex> lock(cmdMutex);
                if (cmdQueue.empty()) break;
                cmd = std::move(cmdQueue.front());
                cmdQueue.pop_front();
            }
            runCommand(cmd);
        }
        const uint64_t now = GetTickCount64();
        shapeTick(now);
        screenTick(now);
        const double q0 = tf::qpcMs();
        // One limit for TrueFont as a whole: each side's size rule counts the other's bytes (spriteswap.h chatFrame, spriteFrame).
        if (installer) tf::chatFrame(*installer, sprites, frame, now, loggedIn);
        const double q1 = tf::qpcMs();
        if (sprites) tf::spriteFrame(*sprites, installer, settings.chat().on, frame, now, loggedIn);   // Chat off holds nothing
        const double q2 = tf::qpcMs();
#ifdef TF_DEV
        selftest.tick();
#endif
        const bool panelWasOpen = panel.open;
        const double q3 = tf::qpcMs();
        panelTick();
        const double q4 = tf::qpcMs();
        frameCosts.add(now, q1 - q0, q2 - q1, panelWasOpen, q4 - q3, panel.open);
        applyPresetWhenReady();
        takePresetOutcomes();
        for (const auto& n : disk.takeNotices()) chat(n.text, n.color);
        if (logStarted && fileLog.takeWriteWarning()) chat("the log cannot be written (" + logPathText() + ").", 0x68);
        if (logStarted && fileLog.takeTrimWarning()) chat("the log is over its size cap and cannot be trimmed (" + logPathText() + ").", 0x68);
    }
};

extern "C" {
__declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char*) { return new TrueFont(); }
__declspec(dllexport) void __stdcall expDestroyPlugin(void* instance) { delete static_cast<TrueFont*>(instance); }
__declspec(dllexport) double __stdcall expGetInterfaceVersion() { return ASHITA_INTERFACE_VERSION; }
}
