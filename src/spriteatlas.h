// Build sprite-font composites at k times the table's dimensions, preserving normalized UVs. The readback may be a scaled
// sheet (s = 2 or 4 times the table): every read of it is in its own texels (s per table texel), and k is a multiple of s.
// Workers use copied inputs; unowned or unsupported glyphs retain the native readback.
// RGB is a white core over a black rim; alpha covers core, rim and optional shadow at the native maximum.
// Measure placement from native cores, or shipped metrics for custom art. Keep a common row baseline
// and fit letters without moving it; damage rows normalize baselines by each rect's height.
// Thin/Thick outlines span 1/2 native px; shadows offset 1 px, blur 0.5 px and use 60% coverage.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include "atlas.h"
#include "fonttables.h"

namespace tf {

enum class Outline : uint8_t { Off = 0, Thin, Thick };

struct GroupStyle {
    std::wstring family;
    int weight = FW_BOLD;
    bool italic = false;
    bool fauxBold = false;
    Outline outline = Outline::Thin;
    bool shadow = false;
};
inline bool sameStyle(const GroupStyle& a, const GroupStyle& b) {
    return a.family == b.family && a.weight == b.weight && a.italic == b.italic && a.fauxBold == b.fauxBold && a.outline == b.outline && a.shadow == b.shadow;
}

struct SpriteOptions {
    int k = 2;                                  // 1..4: the composite is k times the record's size
    GroupStyle style[kGroupCount];              // by Group (Chat is atlas.h's and unused here)
    bool on[kGroupCount] = {true, true, true, true, true, true, true, true};   // by Group; off keeps the game's art (Chat unused)
    bool missRedraw = false;                    // redraw "Miss!" (Damage numbers); default off
    bool compress = false;                      // Quality's Compress: uploaded as DXT3
    bool hinting = true;                        // Quality's Hinting
    Engine engine = Engine::Windows;            // Quality's Engine (ftface.h)
    enum class Mode : uint8_t { Normal, Identity, Coverage };
    Mode mode = Mode::Normal;
    bool plateMarks = false;                    // the nameplate composite: the plate marks draw as Nameplates
    // Sharpness (settings.h; spriteswap.h turns these into each texture's k). `k` above is only the builder's input.
    int sharp[kGroupCount] = {};                // by Group: 0 Auto, 2..4
    int autoKPlate = 2, autoKDamage = 2;        // Auto's k for Nameplates and Damage numbers (screen.h, from the window, each group's Aspect and Size)
    int forceK = 0;                             // DEV modes: every texture at this k; 0: by Sharpness
    // The k each texture is built at once spriteswap.h's limits applied (set on a batch's copy; 0: as Sharpness asks).
    int kRec[kSpriteRecCount] = {};
    int kPlate = 0, kDamage = 0, kHud = 0, kJobs = 0;
    // Each record's sheet scale (records.h: 2 or 4 for a scaled sheet); spriteswap.h keeps each texture's k a multiple of it.
    int sheet[kSpriteRecCount] = {1, 1, 1, 1};
    // Builder defaults; the plugin replaces these with Settings::style.
    static SpriteOptions defaults() {
        SpriteOptions o;
        o.style[int(Group::Labels)] = {L"Segoe UI", FW_BOLD, false, false, Outline::Thin};
        o.style[int(Group::NamesHud)] = {L"Arial", FW_BOLD, true, false, Outline::Thin};
        o.style[int(Group::Damage)] = {L"Arial", FW_BOLD, true, false, Outline::Thin};
        o.style[int(Group::Headings)] = {L"Cambria", FW_BOLD, true, false, Outline::Thin};
        o.style[int(Group::JobTags)] = {L"Cambria", FW_BOLD, false, false, Outline::Thin};
        o.style[int(Group::Nameplates)] = {L"Arial", FW_BOLD, true, false, Outline::Thin};
        o.style[int(Group::Compass)] = {L"Cambria", FW_BOLD, true, false, Outline::Thin};
        return o;
    }
};
inline bool sameOptions(const SpriteOptions& a, const SpriteOptions& b) {
    if (a.k != b.k || a.missRedraw != b.missRedraw || a.compress != b.compress || a.hinting != b.hinting || a.engine != b.engine || a.mode != b.mode ||
        a.plateMarks != b.plateMarks)
        return false;
    if (a.autoKPlate != b.autoKPlate || a.autoKDamage != b.autoKDamage || a.forceK != b.forceK) return false;
    for (int g = int(Group::Labels); g < kGroupCount; g++)   // Chat's are atlas.h's
        if (a.on[g] != b.on[g] || !sameStyle(a.style[g], b.style[g]) || a.sharp[g] != b.sharp[g]) return false;
    return true;
}
// Plate marks use the Nameplates style but retain their original group's size metrics.
inline Group drawGroup(const GlyphSlot& s, const SpriteOptions& o) { return o.plateMarks && s.plateMark ? Group::Nameplates : s.group; }
// The nameplate composite's options: HUD text's rects and the plate marks drawn as Nameplates are.
inline SpriteOptions plateOptions(const SpriteOptions& o) {
    SpriteOptions p = o;
    p.style[int(Group::NamesHud)] = o.style[int(Group::Nameplates)];
    p.on[int(Group::NamesHud)] = o.on[int(Group::Nameplates)];
    p.plateMarks = true;
    return p;
}
// A slot is drawn: its drawing group is on, and "Miss!" only with Redraw "Miss!".
inline bool slotDrawn(const GlyphSlot& s, const SpriteOptions& o) {
    if (s.group == Group::None || s.group == Group::Chat || !o.on[int(drawGroup(s, o))]) return false;
    return s.text != L"Miss!" || o.missRedraw;
}

struct SpritePixels {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool valid() const { return w > 0 && h > 0 && px.size() == size_t(w) * size_t(h); }
    uint32_t at(int x, int y) const { return px[size_t(y) * size_t(w) + size_t(x)]; }
};

// The DEV coverage colours, without alpha.
inline uint32_t coverageColour(Group g, bool miss) {
    if (miss) return 0xFF8000;
    switch (g) {
    case Group::Labels: return 0x00FFFF;
    case Group::NamesHud: return 0xFFFF00;
    case Group::Damage: return 0xFF0000;
    case Group::Headings: return 0x00FF00;
    case Group::JobTags: return 0xFF00FF;
    case Group::Compass: return 0x0080FF;
    default: return 0xFFFFFF;
    }
}

enum class SlotOutcome : uint8_t { Rendered, Native, NoGlyph, NoNativeInk, Error };
struct SlotResult {
    SlotOutcome outcome = SlotOutcome::Native;
    Rect ink;                  // the drawn ink with its rim, in composite texels (k-space); empty when not drawn
    bool condensed = false, squeezed = false, shifted = false, clipped = false;
    bool trimmed = false;          // rim, shadow or faint edge over or under the fit box cut (letter texels: `clipped`)
    Rect core;                     // the letter texels written (the core, without rim and shadow), in composite texels
    Rect box;                      // where the slot may draw: its fit box, over it the rows only its union has (for the rim)
};
struct SpriteClassStats {
    bool used = false; int emPx = 0, nativeCap = 0, oursCap = 0; wchar_t ref = 0; double condense = 1.0; /* the class's condense */
    double squeezeX = 1.0, squeezeCaps = 1.0;   // the squeeze the x-height letters / the capitals share
};
struct SpriteGroupStats {
    int slots = 0, rendered = 0, native = 0, noGlyph = 0, noInk = 0, errors = 0, condensed = 0, squeezed = 0, shifted = 0, clipped = 0, trimmed = 0;
    SpriteClassStats cls[4];
    std::wstring face;          // the face GDI selected (first class)
    bool substituted = false;
    Engine engine = Engine::Windows;   // what drew it (first class)
    bool engineMixed = false;   // another class of it was drawn by the other engine (a face that fell back or faulted)
    std::string engineNote;     // FreeType could not open a face of the group, and why ("" = none)
    bool engineFaulted = false; // ... FreeType failed on a face of it during the build (after it opened)
};
inline std::string groupEngineText(Engine e, bool mixed) { return std::string(engineName(e)) + (mixed ? (e == Engine::FreeType ? " (partly Windows)" : " (partly FreeType)") : ""); }
// Track mixed engines when individual faces fall back.
inline void noteGroupFaces(SpriteGroupStats& gs, const std::vector<const GlyphFace*>& faces) {
    bool first = true;
    for (const GlyphFace* f : faces) {
        if (!f) continue;
        if (first) {
            gs.engine = f->used();
            gs.engineMixed = false;
            first = false;
        } else if (f->used() != gs.engine) gs.engineMixed = true;
        if (f->fellBack() && gs.engineNote.empty()) gs.engineNote = fallbackNote(*f);
        if (f->faulted()) gs.engineFaulted = true;
    }
}
struct SpriteAtlas {
    SpriteRec rec = SpriteRec::FontFont;
    int k = 2, w = 0, h = 0;
    std::vector<uint32_t> px;                // empty once converted into `tex`
    TexImage tex;                            // the upload-ready bytes (the worker's conversion)
    std::vector<SlotResult> slots;           // parallel to the slots passed in
    SpriteGroupStats group[kGroupCount];
    uint8_t alphaFull = 0x80;                // the native maximum in the owned areas
    double ms = 0;
    int rendered() const { int n = 0; for (const auto& g : group) n += g.rendered; return n; }
    int clipped() const { int n = 0; for (const auto& g : group) n += g.clipped; return n; }
};

namespace spritedetail {

inline bool isFlat(wchar_t c) {   // sits on the baseline, nothing below it
    return (c >= L'A' && c <= L'Z' && c != L'J' && c != L'Q') || (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z' && !std::wcschr(L"gjpqy", c)) || std::wcschr(L"!#%&?", c);
}
// A letter or digit whose bottom is straight (stems, feet, a flat stroke): nothing of it belongs under the baseline.
inline bool isStraightBottom(wchar_t c) { return c && std::wcschr(L"BDEFHIKLMNPRTXZhiklmnrxz1247", c); }
inline bool isCap(wchar_t c) { return (c >= L'A' && c <= L'Z' && c != L'J' && c != L'Q') || (c >= L'0' && c <= L'9'); }
inline bool isAlnum(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); }
inline bool flatText(const std::wstring& t) {   // a flat glyph, or a word of flat glyphs
    if (t.empty()) return false;
    for (const wchar_t c : t)
        if (!isFlat(c)) return false;
    return true;
}
inline int median(std::vector<int> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
// Use the modal bottom; break ties upward to reject isolated lower rim texels.
inline int commonBottom(std::vector<int> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    int best = v[0], bestN = 0;
    for (size_t i = 0; i < v.size();) {
        size_t j = i;
        while (j < v.size() && v[j] == v[i]) ++j;
        if (int(j - i) > bestN) { best = v[i]; bestN = int(j - i); }
        i = j;
    }
    return best;
}
// Modal baseline share above the rect bottom; cluster within tol and break ties toward the larger share.
inline double commonShare(std::vector<double> f, double tol) {
    if (f.empty()) return 0;
    std::sort(f.begin(), f.end());
    size_t bestAt = 0, bestN = 0;
    for (size_t i = 0; i < f.size(); i++) {
        size_t j = i;
        while (j < f.size() && f[j] - f[i] <= tol) ++j;
        if (j - i >= bestN) { bestN = j - i; bestAt = i; }
    }
    return f[bestAt + bestN / 2];
}
// The x-height letters (the bowls of g p q y included: their part above the baseline).
inline bool isXHeight(wchar_t c) { return c && std::wcschr(L"acemnorsuvwxzgpqy", c); }
// The text a slot is drawn with (the apostrophe as U+2019).
inline std::wstring drawnText(const std::wstring& t, const GlyphFace& f) {
    if (t == L"'" && f.has(L'\u2019')) return L"\u2019";
    return t;
}

// A run of glyphs rasterised at a scale: coverage 0..64, `asc` rows above the baseline edge.
struct Run {
    int w = 0, h = 0, asc = 0;
    std::vector<uint8_t> cov;
    int missing = 0;           // characters the font lacks
    bool error = false;
};
inline Run rasterRun(const GlyphFace& face, const std::wstring& text, double condense, double squeeze) {
    Run r;
    struct Piece { int x, top, w, h, stride; std::vector<uint8_t> bits; };
    std::vector<Piece> pieces;
    int pen = 0, minX = 1 << 30, maxX = -(1 << 30), maxAsc = -(1 << 30), maxDesc = -(1 << 30);
    for (const wchar_t ch : text) {
        if (!face.has(ch)) { ++r.missing; continue; }
        GLYPHMETRICS gm{};
        std::vector<uint8_t> bits;
        bool blank = false;
        if (!face.raster(ch, condense, squeeze, gm, bits, blank)) { r.error = true; return r; }
        if (!blank) {
            const int at = pen + int(gm.gmptGlyphOrigin.x);
            Piece p{at, int(gm.gmptGlyphOrigin.y), int(gm.gmBlackBoxX), int(gm.gmBlackBoxY), (int(gm.gmBlackBoxX) + 3) & ~3, std::move(bits)};
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x + p.w);
            maxAsc = std::max(maxAsc, p.top);
            maxDesc = std::max(maxDesc, p.h - p.top);
            pieces.push_back(std::move(p));
        }
        pen += int(gm.gmCellIncX);
    }
    if (pieces.empty()) return r;
    r.w = maxX - minX;
    r.h = maxAsc + maxDesc;
    r.asc = maxAsc;
    r.cov.assign(size_t(r.w) * size_t(r.h), 0);
    for (const Piece& p : pieces)
        for (int y = 0; y < p.h; y++)
            for (int x = 0; x < p.w; x++) {
                const uint8_t v = p.bits[size_t(y) * size_t(p.stride) + size_t(x)];
                uint8_t& d = r.cov[size_t(maxAsc - p.top + y) * size_t(r.w) + size_t(p.x - minX + x)];
                d = std::max(d, v);
            }
    return r;
}

// How far a run's letter texels reach under the baseline (rows; 0: they end on it, < 0: above it). A letter texel is
// coverage >= 25 of 64, the core level (60h) under a full rim.
inline constexpr int kCoreCov = 25;
inline int coreBelow(const Run& r) {
    for (int y = r.h - 1; y >= 0; y--)
        for (int x = 0; x < r.w; x++)
            if (r.cov[size_t(y) * size_t(r.w) + size_t(x)] >= kCoreCov) return y + 1 - r.asc;
    return 0;
}
// How far a run's letter texels reach over the baseline (rows; 0: none over it).
inline int coreAbove(const Run& r) {
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
            if (r.cov[size_t(y) * size_t(r.w) + size_t(x)] >= kCoreCov) return r.asc - y;
    return 0;
}

// A run's ink: faux bold (right by `bold`), then the rim (a disc of radius `rim`), then the shadow (moved `sOff` right
// and down, blurred by `sBlur`); coverage 0..64. `extra` is the shadow's reach past the rim, right and below.
struct Ink {
    int w = 0, h = 0, asc = 0, extra = 0;
    std::vector<uint8_t> core, rim, shadow;   // shadow empty: none
};
inline int shadowOffset(int k) { return k; }                    // 1 native px
inline int shadowBlur(int k) { return std::max(1, k / 2); }      // half a native px, 1 texel at least
inline int shadowReach(int k) { return shadowOffset(k) + shadowBlur(k); }
inline Ink makeInk(const Run& run, int bold, int rim, int sOff = 0, int sBlur = 0) {
    Ink k;
    const bool shadow = sOff > 0;
    k.extra = shadow ? sOff + sBlur : 0;
    k.w = run.w + bold + 2 * rim + k.extra;
    k.h = run.h + 2 * rim + k.extra;
    k.asc = run.asc + rim;
    k.core.assign(size_t(k.w) * size_t(k.h), 0);
    for (int y = 0; y < run.h; y++)
        for (int x = 0; x < run.w; x++) k.core[size_t(y + rim) * size_t(k.w) + size_t(x + rim)] = run.cov[size_t(y) * size_t(run.w) + size_t(x)];
    if (bold > 0) dilateRight(k.core.data() + size_t(rim) * size_t(k.w) + size_t(rim), run.w + bold, run.h, k.w, bold);
    const auto addShadow = [&] {
        if (!shadow) return;
        // Offset and box-blur the rim at 60% opacity. sOff >= sBlur keeps the shadow within the allocated box.
        const int b = std::max(0, sBlur), n = (2 * b + 1) * (2 * b + 1);
        k.shadow.assign(k.core.size(), 0);
        for (int y = 0; y < k.h; y++)
            for (int x = 0; x < k.w; x++) {
                int sum = 0;
                for (int dy = -b; dy <= b; dy++)
                    for (int dx = -b; dx <= b; dx++) {
                        const int sx = x - sOff + dx, sy = y - sOff + dy;
                        if (sx >= 0 && sy >= 0 && sx < k.w && sy < k.h) sum += k.rim[size_t(sy) * size_t(k.w) + size_t(sx)];
                    }
                k.shadow[size_t(y) * size_t(k.w) + size_t(x)] = uint8_t(std::min(64, int(std::lround(0.6 * sum / n))));
            }
    };
    if (rim <= 0) { k.rim = k.core; addShadow(); return k; }
    k.rim.assign(k.core.size(), 0);
    const int r2 = rim * rim + rim;   // a slightly round disc
    for (int y = 0; y < k.h; y++)
        for (int x = 0; x < k.w; x++) {
            uint8_t m = 0;
            for (int dy = -rim; dy <= rim; dy++) {
                const int yy = y + dy;
                if (yy < 0 || yy >= k.h) continue;
                for (int dx = -rim; dx <= rim; dx++) {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= k.w || dx * dx + dy * dy > r2) continue;
                    m = std::max(m, k.core[size_t(yy) * size_t(k.w) + size_t(xx)]);
                }
            }
            k.rim[size_t(y) * size_t(k.w) + size_t(x)] = m;
        }
    addShadow();
    return k;
}

// Empty rows added over an ink (a glyph wholly under the baseline has a negative `asc`: padded, its row 0 is the baseline).
inline void padTop(Ink& g, int n) {
    if (n <= 0) return;
    const auto pad = [&](std::vector<uint8_t>& layer) { if (!layer.empty()) layer.insert(layer.begin(), size_t(g.w) * size_t(n), uint8_t(0)); };
    pad(g.core);
    pad(g.rim);
    pad(g.shadow);
    g.h += n;
    g.asc += n;
}
// Area-average each layer below the baseline into the available rows. Track lost core and rim separately.
// Give real descenders at least half their original depth when space allows, then fit the remaining rim.
// Marks and round overshoots shrink uniformly. Straight-bottomed letters have no overshoot:
// treat their shallow hinted extension as rim instead of moving the baseline.
inline void squashBelow(Ink& g, int room, bool& lostCore, bool& lostRim, int k, bool letter, bool flat, bool straight, bool outline) {
    const int n = g.h - g.asc, keep = std::max(room, 0);
    if (n <= keep) return;
    int depth = 0;   // the letter texels' rows under the baseline
    for (int y = n - 1; y >= 0 && !depth; y--)
        for (int x = 0; x < g.w; x++)
            if (g.core[size_t(g.asc + y) * size_t(g.w) + size_t(x)] >= kCoreCov) { depth = y + 1; break; }
    const int half = (depth + 1) / 2;
    const bool descender = letter && (depth > k || !flat);
    if (straight && !descender && depth > 0) {   // an outline (a dilation of the core) already covers these rows
        std::fill_n(g.core.begin() + ptrdiff_t(g.asc) * g.w, size_t(depth) * size_t(g.w), uint8_t(0));
        if (!outline) std::fill_n(g.rim.begin() + ptrdiff_t(g.asc) * g.w, size_t(depth) * size_t(g.w), uint8_t(0));   // no outline: the rim is the core
    }
    const int split = descender && keep > 0 && depth > 0 && depth < n && double(depth) * keep / n < half ? std::min(keep, half) : -1;   // the letter's rows
    const auto squash = [&](std::vector<uint8_t>& layer, bool& lost, int minLost) {
        if (layer.empty()) return;
        std::vector<uint8_t> out(size_t(g.w) * size_t(g.asc + keep), 0);
        std::copy_n(layer.begin(), size_t(g.w) * size_t(g.asc), out.begin());
        const auto rows = [&](int from, int count, int into, int at) {   // `count` rows from `from` into `into` rows at `at`
            if (into > 0)
                averageRows(count, into, g.w, [&](int v, int j) { return int(layer[size_t(g.asc + from + v) * size_t(g.w) + size_t(j)]); },
                            [&](int r, int j, int v) { out[size_t(g.asc + at + r) * size_t(g.w) + size_t(j)] = uint8_t(std::min(v, 64)); });
            else
                for (size_t t = size_t(g.w) * size_t(g.asc + from); t < size_t(g.w) * size_t(g.asc + from + count); t++) lost = lost || layer[t] >= minLost;
        };
        if (split < 0) rows(0, n, keep, 0);
        else {
            rows(0, depth, split, 0);
            rows(depth, n - depth, keep - split, split);
        }
        layer.swap(out);
    };
    squash(g.core, lostCore, kCoreCov);   // a letter texel (the core level) cut; fainter edge coverage counts as rim
    squash(g.rim, lostRim, 1);
    squash(g.shadow, lostRim, 1);
    g.h = g.asc + keep;
}

// The native ink box of a slot inside its intersection box (alpha > 0; for the core, a channel >= minLevel).
inline constexpr int kCoreLevel = 0x60;
inline Rect nativeInk(const SpritePixels& n, const Rect& box, int minLevel = 0) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = std::max<int>(box.v, 0); y < std::min(box.y1(), n.h); y++)
        for (int x = std::max<int>(box.u, 0); x < std::min(box.x1(), n.w); x++) {
            const uint32_t p = n.at(x, y);
            if (!(p >> 24)) continue;
            if (minLevel && int(std::max({(p >> 16) & 255u, (p >> 8) & 255u, p & 255u})) < minLevel) continue;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
        }
    return x1 < 0 ? Rect{} : Rect::fromEdges(x0, y0, x1, y1);
}
// The same on a sheet `s` times the table: the box read at the sheet's own texels, the ink back in table texels (every
// table texel an ink texel falls in).
inline Rect nativeInkOn(const SpritePixels& n, int s, const Rect& box, int minLevel = 0) {
    if (s <= 1) return nativeInk(n, box, minLevel);
    const Rect r = nativeInk(n, Rect::fromEdges(box.u * s, box.v * s, box.x1() * s, box.y1() * s), minLevel);
    return r.empty() ? r : Rect::fromEdges(r.u / s, r.v / s, (r.x1() + s - 1) / s, (r.y1() + s - 1) / s);
}

}  // namespace spritedetail

// Build one record's composite. Custom art uses shipped metrics for placement but readback pixels
// for alpha and fallback. Reject mismatched metric tables; return false on cancellation or failure.
inline bool buildSpriteAtlas(SpriteRec rec, const std::vector<GlyphSlot>& slots, const SpritePixels& native, const SpriteOptions& o, SpriteAtlas& a, const std::atomic<bool>* cancel,
                             std::string& why, ShippedInk shipped = ShippedInk{nullptr, 0}) {
    using namespace spritedetail;
    const auto t0 = std::chrono::steady_clock::now();
    FtScope ftScope;   // this build's FreeType, freed when it returns
    const SpriteRecInfo& info = kSpriteRecs[int(rec)];
    if (o.k < 1 || o.k > 4) { why = "k must be 1..4"; return false; }
    const int sheet = native.valid() ? sheetScale(native.w, native.h, info.w, info.h) : 0;   // readback texels per table texel
    if (!sheet) { why = "the readback is not the record's size (nor 2x or 4x it)"; return false; }
    if (o.k % sheet) {
        char b[160];
        _snprintf_s(b, sizeof b, _TRUNCATE, "k %d is not a multiple of the %dx sheet's scale (its art would be resampled unevenly)", o.k, sheet);
        why = b;
        return false;
    }
    if (shipped.r && shipped.n != slots.size()) {
        char b[160];
        _snprintf_s(b, sizeof b, _TRUNCATE, "the shipped retail measurements (S15) have %zu slots and the record %zu; custom letter art cannot be placed", shipped.n, slots.size());
        why = b;
        return false;
    }
    for (const GlyphSlot& s : slots) {
        const Rect all = Rect::fromEdges(0, 0, info.w, info.h);   // the slots are in table texels
        if (s.uni.empty() || !all.contains(s.uni) || !s.uni.contains(s.safe) || !s.uni.contains(s.fit)) {
            char b[128];
            _snprintf_s(b, sizeof b, _TRUNCATE, "a glyph slot (%d,%d %dx%d) lies outside the %dx%d texture", s.uni.u, s.uni.v, s.uni.w, s.uni.h, int(info.w), int(info.h));
            why = b;
            return false;
        }
    }
    const int k = o.k, d = k / sheet;   // d: composite texels per readback texel
    a = SpriteAtlas{};
    a.rec = rec;
    a.k = k;
    a.w = int(info.w) * k;
    a.h = int(info.h) * k;
    a.px.resize(size_t(a.w) * size_t(a.h));
    for (int y = 0; y < a.h; y++)
        for (int x = 0; x < a.w; x++) a.px[size_t(y) * size_t(a.w) + size_t(x)] = native.at(x / d, y / d);
    a.slots.assign(slots.size(), SlotResult{});
    if (o.mode == SpriteOptions::Mode::Identity) { a.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); return true; }

    std::vector<char> active(slots.size(), 0);
    std::vector<Rect> ink(slots.size());   // the native core; the whole ink when the glyph has no bright core
    int alphaMax = 0;
    for (size_t i = 0; i < slots.size(); i++) {
        const GlyphSlot& s = slots[i];
        if (s.group == Group::None || s.group == Group::Chat) continue;
        for (int y = s.uni.v * sheet; y < s.uni.y1() * sheet; y++)   // full alpha over every owned area, a group that is off included
            for (int x = s.uni.u * sheet; x < s.uni.x1() * sheet; x++) alphaMax = std::max(alphaMax, int(native.at(x, y) >> 24));
        // Measure Labels even when disabled so nameplate marks retain their native size.
        const bool drawn = o.on[int(drawGroup(s, o))];
        if (!drawn && !o.on[int(s.group)] && !(o.plateMarks && s.group == Group::Labels)) continue;
        if (drawn) {
            ++a.group[int(drawGroup(s, o))].slots;
            active[i] = slotDrawn(s, o);
        }
        ink[i] = nativeInkOn(native, sheet, s.safe, kCoreLevel);
        if (ink[i].empty()) ink[i] = nativeInkOn(native, sheet, s.safe);
        if (shipped.r && !ink[i].empty()) ink[i] = shipped.r[i];   // (shipped.n == slots.size()): placed by the retail measurements (the native fallback stays the sheet's own)
    }
    a.alphaFull = uint8_t(alphaMax ? alphaMax : 0x80);
    // A stretched row's glyph is drawn at its rect's share of its class's tallest rect.
    std::vector<double> vScale(slots.size(), 1.0);
    int tallest[kGroupCount][4] = {};
    for (size_t i = 0; i < slots.size(); i++)
        if (!ink[i].empty() && stretchedRows(slots[i].group)) tallest[int(slots[i].group)][slots[i].cls] = std::max<int>(tallest[int(slots[i].group)][slots[i].cls], slots[i].safe.h);
    for (size_t i = 0; i < slots.size(); i++)
        if (!ink[i].empty() && stretchedRows(slots[i].group)) vScale[i] = double(slots[i].safe.h) / double(tallest[int(slots[i].group)][slots[i].cls]);
    // Per (group, class): the native cap height (a stretched row's in the tallest rect's rows) and the reference character
    // (the first class wins for a class with none).
    int nativeCap[kGroupCount][4] = {};
    wchar_t refCh[kGroupCount][4] = {};
    for (int g = 1; g < kGroupCount; g++)
        for (int c = 0; c < 4; c++) {
            std::vector<int> caps, words;
            for (size_t i = 0; i < slots.size(); i++) {
                if (int(slots[i].group) != g || slots[i].cls != c || ink[i].empty()) continue;
                const int h = int(std::lround(ink[i].h / vScale[i]));
                if (slots[i].word()) words.push_back(h);
                else if (isCap(slots[i].text[0])) caps.push_back(h);
            }
            if (!caps.empty()) { nativeCap[g][c] = median(caps); refCh[g][c] = L'H'; }
            else if (!words.empty()) { nativeCap[g][c] = median(words); refCh[g][c] = L'H'; }
        }
    for (int g = 1; g < kGroupCount; g++)
        for (int c = 1; c < 4; c++)
            if (!nativeCap[g][c]) { nativeCap[g][c] = nativeCap[g][0]; refCh[g][c] = refCh[g][0]; }

    // Clear every active slot's union first (a neighbour's draw never lands in a box cleared later).
    for (size_t i = 0; i < slots.size(); i++) {
        if (!active[i]) continue;
        const Rect& u = slots[i].uni;
        for (int y = u.v * k; y < u.y1() * k; y++) std::fill_n(a.px.data() + size_t(y) * size_t(a.w) + size_t(u.u * k), size_t(u.w * k), 0u);
    }

    // Fit each (drawing group, measured group, class). Plate marks borrow Labels' dimensions, not its style.
    std::map<int, std::unique_ptr<GlyphFace>> faces;
    std::vector<const GlyphFace*> groupFaces[kGroupCount];   // each group's faces in the order made (noteGroupFaces)
    const auto faceFor = [&](int g, int m, int c) -> const GlyphFace* {
        const int key = (g * kGroupCount + m) * 4 + c;
        auto it = faces.find(key);
        if (it != faces.end()) return it->second.get();
        const GroupStyle& st = o.style[g];
        SpriteClassStats& cs = a.group[g].cls[c];
        cs.used = true;
        cs.nativeCap = nativeCap[m][c];
        cs.ref = refCh[m][c] ? refCh[m][c] : L'H';
        const int target = std::max(2, nativeCap[m][c] * k);
        GlyphFace ref;
        constexpr int kRefEm = 512;
        int best = std::max(2, target * 10 / 7);
        const bool opened = ref.open(st.family, kRefEm, st.weight, st.italic, o.hinting, o.engine);
        if (opened && !ref.has(cs.ref) && ref.has(L'0')) cs.ref = L'0';   // a font without 'H' (a symbol font)
        if (opened && ref.has(cs.ref)) {
            const double ratio = double(ref.boxHeight(cs.ref)) / kRefEm;
            const int est = ratio > 0.05 ? std::max(2, int(std::lround(target / ratio))) : best;
            int bestErr = 1 << 30;
            for (int d : {0, -1, 1, -2, 2}) {
                const int em = est + d;
                if (em < 2) continue;
                GlyphFace t;
                if (!t.open(st.family, em, st.weight, st.italic, o.hinting, o.engine)) continue;
                const int err = std::abs(t.boxHeight(cs.ref) - target);
                if (err < bestErr) { bestErr = err; best = em; }
            }
        }
        auto f = std::make_unique<GlyphFace>();
        if (!f->open(st.family, best, st.weight, st.italic, o.hinting, o.engine)) { faces[key] = nullptr; return nullptr; }
        cs.emPx = best;
        cs.oursCap = f->boxHeight(cs.ref);
        if (a.group[g].face.empty()) {
            a.group[g].face = f->face();
            a.group[g].substituted = !sameFace(f->face(), st.family);
        }
        groupFaces[g].push_back(f.get());
        noteGroupFaces(a.group[g], groupFaces[g]);
        faces[key] = std::move(f);
        return faces[key].get();
    };

    // Cache rasters shared by squeeze and baseline fitting.
    std::map<std::tuple<const GlyphFace*, std::wstring, double, double>, Run> rasters;
    const auto raster = [&](const GlyphFace& face, const std::wstring& text, double condense, double squeeze) -> const Run& {
        const auto key = std::make_tuple(&face, text, condense, squeeze);
        auto it = rasters.find(key);
        if (it == rasters.end()) it = rasters.emplace(key, rasterRun(face, text, condense, squeeze)).first;
        return it->second;
    };
    // Each class's common horizontal factor (the median of what its glyphs need at their natural width).
    double classCondense[kGroupCount][4];
    for (auto& row : classCondense) for (double& v : row) v = 1.0;
    if (o.mode == SpriteOptions::Mode::Normal) {
        std::vector<double> need[kGroupCount][4];
        const auto addNeed = [&](const GlyphSlot& s, int g) {
            const GroupStyle& st = o.style[g];
            const GlyphFace* face = faceFor(g, int(s.group), s.cls);
            if (!face) return;
            const Run& run = raster(*face, drawnText(s.text, *face), 1.0, 1.0);
            if (run.error || run.missing || run.w <= 0) return;
            const int bold = st.fauxBold ? k : 0, rim = st.outline == Outline::Off ? 0 : st.outline == Outline::Thin ? k : 2 * k;
            const int room = s.fit.w * k - bold - 2 * rim - (st.shadow ? shadowReach(k) : 0);
            need[g][s.cls].push_back(std::min(1.0, double(std::max(1, room)) / double(run.w)));
        };
        for (size_t i = 0; i < slots.size(); i++) {
            if (ink[i].empty()) continue;
            const GlyphSlot& s = slots[i];
            const int dg = int(drawGroup(s, o));
            if (active[i]) addNeed(s, dg);
            // Match the plate mark's condensation to its original group in the shared texture.
            if (dg != int(s.group) && o.on[int(s.group)]) addNeed(s, int(s.group));
        }
        for (int g = 0; g < kGroupCount; g++)
            for (int c = 0; c < 4; c++)
                if (!need[g][c].empty()) {
                    std::sort(need[g][c].begin(), need[g][c].end());
                    classCondense[g][c] = need[g][c][need[g][c].size() / 2];
                    a.group[g].cls[c].condense = classCondense[g][c];
                }
    }

    // Every glyph's baseline is its rect row's common line, in composite texels (k-space).
    std::vector<int> rowBase(slots.size(), 0);
    std::vector<char> rowKnown(slots.size(), 0);   // the row has flat glyphs: its baseline is known
    for (size_t i = 0; i < slots.size(); i++) {
        if (ink[i].empty()) continue;
        std::vector<int> row;
        std::vector<double> share;   // a stretched row: each flat glyph's line as a share of its rect from the bottom
        int hMax = 1;
        for (size_t j = 0; j < slots.size(); j++)
            if (slots[j].group == slots[i].group && slots[j].cls == slots[i].cls && !ink[j].empty() && flatText(slots[j].text) && std::abs(slots[j].safe.v - slots[i].safe.v) <= 4) {
                row.push_back(ink[j].y1());
                share.push_back(double(slots[j].safe.y1() - ink[j].y1()) / double(std::max<int>(1, slots[j].safe.h)));
                hMax = std::max<int>(hMax, slots[j].safe.h);
            }
        rowKnown[i] = !row.empty();
        if (row.empty()) rowBase[i] = ink[i].y1() * k;
        else if (!stretchedRows(slots[i].group)) rowBase[i] = commonBottom(row) * k;
        else rowBase[i] = slots[i].safe.y1() * k - int(std::lround(commonShare(share, 0.5 / hMax) * slots[i].safe.h * k));
    }
    const auto rimOf = [&](const GroupStyle& st) { return st.outline == Outline::Off ? 0 : st.outline == Outline::Thin ? k : 2 * k; };
    // The top of the rows over each slot's fit box that only its own union covers (k-space): its rim may go there.
    std::vector<int> rimTop(slots.size(), 0);
    for (size_t i = 0; i < slots.size(); i++) {
        if (!active[i]) continue;
        const GlyphSlot& s = slots[i];
        int top = s.fit.v;
        for (bool free = true; free && top > s.uni.v;) {
            const Rect row = Rect::fromEdges(s.fit.u, top - 1, s.fit.x1(), top);
            for (size_t j = 0; j < slots.size() && free; j++) free = j == i || !row.overlaps(slots[j].uni);
            for (size_t j = 0; j < s.foreign.size() && free; j++) free = !row.overlaps(s.foreign[j]);   // a foreign rect's rows are not its own
            if (free) --top;
        }
        rimTop[i] = top * k;
    }
    // Hinting can shift the bottom after scaling. Re-anchor flat glyphs to the class's original baseline.
    const auto anchored = [&](const GlyphFace& face, const std::wstring& text, double condense, double squeeze, double classC, double base, bool flat) {
        Run run = raster(face, text, condense, squeeze);
        if (!flat || run.error || run.missing || run.w <= 0 || (squeeze == base && condense == classC)) return run;
        const Run& ref = raster(face, text, classC, base);
        if (!ref.error && !ref.missing && ref.w > 0) run.asc += coreBelow(run) - coreBelow(ref);
        return run;
    };
    const auto lineKey = [&](const GlyphSlot& s) { return (int(drawGroup(s, o)) * kGroupCount + int(s.group)) * 4 + s.cls; };
    // Share the largest fitting vertical scale within each row's x-height and capital/digit sets.
    // Recheck actual rasters because hinting can round them beyond the box.
    std::vector<char> xMember(slots.size(), 0);
    std::map<int, double> lineSqueeze;   // (line key) * 4 + set
    if (o.mode == SpriteOptions::Mode::Normal) {
        struct Cand { size_t i; int asc, core; const GlyphFace* face; };
        std::map<int, std::vector<Cand>> cands;
        for (size_t i = 0; i < slots.size(); i++) {
            const GlyphSlot& s = slots[i];
            if (!active[i] || ink[i].empty() || s.text.size() != 1 || !isAlnum(s.text[0])) continue;
            const int dg = int(drawGroup(s, o));
            const GlyphFace* face = faceFor(dg, int(s.group), s.cls);
            if (!face) continue;
            const Run& run = raster(*face, s.text, classCondense[dg][s.cls], vScale[i]);
            if (run.error || run.missing || run.w <= 0) continue;
            cands[lineKey(s)].push_back(Cand{i, run.asc, coreAbove(run), face});
        }
        for (const auto& [key, v] : cands) {
            std::vector<int> xs, cs;
            for (const Cand& c : v) {
                const wchar_t ch = slots[c.i].text[0];
                if (std::wcschr(L"acemnorsuvwxz", ch)) xs.push_back(c.asc);
                if (ch >= L'A' && ch <= L'Z') cs.push_back(c.asc);
            }
            const int xm = xs.empty() ? -(1 << 20) : median(xs), cm = cs.empty() ? (1 << 20) : median(cs);
            const bool oldStyle = cm > xm + k;   // the capitals stand more than 1 native px over the x-height
            double f[3] = {1.0, 1.0, 1.0};
            for (const Cand& c : v) {
                const GlyphSlot& s = slots[c.i];
                const wchar_t ch = s.text[0];
                const bool nearX = std::abs(c.asc - xm) < k;   // strictly under 1 native px from the x-height
                const bool x = isXHeight(ch) || (ch >= L'a' && ch <= L'z' && nearX) || (ch >= L'0' && ch <= L'9' && nearX && oldStyle);
                const int set = x ? 1 : isCap(ch) || ch == L'J' || ch == L'Q' ? 2 : 0;
                if (!set) continue;
                // The letter under the box's top, the ink with its rim under the rows over it that are its own.
                const int roomCore = rowBase[c.i] - s.fit.v * k, room = rowBase[c.i] - rimTop[c.i] - rimOf(o.style[int(drawGroup(s, o))]);
                if (roomCore <= 0 || room <= 0) continue;   // no room at all: squeezed on its own (the pass loop), not its whole line
                xMember[c.i] = char(set);
                f[set] = std::min({f[set], double(room) / double(std::max(1, c.asc)), double(roomCore) / double(std::max(1, c.core))});
            }
            // Reduce the shared scale until every raster fits; do not shrink individual letters inconsistently.
            const auto worstAt = [&](int set, double sq) {
                double worst = 1.0;
                for (const Cand& c : v) {
                    const GlyphSlot& s = slots[c.i];
                    const int dg = int(drawGroup(s, o));
                    if (xMember[c.i] != set) continue;
                    const int roomCore = rowBase[c.i] - s.fit.v * k, room = rowBase[c.i] - rimTop[c.i] - rimOf(o.style[dg]);
                    const Run r = anchored(*c.face, s.text, classCondense[dg][s.cls], sq * vScale[c.i], classCondense[dg][s.cls], vScale[c.i], isFlat(s.text[0]));
                    if (r.error || r.missing || r.w <= 0) continue;
                    if (r.asc > room) worst = std::max(worst, double(r.asc) / double(std::max(1, room)));
                    const int ca = coreAbove(r);
                    if (ca > roomCore) worst = std::max(worst, double(ca) / double(std::max(1, roomCore)));
                }
                return worst;
            };
            for (int set = 1; set <= 2; set++) {
                if (f[set] >= 1.0) continue;
                double hi = f[set], w = worstAt(set, hi);
                if (w <= 1.0) continue;
                double lo = hi;   // down until they fit, then the largest squeeze between that still fits
                for (int it = 0; it < 12 && w > 1.0; it++) {
                    hi = lo;
                    lo = std::min(lo / w, lo * 0.99);
                    w = worstAt(set, lo);
                }
                for (int it = 0; it < 5 && w <= 1.0; it++) {
                    const double mid = (lo + hi) / 2;
                    if (worstAt(set, mid) <= 1.0) lo = mid;
                    else hi = mid;
                }
                f[set] = lo;
            }
            lineSqueeze[key * 4 + 1] = f[1];
            lineSqueeze[key * 4 + 2] = f[2];
            const int dg = key / (kGroupCount * 4), mg = key / 4 % kGroupCount;
            if (dg == mg) { a.group[dg].cls[key % 4].squeezeX = f[1]; a.group[dg].cls[key % 4].squeezeCaps = f[2]; }
        }
    }

    struct Placed {   // a drawn glyph and where it goes (written once every slot is measured)
        size_t slot; Ink out; int x0, y0; bool onLine; int rim;
    };
    std::vector<Placed> placed;
    for (size_t i = 0; i < slots.size(); i++) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { why = "cancelled"; return false; }
        if (!active[i]) continue;
        const GlyphSlot& s = slots[i];
        SlotResult& res = a.slots[i];
        const int dg = int(drawGroup(s, o));
        SpriteGroupStats& gs = a.group[dg];
        const GroupStyle& st = o.style[dg];
        const bool miss = s.text == L"Miss!";
        const Rect F = Rect::fromEdges(s.fit.u * k, s.fit.v * k, s.fit.x1() * k, s.fit.y1() * k);
        if (o.mode == SpriteOptions::Mode::Coverage) {
            const uint32_t c = (uint32_t(a.alphaFull) << 24) | coverageColour(s.group, miss);
            for (int y = F.v; y < F.y1(); y++) std::fill_n(a.px.data() + size_t(y) * size_t(a.w) + size_t(F.u), size_t(F.w), c);
            res.outcome = SlotOutcome::Rendered;
            res.ink = F;
            res.box = F;
            ++gs.rendered;
            continue;
        }
        if (ink[i].empty()) {   // its native pixels come back (a space has none to lose)
            res.outcome = SlotOutcome::NoNativeInk;
            ++gs.noInk;
            for (int y = s.uni.v * k; y < s.uni.y1() * k; y++)
                for (int x = s.uni.u * k; x < s.uni.x1() * k; x++) a.px[size_t(y) * size_t(a.w) + size_t(x)] = native.at(x / d, y / d);
            continue;
        }
        const GlyphFace* face = faceFor(dg, int(s.group), s.cls);
        if (!face) {
            res.outcome = SlotOutcome::Error;
            ++gs.errors;
            for (int y = s.uni.v * k; y < s.uni.y1() * k; y++)
                for (int x = s.uni.u * k; x < s.uni.x1() * k; x++) a.px[size_t(y) * size_t(a.w) + size_t(x)] = native.at(x / d, y / d);
            continue;
        }
        const std::wstring text = drawnText(s.text, *face);
        const int baseK = rowBase[i];   // the row's common baseline
        const int roomAbove = baseK - F.v, roomBelow = F.y1() - baseK;
        const int roomRim = baseK - rimTop[i];   // the rim may use the rows over the box that are only this slot's
        const auto fitsAbove = [&](const Run& r, const Ink& g) { return g.asc <= roomRim && coreAbove(r) <= roomAbove; };
        const int centreK2 = (ink[i].u + ink[i].x1()) * k;   // twice the centre, in k-space
        const int bold = st.fauxBold ? k : 0;
        const int sOff = st.shadow ? shadowOffset(k) : 0, sBlur = st.shadow ? shadowBlur(k) : 0, ext = st.shadow ? shadowReach(k) : 0;
        const auto shared = lineSqueeze.find(lineKey(s) * 4 + xMember[i]);
        const double lineSq = xMember[i] && shared != lineSqueeze.end() ? shared->second : 1.0;
        // A thinner outline when the fit box is too small for this one.
        int rim = rimOf(st);
        Ink out;
        Run used;
        int onLine = -1;   // decided on the first raster (a mark whose ink stays clear of the baseline is not)
        const bool flatLine = flatText(s.text) && rowKnown[i] && roomAbove > 0 && roomBelow >= 0;
        for (;;) {
            double condense = classCondense[dg][s.cls], squeeze = lineSq * vScale[i];   // a stretched row's scale
            for (int pass = 0; pass < 5; pass++) {
                Run run = anchored(*face, text, condense, squeeze, classCondense[dg][s.cls], vScale[i], flatLine);
                if (run.error) { res.outcome = SlotOutcome::Error; break; }
                if (run.missing || run.w <= 0) { res.outcome = SlotOutcome::NoGlyph; break; }
                // Only rows with a known baseline anchor letters and crossing marks to it.
                if (onLine < 0) onLine = rowKnown[i] && roomAbove > 0 && roomBelow >= 0 && (s.word() || isAlnum(s.text[0]) || (run.asc > 0 && run.h >= run.asc)) ? 1 : 0;
                // With no room for an overshoot, align the bottom row to the baseline; shift isolated marks into the box.
                if (onLine == 1 && roomBelow == 0) {
                    const int cb = coreBelow(run);
                    if (cb > 0 && (s.word() || isAlnum(s.text[0]))) run.asc += cb;
                    else if (cb > 0) onLine = 0;
                }
                out = makeInk(run, bold, rim, sOff, sBlur);
                used = std::move(run);
                res.outcome = SlotOutcome::Rendered;
                const bool wide = out.w > F.w, tall = onLine ? !fitsAbove(used, out) : out.h > F.h;
                if ((!wide && !tall) || pass == 4) break;
                const double shave = pass == 0 ? 1.0 : 0.97;
                if (wide) { condense *= double(std::max(1, F.w - bold - 2 * rim - ext)) / double(std::max(1, used.w)) * shave; res.condensed = true; }
                if (tall && onLine) {
                    squeeze *= std::min(double(std::max(1, roomRim - rim)) / double(std::max(1, used.asc)), double(std::max(1, roomAbove)) / double(std::max(1, coreAbove(used)))) * shave;
                    res.squeezed = true;
                }
                else if (tall) { squeeze *= double(std::max(1, F.h - 2 * rim - ext)) / double(std::max(1, used.h)) * shave; res.squeezed = true; }
            }
            if (res.outcome != SlotOutcome::Rendered || rim == 0 || (out.w <= F.w && (onLine ? fitsAbove(used, out) : out.h <= F.h))) break;
            rim -= k;
        }
        if (lineSq < 1.0) res.squeezed = true;
        if (res.outcome == SlotOutcome::Error || res.outcome == SlotOutcome::NoGlyph) {
            // The native art comes back (its union was cleared above; no other slot's fit box overlaps it).
            ++(res.outcome == SlotOutcome::Error ? gs.errors : gs.noGlyph);
            for (int y = s.uni.v * k; y < s.uni.y1() * k; y++)
                for (int x = s.uni.u * k; x < s.uni.x1() * k; x++) a.px[size_t(y) * size_t(a.w) + size_t(x)] = native.at(x / d, y / d);
            continue;
        }
        // Place: centred on the native ink (the glyph without its shadow), on the row's baseline.
        const int px0 = (centreK2 - (out.w - out.extra)) / 2, py0 = baseK - out.asc;
        placed.push_back(Placed{i, std::move(out), px0, py0, onLine == 1, rim});
    }
    // Fit each glyph horizontally; preserve known baselines and shift isolated marks into their boxes.
    for (Placed& p : placed) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { why = "cancelled"; return false; }
        const GlyphSlot& s = slots[p.slot];
        SlotResult& res = a.slots[p.slot];
        SpriteGroupStats& gs = a.group[int(drawGroup(s, o))];
        const Rect F = Rect::fromEdges(s.fit.u * k, s.fit.v * k, s.fit.x1() * k, s.fit.y1() * k);
        int y0 = p.y0;
        if (p.onLine) {
            if (p.out.asc < 0) { const int n = -p.out.asc; padTop(p.out, n); p.y0 -= n; y0 = p.y0; }
            bool lostCore = false, lostRim = false;
            squashBelow(p.out, F.y1() - (p.y0 + p.out.asc), lostCore, lostRim, k, s.word() || isAlnum(s.text[0]), flatText(s.text),
                        s.text.size() == 1 && isStraightBottom(s.text[0]), p.rim > 0);
            if (lostCore) res.clipped = true;
            if (lostRim) res.trimmed = true;
        } else {
            y0 = p.out.h <= F.h ? std::clamp(p.y0, int(F.v), F.y1() - p.out.h) : F.v + (F.h - p.out.h) / 2;
        }
        int x0 = p.x0;
        const int wantX = x0;
        if (p.out.w <= F.w) x0 = std::clamp(x0, int(F.u), F.x1() - p.out.w);
        else x0 = F.u + (F.w - p.out.w) / 2;
        res.shifted = x0 != wantX || y0 != p.y0;
        const int topK = p.onLine ? rimTop[p.slot] : F.v;   // a baseline glyph's rim may go over its box
        res.box = Rect::fromEdges(F.u, topK, F.x1(), F.y1());
        res.ink = Rect::fromEdges(std::max<int>(x0, F.u), std::max<int>(y0, topK), std::min(x0 + p.out.w, F.x1()), std::min(y0 + p.out.h, F.y1()));
        int cx0 = 1 << 30, cy0 = 1 << 30, cx1 = -1, cy1 = -1;
        for (int y = 0; y < p.out.h; y++)
            for (int x = 0; x < p.out.w; x++) {
                const size_t at = size_t(y) * size_t(p.out.w) + size_t(x);
                const int rr = std::max<int>(p.out.rim[at], p.out.shadow.empty() ? 0 : p.out.shadow[at]);   // core <= rim <= this
                if (!rr) continue;
                const int X = x0 + x, Y = y0 + y;
                const int c = p.out.core[at];
                const bool spill = p.onLine && c < kCoreCov && X >= F.u && X < F.x1() && Y >= topK && Y < F.v;   // rim over the box
                if (!spill && (X < F.u || X >= F.x1() || Y < F.v || Y >= F.y1())) {
                    // Vertical loss of rim, shadow or faint edges is trimming; lost core or horizontal overflow is clipping.
                    if (p.onLine && c < kCoreCov && X >= F.u && X < F.x1()) res.trimmed = true;
                    else res.clipped = true;
                    continue;
                }
                const uint32_t A = uint32_t(std::lround(a.alphaFull * double(rr) / 64.0));
                const uint32_t L = uint32_t(std::clamp<long>(std::lround(255.0 * double(c) / double(rr)), 0, 255));
                a.px[size_t(Y) * size_t(a.w) + size_t(X)] = (A << 24) | (L << 16) | (L << 8) | L;
                if (c) { cx0 = std::min(cx0, X); cy0 = std::min(cy0, Y); cx1 = std::max(cx1, X + 1); cy1 = std::max(cy1, Y + 1); }
            }
        if (cx1 >= 0) res.core = Rect::fromEdges(cx0, cy0, cx1, cy1);
        ++gs.rendered;
        if (res.condensed) ++gs.condensed;
        if (res.squeezed) ++gs.squeezed;
        if (res.shifted) ++gs.shifted;
        if (res.clipped) ++gs.clipped;
        if (res.trimmed) ++gs.trimmed;
    }
    for (int g = 1; g < kGroupCount; g++) {
        a.group[g].native = a.group[g].slots - a.group[g].rendered;
        noteGroupFaces(a.group[g], groupFaces[g]);   // a face FreeType faulted on while drawing is Windows' now
    }
    a.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace tf
