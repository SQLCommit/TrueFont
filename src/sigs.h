// Chat/menu font signatures. Wildcard relocated operands, validate call targets and require unique matches.
// Cross-check shared addresses, the moji name and grid constants to reject incorrect load bases or layouts.
// S1-S4, K and their derived addresses are required. Missing S5 support is logged only;
// contradictory addresses, names, constants or function bodies always refuse the subsystem.
#pragma once
#include "tf_mem.h"
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tf {

enum SigId {
    SigGlyphBridge,   // S1 (0x11BF40): glyph bridge; `mov eax,[renderer]` at +1, then the caller colour into the 4 vertices
    SigBind,          // S2 (0x11BAE0): atlas bind; `mov eax,[esi+74]`, the name "font    moji    " at +14, the texture cache at +29
    SigAdvance,       // S3 (0x11D840): advance (glyph, int* pen); the width table 36C428
    SigDecoder,       // S4 (0x11D940): decoder (const char*) -> glyph; calls the byte length and the SJIS map
    SigAltFont,       // S5 (0x11C720): the 0036 alternate-font loader: config, device slot, file table
    SigQuad,          // K  (0x11BBD0): the glyph quad; its UV steps 1/64 and 1/128 (the 64 x 128 grid) and the 16 px cell
    SigCount
};

struct SigDef { const char* id; const char* name; const char* pattern; uint32_t sep10; };
inline constexpr SigDef kSigs[SigCount] = {
    {"S1", "glyph-bridge", "A1 ?? ?? ?? ?? 8B 4C 24 08 89 48 64 89 48 48 89 48 2C 89 48 10 8B 44 24 04 66 3D 15 21", 0x11BF40},
    {"S2", "atlas-bind", "83 EC 10 56 8B F1 8B 46 74 85 C0 75 ?? 68 ?? ?? ?? ?? 8D 4C 24 08 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8D 44 24 04 50 E8", 0x11BAE0},
    {"S3", "advance", "66 8B 44 24 04 66 85 C0 7C ?? 66 3D 15 21 7C ?? 66 3D 43 21 7F", 0x11D840},
    {"S4", "decoder", "56 8B 74 24 08 56 E8 ?? ?? ?? ?? 83 C4 04 83 F8 01 75 08 0F BE 06 83 E8 20 5E C3 56 E8 ?? ?? ?? ?? 83 C4 04 5E C3", 0x11D940},
    {"S5", "alt-font-loader",
     "51 A1 ?? ?? ?? ?? 53 55 57 85 C0 8B F9 0F 84 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 48 8A 91 31 09 00 00 84 D2 0F 85 ?? ?? ?? ?? "
     "81 B9 34 09 00 00 00 08 00 00 73 02 33 C0 81 B9 38 09 00 00 00 10 00 00 73 02 33 C0 8D 04 40 C1 E0 02 8B 88 ?? ?? ?? ??", 0x11C720},
    {"K", "glyph-quad",
     "8B 44 24 08 83 EC 10 DB 00 53 55 56 8B F1 D8 25 ?? ?? ?? ?? 57 D9 56 38 D9 1E D9 44 24 30 D8 0D ?? ?? ?? ?? DA 00 D8 25 ?? ?? ?? ?? "
     "D9 56 54 D9 5E 1C DB 40 04 D8 25 ?? ?? ?? ?? D9 56 20 D9 5E 04 D9 44 24 34 D8 0D ?? ?? ?? ?? DA 40 04 0F BF 44 24 24 8B C8 81 E1 3F 00 00 80 "
     "D8 25 ?? ?? ?? ?? D9 56 58 D9 5E 3C 79 05 49 83 C9 C0 41 99 83 E2 3F 89 4C 24 24 DB 44 24 24 03 C2 8D 6E 10 C1 F8 06 89 44 24 24 33 FF "
     "DB 44 24 24 8B DD D9 5C 24 24 D9 05 ?? ?? ?? ?? D8 C9 D9 56 4C D9 5E 14 D8 05 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 56 68 D9 5E 30 "
     "D9 44 24 24 D8 0D ?? ?? ?? ?? D9 56 34 D9 5E 18 D9 44 24 24 D8 05 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 56 6C D9 5E 50", 0x11BBD0},
};

// Pins: bytes at a fixed offset after a signature's match (or at a derived function entry).
struct PinDef { const char* name; const char* pattern; uint32_t offset; };
// S1 +0x28: the special-glyph path, then `mov ecx,[renderer]; call state-setup; ...; jmp bind`.
inline constexpr PinDef kS1Body = {"glyph-bridge body",
    "8B 0D ?? ?? ?? ?? 52 68 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C3 8B 0D ?? ?? ?? ?? 85 C9 74 ?? 66 3D 43 21 7F ?? 8B 09 0F BF C0 8B 8C 81 ?? ?? ?? ?? "
    "85 C9 74 ?? 8B 15 ?? ?? ?? ?? A1 ?? ?? ?? ?? 6A 00 6A 00 68 80 80 80 80 52 50 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "8B 0D ?? ?? ?? ?? E9 ?? ?? ?? ??", 0x28};
// S2 +0x39: the alternate path (`[esi+80]`, `[esi+78]`, loader, selector, `[[esi+78]+8]+40` else +44), SetTexture.
inline constexpr PinDef kS2Alt = {"atlas-bind alternate path",
    "80 BE 80 00 00 00 01 75 ?? 8B 46 78 85 C0 75 ?? 8B CE E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 4E 78 8B 41 08 8B 48 40 85 C9 74 ?? "
    "8B C1 EB ?? 8B 40 44 8B 15 ?? ?? ?? ?? 50 6A 00 8B 52 0C 52 8B 0A FF 91 F4 00 00 00", 0x39};
// S2 +0x8B (0x11BB6B): the moji path: `[esi+74]+40` else +44, then SetTexture(0, tex) through the device slot.
inline constexpr PinDef kS2Moji = {"atlas-bind moji path",
    "8B 76 74 8B 46 40 85 C0 75 03 8B 46 44 8B 15 ?? ?? ?? ?? 50 6A 00 8B 4A 0C 51 8B 11 FF 92 F4 00 00 00", 0x8B};
// S3 +0x16: the special width, `cmp ax,440h`, the width-table read and the default width 14.
inline constexpr PinDef kS3Body = {"advance body",
    "8B 0D ?? ?? ?? ?? 0F BF C0 8B 11 8B 84 82 ?? ?? ?? ?? 0F BF 48 22 0F BF 50 20 8B 44 24 08 2B CA 8B 10 03 D1 89 10 32 C0 C3 66 3D 40 04 73 ?? "
    "0F BF C0 0F BE 88 ?? ?? ?? ?? 8B 44 24 08 8B 10 03 D1 89 10 32 C0 C3 8B 44 24 08 83 00 0E", 0x16};
// K +0xF7 (0x11BCC7): `mov eax,[device slot]; push 144h` (the FVF) ... SetVertexShader (vtable +130).
inline constexpr PinDef kQuadDevice = {"glyph-quad device read", "A1 ?? ?? ?? ?? 68 44 01 00 00 8B 40 0C 50 8B 08 FF 91 30 01 00 00", 0xF7};
// Function bodies reached through a pinned call (offset 0 = the call's target).
// The glyph draw 0x11BBA0 (S1 body's call): forwards to the quad with a 80808080 colour; its call must reach K.
inline constexpr PinDef kGlyphDraw = {"glyph draw", "8B 44 24 10 8B 54 24 0C 50 8B 44 24 0C 52 8B 54 24 0C 68 80 80 80 80 50 52 E8 ?? ?? ?? ?? C2 10 00", 0};
// The byte length 0x11DA80 (S4's first call): 1 + (lead-class(byte) != 0).
inline constexpr PinDef kByteLength = {"byte length", "8B 4C 24 04 33 C0 8A 01 50 E8 ?? ?? ?? ?? 83 C4 04 F7 D8 1B C0 F7 D8 40 C3", 0};
// The lead classifier 0x11DA30 (the byte length's call): 1 for 81..9F, E0..EF, FA..FC (the ranges are in the bytes).
inline constexpr PinDef kLeadClass = {"lead classifier",
    "8B 44 24 04 3D 81 00 00 00 72 0D 3D 9F 00 00 00 77 06 B8 01 00 00 00 C3 3D E0 00 00 00 72 0D 3D EF 00 00 00 77 06 B8 01 00 00 00 C3 "
    "3D FA 00 00 00 72 0D 3D FC 00 00 00 77 06 B8 01 00 00 00 C3 33 C0 C3", 0};
// The SJIS map 0x11C940 (S4's second call), all 0x208 bytes: byte-identical means charmap.h's port is exact.
inline constexpr PinDef kSjisMap = {"SJIS map",
    "8B 4C 24 04 33 C0 33 D2 8A 01 8A 51 01 3D 9F 00 00 00 8B CA 77 05 83 E8 71 EB 05 2D B1 00 00 00 03 C0 0C 01 83 F9 7F 76 "
    "01 49 81 F9 9E 00 00 00 72 06 40 83 E9 7D EB 03 83 E9 1F 8B D0 C1 E2 08 0B D1 81 FA 20 93 00 00 72 05 66 B8 CB 00 C3 81 "
    "FA 21 79 00 00 72 10 8D 14 40 C1 E2 04 2B D0 8D 84 51 2F F3 FF FF C3 81 FA 21 50 00 00 72 10 8D 14 40 C1 E2 04 2B D0 8D "
    "84 51 14 F2 FF FF C3 81 FA 21 30 00 00 72 10 8D 14 40 C1 E2 04 2B D0 8D 84 51 3F F2 FF FF C3 81 FA 21 2F 00 00 72 08 8D "
    "84 01 1E 02 00 00 C3 81 FA 21 2E 00 00 72 08 8D 84 01 71 02 00 00 C3 81 FA 46 2E 00 00 72 08 8D 84 01 71 02 00 00 C3 81 "
    "FA 5F 2D 00 00 72 08 8D 84 01 51 03 00 00 C3 81 FA 40 2D 00 00 72 08 8D 84 01 59 03 00 00 C3 81 FA 21 2D 00 00 72 08 8D "
    "84 01 5A 03 00 00 C3 81 FA 21 2A 00 00 72 08 8D 84 01 23 02 00 00 C3 81 FA 21 29 00 00 72 08 8D 84 01 76 02 00 00 C3 81 "
    "FA 21 28 00 00 72 08 8D 84 01 03 02 00 00 C3 81 FA 51 27 00 00 72 08 8D 84 01 B3 01 00 00 C3 81 FA 21 27 00 00 72 08 8D "
    "84 01 C2 01 00 00 C3 81 FA 41 26 00 00 72 08 8D 84 01 8B 01 00 00 C3 81 FA 21 26 00 00 72 08 8D 84 01 93 01 00 00 C3 81 "
    "FA 21 25 00 00 72 08 8D 84 01 3E 01 00 00 C3 81 FA 21 24 00 00 72 08 8D 84 01 EC 00 00 00 C3 81 FA 61 23 00 00 72 08 8D "
    "84 01 93 00 00 00 C3 81 FA 41 23 00 00 72 08 8D 84 01 99 00 00 00 C3 81 FA 30 23 00 00 72 08 8D 84 01 A0 00 00 00 C3 81 "
    "FA 7F 22 00 00 75 05 66 B8 F2 00 C3 81 FA 72 22 00 00 72 05 8D 44 01 56 C3 81 FA 5C 22 00 00 72 05 8D 44 01 5D C3 81 FA "
    "4A 22 00 00 72 05 8D 44 01 68 C3 81 FA 3A 22 00 00 72 05 8D 44 01 70 C3 8D 14 40 C1 E2 04 2B D0 8D 84 51 21 F4 FF FF C3", 0};

// Offsets TrueFont relies on, each pinned in the bytes above (S2 and its two pins).
inline constexpr uint32_t kRendMoji = 0x74;      // renderer+74: CYyTex* of "font    moji    "
inline constexpr uint32_t kRendAlt = 0x78;       // renderer+78: the alternate wrapper; [+78]+8 = its CYyTex*
inline constexpr uint32_t kRendAltOn = 0x80;     // renderer+80: alternate-enable byte
inline constexpr uint32_t kAltRecord = 0x08;     // [renderer+78]+8
inline constexpr uint32_t kTexPrimary = 0x40;    // CYyTex+40: IDirect3DTexture8* (the pointer TrueFont swaps)
inline constexpr uint32_t kTexAlternate = 0x44;  // CYyTex+44: the fallback the bind uses when +40 is null
inline constexpr uint32_t kWidthTableLen = 0x440;   // `cmp ax,440h` in the advance
inline constexpr char kMojiName[] = "font    moji    ";   // 16 bytes at the name operand of S2
// The font high record's name: 15 characters plus the NUL (the client's string at 36C3F4).
inline constexpr char kHighName[] = "font    high   ";

// Everything TrueFont resolves, as RVAs (0 = not resolved).
struct Resolved {
    uint32_t base = 0;   // the load address the absolute operands were read against (not an RVA)
    uint32_t glyphBridge = 0, bind = 0, advance = 0, decoder = 0, altLoader = 0, quad = 0;
    uint32_t glyphDraw = 0, stateSetup = 0, altSelect = 0, byteLength = 0, leadClass = 0, sjisMap = 0;
    uint32_t renderer = 0, penX = 0, usgaiji = 0, mojiName = 0, texCache = 0, deviceSlot = 0, widthTable = 0;
    uint32_t altConfig = 0, altTable = 0;
    uint32_t uStep = 0, vStep = 0, halfPixel = 0, cellSize = 0, one = 0;   // K's constants (each read and checked)
    uint32_t at[SigCount] = {};     // match RVA of each signature (0 = none or ambiguous)
    uint32_t hits[SigCount] = {};   // match count (capped at 3)
    std::vector<std::string> notes; // cross-check failures, in words (each one refuses)
    std::vector<std::string> infos; // supporting checks that did not resolve (logged only)
    std::string failure;            // the first failing required check, in words ("" when everything required resolved)
};
inline bool sigSupporting(int s) { return s == SigAltFont; }

// Known signature matches by PE timestamp, newest first. Only entries marked live were tested in-game.
struct KnownBuild { uint32_t stamp; const char* name; bool live; };
inline constexpr KnownBuild kKnownBuilds[] = {
    {0x6A995428, "the 2026-09-10 retail client", true},
    {0x6A7297F5, "the 2026-08-18 retail client", false},
    {0x6A4CC4C5, "the 2026-08-03 retail client", false},
    {0x69144FB9, "the HorizonXI client (2025-11)", false},
};
inline const char* knownBuild(uint32_t stamp) {
    for (const auto& b : kKnownBuilds) if (b.stamp == stamp) return b.name;
    return nullptr;
}
inline bool knownBuildLive(uint32_t stamp) {
    for (const auto& b : kKnownBuilds) if (b.stamp == stamp) return b.live;
    return false;
}
// Newer than every known build (a PE stamp is the link time): the game was updated after this TrueFont.
inline bool newerThanKnown(uint32_t stamp) {
    for (const auto& b : kKnownBuilds) if (stamp <= b.stamp) return false;
    return true;
}

struct Item { const char* name; uint32_t Resolved::*field; uint32_t sep10; const char* what; bool supporting = false; };
inline const Item kItems[] = {
    {"glyph bridge (S1)", &Resolved::glyphBridge, 0x11BF40, "reads the renderer global"},
    {"atlas bind (S2)", &Resolved::bind, 0x11BAE0, "S1's tail jumps here; binds [+74]+40 or [[+78]+8]+40"},
    {"advance (S3)", &Resolved::advance, 0x11D840, "table / sprite / 14"},
    {"decoder (S4)", &Resolved::decoder, 0x11D940, "single byte signed - 20h, else the SJIS map"},
    {"alt-font loader (S5)", &Resolved::altLoader, 0x11C720, "S2 calls it; 0036 PNG atlas (diagnostics)", true},
    {"glyph quad (K)", &Resolved::quad, 0x11BBD0, "UV steps 1/64, 1/128; 16 px"},
    {"glyph draw", &Resolved::glyphDraw, 0x11BBA0, "S1 calls it; it calls K"},
    {"state setup", &Resolved::stateSetup, 0x11BDC0, "S1's tail calls it (ONE/ONE blend)"},
    {"alt select", &Resolved::altSelect, 0x11C850, "S2 calls it"},
    {"byte length", &Resolved::byteLength, 0x11DA80, "S4's first call"},
    {"lead classifier", &Resolved::leadClass, 0x11DA30, "81..9F, E0..EF, FA..FC"},
    {"SJIS map", &Resolved::sjisMap, 0x11C940, "S4's second call; body byte-identical to the port"},
    {"renderer global", &Resolved::renderer, 0x4E1B90, "*slot = renderer; +74 moji, +78 alternate, +80 enable"},
    {"pen x global", &Resolved::penX, 0x4E1B88, "pen y at +4"},
    {"usgaiji slot", &Resolved::usgaiji, 0x621934, "special glyph sprites (S1 and S3 agree)"},
    {"moji name", &Resolved::mojiName, 0x36C224, "\"font    moji    \""},
    {"texture cache", &Resolved::texCache, 0x47B970, "teardown guard (gameAlive); the sprite fonts' cache walk"},
    {"device slot", &Resolved::deviceSlot, 0x45666C, "S2 (twice), S5 and K agree"},
    {"width table", &Resolved::widthTable, 0x36C428, "0x440 signed bytes, read only"},
    {"0036 config", &Resolved::altConfig, 0x456AA0, "log only", true},
    {"0036 file table", &Resolved::altTable, 0x333000, "{file, w, h} x 2, log only", true},
    {"u step (1/64)", &Resolved::uStep, 0x32A9E8, "read and checked"},
    {"v step (1/128)", &Resolved::vStep, 0x32A778, "read and checked"},
    {"half pixel (0.5)", &Resolved::halfPixel, 0x329A08, "read and checked"},
    {"cell size (16.0)", &Resolved::cellSize, 0x32A1AC, "read and checked"},
    {"one (1.0)", &Resolved::one, 0x32961C, "read and checked"},
};

// Start offset and length of each run of ?? in a pattern.
inline std::vector<std::pair<size_t, size_t>> wildGroups(const char* pattern) {
    const auto pat = parsePattern(pattern);
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < pat.size();) {
        if (pat[i] >= 0) { i++; continue; }
        size_t j = i;
        while (j < pat.size() && pat[j] < 0) j++;
        out.emplace_back(i, j - i);
        i = j;
    }
    return out;
}
// Offset of the n-th 4-byte wildcard group (1-byte rel8 groups are not counted); SIZE_MAX when absent.
inline size_t op4(const char* pattern, size_t n) {
    size_t k = 0;
    for (const auto& g : wildGroups(pattern)) {
        if (g.second != 4) continue;
        if (k++ == n) return g.first;
    }
    return size_t(-1);
}

inline uint32_t rd32(const uint8_t* data, size_t size, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= size) std::memcpy(&v, data + at, 4);
    return v;
}
// Target RVA of the rel32 at `off` (0 when outside the image).
inline uint32_t relTarget(const uint8_t* data, size_t size, size_t off) {
    const int64_t t = int64_t(off) + 4 + int32_t(rd32(data, size, off));
    return (t > 0 && uint64_t(t) < size) ? uint32_t(t) : 0;
}

// The load address an image was mapped at, from the votes of its `call [imm32]` IAT sites (dump or live module).
inline uint32_t imageLoadBase(const uint8_t* b, size_t size) {
    const uint32_t pe = rd32(b, size, 0x3C);
    if (uint64_t(pe) + 0xF8 > size) return 0;
    const uint32_t iatRva = rd32(b, size, pe + 0x18 + 0x60 + 12 * 8), iatSize = rd32(b, size, pe + 0x18 + 0x60 + 12 * 8 + 4);
    if (!iatRva || !iatSize) return 0;
    std::map<uint32_t, int> votes;
    for (size_t i = 0; i + 6 <= size; i++) {
        if (b[i] != 0xFF || b[i + 1] != 0x15) continue;
        const uint32_t v = rd32(b, size, i + 2);
        const uint32_t base = (v - iatRva) & ~0xFFFFu;
        if (v >= base + iatRva && v < base + iatRva + iatSize) ++votes[base];
    }
    uint32_t best = 0;
    int bestN = 0;
    for (const auto& kv : votes) if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
    return best;
}

// The float K expects at each constant.
inline bool floatIs(const uint8_t* data, size_t size, uint32_t rva, float want) {
    if (!rva || uint64_t(rva) + 4 > size) return false;
    float v = 0;
    std::memcpy(&v, data + rva, 4);
    return std::memcmp(&v, &want, 4) == 0;
}
inline float floatAt(const uint8_t* data, size_t size, uint32_t rva) {
    float v = 0;
    if (rva && uint64_t(rva) + 4 <= size) std::memcpy(&v, data + rva, 4);
    return v;
}

// The resolver. `base`: the load address the operands were relocated to. True when everything resolved and agreed.
inline bool resolveAll(const uint8_t* data, size_t size, uintptr_t base, Resolved& r, std::vector<std::string>& log) {
    r = Resolved{};
    r.base = uint32_t(base);
    const CodeRange code = codeRange(data, size);   // the signatures are code; the search stays in it
    for (int i = 0; i < SigCount; i++) {
        const auto hits = findSig(data, size, parsePattern(kSigs[i].pattern), code, 3);
        r.hits[i] = uint32_t(hits.size());
        r.at[i] = hits.size() == 1 ? uint32_t(hits[0]) : 0;
    }
    const auto note = [&](const std::string& s) { r.notes.push_back(s); };
    const auto hex6 = [](uint32_t v) { char b[16]; _snprintf_s(b, sizeof b, _TRUNCATE, "%06X", v); return std::string(b); };
    const auto unique = [&](int s) -> uint32_t { return r.hits[s] == 1 ? r.at[s] : 0; };
    // An absolute operand at `at` -> RVA (0 unless inside the image).
    const auto absRva = [&](size_t at) -> uint32_t {
        if (!at || at + 4 > size) return 0;
        const uint32_t v = rd32(data, size, at);
        if (v < base || uint64_t(v) - base >= size) return 0;
        return uint32_t(v - base);
    };
    // Operand n (4-byte groups only) of a signature match / of a pin at `start`.
    const auto sigAbs = [&](int s, size_t n) -> uint32_t { const size_t o = op4(kSigs[s].pattern, n); return unique(s) && o != size_t(-1) ? absRva(unique(s) + o) : 0; };
    const auto pinAbs = [&](const PinDef& p, uint32_t start, size_t n) -> uint32_t { const size_t o = op4(p.pattern, n); return start && o != size_t(-1) ? absRva(start + o) : 0; };
    const auto pinRel = [&](const PinDef& p, uint32_t start, size_t n) -> uint32_t { const size_t o = op4(p.pattern, n); return start && o != size_t(-1) ? relTarget(data, size, start + o) : 0; };
    // A pin at `anchor + p.offset`: its start, or 0 with a note.
    const auto pinAt = [&](const PinDef& p, uint32_t anchor) -> uint32_t {
        if (!anchor) return 0;
        const uint32_t at = anchor + p.offset;
        if (matchesAt(data, size, at, parsePattern(p.pattern))) return at;
        note(std::string(p.name) + ": the bytes at " + hex6(at) + " are not the expected ones");
        return 0;
    };
    // Every derivation found must name the same address; one missing is noted, two that disagree lose the address.
    // `supporting`: bit i marks vals[i] as a supporting signature's vote (missing, it is logged only).
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

    const uint32_t s1 = unique(SigGlyphBridge), s2 = unique(SigBind), s3 = unique(SigAdvance), s4 = unique(SigDecoder), s5 = unique(SigAltFont), k = unique(SigQuad);
    const uint32_t s1Body = pinAt(kS1Body, s1), s2Alt = pinAt(kS2Alt, s2), s2Moji = pinAt(kS2Moji, s2), s3Body = pinAt(kS3Body, s3), kDev = pinAt(kQuadDevice, k);

    // The renderer: S1's `mov eax,[g]` and the three `mov ecx,[g]` in its body.
    r.renderer = agree("renderer global", {sigAbs(SigGlyphBridge, 0), pinAbs(kS1Body, s1Body, 0), pinAbs(kS1Body, s1Body, 8), pinAbs(kS1Body, s1Body, 10)});
    // The pen: `push penX` and `mov eax,[penX]` agree; `mov edx,[penY]` is penX + 4.
    r.penX = agree("pen x global", {pinAbs(kS1Body, s1Body, 1), pinAbs(kS1Body, s1Body, 6)});
    if (r.penX && pinAbs(kS1Body, s1Body, 5) != r.penX + 4) { note("pen y is not pen x + 4"); r.penX = 0; }
    // The bind: S2's match and S1's tail jump.
    r.bind = agree("atlas bind", {s2, pinRel(kS1Body, s1Body, 11)});
    r.stateSetup = pinRel(kS1Body, s1Body, 9);
    // The usgaiji container: S1's special path and S3's special width.
    r.usgaiji = agree("usgaiji slot", {pinAbs(kS1Body, s1Body, 3), pinAbs(kS3Body, s3Body, 0)});
    // The glyph draw S1 calls, and the quad it calls: must be K.
    if (const uint32_t draw = pinRel(kS1Body, s1Body, 2)) {
        const uint32_t drawAt = pinAt(kGlyphDraw, draw);
        const uint32_t target = pinRel(kGlyphDraw, drawAt, 0);
        if (drawAt && target && target == k) { r.glyphDraw = drawAt; r.quad = k; }
        else if (drawAt) note("glyph draw " + hex6(draw) + " calls " + hex6(target) + ", not the glyph quad (K) " + hex6(k));
    }
    // The moji name and the texture cache (S2); the name must read "font    moji    ".
    if (const uint32_t name = sigAbs(SigBind, 0)) {
        if (uint64_t(name) + 16 <= size && std::memcmp(data + name, kMojiName, 16) == 0) r.mojiName = name;
        else note("moji name: the bind's name operand " + hex6(name) + " does not read \"font    moji    \"");
    }
    // Only an S2 whose name checked out names the texture cache (the sprite side counts it as a vote).
    r.texCache = r.mojiName ? sigAbs(SigBind, 2) : 0;
    // The alternate path: S2 calls the loader (must be S5) and the selector.
    r.altLoader = agree("alt-font loader", {s5, pinRel(kS2Alt, s2Alt, 0)}, 1u);
    r.altSelect = pinRel(kS2Alt, s2Alt, 1);
    r.altConfig = sigAbs(SigAltFont, 0);
    r.altTable = sigAbs(SigAltFont, 4);
    // The device slot: both bind paths, the alternate loader and the quad name one slot.
    r.deviceSlot = agree("device slot", {pinAbs(kS2Alt, s2Alt, 2), pinAbs(kS2Moji, s2Moji, 0), sigAbs(SigAltFont, 2), pinAbs(kQuadDevice, kDev, 0)}, 4u);
    // The advance and its width table (read only, 0x440 signed bytes).
    r.advance = s3Body ? s3 : 0;
    r.widthTable = pinAbs(kS3Body, s3Body, 2);
    if (r.widthTable && uint64_t(r.widthTable) + kWidthTableLen > size) { note("width table runs past the image"); r.widthTable = 0; }
    // The decoder, its byte length, the lead classifier and the SJIS map (its whole body pinned).
    if (s4) {
        const uint32_t len = relTarget(data, size, s4 + op4(kSigs[SigDecoder].pattern, 0));
        const uint32_t map = relTarget(data, size, s4 + op4(kSigs[SigDecoder].pattern, 1));
        const uint32_t lenAt = pinAt(kByteLength, len);
        const uint32_t lead = pinRel(kByteLength, lenAt, 0);
        r.byteLength = lenAt;
        r.leadClass = pinAt(kLeadClass, lead);
        r.sjisMap = pinAt(kSjisMap, map);
        if (r.byteLength && r.leadClass && r.sjisMap) r.decoder = s4;
    }
    // K: the constants, each named twice or more (they must agree), then read: exactly 0.5, 16, 1/64, 1, 1/128.
    if (k) {
        r.halfPixel = agree("half pixel", {sigAbs(SigQuad, 0), sigAbs(SigQuad, 2), sigAbs(SigQuad, 3), sigAbs(SigQuad, 5)});
        r.cellSize = agree("cell size", {sigAbs(SigQuad, 1), sigAbs(SigQuad, 4)});
        r.uStep = agree("u step", {sigAbs(SigQuad, 6), sigAbs(SigQuad, 8)});
        r.one = agree("one", {sigAbs(SigQuad, 7), sigAbs(SigQuad, 10)});
        r.vStep = agree("v step", {sigAbs(SigQuad, 9), sigAbs(SigQuad, 11)});
        const struct { uint32_t Resolved::*f; float want; const char* what; } consts[] = {
            {&Resolved::uStep, 1.0f / 64, "u step (1/64)"}, {&Resolved::vStep, 1.0f / 128, "v step (1/128)"}, {&Resolved::halfPixel, 0.5f, "half pixel (0.5)"},
            {&Resolved::cellSize, 16.0f, "cell size (16.0)"}, {&Resolved::one, 1.0f, "one (1.0)"}};
        for (const auto& c : consts) {
            if (!(r.*(c.f))) continue;
            if (!floatIs(data, size, r.*(c.f), c.want)) {
                char b[160];
                _snprintf_s(b, sizeof b, _TRUNCATE, "%s at %06X reads %.9g: the glyph grid is not the one TrueFont knows", c.what, r.*(c.f), double(floatAt(data, size, r.*(c.f))));
                note(b);
                r.*(c.f) = 0;
            }
        }
    }
    // The width table must look like one: every entry 1..16 (Sep-10: 4..16), since TrueFont places glyphs by it.
    if (r.widthTable) {
        int bad = 0;
        for (uint32_t i = 0; i < kWidthTableLen; i++) {
            const int w = int(int8_t(data[r.widthTable + i]));
            if (w < 1 || w > 16) ++bad;
        }
        if (bad) { note("width table at " + hex6(r.widthTable) + ": " + std::to_string(bad) + " entries outside 1..16"); r.widthTable = 0; }
    }
    r.glyphBridge = (s1Body && r.renderer) ? s1 : 0;
    // A wrong load base shifts every operand together; the name and K's constants are content checks it cannot pass.
    if (sigAbs(SigBind, 0) && k && !r.mojiName && !r.uStep && !r.vStep && !r.halfPixel && !r.cellSize && !r.one) {
        const Resolved keep = r;
        r = Resolved{};
        r.base = keep.base;
        std::memcpy(r.at, keep.at, sizeof r.at);
        std::memcpy(r.hits, keep.hits, sizeof r.hits);
        char b[200];
        _snprintf_s(b, sizeof b, _TRUNCATE, "load base %08X is wrong: neither the moji name nor any of K's constants reads what it must; nothing resolved", unsigned(base));
        note(b);
    }
    for (int i = 0; i < SigCount; i++)
        if (r.hits[i] != 1) {
            const std::string s = std::string(kSigs[i].id) + " " + kSigs[i].name + ": " + std::to_string(r.hits[i]) + " matches (want 1)";
            if (sigSupporting(i)) r.infos.push_back(s + " (a supporting signature: log only)");
            else note(s);
        }

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "resolve: image %zu bytes, load base %08X", size, unsigned(base));
    log.push_back(line);
    for (int i = 0; i < SigCount; i++) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "resolve: sig %-2s %-16s hits %u at %06X (Sep-10 %06X) %s%s", kSigs[i].id, kSigs[i].name, r.hits[i], r.at[i], kSigs[i].sep10,
                    r.hits[i] != 1 ? "UNRESOLVED" : r.at[i] == kSigs[i].sep10 ? "same" : "moved", sigSupporting(i) ? " (supporting)" : "");
        log.push_back(line);
    }
    bool all = true;
    std::string firstMissing;
    for (const auto& it : kItems) {
        const uint32_t v = r.*(it.field);
        if (!it.supporting) {
            all = all && v != 0;
            if (!v && firstMissing.empty()) firstMissing = std::string(it.name) + " did not resolve";
        }
        _snprintf_s(line, sizeof line, _TRUNCATE, "resolve: %-22s %06X (Sep-10 %06X) %-7s %s%s", it.name, v, it.sep10, !v ? "MISSING" : v == it.sep10 ? "same" : "differs", it.what,
                    it.supporting ? " (supporting)" : "");
        log.push_back(line);
    }
    for (const auto& n : r.notes) log.push_back("resolve: NOTE " + n);
    for (const auto& n : r.infos) log.push_back("resolve: INFO " + n);
    // Report the earliest cause: missing required signature, failed cross-check, then missing derived address.
    for (int i = 0; i < SigCount && r.failure.empty(); i++)
        if (r.hits[i] != 1 && !sigSupporting(i)) r.failure = std::string(kSigs[i].id) + " " + kSigs[i].name + ": " + std::to_string(r.hits[i]) + " matches (want 1)";
    if (r.failure.empty() && !r.notes.empty()) r.failure = r.notes[0];
    if (r.failure.empty() && !all) r.failure = firstMissing;
    return all && r.notes.empty();
}
// The same, with the load base derived from the image itself (imageLoadBase).
inline bool resolveAll(const uint8_t* image, size_t size, Resolved& out, std::vector<std::string>& log) {
    const uint32_t base = imageLoadBase(image, size);
    if (!base) {
        out = Resolved{};
        log.push_back("resolve: the load base could not be derived (no IAT calls found); nothing resolved");
        return false;
    }
    return resolveAll(image, size, base, out, log);
}

}  // namespace tf
