// DXT3 (BC2) and DXT1 (BC1) decoding, pure (the standard block layouts). DXT3: alpha nibbles expand as a*17; colour is
// always four-colour mode. DXT1: c0 > c1 is four-colour mode; c0 <= c1 is three-colour mode, index 2 the halfway colour
// and index 3 transparent black (A 0, RGB 0); every other texel has A FFh. c2/c3 round down, so a texel can differ from
// the GPU's sampling by 1 per colour channel.
#pragma once
#include <cstdint>
#include <cstring>

namespace tf {

inline void rgb565(uint16_t c, uint32_t& r, uint32_t& g, uint32_t& b) {
    const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    r = (r5 << 3) | (r5 >> 2);
    g = (g6 << 2) | (g6 >> 4);
    b = (b5 << 3) | (b5 >> 2);
}

inline void decodeDxt3Block(const uint8_t* block, uint32_t out[16]) {
    uint16_t c0 = 0, c1 = 0;
    uint32_t idx = 0;
    std::memcpy(&c0, block + 8, 2);
    std::memcpy(&c1, block + 10, 2);
    std::memcpy(&idx, block + 12, 4);
    uint32_t r[4], g[4], b[4];
    rgb565(c0, r[0], g[0], b[0]);
    rgb565(c1, r[1], g[1], b[1]);
    r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
    r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
    for (int i = 0; i < 16; i++) {
        const uint32_t a4 = (block[i / 2] >> ((i & 1) * 4)) & 15;
        const uint32_t k = (idx >> (2 * i)) & 3;
        out[i] = ((a4 * 17) << 24) | (r[k] << 16) | (g[k] << 8) | b[k];
    }
}

// A whole surface of w x h texels (multiples of 4), `pitch` bytes per block row, into w * h texels.
inline bool decodeDxt3(const uint8_t* blocks, int w, int h, int pitch, uint32_t* out) {
    if (!blocks || !out || w <= 0 || h <= 0 || (w & 3) || (h & 3) || pitch < (w / 4) * 16) return false;
    uint32_t t[16];
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            decodeDxt3Block(blocks + size_t(by) * size_t(pitch) + size_t(bx) * 16, t);
            for (int y = 0; y < 4; y++) std::memcpy(out + size_t(by * 4 + y) * size_t(w) + size_t(bx) * 4, t + y * 4, 16);
        }
    return true;
}

// One DXT1 block (8 bytes: c0, c1, 32 index bits) -> 16 texels (row-major, 4 per row).
inline void decodeDxt1Block(const uint8_t* block, uint32_t out[16]) {
    uint16_t c0 = 0, c1 = 0;
    uint32_t idx = 0;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    std::memcpy(&idx, block + 4, 4);
    uint32_t r[4], g[4], b[4], a[4] = {255, 255, 255, 255};
    rgb565(c0, r[0], g[0], b[0]);
    rgb565(c1, r[1], g[1], b[1]);
    if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = a[3] = 0;
    }
    for (int i = 0; i < 16; i++) {
        const uint32_t k = (idx >> (2 * i)) & 3;
        out[i] = (a[k] << 24) | (r[k] << 16) | (g[k] << 8) | b[k];
    }
}

// A whole DXT1 surface of w x h texels (multiples of 4), `pitch` bytes per block row (8 a block), into w * h texels.
inline bool decodeDxt1(const uint8_t* blocks, int w, int h, int pitch, uint32_t* out) {
    if (!blocks || !out || w <= 0 || h <= 0 || (w & 3) || (h & 3) || pitch < (w / 4) * 8) return false;
    uint32_t t[16];
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            decodeDxt1Block(blocks + size_t(by) * size_t(pitch) + size_t(bx) * 8, t);
            for (int y = 0; y < 4; y++) std::memcpy(out + size_t(by * 4 + y) * size_t(w) + size_t(bx) * 4, t + y * 4, 16);
        }
    return true;
}

}  // namespace tf
