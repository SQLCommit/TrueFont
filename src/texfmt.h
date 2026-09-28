// Grey atlases upload as lossless A8L8, or DXT3 with Compress; A8R8G8B8 is the fallback.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace tf {

enum class TexFormat : uint8_t { A8L8, Dxt3, A8R8G8B8 };

inline uint32_t luma(uint32_t argb) {   // exact for grey (77 + 150 + 29 = 256)
    return (((argb >> 16) & 255) * 77 + ((argb >> 8) & 255) * 150 + (argb & 255) * 29 + 128) >> 8;
}
inline size_t texBytes(TexFormat f, int w, int h) { return size_t(w) * size_t(h) * (f == TexFormat::Dxt3 ? 1u : f == TexFormat::A8R8G8B8 ? 4u : 2u); }
inline const char* texFormatName(TexFormat f) { return f == TexFormat::Dxt3 ? "DXT3" : f == TexFormat::A8R8G8B8 ? "A8R8G8B8" : "A8L8"; }

// The Chat and menus atlas grid (glyph.h kGridCols x kGridRows; swap.h asserts they agree).
inline constexpr int kChatGridCols = 64, kChatGridRows = 128;
// The ceiling on what TrueFont holds at once, in a 32-bit process: Chat and menus' build peak (chatPeak) and the menu
// and HUD fonts' share together, whichever side builds.
inline constexpr uint64_t kMemoryLimit = 160ull << 20;
// Count MANAGED textures once against process address space; WDDM keeps the video copy in GPU memory.
inline constexpr uint64_t kManagedCopies = 1;
inline uint64_t managedBytes(uint64_t textureBytes) { return kManagedCopies * textureBytes; }
inline constexpr const char* kManagedCountText = kManagedCopies == 1 ? "once" : kManagedCopies == 2 ? "twice" : "more than twice";   // the log's words
// Budget a same-size rebuild so the chosen size does not depend on the installed atlas.
// Include both textures, retained glyph shapes/readback and sprite bytes. Build and upload do not overlap;
// the larger phase determines peak usage (DXT3 encoding also retains its A8L8 source).
struct ChatPeak {
    uint64_t installed = 0, texture = 0, image = 0, encode = 0, shapes = 0, readback = 0, others = 0;
    uint64_t worker() const { return installed + image + encode + shapes + readback; }
    uint64_t upload() const { return installed + texture + image + shapes + readback; }
    uint64_t own() const { return (std::max)(worker(), upload()); }
    uint64_t total() const { return own() + others; }
};
inline ChatPeak chatPeak(int cell, TexFormat f, uint64_t readbackBytes, uint64_t shapeCells, uint64_t otherBytes = 0) {
    const uint64_t w = uint64_t(kChatGridCols) * uint64_t(cell), h = uint64_t(kChatGridRows) * uint64_t(cell);
    const uint64_t t = uint64_t(texBytes(f, int(w), int(h)));
    ChatPeak p;
    p.installed = managedBytes(t);
    p.texture = managedBytes(t);
    p.image = t;
    p.encode = f == TexFormat::Dxt3 ? 2 * w * h : 0;   // the A8L8 canvas (2 bytes a texel)
    p.shapes = shapeCells * uint64_t(cell) * uint64_t(cell);
    p.readback = readbackBytes;
    p.others = otherBytes;
    return p;
}
// Whole MB, rounded up: a figure said against the limit never rounds down onto it.
inline int mbUp(uint64_t bytes) { return int((bytes + (1ull << 20) - 1) >> 20); }
// Sharpness (<g>_sharp): 0 is Auto, else 2, 3 or 4; Chat and menus' cell size for it (Auto 16 px; the installer makes it
// 32 under the game's high-resolution font).
inline int clampSharp(int s) { return s >= 2 && s <= 4 ? s : 0; }
inline int chatCellFor(int sharp) { return clampSharp(sharp) ? 16 * clampSharp(sharp) : 16; }
inline int chatSharpX(int cell) { return cell >= 64 ? 4 : cell >= 48 ? 3 : cell >= 32 ? 2 : 1; }

inline uint16_t toA8L8(uint32_t argb) { return uint16_t(((argb >> 24) << 8) | luma(argb)); }
inline void packA8L8Row(const uint32_t* src, int w, uint8_t* dst) {
    for (int x = 0; x < w; x++) {
        const uint16_t t = toA8L8(src[x]);
        std::memcpy(dst + size_t(x) * 2, &t, 2);
    }
}

namespace detail {
inline uint32_t expand5(uint32_t q) { return (q << 3) | (q >> 2); }
inline uint32_t expand6(uint32_t q) { return (q << 2) | (q >> 4); }
// The grey 5:6:5 colour for a 5-bit level: green takes the 6-bit value nearest the 5-bit expansion, so the colour
// stays grey to within 2 of 255.
inline uint16_t grey565(uint32_t q5) {
    const uint32_t v = expand5(q5);
    uint32_t g = (v * 63 + 127) / 255, best = g;
    int bestErr = 1 << 30;
    for (uint32_t c = g ? g - 1 : 0; c <= (g < 63 ? g + 1 : 63); c++) {
        const int e = int(expand6(c)) - int(v);
        if (e * e < bestErr) { bestErr = e * e; best = c; }
    }
    return uint16_t((q5 << 11) | (best << 5) | q5);
}
// The four palette lumas of endpoints c0, c1 as the decoder (dxt.h) and the GPU form them: 2/3 and 1/3 mixes.
inline void palette(uint16_t c0, uint16_t c1, uint32_t y[4]) {
    const uint32_t r0 = expand5((c0 >> 11) & 31), g0 = expand6((c0 >> 5) & 63), b0 = expand5(c0 & 31);
    const uint32_t r1 = expand5((c1 >> 11) & 31), g1 = expand6((c1 >> 5) & 63), b1 = expand5(c1 & 31);
    const auto lum = [](uint32_t r, uint32_t g, uint32_t b) { return (r * 77 + g * 150 + b * 29 + 128) >> 8; };
    y[0] = lum(r0, g0, b0);
    y[1] = lum(r1, g1, b1);
    y[2] = lum((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
    y[3] = lum((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
}
}  // namespace detail

namespace detail {
// grey565 and palette take only 32 levels each, so they are tabled once; the bytes are the per-block computation's.
struct Dxt3Tables {
    uint16_t grey[32];
    uint8_t pal[32][32][4];   // palette(grey[hi], grey[lo]) lumas
    uint8_t a4[256];          // the alpha byte's 4 bits, rounded
    Dxt3Tables() {
        for (uint32_t q = 0; q < 32; q++) grey[q] = grey565(q);
        for (uint32_t a = 0; a < 32; a++)
            for (uint32_t b = 0; b < 32; b++) {
                uint32_t y[4];
                palette(grey[a], grey[b], y);
                for (int k = 0; k < 4; k++) pal[a][b][k] = uint8_t(y[k]);
            }
        for (uint32_t v = 0; v < 256; v++) a4[v] = uint8_t((v * 15 + 127) / 255);
    }
};
inline const Dxt3Tables& dxt3Tables() {
    static const Dxt3Tables t;
    return t;
}
}  // namespace detail

// One 4x4 block from its alphas and lumas into 16 DXT3 bytes. Alpha: explicit 4 bits, rounded. Colour: grey endpoints at
// the block's lowest and highest luma, each tried rounded down and up (least squared error wins); a lowest luma of 0 stays
// 0 exactly (the Chat atlas draws additively: a non-zero floor would light the whole cell).
inline void encodeDxt3BlockAY(const uint8_t al[16], const uint8_t y[16], uint8_t out[16]) {
    const detail::Dxt3Tables& T = detail::dxt3Tables();
    uint32_t any = 0, lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        any |= uint32_t(al[i]) | uint32_t(y[i]);
        if (y[i] < lo) lo = y[i];
        if (y[i] > hi) hi = y[i];
    }
    std::memset(out, 0, 16);
    if (!any) return;
    for (int i = 0; i < 16; i++) out[i / 2] |= uint8_t(T.a4[al[i]] << ((i & 1) * 4));
    const auto candidates = [](uint32_t v, uint32_t q[2]) {
        const uint32_t d = (v * 31) / 255;
        q[0] = d;
        q[1] = d < 31 ? d + 1 : 31;
    };
    uint32_t qlo[2], qhi[2];
    candidates(lo, qlo);
    candidates(hi, qhi);
    if (lo == 0) qlo[1] = 0;
    const int nhi = qhi[1] == qhi[0] ? 1 : 2, nlo = qlo[1] == qlo[0] ? 1 : 2;
    uint16_t bestC0 = 0, bestC1 = 0;
    uint32_t bestIdx = 0;
    uint64_t bestErr = UINT64_MAX;
    for (int ia = 0; ia < nhi; ia++)
        for (int ib = 0; ib < nlo; ib++) {
            const uint32_t a = qhi[ia], b = qlo[ib];
            const uint8_t* p = T.pal[a][b];
            uint64_t err = 0;
            uint32_t idx = 0;
            for (int i = 0; i < 16; i++) {
                int bk = 0, be = 1 << 30;
                for (int k = 0; k < 4; k++) {
                    const int e = int(p[k]) - int(y[i]);
                    if (e * e < be) { be = e * e; bk = k; }
                }
                err += uint64_t(be);
                idx |= uint32_t(bk) << (2 * i);
            }
            if (err < bestErr) { bestErr = err; bestC0 = T.grey[a]; bestC1 = T.grey[b]; bestIdx = idx; }
        }
    std::memcpy(out + 8, &bestC0, 2);
    std::memcpy(out + 10, &bestC1, 2);
    std::memcpy(out + 12, &bestIdx, 4);
}
inline void encodeDxt3Block(const uint32_t t[16], uint8_t out[16]) {
    uint8_t al[16], y[16];
    for (int i = 0; i < 16; i++) {
        al[i] = uint8_t(t[i] >> 24);
        y[i] = uint8_t(luma(t[i]));
    }
    encodeDxt3BlockAY(al, y, out);
}

inline void encodeDxt3BlockRow(const uint32_t* src, int w, size_t stride, uint8_t* dst) {
    uint32_t t[16];
    for (int bx = 0; bx < w / 4; bx++) {
        for (int y = 0; y < 4; y++) std::memcpy(t + y * 4, src + size_t(y) * stride + size_t(bx) * 4, 16);
        encodeDxt3Block(t, dst + size_t(bx) * 16);
    }
}

// The same from A8L8 texels (alpha high, luma low): the bytes encodeDxt3BlockRow gives the A8R8G8B8 texels they came from.
inline void encodeDxt3BlockRowA8L8(const uint16_t* src, int w, size_t stride, uint8_t* dst) {
    uint8_t al[16], y[16];
    for (int bx = 0; bx < w / 4; bx++) {
        for (int yy = 0; yy < 4; yy++)
            for (int x = 0; x < 4; x++) {
                const uint16_t v = src[size_t(yy) * stride + size_t(bx) * 4 + size_t(x)];
                al[yy * 4 + x] = uint8_t(v >> 8);
                y[yy * 4 + x] = uint8_t(v & 255);
            }
        encodeDxt3BlockAY(al, y, dst + size_t(bx) * 16);
    }
}

// An atlas in its upload format: rows() rows (DXT3: block rows) of rowBytes() each, packed tight. Held as words so the
// A8R8G8B8 fallback takes the atlas's own buffer without a copy.
struct TexImage {
    TexFormat format = TexFormat::A8R8G8B8;
    int w = 0, h = 0;
    std::vector<uint32_t> words;
    double ms = 0;   // the conversion
    const uint8_t* bytes() const { return reinterpret_cast<const uint8_t*>(words.data()); }
    size_t rowBytes() const { return format == TexFormat::Dxt3 ? size_t(w / 4) * 16 : size_t(w) * (format == TexFormat::A8L8 ? 2u : 4u); }
    int rows() const { return format == TexFormat::Dxt3 ? h / 4 : h; }
    bool valid() const { return w > 0 && h > 0 && words.size() * 4 >= rowBytes() * size_t(rows()); }
};

// `px` (A8R8G8B8, w x h) into `out` in format `f`; on success `px` is freed. False when cancelled, or DXT3 with a size
// that is not a multiple of 4 (`px` is then kept).
inline bool toTexImage(std::vector<uint32_t>& px, int w, int h, TexFormat f, TexImage& out, const std::atomic<bool>* cancel = nullptr) {
    const auto t0 = std::chrono::steady_clock::now();
    if (w <= 0 || h <= 0 || px.size() != size_t(w) * size_t(h) || (f == TexFormat::Dxt3 && ((w & 3) || (h & 3)))) return false;
    TexImage t;
    t.format = f;
    t.w = w;
    t.h = h;
    if (f == TexFormat::A8R8G8B8) t.words = std::move(px);
    else {
        t.words.assign((texBytes(f, w, h) + 3) / 4, 0);
        uint8_t* dst = reinterpret_cast<uint8_t*>(t.words.data());
        const size_t row = t.rowBytes();
        for (int r = 0; r < t.rows(); r++) {
            if (cancel && (r & 15) == 0 && cancel->load(std::memory_order_relaxed)) return false;
            if (f == TexFormat::A8L8) packA8L8Row(px.data() + size_t(r) * size_t(w), w, dst + size_t(r) * row);
            else encodeDxt3BlockRow(px.data() + size_t(r) * 4 * size_t(w), w, size_t(w), dst + size_t(r) * row);
        }
    }
    std::vector<uint32_t>().swap(px);   // freed (a moved-from vector is only "valid but unspecified")
    t.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out = std::move(t);
    return true;
}

// An A8L8 image (two texels to a word) as A8L8 (taken whole) or DXT3 (encoded; `words` freed): toTexImage's bytes for the
// A8R8G8B8 image the texels came from. False when cancelled, for another format, or a size DXT3 cannot hold.
inline bool toTexImageA8L8(std::vector<uint32_t>& words, int w, int h, TexFormat f, TexImage& out, const std::atomic<bool>* cancel = nullptr) {
    const auto t0 = std::chrono::steady_clock::now();
    if (w <= 0 || h <= 0 || words.size() * 2 != size_t(w) * size_t(h) || f == TexFormat::A8R8G8B8 || (f == TexFormat::Dxt3 && ((w & 3) || (h & 3)))) return false;
    TexImage t;
    t.format = f;
    t.w = w;
    t.h = h;
    if (f == TexFormat::A8L8) t.words = std::move(words);
    else {
        t.words.assign((texBytes(f, w, h) + 3) / 4, 0);
        uint8_t* dst = reinterpret_cast<uint8_t*>(t.words.data());
        const uint16_t* src = reinterpret_cast<const uint16_t*>(words.data());
        const size_t row = t.rowBytes();
        for (int r = 0; r < t.rows(); r++) {
            if (cancel && (r & 15) == 0 && cancel->load(std::memory_order_relaxed)) return false;
            encodeDxt3BlockRowA8L8(src + size_t(r) * 4 * size_t(w), w, size_t(w), dst + size_t(r) * row);
        }
    }
    std::vector<uint32_t>().swap(words);
    t.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out = std::move(t);
    return true;
}

}  // namespace tf
