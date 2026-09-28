// CPU atlas builder for the game's 64 x 128 glyph grid. Workers use copied inputs; no game memory or Direct3D.
// Retain native cells for unmapped, excluded, empty or missing glyphs, and on rendering errors.
// Cache glyph coverage so Brightness and Compress changes do not require rasterization.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include "charmap.h"
#include "glyph.h"
#include "texfmt.h"
#include "fontgate.h"
#include "ftface.h"

namespace tf {

// The advance of a glyph past the width table (`add dword [eax],0Eh`, `cmp ax,440h` in the client's advance, 11D840).
inline constexpr int kAtlasDefaultAdvance = 14;
inline constexpr size_t kAtlasWidths = 0x440;

struct AtlasOptions {
    std::wstring family = L"Segoe UI";
    int weight = FW_NORMAL;    // 400 Regular, 600 Semibold, 700 Bold
    bool fauxBold = false;     // 1-unit dilation
    bool italic = false;       // the font's italic face, or GDI's oblique (placed by its ink box, overhang included)
    double gamma = 1.0;        // 0.6..1.6
    int cell = 16;             // 16, 32, 48 or 64 (Chat and menus' Sharpness: texfmt.h chatCellFor)
    bool allScripts = false;   // false: Latin only (glyphs < 60h and 26E..2FF)
    bool compress = false;     // Quality's Compress: uploaded as DXT3 (the build itself is the same)
    bool hinting = true;       // Quality's Hinting: off draws the unhinted outlines
    Engine engine = Engine::Windows;   // Quality's Engine (ftface.h)
    std::wstring jpFamily;     // the Japanese font: the full-width face's family with All scripts ("" = `family`)
    const std::wstring& fullFamily() const { return jpFamily.empty() ? family : jpFamily; }
    // DEV modes: Identity copies every cell from the readback; Coverage fills every rendered cell magenta.
    enum class Mode : uint8_t { Normal, Identity, Coverage };
    Mode mode = Mode::Normal;
};
inline bool sameOptions(const AtlasOptions& a, const AtlasOptions& b) {
    // The Japanese font draws only with All scripts, so it counts only then.
    return a.family == b.family && a.weight == b.weight && a.fauxBold == b.fauxBold && a.italic == b.italic && a.gamma == b.gamma && a.cell == b.cell &&
           a.allScripts == b.allScripts && a.compress == b.compress && a.hinting == b.hinting && a.engine == b.engine && a.mode == b.mode &&
           (!a.allScripts || a.fullFamily() == b.fullFamily());
}

// Original atlas pixels. Greyscale readbacks use lossless A8L8 storage at half the ARGB memory cost.
struct Readback {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // A8R8G8B8 (empty while `la` holds the texels)
    std::vector<uint16_t> la;   // A8L8, only for a grey texture (compactIfGrey)
    int ink = -1;      // cells with ink (countInk, on the worker); -1 not counted
    double ms = 0;     // the worker's decode or load
    std::string note;  // the DAT fallback's file size and CRC32 (datfallback.h), for the log
    int cell() const { return w / kGridCols; }
    size_t texels() const { return size_t(w) * size_t(h); }
    bool grey() const { return !la.empty(); }
    bool valid() const { const int s = cell(); return s >= 1 && w == s * kGridCols && h == s * kGridRows && (px.size() == texels() || la.size() == texels()); }
    size_t bytes() const { return px.size() * 4 + la.size() * 2; }
    static uint32_t expand(uint16_t v) { const uint32_t l = v & 255u; return (uint32_t(v >> 8) << 24) | (l << 16) | (l << 8) | l; }
    uint32_t at(int x, int y) const { const size_t i = size_t(y) * size_t(w) + size_t(x); return la.empty() ? px[i] : expand(la[i]); }
    std::vector<uint32_t> argb() const {
        if (la.empty()) return px;
        std::vector<uint32_t> out(la.size());
        for (size_t i = 0; i < la.size(); i++) out[i] = expand(la[i]);
        return out;
    }
    // A8R8G8B8 texels in; kept at 2 bytes a texel when every one is grey.
    void setArgb(std::vector<uint32_t> v) {
        px = std::move(v);
        la.clear();
        compactIfGrey();
    }
    void compactIfGrey() {
        if (px.empty()) return;
        for (const uint32_t p : px)
            if (((p >> 16) & 255) != ((p >> 8) & 255) || ((p >> 8) & 255) != (p & 255)) return;
        la.resize(px.size());
        for (size_t i = 0; i < px.size(); i++) la[i] = uint16_t(((px[i] >> 24) << 8) | (px[i] & 255));
        std::vector<uint32_t>().swap(px);
    }
    // No texel of the cell has alpha above 0.
    bool cellEmpty(int g) const {
        const int s = cell(), x0 = (g % kGridCols) * s, y0 = (g / kGridCols) * s;
        for (int y = 0; y < s; y++) {
            const size_t row = size_t(y0 + y) * size_t(w) + size_t(x0);
            if (la.empty()) { for (int x = 0; x < s; x++) if (px[row + size_t(x)] >> 24) return false; }
            else for (int x = 0; x < s; x++) if (la[row + size_t(x)] >> 8) return false;
        }
        return true;
    }
};

inline int countInk(const Readback& rb) {
    int n = 0;
    for (int g = 0; g < kGridCells; g++) n += rb.cellEmpty(g) ? 0 : 1;
    return n;
}

enum class CellOutcome : uint8_t { Rendered, KeptFixed, KeptUnmapped, KeptScript, KeptEmpty, KeptNoGlyph, KeptBlank, KeptError };
inline const char* outcomeName(CellOutcome o) {
    switch (o) {
    case CellOutcome::Rendered: return "rendered";
    case CellOutcome::KeptFixed: return "fixed list";
    case CellOutcome::KeptUnmapped: return "no mapping";
    case CellOutcome::KeptScript: return "outside the scripts";
    case CellOutcome::KeptEmpty: return "native empty";
    case CellOutcome::KeptNoGlyph: return "font lacks it";
    case CellOutcome::KeptBlank: return "blank in the font";
    case CellOutcome::KeptError: return "engine error";
    }
    return "?";
}

struct AtlasStats {
    int count[8] = {};           // by CellOutcome
    int condensed = 0;           // rendered cells drawn with eM11 < 1
    int squeezed = 0;            // rendered cells drawn with eM22 < 1 (accented capitals, tall brackets)
    int descenders = 0;          // rendered cells whose rows under the baseline were area-averaged into the room
    int clipped = 0;             // rendered cells with ink outside the cell (clipped)
    int spaces = 0;              // rendered cells that are white space (drawn blank)
    int emPx = 0, capPx = 0;     // the Latin face's pixel height and its measured 'H' height
    int fullEmPx = 0;            // the full-width face's pixel height (All scripts)
    int fullBase = 0;            // its baseline row (the reference ideograph's ink ends on row C-1)
    int fullRefPx = 0;           // the reference ideograph's measured ink height (0: not measured)
    std::wstring fullFace;       // the face GDI selected for the full-width cells (All scripts)
    bool fullSubstituted = false;   // ... another face than the one asked for (the Japanese font, or the Font)
    wchar_t backslash = 0;       // the character drawn for the backslash cell 3Ch (0: kept native)
    double capRatio = 0;         // cap height / em from the font
    std::wstring face;           // the face GDI actually selected
    bool substituted = false;    // GDI substituted another face for the one asked for
    Engine engine = Engine::Windows, fullEngine = Engine::Windows;   // what drew the Latin and the full-width cells
    std::string engineNote;      // a face FreeType could not open, and why ("" = none)
    bool engineFaulted = false;  // ... FreeType failed on a face during the build (after it opened)
    double ms = 0;               // build time
    long rasters = 0;            // glyph rasters drawn (engine calls that draw a glyph; 0 when the shapes were re-used)
    bool shapesReused = false;   // the kept shapes re-mapped (a Brightness or Compress change), no glyph drawn
    int rendered() const { return count[int(CellOutcome::Rendered)]; }
    int kept() const { int n = 0; for (int i = 1; i < 8; i++) n += count[i]; return n; }
};

// Cached glyph coverage after faux bold. Reuse requires matching shape options, font generation,
// fitted faces and native empty cells.
struct AtlasShapes {
    AtlasOptions o;                    // gamma and compress do not count
    unsigned fontGen = 0;              // fontGate().generation() while it drew
    std::vector<uint8_t> nativeEmpty;  // per cell: 0 ink, 1 empty, 2 not looked at (the cell was not allowed)
    std::vector<uint32_t> covAt;       // per cell: 1 + the index of its C x C coverage in `cov`; 0: kept native
    std::vector<uint8_t> cov;
    std::vector<CellOutcome> outcome;
    std::vector<uint8_t> boxW;
    AtlasStats stats;                  // the drawing build's (ms, rasters and shapesReused aside)
    bool curly = false;                // the quote cells were drawn curly (curlyQuotes: the sheet's own quotes are)
    size_t bytes() const { return cov.capacity() + covAt.capacity() * 4 + nativeEmpty.capacity() + outcome.capacity() + boxW.capacity(); }
};
inline bool sameShapeOptions(const AtlasOptions& a, const AtlasOptions& b) {
    AtlasOptions x = a;
    x.gamma = b.gamma;
    x.compress = b.compress;
    return sameOptions(x, b);
}

struct Atlas {
    int cell = 16, w = 0, h = 0;
    std::vector<uint32_t> px;              // A8R8G8B8, row-major (buildAtlas only; buildAtlasTex builds straight into `tex`)
    TexImage tex;                          // the upload-ready bytes (the worker's conversion)
    std::vector<CellOutcome> outcome;      // per glyph
    std::vector<uint8_t> boxW;             // per rendered glyph: the advance box its ink was centred in (px)
    AtlasStats stats;
    std::shared_ptr<const AtlasShapes> shapes;   // what drew it (buildAtlasTex; the next build may re-use it)
};

// Native cell g from the readback into an A8R8G8B8 image `w` texels wide at cell size c, scaled to it.
inline void copyNativeCellArgb(const Readback& rb, int g, uint32_t* img, int w, int c) {
    const int s = rb.cell();
    const int sx0 = (g % kGridCols) * s, sy0 = (g / kGridCols) * s, dx0 = (g % kGridCols) * c, dy0 = (g / kGridCols) * c;
    for (int y = 0; y < c; y++) {
        uint32_t* dst = img + size_t(dy0 + y) * size_t(w) + size_t(dx0);
        for (int x = 0; x < c; x++) {
            if (s == 2 * c) {
                uint32_t r = 0, gg = 0, b = 0, al = 0;
                for (int k = 0; k < 4; k++) {
                    const uint32_t p = rb.at(sx0 + 2 * x + (k & 1), sy0 + 2 * y + (k >> 1));
                    r += (p >> 16) & 255; gg += (p >> 8) & 255; b += p & 255; al = (std::max)(al, p >> 24);
                }
                dst[x] = (al << 24) | (((r + 2) / 4) << 16) | (((gg + 2) / 4) << 8) | ((b + 2) / 4);
            } else {
                dst[x] = rb.at(sx0 + x * s / c, sy0 + y * s / c);
            }
        }
    }
}
// The same into an A8L8 image: each texel toA8L8 of copyNativeCellArgb's.
inline void copyNativeCellA8L8(const Readback& rb, int g, uint16_t* img, int w, int c) {
    const int s = rb.cell();
    const int sx0 = (g % kGridCols) * s, sy0 = (g / kGridCols) * s, dx0 = (g % kGridCols) * c, dy0 = (g / kGridCols) * c;
    const bool grey = rb.grey();
    for (int y = 0; y < c; y++) {
        uint16_t* dst = img + size_t(dy0 + y) * size_t(w) + size_t(dx0);
        for (int x = 0; x < c; x++) {
            if (s == 2 * c) {
                if (grey) {
                    uint32_t l = 0, al = 0;
                    for (int k = 0; k < 4; k++) {
                        const uint16_t p = rb.la[size_t(sy0 + 2 * y + (k >> 1)) * size_t(rb.w) + size_t(sx0 + 2 * x + (k & 1))];
                        l += p & 255u; al = (std::max)(al, uint32_t(p >> 8));
                    }
                    dst[x] = uint16_t((al << 8) | ((l + 2) / 4));
                } else {
                    uint32_t r = 0, gg = 0, b = 0, al = 0;
                    for (int k = 0; k < 4; k++) {
                        const uint32_t p = rb.px[size_t(sy0 + 2 * y + (k >> 1)) * size_t(rb.w) + size_t(sx0 + 2 * x + (k & 1))];
                        r += (p >> 16) & 255; gg += (p >> 8) & 255; b += p & 255; al = (std::max)(al, p >> 24);
                    }
                    dst[x] = toA8L8((al << 24) | (((r + 2) / 4) << 16) | (((gg + 2) / 4) << 8) | ((b + 2) / 4));
                }
            } else {
                const size_t i = size_t(sy0 + y * s / c) * size_t(rb.w) + size_t(sx0 + x * s / c);
                dst[x] = grey ? rb.la[i] : toA8L8(rb.px[i]);
            }
        }
    }
}
inline void copyNativeCell(const Readback& rb, int g, Atlas& a) { copyNativeCellArgb(rb, g, a.px.data(), a.w, a.cell); }

// A glyph's outline (GGO_NATIVE's TTPOLYGONHEADER / TTPOLYCURVE records) as rings, curves flattened (for Hinting off).
// Every record is bounds-checked against the buffer; false when one does not fit.
inline bool outlineRings(const uint8_t* buf, size_t size, std::vector<Ring>& rings) {
    rings.clear();
    const auto fx = [](const FIXED& f) { return double(f.value) + double(f.fract) / 65536.0; };
    const auto pt = [&](const POINTFX& p) { return PathPoint{fx(p.x), fx(p.y)}; };
    size_t off = 0;
    while (off < size) {
        if (size - off < sizeof(TTPOLYGONHEADER)) return false;
        TTPOLYGONHEADER h;
        std::memcpy(&h, buf + off, sizeof h);
        if (h.cb < sizeof(TTPOLYGONHEADER) || h.cb > size - off) return false;
        Ring r;
        r.push_back(pt(h.pfxStart));
        size_t c = off + sizeof(TTPOLYGONHEADER);
        const size_t end = off + h.cb;
        while (c < end) {
            if (end - c < 2 * sizeof(WORD)) return false;
            WORD type = 0, n = 0;
            std::memcpy(&type, buf + c, sizeof type);
            std::memcpy(&n, buf + c + sizeof(WORD), sizeof n);
            const size_t bytes = 2 * sizeof(WORD) + size_t(n) * sizeof(POINTFX);
            if (n == 0 || bytes > end - c) return false;
            std::vector<PathPoint> p(n);
            for (WORD i = 0; i < n; i++) {
                POINTFX q;
                std::memcpy(&q, buf + c + 2 * sizeof(WORD) + size_t(i) * sizeof(POINTFX), sizeof q);
                p[i] = pt(q);
            }
            if (type == TT_PRIM_LINE) r.insert(r.end(), p.begin(), p.end());
            else if (type == TT_PRIM_QSPLINE) {   // off-curve points with implied on-curve midpoints; the last is on the curve
                for (WORD i = 0; i + 1 < n; i++) {
                    const PathPoint a = r.back(), b = p[i];
                    const PathPoint e = i + 2 < n ? PathPoint{(p[i].x + p[i + 1].x) / 2, (p[i].y + p[i + 1].y) / 2} : p[i + 1];
                    addQuad(r, a, b, e);
                }
            } else if (type == TT_PRIM_CSPLINE) {   // (control, control, end) triples (a PostScript-outline font)
                for (WORD i = 0; i + 2 < n; i += 3) addCubic(r, r.back(), p[i], p[i + 1], p[i + 2]);
            } else return false;
            c += bytes;
        }
        rings.push_back(std::move(r));
        off = end;
    }
    return true;
}

class GdiFace {
public:
    GdiFace() = default;
    GdiFace(const GdiFace&) = delete;
    GdiFace& operator=(const GdiFace&) = delete;
    ~GdiFace() { close(); }
    // `italic`: the font's italic face, or GDI's oblique when it has none. `hinted` false: every glyph and measurement
    // comes from the unhinted outline.
    bool open(const std::wstring& family, int emPx, int weight, bool italic = false, bool hinted = true) {
        close();
        hinted_ = hinted;
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) return false;
        font_ = CreateFontW(-emPx, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, family.c_str());
        if (!font_) { close(); return false; }
        old_ = SelectObject(dc_, font_);
        wchar_t name[LF_FACESIZE] = {};
        GetTextFaceW(dc_, LF_FACESIZE, name);
        face_ = name;
        embolden_ = 0;
        if (!hinted && weight >= 600) embolden_ = simulatedBold(family, emPx, italic);
        return true;
    }
    void close() {
        if (dc_ && old_) SelectObject(dc_, old_);
        if (font_) DeleteObject(font_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr; font_ = nullptr; old_ = nullptr;
    }
    HDC dc() const { return dc_; }
    const std::wstring& face() const { return face_; }
    bool has(wchar_t ch) const {
        WORD idx = 0xFFFF;
        return GetGlyphIndicesW(dc_, &ch, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) == 1 && idx != 0xFFFF;
    }
    bool hinted() const { return hinted_; }
    int embolden() const { return embolden_; }   // px added to the unhinted outlines (GDI's simulated bold)
    long rasters() const { return rasters_; }    // raster() calls so far
    // A character's black box (hinted, or its unhinted outline's). False on a GDI error.
    bool metrics(wchar_t ch, GLYPHMETRICS& gm) const {
        static const MAT2 id = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        gm = GLYPHMETRICS{};
        if (hinted_) return GetGlyphOutlineW(dc_, ch, GGO_METRICS, &gm, 0, nullptr, &id) != GDI_ERROR;
        std::vector<Ring> rings;
        if (!outline(ch, id, gm, rings)) return false;
        OutlineBox b;
        if (!outlineBox(rings, b)) { gm.gmBlackBoxX = gm.gmBlackBoxY = 0; return true; }
        gm.gmBlackBoxX = UINT(b.w + embolden_);
        gm.gmBlackBoxY = UINT(b.h);
        gm.gmptGlyphOrigin.x = b.originX;
        gm.gmptGlyphOrigin.y = b.originY;
        return true;
    }
    int boxHeight(wchar_t ch) const {
        GLYPHMETRICS gm{};
        return metrics(ch, gm) ? int(gm.gmBlackBoxY) : 0;
    }
    // GGO_GRAY8_BITMAP with horizontal and vertical scales about the pen origin (hinting off: the unhinted outline
    // filled the same way). False on a GDI error.
    bool raster(wchar_t ch, double condense, double squeeze, GLYPHMETRICS& gm, std::vector<uint8_t>& bits, bool& blank) const {
        MAT2 m{};
        const auto toFixed = [](double v) {
            const long f = std::lround(std::clamp(v, 1.0 / 64, 1.0) * 65536.0);
            FIXED x{};
            x.value = short(f >> 16);
            x.fract = WORD(f & 0xFFFF);
            return x;
        };
        ++rasters_;
        m.eM11 = toFixed(condense);
        m.eM22 = toFixed(squeeze);
        gm = GLYPHMETRICS{};
        if (!hinted_) {
            std::vector<Ring> rings;
            if (!outline(ch, m, gm, rings)) return false;
            OutlineBox b;
            blank = !fillOutline(rings, b, bits);
            if (!blank && embolden_ > 0) {   // GDI's simulated bold, which the native outline does not carry
                const int w = b.w + embolden_, stride = (b.w + 3) & ~3, wide = (w + 3) & ~3;
                std::vector<uint8_t> out(size_t(wide) * size_t(b.h), 0);
                for (int y = 0; y < b.h; y++) std::memcpy(out.data() + size_t(y) * size_t(wide), bits.data() + size_t(y) * size_t(stride), size_t(b.w));
                dilateRight(out.data(), w, b.h, wide, embolden_);
                bits.swap(out);
                b.w = w;
            }
            gm.gmBlackBoxX = blank ? 0 : UINT(b.w);
            gm.gmBlackBoxY = blank ? 0 : UINT(b.h);
            gm.gmptGlyphOrigin.x = b.originX;
            gm.gmptGlyphOrigin.y = b.originY;
            return true;
        }
        const DWORD need = GetGlyphOutlineW(dc_, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        if (need == GDI_ERROR) return false;
        blank = need == 0 || gm.gmBlackBoxX == 0 || gm.gmBlackBoxY == 0;
        if (blank) { bits.clear(); return true; }
        bits.assign(need, 0);
        if (GetGlyphOutlineW(dc_, ch, GGO_GRAY8_BITMAP, &gm, need, bits.data(), &m) == GDI_ERROR) return false;
        return true;
    }

private:
    // The unhinted outline under `m` (GGO_NATIVE | GGO_UNHINTED); gm carries the advance. False on a GDI error or a
    // record that does not fit its buffer.
    bool outline(wchar_t ch, const MAT2& m, GLYPHMETRICS& gm, std::vector<Ring>& rings) const {
        rings.clear();
        const DWORD need = GetGlyphOutlineW(dc_, ch, GGO_NATIVE | GGO_UNHINTED, &gm, 0, nullptr, &m);
        if (need == GDI_ERROR) return false;
        if (need == 0) return true;   // no contours (a space)
        std::vector<uint8_t> buf(need);
        if (GetGlyphOutlineW(dc_, ch, GGO_NATIVE | GGO_UNHINTED, &gm, need, buf.data(), &m) == GDI_ERROR) return false;
        return outlineRings(buf.data(), buf.size(), rings);
    }

    // GDI can return unemboldened CFF outlines for a simulated weight. If they match the regular face,
    // dilate by the largest hinted black-box growth among reference glyphs, at least 1 px.
    int simulatedBold(const std::wstring& family, int emPx, bool italic) const {
        GdiFace regular;
        if (!regular.open(family, emPx, FW_NORMAL, italic, true) || regular.face() != face_) return 0;
        static const MAT2 id = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        const auto native = [&](HDC dc, wchar_t c, std::vector<uint8_t>& out) {
            GLYPHMETRICS g{};
            const DWORD n = GetGlyphOutlineW(dc, c, GGO_NATIVE | GGO_UNHINTED, &g, 0, nullptr, &id);
            if (n == GDI_ERROR || !n) return false;
            out.assign(n, 0);
            return GetGlyphOutlineW(dc, c, GGO_NATIVE | GGO_UNHINTED, &g, n, out.data(), &id) != GDI_ERROR;
        };
        int checked = 0, wider = 0;
        for (const wchar_t c : {L'H', L'o', L'0', L'l', L'm', L'n', L'E'}) {
            if (!has(c) || !regular.has(c)) continue;
            std::vector<uint8_t> a, b;
            if (!native(dc_, c, a) || !native(regular.dc_, c, b)) continue;
            if (a != b) return 0;   // a real heavier face: its own outlines
            ++checked;
            GLYPHMETRICS h1{}, h2{};
            if (GetGlyphOutlineW(dc_, c, GGO_METRICS, &h1, 0, nullptr, &id) == GDI_ERROR || GetGlyphOutlineW(regular.dc_, c, GGO_METRICS, &h2, 0, nullptr, &id) == GDI_ERROR) continue;
            wider = std::max(wider, int(h1.gmBlackBoxX) - int(h2.gmBlackBoxX));
        }
        return checked ? std::max(1, wider) : 0;
    }

    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ old_ = nullptr;
    std::wstring face_;
    bool hinted_ = true;
    int embolden_ = 0;
    mutable long rasters_ = 0;
};

// Common GDI/FreeType interface. An unusable FreeType face falls back to GDI.
class GlyphFace {
public:
    GlyphFace() = default;
    GlyphFace(const GlyphFace&) = delete;
    GlyphFace& operator=(const GlyphFace&) = delete;
    bool open(const std::wstring& family, int emPx, int weight, bool italic = false, bool hinted = true, Engine engine = Engine::Windows) {
        close();
        Engine use = engine;
        try {
            if (engine == Engine::Auto) {
                const bool wine = underWine();
                use = pickEngine(engine, !wine && currentFtEnv()->cff(family, weight, italic), wine);
            }
            if (use == Engine::FreeType) {
                auto f = std::make_unique<FtFace>();
                if (f->open(currentFtEnv(), family, emPx, weight, italic, hinted)) {
                    ft_ = std::move(f);
                    family_ = family;
                    em_ = emPx;
                    weight_ = weight;
                    italic_ = italic;
                    hinted_ = hinted;
                    return true;
                }
                fellBack_ = true;
                why_ = f->why();
            }
        } catch (const std::bad_alloc&) {   // no memory for FreeType's part; Windows needs no copy of the font
            ft_.reset();
            if (use == Engine::FreeType) {
                fellBack_ = true;
                why_ = "there was not enough memory for FreeType";
            }
        }
        return gdi_.open(family, emPx, weight, italic, hinted);
    }
    void close() {
        ft_.reset();
        gdi_.close();
        fellBack_ = faulted_ = false;
        why_.clear();
    }
    Engine used() const { return live() ? Engine::FreeType : Engine::Windows; }
    bool fellBack() const { return fellBack_; }
    bool faulted() const { return faulted_; }   // FreeType faulted after the face opened; Windows draws the rest
    const std::string& fallbackWhy() const { return why_; }
    const FtFace* ft() const { return ft_.get(); }
    const std::wstring& face() const { return live() ? ft_->face() : gdi_.face(); }
    bool has(wchar_t ch) const { return live() ? ft_->has(ch) : gdi_.has(ch); }
    bool hinted() const { return live() ? ft_->hinted() : gdi_.hinted(); }
    long rasters() const { return live() ? ft_->rasters() : gdi_.rasters(); }
    bool metrics(wchar_t ch, GLYPHMETRICS& gm) const {
        if (const FtFace* f = live()) {
            const bool ok = f->metrics(ch, gm);
            if (!f->dead()) return ok;
        }
        return live() ? false : gdi_.metrics(ch, gm);
    }
    int boxHeight(wchar_t ch) const {
        GLYPHMETRICS gm{};
        return metrics(ch, gm) ? int(gm.gmBlackBoxY) : 0;
    }
    bool raster(wchar_t ch, double condense, double squeeze, GLYPHMETRICS& gm, std::vector<uint8_t>& bits, bool& blank) const {
        if (const FtFace* f = live()) {
            const bool ok = f->raster(ch, condense, squeeze, gm, bits, blank);
            if (!f->dead()) return ok;
        }
        return live() ? false : gdi_.raster(ch, condense, squeeze, gm, bits, blank);   // a fault: this glyph too
    }

private:
    // Switch a failed FreeType face to GDI once, preserving the original request.
    const FtFace* live() const {
        if (!ft_) return nullptr;
        if (!ft_->dead()) return ft_.get();
        if (!faulted_) {
            faulted_ = fellBack_ = true;
            why_ = ft_->why();
            gdi_.open(family_, em_, weight_, italic_, hinted_);
        }
        return nullptr;
    }

    mutable GdiFace gdi_;
    std::unique_ptr<FtFace> ft_;
    std::wstring family_;
    int em_ = 0, weight_ = 0;
    bool italic_ = false, hinted_ = true;
    mutable bool fellBack_ = false, faulted_ = false;
    mutable std::string why_;
};
inline std::string fallbackNote(const GlyphFace& f) {
    if (!f.fellBack()) return std::string();
    return f.faulted() ? "FreeType failed on it (" + f.fallbackWhy() + "), so Windows draws the rest of it" : "FreeType could not open it (" + f.fallbackWhy() + "), so Windows draws it";
}
// Include fallbacks that occurred after the faces opened.
inline void noteLateFallbacks(AtlasStats& st, const GlyphFace& latin, const GlyphFace& full, bool allScripts) {
    if (latin.faulted() && st.engine == Engine::FreeType) {
        st.engine = Engine::Windows;
        st.engineFaulted = true;
        st.engineNote = fallbackNote(latin) + (st.engineNote.empty() ? "" : "; ") + st.engineNote;
    }
    if (allScripts && full.faulted() && st.fullEngine == Engine::FreeType) {
        st.fullEngine = Engine::Windows;
        st.engineFaulted = true;
        st.engineNote += std::string(st.engineNote.empty() ? "" : "; ") + "kana and kanji: " + fallbackNote(full);
    }
}

inline bool sameFace(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// The Latin face's pixel height.
inline bool fitLatinFace(const AtlasOptions& o, GlyphFace& face, AtlasStats& st, std::string& why) {
    const CellGeom g{o.cell};
    GlyphFace ref;
    constexpr int kRefEm = 1024;
    if (!ref.open(o.family, kRefEm, o.weight, o.italic, o.hinting, o.engine)) { why = "CreateFontW failed"; return false; }
    const double ratio = ref.has(L'H') ? double(ref.boxHeight(L'H')) / kRefEm : 0.0;
    st.capRatio = ratio;
    const int est = emForCap(ratio, o.cell);
    const int target = g.capTarget();
    int best = est, bestErr = 1 << 30;
    if (ref.has(L'H')) {
        for (int d : {0, -1, 1, -2, 2}) {
            const int em = est + d;
            if (em < 1) continue;
            GlyphFace t;
            if (!t.open(o.family, em, o.weight, o.italic, o.hinting, o.engine)) continue;
            const int err = std::abs(t.boxHeight(L'H') - target);
            if (err < bestErr) { bestErr = err; best = em; }
        }
    }
    if (!face.open(o.family, best, o.weight, o.italic, o.hinting, o.engine)) { why = "CreateFontW failed"; return false; }
    st.emPx = best;
    st.engine = face.used();
    st.engineNote = fallbackNote(face);
    st.capPx = face.has(L'H') ? face.boxHeight(L'H') : 0;
    st.face = face.face();
    st.substituted = !sameFace(face.face(), o.family);
    return true;
}

// The full-width face is fitted like native kanji: ten common ideographs set its em (median height closest to
// fullTarget) and baseline (the median bottom on row C-1). With fewer than three, the estimate and the Latin baseline.
struct IdeoMetrics { int n = 0, height = 0, below = 0; };
inline IdeoMetrics measureIdeographs(const GlyphFace& f) {
    static const wchar_t kRefs[] = L"\u56FD\u6C38\u6771\u8A9E\u5B66\u66F8\u8AAD\u65B0\u805E\u96FB";   // 国永東語学書読新聞電
    std::vector<int> hs, bs;
    for (const wchar_t c : kRefs) {
        if (!c || !f.has(c)) continue;
        GLYPHMETRICS gm{};
        if (!f.metrics(c, gm) || !gm.gmBlackBoxY) continue;
        hs.push_back(int(gm.gmBlackBoxY));
        bs.push_back(int(gm.gmBlackBoxY) - int(gm.gmptGlyphOrigin.y));
    }
    IdeoMetrics m;
    m.n = int(hs.size());
    if (m.n < 3) return m;
    std::sort(hs.begin(), hs.end());
    std::sort(bs.begin(), bs.end());
    m.height = hs[hs.size() / 2];
    m.below = bs[bs.size() / 2];
    return m;
}
inline bool fitFullFace(const AtlasOptions& o, GlyphFace& face, AtlasStats& st, std::string& why) {
    const CellGeom g{o.cell};
    const int target = fullTarget(o.cell);
    const std::wstring& family = o.fullFamily();
    GlyphFace ref;
    constexpr int kRefEm = 1024;
    if (!ref.open(family, kRefEm, o.weight, o.italic, o.hinting, o.engine)) { why = "CreateFontW failed (full-width face)"; return false; }
    const IdeoMetrics r = measureIdeographs(ref);
    int best = emForFull(o.cell), bestScore = 1 << 30;
    IdeoMetrics bestM;
    if (r.n >= 3) {
        const int est = std::max(1, int(std::lround(target / (double(r.height) / kRefEm))));
        for (int d : {0, -1, 1, -2, 2}) {
            const int em = est + d;
            if (em < 1) continue;
            GlyphFace t;
            if (!t.open(family, em, o.weight, o.italic, o.hinting, o.engine)) continue;
            const IdeoMetrics m = measureIdeographs(t);
            if (m.n < 3) continue;
            const int score = (m.height > target ? 1000 : 0) + std::abs(m.height - target);
            if (score < bestScore) { bestScore = score; best = em; bestM = m; }
        }
    }
    if (!face.open(family, best, o.weight, o.italic, o.hinting, o.engine)) { why = "CreateFontW failed (full-width face)"; return false; }
    st.fullEmPx = best;
    st.fullEngine = face.used();
    if (face.fellBack()) st.engineNote += std::string(st.engineNote.empty() ? "" : "; ") + "kana and kanji: " + fallbackNote(face);
    st.fullFace = face.face();
    st.fullSubstituted = !sameFace(face.face(), family);
    st.fullRefPx = bestM.height;
    st.fullBase = bestM.n >= 3 ? g.C - bestM.below : 0;   // the median ideograph ends on row C-1
    if (st.fullBase <= 0 || st.fullBase > g.C) st.fullBase = 0;
    return true;
}

// Some Japanese fonts draw U+005C as yen. Prefer an equivalent backslash glyph or retain native art.
inline bool sameBitmap(const GlyphFace& f, wchar_t a, wchar_t b) {
    GLYPHMETRICS ga{}, gb{};
    std::vector<uint8_t> ba, bb;
    bool blankA = false, blankB = false;
    if (!f.raster(a, 1.0, 1.0, ga, ba, blankA) || !f.raster(b, 1.0, 1.0, gb, bb, blankB)) return false;
    return blankA == blankB && ga.gmBlackBoxX == gb.gmBlackBoxX && ga.gmBlackBoxY == gb.gmBlackBoxY && ga.gmptGlyphOrigin.x == gb.gmptGlyphOrigin.x &&
           ga.gmptGlyphOrigin.y == gb.gmptGlyphOrigin.y && ba == bb;
}
// Preserve the source sheet's quote style (including mods): font high normally uses curly quotes, moji straight.
// Opening quotes in cells 02h and 40h need at least 1.2x as much ink below their midpoint as above.
// Use U+201C/U+2019/U+2018 together only if the selected font supports all three.
inline double inkFootShare(const Readback& rb, int g) {   // the lower half of a cell's ink (alpha x level) over its upper half
    const int c = rb.cell(), x0 = (g % kGridCols) * c, y0 = (g / kGridCols) * c;
    std::vector<double> row(size_t(c), 0.0);
    int top = -1, bottom = -1;
    for (int y = 0; y < c; y++) {
        for (int x = 0; x < c; x++) {
            const uint32_t p = rb.at(x0 + x, y0 + y);
            row[size_t(y)] += double(p >> 24) / 255.0 * double(std::max({(p >> 16) & 255u, (p >> 8) & 255u, p & 255u})) / 255.0;
        }
        if (row[size_t(y)] > 0) { if (top < 0) top = y; bottom = y + 1; }
    }
    if (top < 0) return 0;
    const double mid = (top + bottom) / 2.0;
    double up = 0, down = 0;
    for (int y = top; y < bottom; y++) {
        const double inUp = std::clamp(mid - y, 0.0, 1.0);   // the share of row y over the middle
        up += row[size_t(y)] * inUp;
        down += row[size_t(y)] * (1.0 - inUp);
    }
    return up > 0 ? down / up : 0;
}
inline bool curlyQuotes(const Readback& rb) { return rb.valid() && inkFootShare(rb, 0x02) >= 1.2 && inkFootShare(rb, 0x40) >= 1.2; }
inline wchar_t quoteFor(wchar_t ch, const GlyphFace& f, bool curly) {
    if (!curly || !f.has(L'\u201C') || !f.has(L'\u2019') || !f.has(L'\u2018')) return ch;
    return ch == L'"' ? L'\u201C' : ch == L'\'' ? L'\u2019' : ch == L'`' ? L'\u2018' : ch;
}
inline constexpr int kBackslashCell = 0x3C;
inline wchar_t backslashFor(const GlyphFace& f) {
    if (!f.has(L'\\')) return 0;
    if (!f.has(L'\u00A5') || !sameBitmap(f, L'\\', L'\u00A5')) return L'\\';
    for (const wchar_t c : {L'\uFF3C', L'\u29F5', L'\u2216'})
        if (f.has(c) && !sameBitmap(f, c, L'\u00A5')) return c;
    return 0;
}

// Build A8L8/DXT3 uploads on a 2-byte canvas to avoid allocating a full ARGB atlas.
struct AtlasCanvas {
    bool argb = true;
    int w = 0, c = 16;
    std::vector<uint32_t> words;
    uint32_t* p32() { return words.data(); }
    uint16_t* p16() { return reinterpret_cast<uint16_t*>(words.data()); }
    void init(bool isArgb, int W, int H, int C) {
        argb = isArgb;
        w = W;
        c = C;
        words.assign(isArgb ? size_t(W) * size_t(H) : size_t(W) * size_t(H) / 2, 0);
    }
    // A drawn cell: magenta in DEV Coverage, else each coverage through the gamma table (texel()).
    void drawn(int gl, const uint8_t* cov, const GammaLut& lut, bool coverage) {
        const CellGeom g{c};
        const int x0 = g.cellX(gl), y0 = g.cellY(gl);
        for (int y = 0; y < c; y++) {
            const size_t row = size_t(y0 + y) * size_t(w) + size_t(x0);
            if (argb) {
                uint32_t* dst = p32() + row;
                if (coverage) std::fill_n(dst, size_t(c), 0xFFFF00FFu);
                else for (int x = 0; x < c; x++) dst[x] = texel(cov[size_t(y) * size_t(c) + size_t(x)], lut);
            } else {
                uint16_t* dst = p16() + row;
                if (coverage) std::fill_n(dst, size_t(c), toA8L8(0xFFFF00FFu));
                else for (int x = 0; x < c; x++) dst[x] = toA8L8(texel(cov[size_t(y) * size_t(c) + size_t(x)], lut));
            }
        }
    }
    void native(const Readback& rb, int gl) {
        if (argb) copyNativeCellArgb(rb, gl, p32(), w, c);
        else copyNativeCellA8L8(rb, gl, p16(), w, c);
    }
};

// Recheck fitted faces: installed fonts behind a family name may have changed.
inline bool sameFaces(const AtlasStats& a, const AtlasStats& b) {
    return a.face == b.face && a.substituted == b.substituted && a.emPx == b.emPx && a.capPx == b.capPx && a.capRatio == b.capRatio && a.fullFace == b.fullFace &&
           a.fullSubstituted == b.fullSubstituted && a.fullEmPx == b.fullEmPx && a.fullRefPx == b.fullRefPx && a.fullBase == b.fullBase && a.engine == b.engine &&
           a.fullEngine == b.fullEngine;
}

// Why a set of kept shapes was not used (AtlasWorker::shapesRefusedWhy).
enum class ShapesRefusal : int { None = 0, Options, Faces, EmptyCells };
inline const char* shapesRefusalText(ShapesRefusal r) {
    switch (r) {
    case ShapesRefusal::None: return "";
    case ShapesRefusal::Options: return "the options they were drawn with, the fonts folder, or the game's font sheet (16 / 32 px, its quote shapes) changed";
    case ShapesRefusal::Faces: return "the font Windows picks for the family changed";
    case ShapesRefusal::EmptyCells: return "the game's font changed its empty cells";
    }
    return "";
}

// Build upload-ready bytes in a.tex; leave a.px empty. Reuse valid coverage, or report why it was dropped.
// The caller must release its reference to rejected coverage. False on cancellation or build failure.
inline bool buildAtlasTex(const AtlasOptions& o, const CharMap& map, const std::vector<int8_t>& widths, const Readback& rb, TexFormat format, Atlas& a,
                          const std::atomic<bool>* cancel, std::string& why, std::atomic<int>* progress = nullptr, std::shared_ptr<const AtlasShapes> reuse = nullptr,
                          std::atomic<int>* reuseRefused = nullptr) {
    const auto t0 = std::chrono::steady_clock::now();
    if (o.cell != 16 && o.cell != 32 && o.cell != 48 && o.cell != 64) { why = "the cell size must be 16, 32, 48 or 64"; return false; }
    if (!rb.valid()) { why = "no readback of the original atlas"; return false; }
    if (widths.size() < kAtlasWidths) { why = "no width table"; return false; }
    const CellGeom g{o.cell};
    const int u = g.unit(), C = o.cell;
    a = Atlas{};
    a.cell = C;
    a.w = g.atlasW();
    a.h = g.atlasH();
    AtlasStats& st = a.stats;
    FtScope ftScope;   // Shared by this build's faces; retain the environment until they are destroyed.
    GlyphFace latin, full;
    if (!fitLatinFace(o, latin, st, why)) return false;
    if (o.allScripts && !fitFullFace(o, full, st, why)) return false;
    const GammaLut lut(o.gamma);
    const bool coverageMode = o.mode == AtlasOptions::Mode::Coverage;
    const unsigned gen = fontGate().generation();
    AtlasCanvas cv;
    ShapesRefusal refusal = ShapesRefusal::None;
    if (reuse) {
        if (reuse->covAt.size() != size_t(kGridCells) || reuse->nativeEmpty.size() != size_t(kGridCells) || !sameShapeOptions(o, reuse->o) || reuse->fontGen != gen ||
            reuse->curly != curlyQuotes(rb))
            refusal = ShapesRefusal::Options;
        else if (!sameFaces(st, reuse->stats)) refusal = ShapesRefusal::Faces;
        else
            for (int gl = 0; gl < kGridCells; gl++)
                if (reuse->nativeEmpty[size_t(gl)] != 2 && reuse->nativeEmpty[size_t(gl)] != (rb.cellEmpty(gl) ? 1 : 0)) { refusal = ShapesRefusal::EmptyCells; break; }
    }
    const bool reused = reuse && refusal == ShapesRefusal::None;
    if (reused) {
        cv.init(format == TexFormat::A8R8G8B8, a.w, a.h, C);
        a.outcome = reuse->outcome;
        a.boxW = reuse->boxW;
        st = reuse->stats;
        for (int gl = 0; gl < kGridCells; gl++) {
            if (cancel && cancel->load(std::memory_order_relaxed)) { why = "cancelled"; return false; }
            if (progress && (gl & 63) == 0) progress->store(gl, std::memory_order_relaxed);
            const uint32_t at = reuse->covAt[size_t(gl)];
            if (at) cv.drawn(gl, reuse->cov.data() + size_t(at - 1) * size_t(C) * size_t(C), lut, coverageMode);
            else cv.native(rb, gl);
        }
        st.shapesReused = true;
        st.rasters = latin.rasters() + full.rasters();
        a.shapes = std::move(reuse);
    } else {
        if (reuse) {   // not both sets at once: the old goes before the new one reserves its cells
            reuse.reset();
            if (reuseRefused) reuseRefused->store(int(refusal));
        }
        cv.init(format == TexFormat::A8R8G8B8, a.w, a.h, C);
        auto shapes = std::make_shared<AtlasShapes>();
        shapes->o = o;
        shapes->fontGen = gen;
        shapes->curly = curlyQuotes(rb);
        shapes->nativeEmpty.assign(kGridCells, 2);
        shapes->covAt.assign(kGridCells, 0);
        {
            size_t allowed = 0;
            for (int gl = 0; gl < kGridCells; gl++) allowed += map.allowed(gl, o.allScripts) ? 1 : 0;
            shapes->cov.reserve(allowed * size_t(C) * size_t(C));
        }
        a.outcome.assign(kGridCells, CellOutcome::KeptUnmapped);
        a.boxW.assign(kGridCells, 0);
        st.backslash = backslashFor(latin);
        const int dilate = o.fauxBold ? u : 0;
        std::vector<uint8_t> bits, cov(size_t(C) * size_t(C));
        GLYPHMETRICS gm{};
        for (int gl = 0; gl < kGridCells; gl++) {
            if (cancel && cancel->load(std::memory_order_relaxed)) { why = "cancelled"; return false; }
            if (progress && (gl & 63) == 0) progress->store(gl, std::memory_order_relaxed);   // the panel's "N of 8,192"
            const CellInfo& ci = map.cell(gl);
            const bool isLatin = CharMap::latinCell(gl);
            const wchar_t ch = (gl == kBackslashCell && ci.ch == L'\\') ? st.backslash : isLatin ? quoteFor(ci.ch, latin, shapes->curly) : ci.ch;
            CellOutcome oc = CellOutcome::Rendered;
            if (!map.allowed(gl, o.allScripts)) oc = CharMap::fixedKeep(gl) ? CellOutcome::KeptFixed : !ci.ch ? CellOutcome::KeptUnmapped : CellOutcome::KeptScript;
            else {
                const bool empty = rb.cellEmpty(gl);
                shapes->nativeEmpty[size_t(gl)] = empty ? 1 : 0;
                if (empty) oc = CellOutcome::KeptEmpty;
                else if (!ch || !(isLatin ? latin : full).has(ch)) oc = CellOutcome::KeptNoGlyph;
            }
            bool drawn = false;
            if (oc == CellOutcome::Rendered) {
                const GlyphFace& face = isLatin ? latin : full;
                const int adv = size_t(gl) < kAtlasWidths ? int(widths[size_t(gl)]) : kAtlasDefaultAdvance;
                PlaceIn in{C, adv, !isLatin, dilate, {}, isLatin ? 0 : st.fullBase};
                PlaceOut p;
                double condense = 1.0, squeeze = 1.0;
                bool blank = false, ok = true;
                for (int pass = 0; pass < 4; pass++) {
                    if (!face.raster(ch, condense, squeeze, gm, bits, blank)) { ok = false; break; }
                    if (blank) break;
                    in.ink = {int(gm.gmBlackBoxX), int(gm.gmBlackBoxY), int(gm.gmptGlyphOrigin.x), int(gm.gmptGlyphOrigin.y)};
                    p = place(in);
                    if ((p.condense >= 1.0 && p.squeeze >= 1.0) || pass == 3) break;
                    // Hinting can round a scaled glyph back up: after the first pass shave a little more.
                    if (p.condense < 1.0) condense *= p.condense * (pass == 0 ? 1.0 : 0.98);
                    if (p.squeeze < 1.0) squeeze *= p.squeeze * (pass == 0 ? 1.0 : 0.98);
                }
                if (!ok) oc = CellOutcome::KeptError;
                else if (blank) {
                    if (std::iswspace(ch)) { ++st.spaces; drawn = true; std::fill(cov.begin(), cov.end(), uint8_t(0)); }
                    else oc = CellOutcome::KeptBlank;
                } else {
                    if (condense < 1.0) ++st.condensed;
                    if (squeeze < 1.0) ++st.squeezed;
                    if (p.compressBelow) ++st.descenders;
                    std::fill(cov.begin(), cov.end(), uint8_t(0));
                    const int bw = int(gm.gmBlackBoxX), bh = int(gm.gmBlackBoxY), stride = (bw + 3) & ~3;
                    if (blitGlyph(cov.data(), C, bits.data(), bw, bh, stride, p.x, int(gm.gmptGlyphOrigin.y), p.base)) ++st.clipped;
                    dilateRight(cov.data(), C, C, C, dilate);
                    a.boxW[size_t(gl)] = uint8_t(p.boxW);
                    drawn = true;
                }
            }
            a.outcome[size_t(gl)] = oc;
            ++st.count[int(oc)];
            if (o.mode == AtlasOptions::Mode::Identity) drawn = false;   // DEV Identity: every cell native
            if (drawn) {
                shapes->covAt[size_t(gl)] = uint32_t(shapes->cov.size() / (size_t(C) * size_t(C)) + 1);
                shapes->cov.insert(shapes->cov.end(), cov.begin(), cov.end());
                cv.drawn(gl, cov.data(), lut, coverageMode);   // DEV Coverage: the whole cell magenta
            } else {
                cv.native(rb, gl);
            }
        }
        st.rasters = latin.rasters() + full.rasters();
        noteLateFallbacks(st, latin, full, o.allScripts);
        shapes->outcome = a.outcome;
        shapes->boxW = a.boxW;
        shapes->stats = st;
        a.shapes = std::move(shapes);
    }
    // The upload format: A8R8G8B8 and A8L8 are the image itself; DXT3 is encoded from the A8L8 one.
    if (cv.argb) {
        a.tex = TexImage{};
        a.tex.format = TexFormat::A8R8G8B8;
        a.tex.w = a.w;
        a.tex.h = a.h;
        a.tex.words = std::move(cv.words);
    } else if (!toTexImageA8L8(cv.words, a.w, a.h, format, a.tex, cancel)) {
        why = cancel && cancel->load() ? "cancelled" : "the atlas could not be converted to " + std::string(texFormatName(format));
        return false;
    }
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() - a.tex.ms;
    return true;
}

// The whole atlas as A8R8G8B8 in a.px: buildAtlasTex's A8R8G8B8 image, the same texels.
inline bool buildAtlas(const AtlasOptions& o, const CharMap& map, const std::vector<int8_t>& widths, const Readback& rb, Atlas& a, const std::atomic<bool>* cancel,
                       std::string& why, std::atomic<int>* progress = nullptr) {
    if (!buildAtlasTex(o, map, widths, rb, TexFormat::A8R8G8B8, a, cancel, why, progress)) return false;
    a.px = std::move(a.tex.words);
    a.tex = TexImage{};
    return true;
}

// The worker thread. start() hands it copies; the render thread polls state() and take()s the result.
class AtlasWorker {
public:
    enum State : int { Idle, Running, Done, Failed, Cancelled };
    AtlasWorker() = default;
    AtlasWorker(const AtlasWorker&) = delete;
    AtlasWorker& operator=(const AtlasWorker&) = delete;
    ~AtlasWorker() { cancel(); wait(); }

    // Optionally load the readback on the worker before building.
    using ReadbackLoader = std::function<bool(Readback&, std::string&)>;
    // Refuse concurrent builds; reuse cached shapes when compatible.
    bool start(const AtlasOptions& o, std::shared_ptr<const CharMap> map, std::vector<int8_t> widths, std::shared_ptr<const Readback> rb, ReadbackLoader load = {},
               TexFormat format = TexFormat::A8R8G8B8, std::shared_ptr<const AtlasShapes> shapes = nullptr) {
        if (state_.load() == Running) return false;
        wait();
        cancel_.store(false);
        progress_.store(0);
        shapesRefused_.store(0);
        {
            std::lock_guard<std::mutex> lock(m_);
            result_.reset();
            loaded_.reset();
            why_.clear();
            outOfMemory_ = false;
        }
        state_.store(Running);
        try {
            t_ = std::thread([this, o, map = std::move(map), widths = std::move(widths), rb = std::move(rb), load = std::move(load), format, shapes = std::move(shapes)]() mutable {
                std::unique_ptr<Atlas> a;
                std::string why;
                bool ok = false, oom = false;
                std::shared_ptr<const Readback> got;
                // Only C++ exceptions: an access violation must reach the process.
                try {
                    FontGate::Use fonts(fontGate(), &cancel_);   // no folder font swap while this draws (fontgate.h)
                    if (!fonts.in()) throw std::runtime_error("cancelled");
                    a = std::make_unique<Atlas>();
                    if (!rb && load) {
                        auto r = std::make_shared<Readback>();
                        if (load(*r, why)) got = rb = r;
                    }
                    ok = map && rb && buildAtlasTex(o, *map, widths, *rb, format, *a, &cancel_, why, &progress_, std::move(shapes), &shapesRefused_);
                    shapes.reset();
                    if (!rb && why.empty()) why = "no readback of the original atlas";
                    rb.reset();   // the worker's reference: Keep original off frees the readback once the build is installed
                }
                catch (const std::bad_alloc&) {   // the one failure a smaller cell size can cure
                    a.reset();
                    why = "there was not enough memory for the atlas";
                    oom = true;
                }
                catch (const std::exception& e) { why = e.what(); }
                std::lock_guard<std::mutex> lock(m_);
                outOfMemory_ = oom;
                if (ok) result_ = std::move(a);
                loaded_ = std::move(got);
                why_ = ok ? std::string() : why;
                state_.store(ok ? Done : cancel_.load() ? Cancelled : Failed);
            });
        } catch (const std::exception&) {   // std::system_error: no thread (an access violation is not caught)
            state_.store(Failed);
            std::lock_guard<std::mutex> lock(m_);
            why_ = "the worker thread could not start";
            return false;
        }
        return true;
    }
    State state() const { return State(state_.load()); }
    int progress() const { return progress_.load(std::memory_order_relaxed); }   // cells done in the running build
    void cancel() { cancel_.store(true); }
    void wait() { if (t_.joinable()) t_.join(); }
    // Cancel, then wait at most `ms`. False: still running; the holder must stay alive and the DLL mapped.
    bool stop(unsigned long ms) {
        cancel();
        if (!t_.joinable()) return true;
        if (WaitForSingleObject(static_cast<HANDLE>(t_.native_handle()), ms) != WAIT_OBJECT_0) return false;
        t_.join();
        return true;
    }
    // The readback the loader produced (once), or null.
    std::shared_ptr<const Readback> takeLoaded() {
        std::lock_guard<std::mutex> lock(m_);
        return std::move(loaded_);
    }
    // The render thread must release its reference to rejected shapes.
    bool shapesRefused() const { return shapesRefused_.load() != 0; }
    const char* shapesRefusedWhy() const { return shapesRefusalText(ShapesRefusal(shapesRefused_.load())); }
    // The last build failed for want of memory (std::bad_alloc on the worker), not for a GDI or input error.
    bool outOfMemory() {
        std::lock_guard<std::mutex> lock(m_);
        return outOfMemory_;
    }
    // Transfer the result once; return the failure reason through why.
    std::unique_ptr<Atlas> take(std::string* why = nullptr) {
        std::lock_guard<std::mutex> lock(m_);
        if (why) *why = why_;
        return std::move(result_);
    }

private:
    std::thread t_;
    std::atomic<bool> cancel_{false};
    std::atomic<int> progress_{0};
    std::atomic<int> shapesRefused_{0};   // a ShapesRefusal
    std::atomic<int> state_{Idle};
    std::mutex m_;
    std::unique_ptr<Atlas> result_;
    std::shared_ptr<const Readback> loaded_;
    std::string why_;
    bool outOfMemory_ = false;
};

}  // namespace tf
