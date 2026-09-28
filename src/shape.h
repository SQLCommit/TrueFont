// Aspect/Size redirects glyph-scale operands to four floats in a separate 4 KB page.
// Nameplate, star and damage draws: Sep-10 RVAs 86210, 86900, 41AF0.
// Native c = 0.00234375f (0x3B19999A); W = c*a*s, H = c*s, s = Size/100.
// For rho = window H/W, a is 1 (Game), 4*rho/3 (4:3), or rho (True).
//
// Only the render callback writes code, using guarded 4-byte CAS. A split-lock fault refuses the group.
// Set floats before installing operands; restore c before removing them. Other writers retain ownership.
// The page outlives the DLL if any previously patched site is not verified stock at full release,
// so nested patches can safely restore saved operands. Later loads recognize tagged TFKS pages.
#pragma once
#include "sigs.h"
#include "tf_mem.h"
#include "swaplogic.h"
#include "screen.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace tf {

inline constexpr uint32_t kShapeConstBits = 0x3B19999Au;   // 0.00234375f, the game's glyph scale
inline constexpr int kShapeGroupCount = 2;                 // Nameplates, Damage numbers
inline constexpr int kShapeSiteCount = 3;
enum class ShapeGroup : uint8_t { Plates, Damage };
inline const char* shapeGroupName(ShapeGroup g) { return g == ShapeGroup::Plates ? "Nameplates" : "Damage numbers"; }

// Aspect (settings <g>_aspect): Game (the game's own width), 4:3 (as on a 4:3 screen), True (the font's own shape).
enum class Aspect : uint8_t { Game, FourThree, True };
inline const char* aspectKey(Aspect a) { return a == Aspect::FourThree ? "4:3" : a == Aspect::True ? "true" : "game"; }
inline const char* aspectName(Aspect a) { return a == Aspect::FourThree ? "4:3" : a == Aspect::True ? "True" : "Game"; }
// Size (settings <g>_size): a percentage of the game's size, 50..200 in steps of 10.
inline constexpr int kSizeMin = 50, kSizeMax = 200, kSizeStep = 10, kSizeDefault = 100;
inline bool sizeAllowed(int pct) { return pct >= kSizeMin && pct <= kSizeMax && pct % kSizeStep == 0; }
inline bool shapeIsStock(Aspect a, int sizePct) { return a == Aspect::Game && sizePct == kSizeDefault; }

// Window H/W, falling back to the viewport, clamped to [1/8, 2]; zero if neither is valid.
inline double shapeRho(const ScreenSizes& s) {
    double w = 0, h = 0;
    if (plausibleSize(s.clientW, s.clientH)) { w = s.clientW; h = s.clientH; }
    else if (plausibleSize(s.vpW, s.vpH)) { w = s.vpW; h = s.vpH; }
    if (w <= 0) return 0;
    const double r = h / w;
    return r < 0.125 ? 0.125 : r > 2.0 ? 2.0 : r;
}
// The width relative to the game's: Game 1, 4:3 (4/3) x rho, True rho (1 while rho is not known).
inline double aspectFactor(Aspect a, double rho) {
    if (rho <= 0) return 1.0;
    return a == Aspect::FourThree ? rho * 4.0 / 3.0 : a == Aspect::True ? rho : 1.0;
}
inline float shapeWidth(float c, Aspect a, double rho, int sizePct) { return float(double(c) * aspectFactor(a, rho) * sizePct / 100.0); }
inline float shapeHeight(float c, int sizePct) { return float(double(c) * sizePct / 100.0); }

// Name the conflicting plugin only when its DLL is loaded.
inline bool nameplateLoaded() { return GetModuleHandleA("Nameplate.dll") != nullptr; }
inline const char* shapeTakenNote(bool nameplate) {
    return nameplate ? "Another plugin (Nameplate) already changes how these are drawn. Unload it to use this."
                     : "Another plugin already changes how these are drawn. Unload it to use this.";
}
inline std::string shapeTakenChat(ShapeGroup g, bool nameplate) {
    const bool dmg = g == ShapeGroup::Damage;
    const std::string what = dmg ? "the damage numbers" : "the names";
    return std::string(dmg ? "Damage" : "Nameplates") + ": Aspect and Size are off " +
           (nameplate ? "while the Nameplate plugin is loaded (it already sizes " + what + "). Unload Nameplate to use them."
                      : "because another plugin already changes how " + what + " are drawn (the log says where).");
}

// The three sites. Offsets are from the site's start; `callW`/`callH` are the E8s whose targets are pinned.
// Sep-10: nameA 86378 (W 8638C, H 863BD), nameB 86A5C (86A70, 86AA3), damage 41B64 (41B8A, 41BC4).
struct ShapeSiteDef {
    const char* name;
    const char* anchor;
    const char* site;
    uint32_t wOff, hOff, callW, callH;
    ShapeGroup group;
    bool names;   // the calls are the app getters (+18 bgW, +1A bgH); else the damage numbers' viewport entry
};
inline constexpr ShapeSiteDef kShapeSites[kShapeSiteCount] = {
    {"nameA (the names, 86210)",
     "8B 54 24 34 89 54 24 18 E8 ?? ?? ?? ?? 25 FF FF 00 00 89 44 24 10 DB 44 24 10 D8 05 ?? ?? ?? ?? D8 5C 24 18 DF E0 F6 C4 05 0F 8B ?? ?? ?? ??",
     "E8 ?? ?? ?? ?? 25 FF FF 00 00 89 44 24 10 DB 44 24 10 D8 0D ?? ?? ?? ?? D8 8C 24 E0 06 00 00 D9 5C 24 4C E8 ?? ?? ?? ?? 8B BC 24 DC 06 00 00 25 FF FF "
     "00 00 89 44 24 10 83 C9 FF DB 44 24 10 33 C0 F2 AE D8 0D ?? ?? ?? ?? D8 8C 24 E0 06 00 00",
     0x14, 0x45, 0x00, 0x23, ShapeGroup::Plates, true},
    {"nameB (the star row, 86900)",
     "8B 54 24 2C 89 54 24 14 E8 ?? ?? ?? ?? 25 FF FF 00 00 89 44 24 10 DB 44 24 10 D8 05 ?? ?? ?? ?? D8 5C 24 14 DF E0 F6 C4 05 0F 8B ?? ?? ?? ??",
     "E8 ?? ?? ?? ?? 25 FF FF 00 00 89 44 24 10 DB 44 24 10 D8 0D ?? ?? ?? ?? D8 8C 24 D4 06 00 00 D9 5C 24 38 E8 ?? ?? ?? ?? 25 FF FF 00 00 8B AC 24 D0 06 "
     "00 00 89 44 24 10 8B FD DB 44 24 10 83 C9 FF 33 C0 F2 AE D8 0D ?? ?? ?? ?? D8 8C 24 D4 06 00 00",
     0x14, 0x47, 0x00, 0x23, ShapeGroup::Plates, true},
    {"damage (the damage numbers, 41AF0)",
     "8D 4C 24 10 89 44 24 1C 8D 54 24 08 51 8D 44 24 3C 52 8D 4C 24 1C 50 51 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ??",
     "A1 ?? ?? ?? ?? 8B 88 C8 0D 00 00 E8 ?? ?? ?? ?? 8B 50 08 A1 ?? ?? ?? ?? 89 54 24 0C DB 44 24 0C D8 4C 24 08 D8 0D ?? ?? ?? ?? D9 5C 24 20 8B 88 C8 0D 00 00 "
     "E8 ?? ?? ?? ?? 8B 40 0C 8B 0D ?? ?? ?? ?? 89 44 24 0C 8B F1 DB 44 24 0C 8B D6 81 E2 FF FF FF 00 D8 4C 24 08 81 FA 80 80 80 00 D8 0D ?? ?? ?? ?? D9 5C 24 24",
     0x26, 0x60, 0x0B, 0x34, ShapeGroup::Damage, false},
};
inline constexpr const char* kShapeGetterW = "A1 ?? ?? ?? ?? 66 8B 40 18 C3";   // bgW (app+0x18)
inline constexpr const char* kShapeGetterH = "A1 ?? ?? ?? ?? 66 8B 40 1A C3";   // bgH (app+0x1A)
inline constexpr const char* kShapeViewportEntry = "33 C0 8A 81 58 01 00 00 8D 04 40 8D 84 C1 98 00 00 00 C3";   // &viewport[[ecx+158]]

// Unavailable: missing/ambiguous anchor; Refused: contradictory calls or constant; Taken: foreign bytes.
// Partial: one operand ours, one stock. Leftover: a tagged page from an earlier instance.
enum class ShapeSiteState : uint8_t { Unavailable, Refused, Taken, Stock, Ours, Partial, Leftover };
inline const char* shapeSiteStateText(ShapeSiteState s) {
    switch (s) {
    case ShapeSiteState::Unavailable: return "unavailable";
    case ShapeSiteState::Refused: return "REFUSED (contradictory)";
    case ShapeSiteState::Taken: return "taken (another writer's bytes)";
    case ShapeSiteState::Stock: return "stock";
    case ShapeSiteState::Ours: return "ours";
    case ShapeSiteState::Partial: return "partly ours";
    case ShapeSiteState::Leftover: return "a leftover of an earlier TrueFont";
    }
    return "?";
}

struct ShapeSite {
    uint32_t rva = 0;          // the site's start (0: the anchor did not resolve)
    uint32_t wOp = 0, hOp = 0; // the operands' RVAs
    ShapeSiteState state = ShapeSiteState::Unavailable;   // in the image the resolver read
    uint32_t w = 0, h = 0;     // the operands as read
    std::string why;           // an unavailable, refused or taken site's reason (the log)
};
struct ShapeResolved {
    ShapeSite site[kShapeSiteCount];
    uint32_t constRva = 0;     // the constant (Sep-10 32ADE0); 0 while no site's H operand names it
    int refs = 0;              // references to base + constRva in the code (supporting: 6 on the four dumps)
    uint32_t rdataLo = 0, rdataHi = 0;   // .rdata's RVA range (a claimed constant must lie there)
    uintptr_t base = 0;
    bool contradictory = false;          // the sites disagree on the constant, or its bits are not 0x3B19999A: all refuse
};

inline std::string hexBytes(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += fmt(i ? " %02X" : "%02X", p[i]);
    return s;
}
// Whose an operand is: the game's constant, this instance's page, or neither.
inline const char* shapeOperandWhose(uint32_t v, uint32_t stock, uint32_t ours) {
    if (stock && v == stock) return "stock";
    if (ours && v == ours) return "ours";
    return "neither stock nor ours";
}

// Resolve a memory-layout image. A known appSlot also constrains both getter calls.
inline ShapeResolved resolveShape(const uint8_t* image, size_t size, uintptr_t base, uint32_t appSlot, std::vector<std::string>& log) {
    ShapeResolved r;
    r.base = base;
    const CodeRange code = codeRange(image, size);
    peSection(image, size, ".rdata", r.rdataLo, r.rdataHi);   // swaplogic.h
    const auto inImage = [&](uint32_t v) { return v >= base && uint64_t(v) - base + 4 <= size; };
    bool matched[kShapeSiteCount] = {};
    for (int i = 0; i < kShapeSiteCount; i++) {
        const ShapeSiteDef& d = kShapeSites[i];
        ShapeSite& s = r.site[i];
        const std::vector<int> anchor = parsePattern(d.anchor), pat = parsePattern(d.site);
        const auto hits = findSig(image, size, anchor, code, 3);
        if (hits.size() != 1) {
            s.why = fmt("its anchor matched %zu times, not once (a game version TrueFont does not know)", hits.size());
            continue;
        }
        const size_t at = hits[0] + anchor.size();
        s.rva = uint32_t(at);
        s.wOp = uint32_t(at + d.wOff);
        s.hOp = uint32_t(at + d.hOff);
        if (!matchesAt(image, size, at, pat)) {
            s.state = ShapeSiteState::Taken;
            s.why = "the anchor is there but the site is not the game's code: " + hexBytes(image + at, (std::min)(size_t(16), size - at));
            continue;
        }
        matched[i] = true;
        s.w = rd32(image, size, s.wOp);
        s.h = rd32(image, size, s.hOp);
        const uint32_t tW = relTarget(image, size, at + d.callW + 1), tH = relTarget(image, size, at + d.callH + 1);
        bool pinned = false;
        if (d.names) {
            const bool shapes = matchesAt(image, size, tW, parsePattern(kShapeGetterW)) && matchesAt(image, size, tH, parsePattern(kShapeGetterH));
            const uint32_t slotW = rd32(image, size, size_t(tW) + 1), slotH = rd32(image, size, size_t(tH) + 1);
            pinned = tW && tH && shapes && slotW == slotH && (!appSlot || slotW == base + appSlot);
            if (!pinned) s.why = fmt("its calls (%06X, %06X) are not the app getters +18/+1A of the app slot", tW, tH);
        } else {
            const std::vector<int> entry = parsePattern(kShapeViewportEntry);
            pinned = tW && tH && tW == tH && matchesAt(image, size, tW, entry);
            if (!pinned) s.why = fmt("its calls (%06X, %06X) are not the one viewport-entry function", tW, tH);
        }
        if (!pinned) { s.state = ShapeSiteState::Refused; matched[i] = false; }
    }
    // The constant: the H operands that name the image; the one most of them name (a tie refuses all).
    uint32_t cand[kShapeSiteCount] = {};
    int votes[kShapeSiteCount] = {}, nc = 0;
    for (int i = 0; i < kShapeSiteCount; i++) {
        if (!matched[i] || !inImage(r.site[i].h)) continue;
        int k = 0;
        while (k < nc && cand[k] != r.site[i].h) k++;
        if (k == nc) cand[nc++] = r.site[i].h;
        ++votes[k];
    }
    int best = -1;
    bool tie = false;
    for (int k = 0; k < nc; k++) {
        if (best < 0 || votes[k] > votes[best]) { best = k; tie = false; }
        else if (votes[k] == votes[best]) tie = true;
    }
    if (best >= 0 && !tie) {
        const uint32_t c = uint32_t(cand[best] - base);
        if (c < r.rdataLo || c + 4 > r.rdataHi || rd32(image, size, c) != kShapeConstBits) {
            r.contradictory = true;
            log.push_back(fmt("shape: the H operands name %06X, which %s; Aspect and Size refuse on this game version", c,
                              c < r.rdataLo || c + 4 > r.rdataHi ? "is not in .rdata" : fmt("holds %08X, not 0x3B19999A (0.00234375)", rd32(image, size, c)).c_str()));
        } else r.constRva = c;
    } else if (tie) {
        r.contradictory = true;
        log.push_back("shape: the sites' H operands name different constants; Aspect and Size refuse on this game version");
    }
    if (r.constRva) {
        const uint32_t v = uint32_t(base + r.constRva);
        for (size_t i = code.lo; i + 4 <= code.hi; i++)
            if (rd32(image, size, i) == v) ++r.refs;
    }
    for (int i = 0; i < kShapeSiteCount; i++) {
        ShapeSite& s = r.site[i];
        if (!matched[i]) {
            if (s.state != ShapeSiteState::Taken && s.state != ShapeSiteState::Refused) s.state = ShapeSiteState::Unavailable;
            continue;
        }
        if (r.contradictory) { s.state = ShapeSiteState::Refused; s.why = "the constant contradicts"; continue; }
        const uint32_t stock = r.constRva ? uint32_t(base + r.constRva) : 0;
        if (stock && s.w == stock && s.h == stock) s.state = ShapeSiteState::Stock;
        else if (!inImage(s.w) && !inImage(s.h)) { s.state = ShapeSiteState::Leftover; s.why = "both operands name memory outside the image (checked at run time)"; }
        else { s.state = ShapeSiteState::Taken; s.why = fmt("the operands are W %08X, H %08X, not both the constant's %08X", s.w, s.h, stock); }
    }
    for (int i = 0; i < kShapeSiteCount; i++) {
        const ShapeSite& s = r.site[i];
        const bool split = s.rva && ((s.wOp & 63) > 60 || (s.hOp & 63) > 60);   // an operand across a cache line
        log.push_back(fmt("shape: %s at %06X, W operand %06X, H operand %06X: %s%s%s%s", kShapeSites[i].name, s.rva, s.wOp, s.hOp, shapeSiteStateText(s.state),
                          s.why.empty() ? "" : ": ", s.why.c_str(), split ? " (an operand crosses a 64-byte line: its CAS is a split lock)" : ""));
    }
    log.push_back(fmt("shape: the constant 0.00234375 at %06X (Sep-10 32ADE0; HorizonXI 328DE0), %d references in the code (6 expected)%s", r.constRva, r.refs,
                      r.constRva ? "" : " NOT RESOLVED"));
    return r;
}

// The game's memory as ShapePatch sees it: the running client, or a writable copy of a client image.
struct ShapeMem {
    virtual ~ShapeMem() = default;
    virtual bool read(uintptr_t at, void* out, size_t n) = 0;
    // One 4-byte operand of game code: `replacement` only while it holds `expected` (true when it swapped).
    virtual bool swap32(uintptr_t at, uint32_t expected, uint32_t replacement) = 0;
};

// Guard CAS with SEH and flush the instruction cache; count required VirtualProtect calls.
inline volatile long g_shapeProtectCalls = 0;
inline bool pageWritable(uintptr_t at, size_t n) {
    for (uintptr_t p = at & ~uintptr_t(0xFFF); p < at + n; p += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
        const DWORD w = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & w) || (mbi.Protect & PAGE_GUARD)) return false;
    }
    return true;
}
inline bool casRaw32(uintptr_t at, uint32_t expected, uint32_t replacement) {
    __try {
        return uint32_t(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(at), LONG(replacement), LONG(expected))) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
inline bool swapCode32(uintptr_t at, uint32_t expected, uint32_t replacement) {
    if (!at) return false;
    const bool open = pageWritable(at, 4);
    DWORD old = 0;
    if (!open) {
        _InterlockedIncrement(&g_shapeProtectCalls);
        if (!VirtualProtect(reinterpret_cast<LPVOID>(at), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    }
    const bool ok = casRaw32(at, expected, replacement);
    if (!open) {
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), 4, old, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), 4);
    uint32_t back = 0;
    return ok && readRaw(at, &back, 4) && back == replacement;
}
struct LiveShapeMem final : ShapeMem {
    bool read(uintptr_t at, void* out, size_t n) override { return readRaw(at, out, n); }
    bool swap32(uintptr_t at, uint32_t expected, uint32_t replacement) override { return swapCode32(at, expected, replacement); }
};

// The page: a header naming it, then the four floats.
inline constexpr char kShapeTag[4] = {'T', 'F', 'K', 'S'};
inline constexpr uint32_t kShapePageSize = 0x1000;
struct ShapePageHeader {
    char tag[4];
    uint32_t pid;
    uint32_t cookie;      // this instance's (the log)
    uint32_t stock;       // the stock operand value (base + the constant): a later TrueFont puts it back (reclaim)
    uint32_t version;     // 1
    uint32_t siteRva[kShapeSiteCount];
};
inline constexpr uint32_t kShapeSlotBase = 0x40;   // plates W, plates H, damage W, damage H
inline uint32_t shapeSlot(ShapeGroup g, bool height) { return kShapeSlotBase + uint32_t(g) * 8 + (height ? 4u : 0u); }

// Shape settings are independent of each group's font-replacement toggle.
struct ShapeWant {
    bool on = false;
    Aspect aspect = Aspect::Game;
    int size = kSizeDefault;
    bool gameFont = false;   // the group's own TrueFont box is off: its letters are the game's (Sharpness Auto leaves Aspect and Size out)
    bool active() const { return on && !shapeIsStock(aspect, size); }
};
// Scale Auto sharpness for replacement glyphs only; enlarging native art adds memory without detail.
inline bool autoKTakesShape(const ShapeWant& w, bool inEffect) { return inEffect && !w.gameFont; }
// Hold native geometry during identity comparisons so only texture changes affect the result.
// Player settings remain unchanged.
inline ShapeWant shapeWantForRun(const ShapeWant& player, bool hold) {
    if (!hold) return player;
    ShapeWant w = player;
    w.aspect = Aspect::Game;
    w.size = kSizeDefault;
    return w;
}
inline std::string shapeWantsText(const ShapeWant w[kShapeGroupCount]) {
    std::string t;
    for (int g = 0; g < kShapeGroupCount; g++) {
        t += g ? ", " : "";
        t += shapeGroupName(ShapeGroup(g));
        t += w[g].on ? fmt(" %s %d%%", aspectName(w[g].aspect), w[g].size) : std::string(" off");
    }
    return t;
}
inline std::string shapeHoldLine(bool held, const ShapeWant player[kShapeGroupCount]) {
    return held ? "selftest: Aspect and Size held at the game's (Game, 100%: the game's own code) for the identity shots (T3, D3, D8) and the shots they are judged "
                  "against; the player's (" + shapeWantsText(player) + ") come back after D4, or wherever the run ends"
                : "selftest: Aspect and Size are the player's again (" + shapeWantsText(player) + ")";
}
struct ShapeReleaseReport {
    int restored = 0;                 // operands put back to stock
    std::vector<std::string> left;    // sites left (the log)
    bool pageFreed = false, pageKept = false;
    bool touched = false;             // anything was written (floats included)
};

class ShapePatch {
public:
    enum class GroupState : uint8_t { Available, Taken, Unavailable };
    using LogFn = std::function<void(const char* level, const std::string&)>;

    ShapePatch() = default;
    ShapePatch(const ShapePatch&) = delete;
    ShapePatch& operator=(const ShapePatch&) = delete;
    ~ShapePatch() = default;   // the page is never freed here: only release() decides

    void setup(const ShapeResolved& r, ShapeMem* mem, LogFn log = nullptr) {
        r_ = r;
        mem_ = mem;
        log_ = std::move(log);
        for (int i = 0; i < kShapeSiteCount; i++) {
            live_[i] = r_.site[i].state;
            usable_[i] = r_.site[i].rva != 0 && r_.site[i].state != ShapeSiteState::Refused;
            pat_[i] = parsePattern(kShapeSites[i].site);
        }
        ready_ = mem_ != nullptr;
    }
    bool ready() const { return ready_; }
    uintptr_t page() const { return page_; }
    const ShapeResolved& resolved() const { return r_; }
    ShapeSiteState siteState(int i) const { return live_[i]; }
    uint32_t constAddr() const { return r_.constRva ? uint32_t(r_.base + r_.constRva) : 0; }

    GroupState groupState(ShapeGroup g) const {
        bool taken = false;
        for (int i = 0; i < kShapeSiteCount; i++) {
            if (kShapeSites[i].group != g) continue;
            if (!usable_[i] || r_.contradictory) return GroupState::Unavailable;
            if (live_[i] == ShapeSiteState::Taken || live_[i] == ShapeSiteState::Leftover) taken = true;
        }
        return taken ? GroupState::Taken : GroupState::Available;
    }
    // Every site of the group points at this instance's floats.
    bool applied(ShapeGroup g) const {
        if (!page_) return false;
        for (int i = 0; i < kShapeSiteCount; i++)
            if (kShapeSites[i].group == g && live_[i] != ShapeSiteState::Ours) return false;
        return true;
    }
    // Applied and wanted: Sharpness Auto takes this group's Aspect and Size.
    bool inEffect(ShapeGroup g) const { return applied(g) && want_[int(g)].active(); }
    float floatNow(ShapeGroup g, bool height) const {
        float v = 0;
        if (page_) std::memcpy(&v, reinterpret_cast<const void*>(page_ + shapeSlot(g, height)), 4);
        return v;
    }
    // The group was refused since this was last asked (it wanted a write and its site is taken or unavailable).
    bool takeRefusal(ShapeGroup g) {
        const bool r = refused_[int(g)];
        refused_[int(g)] = false;
        return r;
    }

    // Render thread only. Recheck ownership periodically, reclaim leftovers, and synchronize operands and floats.
    void sync(const ShapeWant want[kShapeGroupCount], double rho, bool showOriginal, bool recheck) {
        if (!ready_) return;
        const bool first = !checked_;
        if (recheck || first) refresh();
        for (int g = 0; g < kShapeGroupCount; g++) {
            const ShapeWant& a = want_[g];
            if (a.on != want[g].on || a.aspect != want[g].aspect || a.size != want[g].size) wasRefused_[g] = false;   // asked again: a refusal is new
            want_[g] = want[g];
        }
        rho_ = rho;
        showOriginal_ = showOriginal;
        for (int g = 0; g < kShapeGroupCount; g++) syncGroup(ShapeGroup(g), recheck || first);
    }

    // Restore floats before operands. Off-thread release changes floats only; a running callback permits no writes.
    // Free the page only on full release after every previously patched site is verified stock.
    ShapeReleaseReport release(ReleaseMode mode, bool frameLeft, bool gameGone, bool gameThread = true) {
        ShapeReleaseReport rep;
        if (!ready_ || mode == ReleaseMode::Nothing || gameGone) return rep;
        if (!frameLeft) {   // the frame callback may still be inside sync() writing the floats: nothing here (the DLL is pinned)
            if (page_) {
                rep.pageKept = true;
                say("warn", "shape: release inside a running frame: nothing written (the frame callback may still be writing); the operands, the page and its floats "
                            "stay until the game closes");
            }
            return rep;
        }
        if (page_) {
            setStockFloats();
            rep.touched = true;
        }
        if (!gameThread) {
            if (page_) {
                rep.pageKept = true;
                say("warn", "shape: release off the game thread: the floats are the game's; the operands and the page stay (the next load puts the game's code back)");
            }
            return rep;
        }
        if (!page_) return rep;
        for (int i = 0; i < kShapeSiteCount; i++) rep.restored += restoreSite(i);
        refresh();
        bool allStock = true;
        for (int i = 0; i < kShapeSiteCount; i++) {
            if (!everOurs_[i]) continue;
            if (live_[i] != ShapeSiteState::Stock) {
                allStock = false;
                rep.left.push_back(fmt("%s: %s", kShapeSites[i].name, shapeSiteStateText(live_[i])));
            }
        }
        if (mode == ReleaseMode::Full && allStock) {
            VirtualFree(reinterpret_cast<LPVOID>(page_), 0, MEM_RELEASE);
            page_ = 0;
            rep.pageFreed = true;
            say("info", fmt("shape: release: %d operand(s) put back; every site reads stock; the page is freed", rep.restored));
        } else {
            rep.pageKept = true;
            say("info", fmt("shape: release (%s): %d operand(s) put back; the page %08X is kept (4 KB, its floats the game's 0.00234375)%s%s", releaseModeText(mode), rep.restored,
                            unsigned(page_), rep.left.empty() ? "" : ": not stock: ", joined(rep.left).c_str()));
        }
        return rep;
    }

    // Report live operands, ownership and scale floats.
    std::vector<std::string> diagLines() const {
        std::vector<std::string> out;
        if (!ready_) { out.push_back("shape: not set up"); return out; }
        for (int i = 0; i < kShapeSiteCount; i++) {
            const ShapeSite& s = r_.site[i];
            std::string t = fmt("site %s at RVA %06X: %s; ", kShapeSites[i].name, s.rva, shapeSiteStateText(live_[i]));
            uint32_t w = 0, h = 0;
            if (!s.rva) t += "not found" + (s.why.empty() ? std::string() : " (" + s.why + ")");
            else if (readSite(i, w, h)) {
                const uint32_t stock = constAddr();
                t += fmt("W operand at RVA %06X holds %s (%s), H operand at RVA %06X holds %s (%s)", s.wOp, hexBytes(reinterpret_cast<const uint8_t*>(&w), 4).c_str(),
                         shapeOperandWhose(w, stock, ours(i, false)), s.hOp, hexBytes(reinterpret_cast<const uint8_t*>(&h), 4).c_str(), shapeOperandWhose(h, stock, ours(i, true)));
            } else {
                uint8_t head[16] = {};
                const bool read = mem_->read(r_.base + s.rva, head, sizeof head);
                t += read ? "its code is not the game's; it starts " + hexBytes(head, sizeof head) : std::string("its code could not be read");
            }
            out.push_back(t);
        }
        out.push_back(fmt("shape: the page %08X; VirtualProtect calls for a CAS so far: %ld (0: the code pages were writable)", unsigned(page_), long(g_shapeProtectCalls)));
        for (int g = 0; g < kShapeGroupCount; g++)
            out.push_back(fmt("shape: %s: Aspect %s, Size %d%%, %s, rho %.4f, floats W %.8f H %.8f%s", shapeGroupName(ShapeGroup(g)), aspectName(want_[g].aspect), want_[g].size,
                              applied(ShapeGroup(g)) ? "applied" : groupState(ShapeGroup(g)) == GroupState::Taken ? "taken" : groupState(ShapeGroup(g)) == GroupState::Unavailable ? "unavailable" : "stock",
                              rho_, double(floatNow(ShapeGroup(g), false)), double(floatNow(ShapeGroup(g), true)), showOriginal_ ? " (Show Original: the game's)" : ""));
        return out;
    }

private:
    void say(const char* level, const std::string& s) const { if (log_) log_(level, s); }
    static std::string joined(const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); i++) s += (i ? "; " : "") + v[i];
        return s;
    }
    uintptr_t op(int i, bool height) const { return r_.base + (height ? r_.site[i].hOp : r_.site[i].wOp); }
    uint32_t ours(int i, bool height) const { return page_ ? uint32_t(page_ + shapeSlot(kShapeSites[i].group, height)) : 0; }
    // The game's constant, read once from the stock operand's target (resolve checked its bits are 0x3B19999A).
    float c() {
        if (!cKnown_ && constAddr()) {
            uint32_t bits = 0;
            if (mem_->read(constAddr(), &bits, 4) && bits == kShapeConstBits) { std::memcpy(&c_, &bits, 4); cKnown_ = true; }
        }
        if (cKnown_) return c_;
        const uint32_t bits = kShapeConstBits;   // not readable: the value resolve checked
        float f = 0;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    void setFloat(ShapeGroup g, bool height, float v) {
        if (!page_) return;
        float* p = reinterpret_cast<float*>(page_ + shapeSlot(g, height));
        if (std::memcmp(p, &v, 4) != 0) *p = v;   // an aligned 4-byte store: a draw reads the old or the new value
    }
    void setStockFloats() {
        for (int g = 0; g < kShapeGroupCount; g++) { setFloat(ShapeGroup(g), false, c()); setFloat(ShapeGroup(g), true, c()); }
    }
    void wantedFloats(ShapeGroup g) {
        const ShapeWant& w = want_[int(g)];
        if (showOriginal_ || !w.active()) { setFloat(g, false, c()); setFloat(g, true, c()); return; }
        setFloat(g, false, shapeWidth(c(), w.aspect, rho_, w.size));
        setFloat(g, true, shapeHeight(c(), w.size));
    }
    bool ensurePage() {
        if (page_) return true;
        void* p = VirtualAlloc(nullptr, kShapePageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) { say("error", fmt("shape: the float page could not be allocated (error %lu)", GetLastError())); return false; }
        page_ = reinterpret_cast<uintptr_t>(p);
        ShapePageHeader hd{};
        std::memcpy(hd.tag, kShapeTag, 4);
        hd.pid = GetCurrentProcessId();
        hd.cookie = uint32_t(page_) ^ uint32_t(GetTickCount64()) ^ 0x5A5A0000u;
        hd.stock = constAddr();
        hd.version = 1;
        for (int i = 0; i < kShapeSiteCount; i++) hd.siteRva[i] = r_.site[i].rva;
        std::memcpy(p, &hd, sizeof hd);
        setStockFloats();
        say("info", fmt("shape: the float page is at %08X (4 KB, cookie %08X)", unsigned(page_), hd.cookie));
        return true;
    }
    // Recognize a previous instance's tagged page, including a partially restored operand pair.
    bool leftover(int i, uint32_t w, uint32_t h, uint32_t& stock) const {
        const uint32_t sc = constAddr();
        const bool wS = sc && w == sc, hS = sc && h == sc;
        if (wS && hS) return false;
        const uintptr_t p = (wS ? h : w) & ~uintptr_t(0xFFF);
        if (!p || p == page_) return false;
        if (!wS && w != p + shapeSlot(kShapeSites[i].group, false)) return false;
        if (!hS && h != p + shapeSlot(kShapeSites[i].group, true)) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) return false;
        ShapePageHeader hd{};
        if (!readRaw(p, &hd, sizeof hd) || std::memcmp(hd.tag, kShapeTag, 4) != 0 || hd.pid != GetCurrentProcessId() || hd.version != 1) return false;
        // Its stock value must be the constant: known, or the page's own claim checked against the image's .rdata.
        if (constAddr()) { if (hd.stock != constAddr()) return false; }
        else {
            const uint32_t rva = uint32_t(hd.stock - r_.base);
            uint32_t bits = 0;
            if (hd.stock < r_.base || rva < r_.rdataLo || rva + 4 > r_.rdataHi || !mem_->read(hd.stock, &bits, 4) || bits != kShapeConstBits) return false;
        }
        stock = hd.stock;
        return true;
    }
    // Site i's live bytes: false when they are not the game's code (another writer's block); else its two operands.
    bool readSite(int i, uint32_t& w, uint32_t& h) const {
        uint8_t buf[0x80] = {};
        const size_t n = pat_[i].size();
        if (n > sizeof buf || !mem_->read(r_.base + r_.site[i].rva, buf, n) || !matchesAt(buf, n, 0, pat_[i])) return false;
        std::memcpy(&w, buf + kShapeSites[i].wOff, 4);
        std::memcpy(&h, buf + kShapeSites[i].hOff, 4);
        return true;
    }
    // The live state of site i (its pattern and both operands); `oursSeen_`: either operand names this instance's page.
    ShapeSiteState classify(int i, uint32_t& w, uint32_t& h, uint32_t& leftStock) {
        oursSeen_[i] = false;
        if (!readSite(i, w, h)) return ShapeSiteState::Taken;
        oursSeen_[i] = page_ && (w == ours(i, false) || h == ours(i, true));
        // A constant not known yet (every site was elsewhere at load): learned from a stock-looking site.
        if (!constAddr() && w == h && w >= r_.base && w - r_.base >= r_.rdataLo && w - r_.base + 4 <= r_.rdataHi) {
            uint32_t bits = 0;
            if (mem_->read(w, &bits, 4) && bits == kShapeConstBits) {
                r_.constRva = uint32_t(w - r_.base);
                say("info", fmt("shape: the constant is at %06X (learned from %s)", r_.constRva, kShapeSites[i].name));
            }
        }
        const uint32_t stock = constAddr();
        const bool wS = stock && w == stock, hS = stock && h == stock, wO = page_ && w == ours(i, false), hO = page_ && h == ours(i, true);
        if (wS && hS) return ShapeSiteState::Stock;
        if (wO && hO) return ShapeSiteState::Ours;
        if ((wO && hS) || (wS && hO)) return ShapeSiteState::Partial;
        if (leftover(i, w, h, leftStock)) return ShapeSiteState::Leftover;
        return ShapeSiteState::Taken;
    }
    void refresh() {
        checked_ = true;
        const bool np = nameplateLoaded();
        for (int i = 0; i < kShapeSiteCount; i++) {
            if (!usable_[i] || r_.contradictory) continue;
            uint32_t w = 0, h = 0, left = 0;
            ShapeSiteState st = classify(i, w, h, left);
            if (st == ShapeSiteState::Leftover) {   // an earlier TrueFont's (this process, the sole-instance mutex held): back to stock
                const bool a = w == left || mem_->swap32(op(i, false), w, left), b = h == left || mem_->swap32(op(i, true), h, left);   // a stock half stays
                say(a && b ? "info" : "warn", fmt("shape: %s held an earlier TrueFont's page %08X: put back to stock (W %s, H %s)", kShapeSites[i].name, unsigned((w == left ? h : w) & ~0xFFFu),
                                                  a ? "ok" : "FAILED", b ? "ok" : "FAILED"));
                st = classify(i, w, h, left);
            }
            if (st != live_[i] || !logged_[i]) {
                uint8_t head[16] = {};
                mem_->read(r_.base + r_.site[i].rva, head, sizeof head);
                say(st == ShapeSiteState::Taken ? "warn" : "info",
                    fmt("shape: %s at %06X: %s -> %s (W %08X, H %08X)%s", kShapeSites[i].name, r_.site[i].rva, shapeSiteStateText(live_[i]), shapeSiteStateText(st), w, h,
                        st == ShapeSiteState::Taken ? fmt("; bytes %s%s", hexBytes(head, sizeof head).c_str(), np ? "; Nameplate.dll is loaded" : "").c_str() : ""));
                logged_[i] = true;
            }
            live_[i] = st;
        }
    }
    // Restore only operands still owned by this instance; return the count restored.
    int restoreSite(int i) {
        if (!usable_[i] || !page_ || !constAddr()) return 0;
        uint32_t w = 0, h = 0;
        if (!readSite(i, w, h)) return 0;
        int n = 0;
        for (const bool height : {false, true}) {
            const uint32_t now = height ? h : w;
            if (now != ours(i, height)) continue;
            const bool ok = mem_->swap32(op(i, height), ours(i, height), constAddr());
            n += ok ? 1 : 0;
            if (!ok) say("warn", fmt("shape: %s: the %s operand could not be put back (the CAS failed)", kShapeSites[i].name, height ? "H" : "W"));
        }
        return n;
    }
    void syncGroup(ShapeGroup g, bool recheck) {
        const int gi = int(g);
        const ShapeWant& w = want_[gi];
        // Update floats before operands, even under an external hook that may still execute our operands.
        // Inactive groups and Show Original use the native constant.
        if (page_) wantedFloats(g);
        bool anyOurs = false;
        for (int i = 0; i < kShapeSiteCount; i++)
            if (kShapeSites[i].group == g && oursSeen_[i]) anyOurs = true;
        if (!w.active()) {
            if (anyOurs && (!restoreFailed_[gi] || recheck)) {   // the operands back (a CAS that failed: again once a second)
                int n = 0;
                for (int i = 0; i < kShapeSiteCount; i++)
                    if (kShapeSites[i].group == g) n += restoreSite(i);
                refresh();
                bool still = false;
                for (int i = 0; i < kShapeSiteCount; i++)
                    if (kShapeSites[i].group == g && oursSeen_[i]) still = true;
                restoreFailed_[gi] = still;
                say(still ? "warn" : "info", fmt("shape: %s back to the game's (%d operand(s) put back)%s", shapeGroupName(g), n, still ? "; NOT all: see above" : ""));
            }
            wasRefused_[gi] = false;
            return;
        }
        if (applied(g)) { wasRefused_[gi] = false; return; }
        const GroupState gs = groupState(g);
        if (gs != GroupState::Available) {
            if (!wasRefused_[gi]) {
                wasRefused_[gi] = true;
                refused_[gi] = true;
                say("warn", fmt("shape: %s: Aspect %s, Size %d%% refused: %s", shapeGroupName(g), aspectName(w.aspect), w.size,
                                gs == GroupState::Taken ? "a site is another writer's (see its line above)" : "a site is not available on this game version"));
            }
            return;
        }
        wasRefused_[gi] = false;
        if (!constAddr() || !ensurePage()) return;
        wantedFloats(g);   // the floats before the operands
        bool ok = true;
        for (int i = 0; i < kShapeSiteCount && ok; i++) {
            if (kShapeSites[i].group != g) continue;
            everOurs_[i] = true;
            for (const bool height : {false, true}) {
                uint32_t now = 0;
                if (!mem_->read(op(i, height), &now, 4)) { ok = false; break; }
                if (now == ours(i, height)) continue;
                if (now != constAddr() || !mem_->swap32(op(i, height), constAddr(), ours(i, height))) { ok = false; break; }
            }
        }
        if (!ok) {   // the group's own back
            for (int i = 0; i < kShapeSiteCount; i++)
                if (kShapeSites[i].group == g) restoreSite(i);
            refresh();
            wasRefused_[gi] = refused_[gi] = true;
            say("warn", fmt("shape: %s: a write did not take (the CAS failed); every operand of the group was put back", shapeGroupName(g)));
            return;
        }
        refresh();
        say("info", fmt("shape: %s: Aspect %s, Size %d%% applied (rho %.4f; W %.8f, H %.8f)", shapeGroupName(g), aspectName(w.aspect), w.size, rho_, double(floatNow(g, false)),
                        double(floatNow(g, true))));
    }

    ShapeResolved r_;
    ShapeMem* mem_ = nullptr;
    LogFn log_;
    bool ready_ = false, checked_ = false;
    uintptr_t page_ = 0;
    ShapeSiteState live_[kShapeSiteCount] = {};
    bool usable_[kShapeSiteCount] = {}, everOurs_[kShapeSiteCount] = {}, logged_[kShapeSiteCount] = {}, oursSeen_[kShapeSiteCount] = {};
    std::vector<int> pat_[kShapeSiteCount];
    ShapeWant want_[kShapeGroupCount];
    bool refused_[kShapeGroupCount] = {}, wasRefused_[kShapeGroupCount] = {}, restoreFailed_[kShapeGroupCount] = {};
    double rho_ = 0;
    bool showOriginal_ = false;
    float c_ = 0;
    bool cKnown_ = false;
};

}  // namespace tf
