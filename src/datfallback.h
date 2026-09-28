// Native atlas fallback: decode ROM\272\120.DAT through WIC on the atlas worker.
// Require the live texture's dimensions when known; a different layout cannot supply matching cells.
#pragma once
#include "atlas.h"
#include "wicpng.h"
#include <cstdio>
#include <string>
#include <vector>

namespace tf {

// CRC-32 (IEEE, reflected, the zip / PNG one), streamed.
inline uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
// The file's size and CRC32, read in 64 KB pieces. False when it cannot be opened or read.
inline bool fileCrc32(const std::wstring& path, uint64_t& bytes, uint32_t& crc) {
    bytes = 0;
    crc = 0;
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> buf;
    bool ok = true;
    try { buf.resize(64 * 1024); } catch (const std::bad_alloc&) { ok = false; }
    for (DWORD got = 0; ok;) {
        if (!ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        crc = crc32Update(crc, buf.data(), got);
        bytes += got;
    }
    CloseHandle(h);
    return ok;
}

// `expectW` x `expectH`: the live moji texture's size (0: not known, not checked).
inline bool loadDatReadback(const std::wstring& path, Readback& r, std::string& why, unsigned expectW = 0, unsigned expectH = 0) {
    if (path.empty()) { why = "the game folder is unknown"; return false; }
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
    uint64_t bytes = 0;
    uint32_t crc = 0;
    char note[96] = {};
    if (fileCrc32(path, bytes, crc)) _snprintf_s(note, sizeof note, _TRUNCATE, "%llu bytes, CRC32 %08X", static_cast<unsigned long long>(bytes), crc);
    int w = 0, h = 0;
    if (!readPngW(path.c_str(), w, h, r.px, why, 2048, 4096)) { why = std::string(narrow) + ": " + why; return false; }
    r.w = w;
    r.h = h;
    r.note = note;
    if (!r.valid()) {
        char b[160];
        _snprintf_s(b, sizeof b, _TRUNCATE, " is %dx%d, not a 64 x 128 grid", w, h);
        why = std::string(narrow) + b;
        return false;
    }
    if (expectW && expectH && (unsigned(w) != expectW || unsigned(h) != expectH)) {
        char b[256];
        _snprintf_s(b, sizeof b, _TRUNCATE, " (%s) is %dx%d, but the game's font texture is %ux%u (a game update or a font mod): its cells would not be the game's", note, w, h, expectW, expectH);
        why = std::string(narrow) + b;
        r.px.clear();
        return false;
    }
    r.compactIfGrey();   // 2 bytes a texel when that is lossless
    return true;
}

}  // namespace tf
