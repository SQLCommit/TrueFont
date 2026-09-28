// Pure image helpers for the PNG output.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace tf {

inline size_t bmpStride(uint32_t width) { return (size_t(width) * 3 + 3) & ~size_t(3); }

// A 24-bit top-down image: rows of bmpStride(w) bytes, B, G, R per pixel.
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bgr;
    void resize(uint32_t width, uint32_t height) { w = width; h = height; bgr.assign(bmpStride(w) * h, 0); }
    void set(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = bgr.data() + size_t(y) * bmpStride(w) + size_t(x) * 3;
        p[0] = b; p[1] = g; p[2] = r;
    }
};

// An A8R8G8B8 buffer (w x h, row-major) as BGR.
inline Image fromArgb(const uint32_t* px, uint32_t w, uint32_t h) {
    Image img;
    img.resize(w, h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t p = px[size_t(y) * w + x];
            img.set(x, y, uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p));
        }
    return img;
}

// Compares two pairs of shots: only pixels static within each pair (a1 == a2, b1 == b2) are compared; each one's largest
// channel difference a1 vs b1 is counted (0, 1, 2, above 2). The verdict is on luma: A8L8 drops the 5:6:5 tint of the
// game's DXT3, so channels may differ where the luma does not.
struct DiffStats {
    uint64_t pixels = 0, staticPx = 0, d0 = 0, d1 = 0, d2 = 0, over = 0;
    uint64_t d3 = 0, d4 = 0, d5plus = 0;   // the "above 2" bucket split (vendor BC2 palettes differ)
    int maxDiff = 0;
    uint64_t lumaOver = 0;                 // static pixels whose luma differs by more than 2
    int lumaMax = 0;
    bool ok = false;   // the four images have one size
};
inline DiffStats compareStatic(const Image& a1, const Image& a2, const Image& b1, const Image& b2) {
    DiffStats d;
    if (!a1.w || a1.w != a2.w || a1.w != b1.w || a1.w != b2.w || a1.h != a2.h || a1.h != b1.h || a1.h != b2.h) return d;
    const size_t stride = bmpStride(a1.w);
    if (a1.bgr.size() < stride * a1.h || a2.bgr.size() < stride * a1.h || b1.bgr.size() < stride * a1.h || b2.bgr.size() < stride * a1.h) return d;
    d.ok = true;
    for (uint32_t y = 0; y < a1.h; y++)
        for (uint32_t x = 0; x < a1.w; x++) {
            const size_t at = size_t(y) * stride + size_t(x) * 3;
            ++d.pixels;
            if (std::memcmp(&a1.bgr[at], &a2.bgr[at], 3) != 0 || std::memcmp(&b1.bgr[at], &b2.bgr[at], 3) != 0) continue;
            ++d.staticPx;
            int m = 0;
            for (int c = 0; c < 3; c++) {
                const int v = int(a1.bgr[at + size_t(c)]) - int(b1.bgr[at + size_t(c)]);
                m = (std::max)(m, v < 0 ? -v : v);
            }
            d.maxDiff = (std::max)(d.maxDiff, m);
            const auto luma = [](const uint8_t* p) { return (int(p[2]) * 77 + int(p[1]) * 150 + int(p[0]) * 29 + 128) >> 8; };   // BGR
            const int dl = std::abs(luma(&a1.bgr[at]) - luma(&b1.bgr[at]));
            d.lumaMax = (std::max)(d.lumaMax, dl);
            if (dl > 2) ++d.lumaOver;
            if (m == 0) ++d.d0;
            else if (m == 1) ++d.d1;
            else if (m == 2) ++d.d2;
            else {
                ++d.over;
                if (m == 3) ++d.d3;
                else if (m == 4) ++d.d4;
                else ++d.d5plus;
            }
        }
    return d;
}

// A comparison only means something when the capture could have shown a difference: the pixels static within (b1, b2)
// whose largest channel differs from `other` (a shot of another atlas) by more than `threshold`.
inline uint64_t countChanged(const Image& b1, const Image& b2, const Image& other, int threshold) {
    if (!b1.w || b1.w != b2.w || b1.w != other.w || b1.h != b2.h || b1.h != other.h) return 0;
    const size_t stride = bmpStride(b1.w);
    if (b1.bgr.size() < stride * b1.h || b2.bgr.size() < stride * b1.h || other.bgr.size() < stride * b1.h) return 0;
    uint64_t n = 0;
    for (uint32_t y = 0; y < b1.h; y++)
        for (uint32_t x = 0; x < b1.w; x++) {
            const size_t at = size_t(y) * stride + size_t(x) * 3;
            if (std::memcmp(&b1.bgr[at], &b2.bgr[at], 3) != 0) continue;
            int m = 0;
            for (int c = 0; c < 3; c++) {
                const int v = int(b1.bgr[at + size_t(c)]) - int(other.bgr[at + size_t(c)]);
                m = (std::max)(m, v < 0 ? -v : v);
            }
            if (m > threshold) ++n;
        }
    return n;
}
// A capture that shows nothing (all black, or one colour: GDI on an exclusive full-screen window): fewer than 0.5% of
// its pixels differ from the first one by more than 16 in any channel.
inline bool uniformCapture(const Image& img) {
    if (!img.w || !img.h) return true;
    const size_t stride = bmpStride(img.w);
    if (img.bgr.size() < stride * img.h) return true;
    const uint8_t* p0 = img.bgr.data();
    uint64_t differ = 0;
    for (uint32_t y = 0; y < img.h; y++)
        for (uint32_t x = 0; x < img.w; x++) {
            const uint8_t* p = img.bgr.data() + size_t(y) * stride + size_t(x) * 3;
            for (int c = 0; c < 3; c++)
                if (std::abs(int(p[c]) - int(p0[c])) > 16) { ++differ; break; }
        }
    return differ * 200 < uint64_t(img.w) * img.h;
}

}  // namespace tf
