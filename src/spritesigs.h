// Sprite-font signatures, resolved independently of the chat font:
//   YC/YD: CYyTex constructor/destructor, vtable, cache [47B970], link +38 and texture +40.
//   LB/LP: leaf bind/parse, name +32, record +4C, record dimensions +24/+26 and glyph rects.
//   PF/PD: fontshp/dmgnum page caches, registry 4E1BF8 and list iterators.
//   CR: composite release, count +3C and array +20.
//   PR/DR: page reads at 120590/1207B0, registry slots +1C/+30, calling PF/PD respectively.
// Pinned helpers: cache lookup 3AC80 and registry find 120450. RVAs are for Sep-10.
//
// YC, YD, LB, LP, PF, CR and their derived addresses are required. Missing PD or sigs.h
// support is logged; contradictory results refuse the subsystem. PR/DR gate their own splits.
// All splits require CYyTex allocations to match kRecordBytes before copying records.
#pragma once
#include "sigs.h"
#include "swaplogic.h"

namespace tf {

enum SpriteSigId { SsTexCtor, SsTexDtor, SsLeafBind, SsLeafParse, SsPageFontshp, SsPageDmgnum, SsCompRelease, SsCount };

inline constexpr SigDef kSpriteSigs[SsCount] = {
    {"YC", "tex-ctor", "56 8B F1 E8 ?? ?? ?? ?? 33 C0 C7 06 ?? ?? ?? ?? 89 46 38 89 46 3C 89 46 40 89 46 44 A1 ?? ?? ?? ?? 89 46 34 8B C6 5E C3", 0x39920},
    {"YD", "tex-dtor", "56 8B F1 56 C7 06 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 08 8B 51 38 89 10 8B 46 40 85 C0 74 ?? 8B 08 50 FF 51 08", 0x39970},
    {"LB", "leaf-bind",
     "83 EC 14 53 56 8B F1 57 8D 7E 32 57 E8 ?? ?? ?? ?? 83 C4 04 8D 4C 24 10 57 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8D 44 24 10 50 E8 ?? ?? ?? ?? "
     "33 DB 89 46 4C 3B C3 75 ?? 5F 5E 32 C0 5B 83 C4 14 C3 0F BF 7E 0C 33 C9 33 D2 66 8B 48 24 66 8B 50 26 8A 46 1C 89 7C 24 0C A8 01 75 ?? "
     "89 4C 24 10 89 5C 24 14 DF 6C 24 10 DB 44 24 0C 0F BF 4E 08", 0x11E6D0},
    {"LP", "leaf-parse",
     "51 53 55 56 8B F1 57 8B 7C 24 18 8D 9E C8 00 00 00 BD 04 00 00 00 6A 02 8D 44 24 1C 57 50 E8 ?? ?? ?? ?? 6A 02 8D 4C 24 20 57 51 E8 ?? ?? ?? ?? "
     "66 8B 54 24 30 66 8B 44 24 28 66 89 53 F8 66 89 03 83 C4 18 83 C3 02 4D 75 CC 6A 02 8D 4E 08 57 51 E8 ?? ?? ?? ?? 6A 02 8D 56 0A 57 52 E8 ?? ?? ?? ?? "
     "6A 02 8D 46 0C 57 50 E8 ?? ?? ?? ?? 6A 02 8D 4E 0E 57 51 E8", 0x11E4E0},
    {"PF", "page-fontshp",
     "83 EC 1C 53 55 56 57 8B F9 68 ?? ?? ?? ?? 8D 4C 24 20 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 18 0F 84 ?? ?? ?? ?? "
     "8B 00 33 DB C7 44 24 10 20 00 00 00 8B 00 85 C0 0F 84 ?? ?? ?? ?? 33 ED 83 C7 24 8D 70 04 8B CE E8 ?? ?? ?? ?? 8D 4C 24 14 89 44 24 14 51 8B CE E8 ?? ?? ?? ?? "
     "85 C0 74 ?? 8D 70 32 56 E8 ?? ?? ?? ?? 8D 54 24 20 6A 10 52 56 E8 ?? ?? ?? ?? 83 C4 10 85 C0 74 ?? 56 8D 4C 24 20 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? "
     "8D 44 24 1C 50 E8", 0x120660},
    {"PD", "page-dmgnum",
     "83 EC 1C 53 55 56 57 8B F9 68 ?? ?? ?? ?? 8D 4C 24 20 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 18 0F 84 ?? ?? ?? ?? "
     "8B 00 33 DB C7 44 24 10 20 00 00 00 8B 00 85 C0 0F 84 ?? ?? ?? ?? 33 ED 83 C7 38 8D 70 04 8B CE E8 ?? ?? ?? ?? 8D 4C 24 14 89 44 24 14 51 8B CE E8 ?? ?? ?? ?? "
     "85 C0 74 ?? 8D 70 32 56 E8 ?? ?? ?? ?? 8D 54 24 20 6A 10 52 56 E8 ?? ?? ?? ?? 83 C4 10 85 C0 74 ?? 56 8D 4C 24 20 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? "
     "8D 44 24 1C 50 E8", 0x120880},
    {"CR", "composite-release", "56 57 8B F9 33 F6 66 39 77 3C 76 ?? 8B 47 20 8B 0C B0 85 C9 74 ?? E8 ?? ?? ?? ?? 33 C9 46 66 8B 4F 3C 3B F1 7C", 0x11E2A0},
};

// PR, the fontshp page read (unique on the four dumps: Sep-10 120590, Aug-18 and Aug-03 1205A0, HorizonXI 120180).
inline constexpr SigDef kPageReadSig = {"PR", "page-read", "56 8B F1 8B 46 1C 85 C0 75 ?? E8 ?? ?? ?? ?? 8B 46 1C 85 C0 75 ?? 32 C0 5E C2 0C 00", 0x120590};
inline constexpr uint32_t kPageFontshpSlot = 0x1C;   // registry+1C: fontshp page 0's record (nameplates, the debug labels)
inline constexpr size_t kPageReadCall = 10;          // PR+10: E8 rel32 -> PF (DR+10 -> PD likewise)
// DR, the dmgnum page read (unique on the four builds: Sep-10 1207B0, Aug-18 and Aug-03 1207C0, HorizonXI 1203A0).
inline constexpr SigDef kPageDmgReadSig = {"DR", "page-dmg-read", "56 8B F1 8B 46 30 85 C0 75 ?? E8 ?? ?? ?? ?? 8B 46 30 85 C0 75 ?? 32 C0 5E C2 0C 00", 0x1207B0};
inline constexpr uint32_t kPageDmgnumSlot = 0x30;    // registry+30: dmgnum page 0's record (the damage numbers, 41C20)

// Function bodies reached through a pinned call (offset 0 = the call's target).
inline constexpr PinDef kCacheLookup = {"texture cache lookup",
    "53 56 57 E8 ?? ?? ?? ?? 8B F0 8B 16 85 D2 74 ?? 8B 7C 24 10 8B 42 2C C1 E8 03 A8 01 75 ?? 33 C9 8A 44 0A 0C 8A 1C 39 3A C3 75 ?? 41 83 F9 10 7C ?? "
    "83 F9 10 74 ?? 8D 72 38 8B 52 38 85 D2 75", 0};
inline constexpr PinDef kCacheHead = {"texture cache list head", "8D 41 08 C3", 0};
inline constexpr PinDef kRegistryFind = {"sprite registry find",
    "51 53 55 56 57 8B F9 8B 6F 10 E8 ?? ?? ?? ?? 33 DB 89 44 24 10 85 ED 7E ?? 8D 44 24 10 8B CF 50 E8 ?? ?? ?? ?? 8B 4C 24 18 8B F0 6A 10 51 8D 56 24 52 E8 ?? ?? ?? ?? "
    "83 C4 0C 85 C0 74 ?? 43 3B DD 7C ?? 5F 5E 5D 33 C0 5B 59 C2 04 00 8B CE E8", 0};
inline constexpr PinDef kListBegin = {"list begin", "8B 01 C3", 0};
inline constexpr PinDef kListNext = {"list next",
    "8B 4C 24 04 8B 01 85 C0 75 05 33 C0 C2 04 00 8A 50 14 84 D2 74 0D 8B 00 85 C0 74 EE 8A 50 14 84 D2 75 F3 8B 10 89 11 8B 40 10 C2 04 00", 0};
inline constexpr PinDef kResCompositesPin = {"resource composites", "8D 41 20 C3", 0};

// The offsets pinned above (records.h, sprites.h).
inline constexpr uint32_t kCacheListHead = 0x08;    // cache+8: the first record (0x3AAB0)
inline constexpr uint32_t kRecName = 0x0C;          // record+0C: 16-byte name
inline constexpr uint32_t kRecWidth = 0x24, kRecHeight = 0x26;   // u16: the size UVs are normalised by
inline constexpr uint32_t kRecFlags = 0x2C;         // (flags >> 3) & 1: the lookup skips the record
inline constexpr uint32_t kRecNext = 0x38;          // the cache list link
inline constexpr uint32_t kRegCount = 0x10;         // list object: +0 first node, +10 count
inline constexpr uint32_t kNodeNext = 0x00, kNodeData = 0x10, kNodeDead = 0x14;   // list node
inline constexpr uint32_t kResName = 0x24, kResComposites = 0x20, kResCount = 0x3C;
inline constexpr uint32_t kCompLeaves = 0x04;       // composite+4: the leaf list object (count at +4+10)
inline constexpr uint32_t kLeafW = 0x08, kLeafH = 0x0A, kLeafU = 0x0C, kLeafV = 0x0E, kLeafFlags = 0x1C;
inline constexpr uint32_t kLeafBlend = 0x2D, kLeafName = 0x32, kLeafRecord = 0x4C, kLeafCornerX = 0xC0, kLeafCornerY = 0xC8;
inline constexpr char kFontshpName[] = "font    fontshp ";
inline constexpr char kDmgnumName[] = "menu    dmgnum  ";
inline constexpr char kPnameName[] = "menu    pname   ";   // the party and search lists' job cells (the Job and level tags leaves)

inline constexpr size_t kRecordBytes = 0x48;   // a CYyTex record (the allocation at 1D800)
// `push imm8; call alloc; add esp,4; test eax,eax; jz; mov ecx,eax; call` (the E8 at +16; its rel32 must reach YC).
inline constexpr const char* kRecordAllocPattern = "6A ?? E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 ?? 8B C8 E8";
inline constexpr size_t kRecordAllocCap = 4096;   // the scan's cap (51-52 matches on the four dumps)

struct SpriteResolved {
    uint32_t base = 0;
    uint32_t texCtor = 0, texDtor = 0, leafBind = 0, leafParse = 0, pageFontshp = 0, pageDmgnum = 0, compRelease = 0;
    uint32_t cacheLookup = 0, cacheHead = 0, registryFind = 0, listBegin = 0, listNext = 0, resComposites = 0;
    uint32_t texVtable = 0, texCache = 0, registry = 0, fontshpName = 0, dmgnumName = 0;
    uint32_t pageRead = 0;          // PR (0: the nameplate split is unavailable; the rest is not affected)
    uint32_t pageReadHits = 0;
    uint32_t pageDmgRead = 0;       // DR (0: the Damage split is unavailable; the rest is not affected)
    uint32_t pageDmgReadHits = 0;
    uint32_t recordAllocSites = 0;  // CYyTex allocations found (`push imm8 ... call YC`)
    bool recordBytesOk = false;     // every one allocates kRecordBytes
    bool recordScanCapped = false;  // the allocation scan reached kRecordAllocCap matches (then not verified)
    uint32_t at[SsCount] = {};
    uint32_t hits[SsCount] = {};
    std::vector<std::string> notes;
    std::vector<std::string> infos; // supporting checks that did not resolve (logged only)
    std::string failure;            // the first failing required check ("" when everything required resolved)
    bool complete() const {
        return texVtable && texCache && registry && cacheLookup && cacheHead && registryFind && listBegin && listNext && resComposites && leafBind && leafParse && compRelease && notes.empty();
    }
};

struct SpriteItem { const char* name; uint32_t SpriteResolved::*field; uint32_t sep10; const char* what; bool supporting = false; };
inline bool spriteSigSupporting(int s) { return s == SsPageDmgnum; }
inline const SpriteItem kSpriteItems[] = {
    {"tex ctor (YC)", &SpriteResolved::texCtor, 0x39920, "vtable; +38/+40/+44 zeroed"},
    {"tex dtor (YD)", &SpriteResolved::texDtor, 0x39970, "vtable, cache, unlink +38, Release +40"},
    {"leaf bind (LB)", &SpriteResolved::leafBind, 0x11E6D0, "leaf +32/+4C, record +24/+26"},
    {"leaf parse (LP)", &SpriteResolved::leafParse, 0x11E4E0, "leaf rect +08..+0E, corners +C0/+C8"},
    {"page fontshp (PF)", &SpriteResolved::pageFontshp, 0x120660, "registry, code base 20h"},
    {"page dmgnum (PD)", &SpriteResolved::pageDmgnum, 0x120880, "registry, dmgnum name", true},
    {"composite release (CR)", &SpriteResolved::compRelease, 0x11E2A0, "resource +3C count, +20 array"},
    {"cache lookup", &SpriteResolved::cacheLookup, 0x3AC80, "S2, LB, PF and PD call it"},
    {"cache list head", &SpriteResolved::cacheHead, 0x3AAB0, "cache+8"},
    {"registry find", &SpriteResolved::registryFind, 0x120450, "PF and PD call it"},
    {"list begin", &SpriteResolved::listBegin, 0x126500, "[list]"},
    {"list next", &SpriteResolved::listNext, 0x126520, "node +14 dead, +10 data, +0 next"},
    {"resource composites", &SpriteResolved::resComposites, 0x11E290, "resource+20"},
    {"CYyTex vtable", &SpriteResolved::texVtable, 0x32AA68, "YC and YD agree"},
    {"texture cache", &SpriteResolved::texCache, 0x47B970, "S2, YD, LB, PF, PD agree"},
    {"sprite registry", &SpriteResolved::registry, 0x4E1BF8, "PF and PD agree"},
    {"fontshp name", &SpriteResolved::fontshpName, 0x36CF1C, "\"font    fontshp \""},
    {"dmgnum name", &SpriteResolved::dmgnumName, 0x36CF44, "\"menu    dmgnum  \"", true},
};

// The resolver. `v1` is resolveAll's result on the same image (its texture cache and bind are cross-checked).
inline bool resolveSprites(const uint8_t* data, size_t size, uintptr_t base, const Resolved& v1, SpriteResolved& r, std::vector<std::string>& log) {
    r = SpriteResolved{};
    r.base = uint32_t(base);
    const CodeRange code = codeRange(data, size);   // the signatures are code; the search stays in it
    for (int i = 0; i < SsCount; i++) {
        const auto hits = findSig(data, size, parsePattern(kSpriteSigs[i].pattern), code, 3);
        r.hits[i] = uint32_t(hits.size());
        r.at[i] = hits.size() == 1 ? uint32_t(hits[0]) : 0;
    }
    const auto note = [&](const std::string& s) { r.notes.push_back(s); };
    const auto hex6 = [](uint32_t v) { char b[16]; _snprintf_s(b, sizeof b, _TRUNCATE, "%06X", v); return std::string(b); };
    const auto unique = [&](int s) -> uint32_t { return r.hits[s] == 1 ? r.at[s] : 0; };
    const auto absRva = [&](size_t at) -> uint32_t {
        if (!at || at + 4 > size) return 0;
        const uint32_t v = rd32(data, size, at);
        if (v < base || uint64_t(v) - base >= size) return 0;
        return uint32_t(v - base);
    };
    const auto sigAbs = [&](int s, size_t n) -> uint32_t { const size_t o = op4(kSpriteSigs[s].pattern, n); return unique(s) && o != size_t(-1) ? absRva(unique(s) + o) : 0; };
    const auto sigRel = [&](int s, size_t n) -> uint32_t { const size_t o = op4(kSpriteSigs[s].pattern, n); return unique(s) && o != size_t(-1) ? relTarget(data, size, unique(s) + o) : 0; };
    // The rel32 of a call whose E8 ends a pattern (the operand follows the match).
    const auto tailRel = [&](uint32_t start, const char* pattern) -> uint32_t { return start ? relTarget(data, size, start + parsePattern(pattern).size()) : 0; };
    const auto pinRel = [&](const PinDef& p, uint32_t start, size_t n) -> uint32_t { const size_t o = op4(p.pattern, n); return start && o != size_t(-1) ? relTarget(data, size, start + o) : 0; };
    const auto pinAt = [&](const PinDef& p, uint32_t at) -> uint32_t {
        if (!at) return 0;
        if (matchesAt(data, size, at, parsePattern(p.pattern))) return at;
        note(std::string(p.name) + ": the bytes at " + hex6(at) + " are not the expected ones");
        return 0;
    };
    // `supporting`: bit i marks vals[i] as a supporting vote (PD's, or sigs.h's): missing, it is logged only.
    const auto agree = [&](const char* what, std::initializer_list<uint32_t> vals, unsigned supporting = 0) -> uint32_t {
        uint32_t first = 0;
        bool conflict = false, missing = false, missingSupport = false;
        unsigned i = 0;
        for (uint32_t v : vals) {
            const bool sup = (supporting >> i++) & 1;
            if (!v) { (sup ? missingSupport : missing) = true; continue; }
            if (!first) first = v;
            else if (v != first) conflict = true;
        }
        if (conflict || missing || missingSupport) {
            std::string s = std::string(what) + (conflict ? ": the derivations DISAGREE, not used (" : ": a derivation is missing, the others agree (");
            for (uint32_t v : vals) s += " " + hex6(v);
            s += " )";
            if (conflict || missing) note(s);
            else r.infos.push_back(s + "; the missing one is a supporting signature's");
        }
        return conflict ? 0 : first;
    };
    const auto nameAt = [&](uint32_t rva, const char* want, const char* what) -> uint32_t {
        if (!rva) return 0;
        if (uint64_t(rva) + 16 <= size && std::memcmp(data + rva, want, 16) == 0) return rva;
        note(std::string(what) + ": the operand " + hex6(rva) + " does not read \"" + want + "\"");
        return 0;
    };

    const uint32_t yc = unique(SsTexCtor), yd = unique(SsTexDtor), lb = unique(SsLeafBind), lp = unique(SsLeafParse), pf = unique(SsPageFontshp), pd = unique(SsPageDmgnum),
                   cr = unique(SsCompRelease);
    r.texCtor = yc;
    r.texDtor = yd;
    r.leafBind = lb;
    r.leafParse = lp;
    r.compRelease = cr;
    // The vtable: the constructor's and the destructor's `mov dword [esi], imm32`.
    r.texVtable = agree("CYyTex vtable", {sigAbs(SsTexCtor, 1), sigAbs(SsTexDtor, 0)});
    // The texture cache global: sigs.h's S2, the destructor, the leaf bind and both page caches (operand 12).
    r.texCache = agree("texture cache", {v1.texCache, sigAbs(SsTexDtor, 1), sigAbs(SsLeafBind, 2), sigAbs(SsPageFontshp, 12), sigAbs(SsPageDmgnum, 12)}, 1u | 16u);
    // The cache lookup: v1's S2 tail call, the leaf bind and both page caches call one function; its body is pinned.
    const uint32_t s2Call = v1.bind && v1.mojiName ? tailRel(v1.bind, kSigs[SigBind].pattern) : 0;   // a proven S2 only
    const uint32_t lookup = agree("cache lookup", {s2Call, sigRel(SsLeafBind, 3), tailRel(pf, kSpriteSigs[SsPageFontshp].pattern), tailRel(pd, kSpriteSigs[SsPageDmgnum].pattern)}, 1u | 8u);
    r.cacheLookup = pinAt(kCacheLookup, lookup);
    r.cacheHead = pinAt(kCacheHead, pinRel(kCacheLookup, r.cacheLookup, 0));
    // The registry: both page caches' `mov ecx, imm32`, and the registry find both call.
    r.registry = agree("sprite registry", {sigAbs(SsPageFontshp, 3), sigAbs(SsPageDmgnum, 3)}, 2u);
    r.registryFind = pinAt(kRegistryFind, agree("registry find", {sigRel(SsPageFontshp, 4), sigRel(SsPageDmgnum, 4)}, 2u));
    if (r.registryFind) {
        r.listBegin = pinAt(kListBegin, pinRel(kRegistryFind, r.registryFind, 0));
        r.listNext = pinAt(kListNext, pinRel(kRegistryFind, r.registryFind, 1));
        r.resComposites = pinAt(kResCompositesPin, tailRel(r.registryFind, kRegistryFind.pattern));
    }
    // PF/PD iterator calls (operands 7 and 8) must match the pinned list helpers.
    if (agree("leaf list begin", {r.listBegin, sigRel(SsPageFontshp, 7), sigRel(SsPageDmgnum, 7)}, 4u) != r.listBegin) r.listBegin = 0;
    if (agree("leaf list next", {r.listNext, sigRel(SsPageFontshp, 8), sigRel(SsPageDmgnum, 8)}, 4u) != r.listNext) r.listNext = 0;
    r.pageFontshp = pf;
    r.pageDmgnum = pd;
    // The names the page caches look up must read what TrueFont knows they are.
    r.fontshpName = nameAt(sigAbs(SsPageFontshp, 2), kFontshpName, "fontshp name");
    r.dmgnumName = nameAt(sigAbs(SsPageDmgnum, 2), kDmgnumName, "dmgnum name");
    // The vtable lies in .rdata; the two globals do not.
    {
        uint32_t lo = 0, hi = 0;
        if (peSection(data, size, ".rdata", lo, hi)) {
            if (r.texVtable && !(lo <= r.texVtable && r.texVtable < hi)) { note("CYyTex vtable " + hex6(r.texVtable) + " is not in .rdata"); r.texVtable = 0; }
            if (r.registry && lo <= r.registry && r.registry < hi) { note("sprite registry " + hex6(r.registry) + " lies in .rdata"); r.registry = 0; }
        }
    }
    // A wrong load base preserves address agreement but fails content checks. Ignore absent supporting PD.
    if (pf && !r.fontshpName && (!pd || !r.dmgnumName)) {
        note("load base " + hex6(uint32_t(base)) + " is wrong: neither page-cache name reads what it must; no data address kept");
        r.texVtable = r.texCache = r.registry = 0;
    }
    for (int i = 0; i < SsCount; i++)
        if (r.hits[i] != 1) {
            const std::string s = std::string(kSpriteSigs[i].id) + " " + kSpriteSigs[i].name + ": " + std::to_string(r.hits[i]) + " matches (want 1)";
            if (spriteSigSupporting(i)) r.infos.push_back(s + " (a supporting signature: log only)");
            else note(s);
        }
    // Require every CYyTex allocation to match kRecordBytes. A capped scan cannot prove the copy size safe.
    if (yc) {
        bool ok = true;
        const auto sites = findSig(data, size, parsePattern(kRecordAllocPattern), code, kRecordAllocCap);
        r.recordScanCapped = sites.size() >= kRecordAllocCap;
        for (const size_t at : sites) {
            if (relTarget(data, size, at + 17) != yc) continue;
            ++r.recordAllocSites;
            ok = ok && data[at + 1] == kRecordBytes;
        }
        r.recordBytesOk = ok && r.recordAllocSites > 0 && !r.recordScanCapped;
    }

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "sprites: image %zu bytes, load base %08X", size, unsigned(base));
    log.push_back(line);
    for (int i = 0; i < SsCount; i++) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "sprites: sig %-2s %-17s hits %u at %06X (Sep-10 %06X) %s%s", kSpriteSigs[i].id, kSpriteSigs[i].name, r.hits[i], r.at[i],
                    kSpriteSigs[i].sep10, r.hits[i] != 1 ? "UNRESOLVED" : r.at[i] == kSpriteSigs[i].sep10 ? "same" : "moved", spriteSigSupporting(i) ? " (supporting)" : "");
        log.push_back(line);
    }
    bool all = true;
    std::string firstMissing;
    for (const auto& it : kSpriteItems) {
        const uint32_t v = r.*(it.field);
        if (!it.supporting) {
            all = all && v != 0;
            if (!v && firstMissing.empty()) firstMissing = std::string(it.name) + " did not resolve";
        }
        _snprintf_s(line, sizeof line, _TRUNCATE, "sprites: %-24s %06X (Sep-10 %06X) %-7s %s%s", it.name, v, it.sep10, !v ? "MISSING" : v == it.sep10 ? "same" : "differs", it.what,
                    it.supporting ? " (supporting)" : "");
        log.push_back(line);
    }
    for (const auto& n : r.notes) log.push_back("sprites: NOTE " + n);
    for (const auto& n : r.infos) log.push_back("sprites: INFO " + n);
    for (int i = 0; i < SsCount && r.failure.empty(); i++)
        if (r.hits[i] != 1 && !spriteSigSupporting(i)) r.failure = std::string(kSpriteSigs[i].id) + " " + kSpriteSigs[i].name + ": " + std::to_string(r.hits[i]) + " matches (want 1)";
    if (r.failure.empty() && !r.notes.empty()) r.failure = r.notes[0];
    if (r.failure.empty() && !all) r.failure = firstMissing;
    // PR (resolved apart: it turns off only the nameplate split). Unique, its call lands on PF, and the registry resolved.
    {
        const auto hits = findSig(data, size, parsePattern(kPageReadSig.pattern), code, 3);
        r.pageReadHits = uint32_t(hits.size());
        const uint32_t at = hits.size() == 1 ? uint32_t(hits[0]) : 0;
        const uint32_t calls = at && data[at + kPageReadCall] == 0xE8 ? relTarget(data, size, at + kPageReadCall + 1) : 0;
        const char* why = hits.size() != 1 ? "not exactly one match" : !pf ? "PF did not resolve" : calls != pf ? "its call does not land on PF" : !r.registry ? "the registry did not resolve"
                          : !r.recordBytesOk ? "the CYyTex record's allocation is not the size TrueFont copies"
                                             : "";
        r.pageRead = *why ? 0 : at;
        _snprintf_s(line, sizeof line, _TRUNCATE, "sprites: sig PR %-17s hits %u at %06X (Sep-10 %06X) %s%s%s; the nameplate split %s", kPageReadSig.name, r.pageReadHits, at,
                    kPageReadSig.sep10, r.pageRead ? (at == kPageReadSig.sep10 ? "same" : "moved") : "UNRESOLVED", *why ? ": " : "", why, r.pageRead ? "is available" : "is NOT available");
        log.push_back(line);
        // DR, the same way: unique, its call lands on PD, the registry and the dmgnum name resolved.
        const auto dhits = findSig(data, size, parsePattern(kPageDmgReadSig.pattern), code, 3);
        r.pageDmgReadHits = uint32_t(dhits.size());
        const uint32_t dat = dhits.size() == 1 ? uint32_t(dhits[0]) : 0;
        const uint32_t dcalls = dat && data[dat + kPageReadCall] == 0xE8 ? relTarget(data, size, dat + kPageReadCall + 1) : 0;
        const char* dwhy = dhits.size() != 1 ? "not exactly one match" : !pd ? "PD did not resolve" : dcalls != pd ? "its call does not land on PD" : !r.registry ? "the registry did not resolve"
                           : !r.dmgnumName ? "the dmgnum name did not resolve" : !r.recordBytesOk ? "the CYyTex record's allocation is not the size TrueFont copies" : "";
        r.pageDmgRead = *dwhy ? 0 : dat;
        _snprintf_s(line, sizeof line, _TRUNCATE, "sprites: sig DR %-17s hits %u at %06X (Sep-10 %06X) %s%s%s; the Damage split %s", kPageDmgReadSig.name, r.pageDmgReadHits, dat,
                    kPageDmgReadSig.sep10, r.pageDmgRead ? (dat == kPageDmgReadSig.sep10 ? "same" : "moved") : "UNRESOLVED", *dwhy ? ": " : "", dwhy, r.pageDmgRead ? "is available" : "is NOT available");
        log.push_back(line);
        _snprintf_s(line, sizeof line, _TRUNCATE, "sprites: CYyTex record size: %u allocation site(s) calling YC, %s %zu bytes%s%s", r.recordAllocSites,
                    r.recordBytesOk ? "every one" : "NOT every one", kRecordBytes, r.recordScanCapped ? " (NOT verified: the scan reached its cap)" : "",
                    r.recordBytesOk ? "" : "; the HUD text and Job and level tags splits are NOT available either (their copies are that size)");
        log.push_back(line);
    }
    return all && r.notes.empty();
}

}  // namespace tf
