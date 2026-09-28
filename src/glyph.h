// Glyph placement and coverage. With u=C/16: Latin baseline 14u, cap target 12u; full-width ink fits C x 15u.
// Keep the baseline fixed, compress overflow vertically and reserve u pixels for rightward faux bold.
// Unhinted outlines use 8x8 nonzero-winding samples, matching GDI's 0..64 coverage.
// Map coverage c to round(255 * (c/64)^(1/gamma)); nonzero coverage has alpha 255.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tf {

inline constexpr int kGridCols = 64, kGridRows = 128, kGridCells = kGridCols * kGridRows;

struct CellGeom {
    int C = 16;
    int unit() const { return C / 16; }
    int baseline() const { return 14 * C / 16; }       // y of the baseline edge (rows above it hold the ink above the baseline)
    int capTarget() const { return 12 * C / 16; }      // the cap height the Latin font is fitted to
    int fullBox() const { return 15 * C / 16; }        // the kanji ink height (rows u..C-1); their width is the whole cell
    int atlasW() const { return kGridCols * C; }
    int atlasH() const { return kGridRows * C; }
    int cellX(int glyph) const { return (glyph % kGridCols) * C; }
    int cellY(int glyph) const { return (glyph / kGridCols) * C; }
};

// A rasterised glyph's black box (GLYPHMETRICS); originY is the baseline's distance up to the box's top.
struct InkBox { int w = 0, h = 0, originX = 0, originY = 0; };

struct PlaceIn {
    int C = 16;
    int adv = 14;              // the FFXI advance at 16 px (table 36C428, or 14)
    bool fullWidth = false;    // a kanji / full-width cell (All scripts only)
    int dilate = 0;            // the faux-bold width in px (0 or u)
    InkBox ink;
    int baseline = 0;          // the baseline row; 0 = 14u (the full-width face measures its own)
};
struct PlaceOut {
    int x = 0, y = 0;          // where the black box's top-left goes, relative to the cell's top-left
    int boxW = 0;              // the horizontal box the ink is centred in
    int limit = 0;             // the widest ink (plus dilation) that fits without condensing
    double condense = 1.0;     // < 1: re-rasterise with MAT2 eM11 = condense, then place again
    double squeeze = 1.0;      // < 1: re-rasterise with MAT2 eM22 = squeeze, then place again
    int base = 14;             // the baseline row in the cell (14u)
    bool clippedX = false;     // ink right of the cell (clipped when written)
    bool clippedY = false;     // ink above row 0 before blitGlyph's averaging; clipped only when none of it reaches row 0
    bool compressBelow = false;   // the rows under the baseline do not fit and are area-averaged
};

// The horizontal box and the ink limit for an advance (the rules above).
inline void advanceBox(int C, int adv, bool fullWidth, int& boxW, int& limit) {
    const int u = C / 16;
    const int a = std::clamp(adv, 0, 16);
    if (fullWidth && a >= 14) {
        boxW = C;   // native kanji ink spans x 0..15 (the whole cell, overlapping the next by 2 px at advance 14)
        limit = boxW;
    } else {
        boxW = a * u;
        limit = std::max(boxW - u, u);   // at least one unit: a 1-px glyph in a zero or 1-unit advance is never condensed to nothing
    }
}

inline double condenseFactor(int inkW, int dilate, int limit) {
    const int avail = limit - dilate;
    if (inkW <= 0 || inkW <= avail) return 1.0;
    if (avail <= 0) return 1.0 / double(inkW);
    return double(avail) / double(inkW);
}

inline double squeezeFactor(int above, int room) {
    if (above <= room || above <= 0) return 1.0;
    return double(std::max(room, 1)) / double(above);
}

inline PlaceOut place(const PlaceIn& in) {
    PlaceOut o;
    const CellGeom g{in.C};
    advanceBox(in.C, in.adv, in.fullWidth, o.boxW, o.limit);
    const int wide = in.ink.w + in.dilate;
    o.condense = condenseFactor(in.ink.w, in.dilate, o.limit);
    int x = (o.boxW - wide) / 2;
    if (o.boxW - wide < 0) x = -((wide - o.boxW + 1) / 2);   // wider than the box (before condensing): centred about it
    x = std::min(x, in.C - wide);
    x = std::max(x, 0);
    o.x = x;
    o.clippedX = x + wide > in.C;
    // On the baseline (14u, or the full-width face's own); a full-width box lifted or lowered into the cell; never
    // shifted for the ink above.
    const int below = in.ink.h - in.ink.originY;
    int base = in.baseline > 0 ? in.baseline : g.baseline();
    if (in.fullWidth && below > in.C - base) base = std::max(in.C - below, 1);
    if (in.fullWidth && base - in.ink.originY < 0) {
        const int spare = in.C - (base + std::max(below, 0));
        if (spare > 0) base += std::min(in.ink.originY - base, spare);
    }
    o.base = base;
    o.y = base - in.ink.originY;
    o.clippedY = o.y < 0;
    o.squeeze = squeezeFactor(in.ink.originY, base);
    o.compressBelow = below > in.C - base;
    return o;
}

// Area-averages source rows [0, n) of a column into `rows` rows (rows < n): out(r) = the mean over [r n/rows, (r+1) n/rows).
template <class Src, class Put> inline void averageRows(int n, int rows, int cols, const Src& src, const Put& put) {
    for (int r = 0; r < rows; r++) {
        const double a = double(r) * n / rows, b = double(r + 1) * n / rows;
        for (int j = 0; j < cols; j++) {
            double sum = 0;
            for (int v = int(a); v < n && v < b; v++) sum += src(v, j) * (std::min(b, double(v + 1)) - std::max(a, double(v)));
            put(r, j, int(std::lround(sum / (b - a))));
        }
    }
}

// A glyph's coverage into a C x C cell at column x, baseline row `base`; nothing outside the cell is written. True when
// ink was clipped.
inline bool blitGlyph(uint8_t* cell, int C, const uint8_t* bits, int bw, int bh, int stride, int x, int originY, int base) {
    bool clipped = false;
    const auto put = [&](int row, int col, int v) {
        if (v <= 0) return;
        if (row < 0 || row >= C || col < 0 || col >= C) { clipped = true; return; }
        cell[size_t(row) * size_t(C) + size_t(col)] = uint8_t(std::min(v, 64));
    };
    const int above = std::min(originY, bh), top = base - originY;
    if (top < 0 && top + above > 0) {
        averageRows(above, top + above, bw, [&](int v, int j) { return int(bits[size_t(v) * size_t(stride) + size_t(j)]); }, [&](int r, int j, int v) { put(r, x + j, v); });
    } else {
        for (int i = 0; i < above; i++)
            for (int j = 0; j < bw; j++) put(base - originY + i, x + j, bits[size_t(i) * size_t(stride) + size_t(j)]);
    }
    // Under it: virtual row v (0 = the first row under the baseline) is bitmap row originY + v.
    const int below = bh - originY, room = C - base;
    if (below <= 0) return clipped;
    const auto src = [&](int v, int j) -> int { const int i = originY + v; return (i >= 0 && i < bh) ? bits[size_t(i) * size_t(stride) + size_t(j)] : 0; };
    if (below <= room || room <= 0) {
        for (int v = 0; v < below; v++)
            for (int j = 0; j < bw; j++) put(base + v, x + j, src(v, j));
        return clipped;
    }
    averageRows(below, room, bw, src, [&](int r, int j, int v) { put(base + r, x + j, v); });
    return clipped;
}

// The pixel height (em, as GDI's negative lfHeight) that puts the cap height at the target: capRatio = cap height / em.
inline int emForCap(double capRatio, int C) {
    const CellGeom g{C};
    const double target = double(g.capTarget());
    if (capRatio <= 0.05 || capRatio > 1.5) return std::max(1, int(std::lround(target / 0.7)));   // no usable cap height: a typical 0.7
    return std::max(1, int(std::lround(target / capRatio)));
}
// The em of a full-width cell before measuring: the 15u box, never above the cell.
inline int emForFull(int C) { return std::clamp(15 * C / 16, 1, C); }
// The ink height a reference ideograph is fitted to: native kanji span rows u..C-1 (15u), 4u..C.
inline int fullTarget(int C) { return std::clamp(15 * C / 16, 4 * (C / 16), C); }

struct GammaLut {
    uint8_t v[65] = {};
    explicit GammaLut(double gamma = 1.0) {
        const double gm = std::clamp(gamma, 0.1, 10.0);
        for (int c = 0; c <= 64; c++) {
            const double t = double(c) / 64.0;
            const long r = std::lround(255.0 * std::pow(t, 1.0 / gm));
            v[c] = uint8_t(std::clamp<long>(r, 0, 255));
        }
        v[0] = 0;
        v[64] = 255;
    }
    uint8_t operator()(int c) const { return v[std::clamp(c, 0, 64)]; }
};
inline uint32_t texel(int coverage, const GammaLut& lut) {
    if (coverage <= 0) return 0;
    const uint32_t l = lut(coverage);
    return 0xFF000000u | (l << 16) | (l << 8) | l;
}

// Hinting off: an outline filled at 8 x 8 samples a pixel. Points are in pixels, y up, the pen origin at 0,0 (GGO_NATIVE's space).
struct PathPoint { double x = 0, y = 0; };
using Ring = std::vector<PathPoint>;   // a closed polygon (the last point joins the first)
inline int curveSteps(double dev) { return std::clamp(int(std::ceil(std::sqrt(std::max(dev, 0.0) * 4.0))), 1, 32); }
inline void addQuad(Ring& r, PathPoint a, PathPoint b, PathPoint c) {
    const int n = curveSteps(std::hypot(a.x - 2 * b.x + c.x, a.y - 2 * b.y + c.y));
    for (int i = 1; i <= n; i++) {
        const double t = double(i) / n, s = 1 - t;
        r.push_back({s * s * a.x + 2 * s * t * b.x + t * t * c.x, s * s * a.y + 2 * s * t * b.y + t * t * c.y});
    }
}
inline void addCubic(Ring& r, PathPoint a, PathPoint b, PathPoint c, PathPoint d) {
    const double dev = std::max(std::hypot(a.x - 2 * b.x + c.x, a.y - 2 * b.y + c.y), std::hypot(b.x - 2 * c.x + d.x, b.y - 2 * c.y + d.y));
    const int n = curveSteps(dev * 3);
    for (int i = 1; i <= n; i++) {
        const double t = double(i) / n, s = 1 - t;
        r.push_back({s * s * s * a.x + 3 * s * s * t * b.x + 3 * s * t * t * c.x + t * t * t * d.x, s * s * s * a.y + 3 * s * s * t * b.y + 3 * s * t * t * c.y + t * t * t * d.y});
    }
}
// The rings' pixel box (GLYPHMETRICS' black box: originX / originY are its left column and its top above the baseline).
struct OutlineBox { int w = 0, h = 0, originX = 0, originY = 0; };
inline bool outlineBox(const std::vector<Ring>& rings, OutlineBox& b) {
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (const Ring& r : rings)
        for (const PathPoint& p : r) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    if (x1 < x0 || y1 < y0 || x1 - x0 > 4096 || y1 - y0 > 4096) return false;   // no points, or nothing a cell could hold
    b.originX = int(std::floor(x0));
    b.originY = int(std::ceil(y1));
    b.w = std::max(1, int(std::ceil(x1)) - b.originX);
    b.h = std::max(1, b.originY - int(std::floor(y0)));
    return true;
}
// Fills the rings (nonzero winding) into `bits`, GGO_GRAY8_BITMAP's layout: rows top down, stride (w + 3) & ~3, 0..64.
// False when there is nothing to fill (a blank glyph).
inline bool fillOutline(const std::vector<Ring>& rings, OutlineBox& b, std::vector<uint8_t>& bits) {
    bits.clear();
    if (!outlineBox(rings, b)) return false;
    const int stride = (b.w + 3) & ~3, subRows = b.h * 8, subCols = b.w * 8;
    std::vector<std::vector<std::pair<double, int>>> cross(static_cast<size_t>(subRows));
    for (const Ring& r : rings) {
        const size_t n = r.size();
        for (size_t i = 0; i < n && n >= 2; i++) {
            const PathPoint& p = r[i];
            const PathPoint& q = r[(i + 1) % n];
            if (p.y == q.y) continue;
            const double ylo = std::min(p.y, q.y), yhi = std::max(p.y, q.y);
            const int dir = q.y > p.y ? 1 : -1;
            // Sample row j is at y = originY - (j + 0.5) / 8; the edge holds y in [ylo, yhi).
            const int jFrom = std::max(0, int(std::floor((b.originY - yhi) * 8.0 - 0.5)));
            const int jTo = std::min(subRows - 1, int(std::ceil((b.originY - ylo) * 8.0 - 0.5)));
            for (int j = jFrom; j <= jTo; j++) {
                const double y = b.originY - (j + 0.5) / 8.0;
                if (y < ylo || y >= yhi) continue;
                cross[size_t(j)].push_back({p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y), dir});
            }
        }
    }
    bits.assign(size_t(stride) * size_t(b.h), 0);
    bool any = false;
    for (int j = 0; j < subRows; j++) {
        auto& c = cross[size_t(j)];
        if (c.size() < 2) continue;
        std::sort(c.begin(), c.end(), [](const auto& a, const auto& z) { return a.first < z.first; });
        int wind = 0;
        double start = 0;
        for (const auto& e : c) {
            const int was = wind;
            wind += e.second;
            if (!was && wind) start = e.first;
            else if (was && !wind) {
                // Sample column k is at x = originX + (k + 0.5) / 8; the span holds x in [start, e.first).
                const int k0 = std::max(0, int(std::ceil((start - b.originX) * 8.0 - 0.5)));
                const int k1 = std::min(subCols - 1, int(std::ceil((e.first - b.originX) * 8.0 - 0.5)) - 1);
                for (int k = k0; k <= k1; k++) {
                    uint8_t& v = bits[size_t(j / 8) * size_t(stride) + size_t(k / 8)];
                    if (v < 64) ++v;
                    any = true;
                }
            }
        }
    }
    if (!any) bits.clear();
    return any;
}

// Faux bold: each coverage becomes the maximum of itself and the r pixels to its left, inside the w x h buffer only.
inline void dilateRight(uint8_t* cov, int w, int h, int stride, int r) {
    if (r <= 0 || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        uint8_t* row = cov + size_t(y) * size_t(stride);
        for (int x = w - 1; x >= 0; x--) {
            uint8_t m = row[x];
            for (int k = 1; k <= r && x - k >= 0; k++) m = std::max(m, row[x - k]);
            row[x] = m;
        }
    }
}

}  // namespace tf
