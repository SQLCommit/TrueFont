// Atlas-cell to Unicode mapping from the client's decoder: ASCII, fixed Latin tables, then CP932.
// Ambiguous CP932 pairs prefer consecutive glyph neighbours. Undefined cp1252 bytes remain unmapped.
// Latin cells are below 0x60 or in 0x26E..0x2FF; other cells are full-width.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "glyph.h"

namespace tf {


// 11DA30: 1 for a lead byte (81..9F, E0..EF, FA..FC).
inline bool isLeadByte(uint8_t b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEF) || (b >= 0xFA && b <= 0xFC); }

// 11C940(const uint8_t* p): the full EAX it returns for p[0] = lead, p[1] = trail.
inline uint32_t sjisMap(uint8_t lead, uint8_t trail) {
    uint32_t eax = lead, ecx = trail;
    eax = eax > 0x9F ? eax - 0xB1 : eax - 0x71;
    eax = (eax + eax) | 1;
    if (ecx > 0x7F) ecx -= 1;
    if (ecx >= 0x9E) { eax += 1; ecx -= 0x7D; }
    else ecx -= 0x1F;
    const uint32_t edx = (eax << 8) | ecx;
    const uint32_t rows94 = eax * 94;   // lea edx,[eax+eax*2]; shl edx,4; sub edx,eax; lea eax,[ecx+edx*2+k]
    if (edx >= 0x9320) return (eax & 0xFFFF0000u) | 0xCB;
    if (edx >= 0x7921) return ecx + rows94 + 0xFFFFF32Fu;
    if (edx >= 0x5021) return ecx + rows94 + 0xFFFFF214u;
    if (edx >= 0x3021) return ecx + rows94 + 0xFFFFF23Fu;
    struct Step { uint32_t from, add; };
    static constexpr Step single[] = {{0x2F21, 0x21E}, {0x2E21, 0x271}, {0x2E46, 0x271}, {0x2D5F, 0x351}, {0x2D40, 0x359}, {0x2D21, 0x35A},
                                      {0x2A21, 0x223}, {0x2921, 0x276}, {0x2821, 0x203}, {0x2751, 0x1B3}, {0x2721, 0x1C2}, {0x2641, 0x18B},
                                      {0x2621, 0x193}, {0x2521, 0x13E}, {0x2421, 0xEC},  {0x2361, 0x93},  {0x2341, 0x99},  {0x2330, 0xA0}};
    for (const Step& s : single)
        if (edx >= s.from) return ecx + eax + s.add;
    if (edx == 0x227F) return (eax & 0xFFFF0000u) | 0xF2;
    static constexpr Step tail[] = {{0x2272, 0x56}, {0x225C, 0x5D}, {0x224A, 0x68}, {0x223A, 0x70}};
    for (const Step& s : tail)
        if (edx >= s.from) return ecx + eax + s.add;
    return ecx + rows94 + 0xFFFFF421u;
}
// 11D940(const char* p): the chat decoder (signed single byte - 20h; a lead byte goes through the map). Full EAX.
inline uint32_t decodeChat(const uint8_t* p) {
    if (!isLeadByte(p[0])) return uint32_t(int32_t(int8_t(p[0])) - 0x20);
    return sjisMap(p[0], p[1]);
}

// Compares the port with the client's 11C940 for all 65,536 byte pairs.
struct MapCheck { uint32_t checked = 0, mismatches = 0, first = 0; };
template <class Fn> MapCheck checkSjisMap(Fn&& fn) {
    MapCheck m;
    uint8_t p[4] = {};
    for (uint32_t lead = 0; lead < 256; lead++)
        for (uint32_t trail = 0; trail < 256; trail++) {
            p[0] = uint8_t(lead);
            p[1] = uint8_t(trail);
            const uint32_t want = sjisMap(uint8_t(lead), uint8_t(trail));
            const uint32_t got = uint32_t(fn(p));
            ++m.checked;
            if (got != want) {
                if (!m.mismatches) m.first = (lead << 8) | trail;
                ++m.mismatches;
            }
        }
    return m;
}


enum class Script : uint8_t { None, Ascii, Latin1, Cp1252, Jis, Kana, Nec13, Kanji, Fixed };

struct CellInfo {
    wchar_t ch = 0;          // 0 = no mapping
    uint16_t sjis = 0;       // the lead/trail pair it came from (0 for ASCII and the Latin tables)
    Script script = Script::None;
};

class CharMap {
public:
    // The fixed list: FFXI-only pieces and placeholders, always native.
    static bool fixedKeep(int g) {
        return g == 0x26C || g == 0x26D || g == 0xF2 || (g >= 0x2AE && g <= 0x2BF) || (g >= 0x300 && g <= 0x33F) || (g >= 0x340 && g <= 0x3A7);
    }
    // Latin cells: glyphs below 60h and the European range 26E..2FF.
    static bool latinCell(int g) { return (g >= 0 && g < 0x60) || (g >= 0x26E && g <= 0x2FF); }
    static bool fullWidth(int g) { return !latinCell(g); }
    // The special glyphs 2115..2143 are usgaiji sprites: past the 8,192 cells, never sampled from the atlas.
    static bool special(int g) { return g >= 0x2115 && g <= 0x2143; }

    // Builds the map with the running Windows' CP932 and cp1252 tables. False only when those are unavailable.
    bool build(std::string* why = nullptr) {
        cells_.assign(kGridCells, CellInfo{});
        inverse_.clear();
        duplicates_ = ambiguous_ = conflicts_ = 0;
        for (int g = 0; g < 0x5F; g++) set(g, wchar_t(0x20 + g), 0);
        for (int k = 0; k < 64; k++) set(0x26E + k, wchar_t(0xC0 + k), 0);
        for (int k = 0; k < 64; k++) {
            const char b = char(0x80 + k);
            wchar_t w = 0;
            if (MultiByteToWideChar(1252, MB_ERR_INVALID_CHARS, &b, 1, &w, 1) == 1 && printable(w)) set(0x2C0 + k, w, 0);
        }
        struct Cand { uint16_t sjis; wchar_t ch; };
        std::vector<std::vector<Cand>> cands(kGridCells);
        int valid = 0;
        for (int lead = 0x81; lead <= 0xFC; lead++) {
            if (!isLeadByte(uint8_t(lead))) continue;
            for (int trail = 0x40; trail <= 0xFC; trail++) {
                if (trail == 0x7F) continue;
                const wchar_t ch = cp932(uint8_t(lead), uint8_t(trail));
                if (!ch) continue;
                ++valid;
                const uint32_t g = sjisMap(uint8_t(lead), uint8_t(trail)) & 0xFFFF;
                if (g < uint32_t(kGridCells)) cands[g].push_back({uint16_t((lead << 8) | trail), ch});
            }
        }
        if (!valid) {
            if (why) *why = "the CP932 code page is not available";
            return false;
        }
        for (int g = 0; g < kGridCells; g++) {
            const auto& c = cands[g];
            if (c.empty()) continue;
            if (cells_[g].ch) { conflicts_ += int(c.size()); continue; }   // ASCII or a Latin table has it
            size_t best = 0;
            int bestScore = -1;
            for (size_t i = 0; i < c.size(); i++) {
                const int s = neighbourScore(c[i].sjis, g);
                if (s > bestScore) { bestScore = s; best = i; }
            }
            if (c.size() > 1) ++ambiguous_;
            set(g, c[best].ch, c[best].sjis);
        }
        for (int g = 0; g < kGridCells; g++) cells_[g].script = classify(g, cells_[g].ch);
        return true;
    }

    const CellInfo& cell(int g) const { static const CellInfo none{}; return g >= 0 && g < int(cells_.size()) ? cells_[size_t(g)] : none; }
    // The glyph a character maps to (the first glyph when several do); -1 when none.
    int glyphOf(wchar_t ch) const { const auto it = inverse_.find(ch); return it == inverse_.end() ? -1 : it->second; }
    // A cell TrueFont may render: mapped, not fixed, and Latin (or every script when allScripts).
    bool allowed(int g, bool allScripts) const {
        const CellInfo& c = cell(g);
        if (!c.ch || fixedKeep(g)) return false;
        return allScripts || latinCell(g);
    }
    int mapped() const { int n = 0; for (const auto& c : cells_) n += c.ch ? 1 : 0; return n; }
    int count(Script s) const { int n = 0; for (const auto& c : cells_) n += (c.ch && c.script == s) ? 1 : 0; return n; }
    int ambiguous() const { return ambiguous_; }      // glyphs reached by more than one valid CP932 pair
    int conflicts() const { return conflicts_; }      // CP932 pairs landing on an ASCII or Latin-table glyph
    int duplicates() const { return duplicates_; }    // characters mapped from more than one glyph (the inverse keeps the first)

private:
    std::vector<CellInfo> cells_;
    std::unordered_map<wchar_t, int> inverse_;
    int ambiguous_ = 0, conflicts_ = 0, duplicates_ = 0;

    static bool printable(wchar_t w) { return w >= 0x20 && !(w >= 0x7F && w <= 0x9F); }
    static wchar_t cp932(uint8_t lead, uint8_t trail) {
        const char b[2] = {char(lead), char(trail)};
        wchar_t w[2] = {};
        return MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, b, 2, w, 2) == 1 && printable(w[0]) ? w[0] : 0;
    }
    // How many of the pair's SJIS neighbours (trail - 1, trail + 1) are valid and land on glyph - 1 / glyph + 1.
    static int neighbourScore(uint16_t sjis, int g) {
        const uint8_t lead = uint8_t(sjis >> 8), trail = uint8_t(sjis);
        int s = 0;
        if (trail > 0x40 && cp932(lead, uint8_t(trail - 1)) && int(sjisMap(lead, uint8_t(trail - 1)) & 0xFFFF) == g - 1) ++s;
        if (trail < 0xFC && cp932(lead, uint8_t(trail + 1)) && int(sjisMap(lead, uint8_t(trail + 1)) & 0xFFFF) == g + 1) ++s;
        return s;
    }
    void set(int g, wchar_t ch, uint16_t sjis) {
        cells_[size_t(g)].ch = ch;
        cells_[size_t(g)].sjis = sjis;
        if (!inverse_.emplace(ch, g).second) ++duplicates_;
    }
    static Script classify(int g, wchar_t ch) {
        if (fixedKeep(g)) return Script::Fixed;
        if (!ch) return Script::None;
        if (g < 0x5F) return Script::Ascii;
        if (g >= 0x26E && g <= 0x2AD) return Script::Latin1;
        if (g >= 0x2C0 && g <= 0x2FF) return Script::Cp1252;
        if (g >= 0x3A8 && g <= 0x3FF) return Script::Nec13;
        if (g >= 0x400) return Script::Kanji;
        if (ch >= 0x3040 && ch <= 0x30FF) return Script::Kana;
        return Script::Jis;
    }
};

}  // namespace tf
