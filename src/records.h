// Sprite texture-cache lookup (client RVA 3AC80): cache [47B970], head +08, name +0C,
// skip flag (+2C >> 3) & 1, next +38. Validate a unique live record before use.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "fonttables.h"
#include "spritesigs.h"

namespace tf {

// Reads game memory through a replaceable reader. False when the range is unreadable.
struct MemReader {
    std::function<bool(uintptr_t, void*, size_t)> read;
    // An unset reader fails safely.
    bool bytes(uintptr_t at, void* dst, size_t n) const { return at && read && read(at, dst, n); }
    template <typename T> bool get(uintptr_t at, T& v) const { v = T{}; return bytes(at, &v, sizeof(T)); }
    uint32_t u32(uintptr_t at, bool& ok) const { uint32_t v = 0; if (!get(at, v)) ok = false; return v; }
};
// Reject low or unaligned heap pointers before reading: an SEH read can consume a stack guard-page fault.
inline bool plausible(uintptr_t p) { return p >= 0x10000 && (p & 3) == 0; }
inline MemReader liveReader() { return MemReader{[](uintptr_t a, void* d, size_t n) { return readRaw(a, d, n); }}; }

struct TexRecord {
    uintptr_t rec = 0;
    char name[17] = {};
    uint32_t vtable = 0;
    uint16_t w = 0, h = 0;      // +24 / +26
    uint8_t b2A = 0;            // +2A
    uint32_t flags = 0;         // +2C
    uintptr_t tex40 = 0, tex44 = 0;
    bool skipped() const { return (flags >> 3) & 1; }
};

struct RecordFind {
    const char* name = "";
    uint16_t wantW = 0, wantH = 0;
    int hits = 0;               // live (not skipped) records with the name
    int skippedHits = 0;        // records with the name the lookup skips
    TexRecord first;            // the first live hit
    bool valid = false;         // exactly one hit, and it validated
    int scale = 0;              // valid: its sheet's scale (fonttables.h sheetScale: 1, 2 or 4)
    bool sizeRefused = false;   // not valid for its +24/+26 alone (no scale of the table's size)
    std::string why;            // why not valid
};

struct CacheScan {
    int walked = 0;             // nodes visited
    bool limitHit = false;      // stopped at the node limit (a cycle or a corrupt list)
    bool fault = false;         // a read failed
    RecordFind rec[kSpriteRecCount];
    RecordFind ustatshd;        // logged only
};

// Validate identity and dimensions; installers must recheck before writing. +24/+26 may be the tables' size or a scaled
// sheet's (2x, 4x): `scale` gets which, `sizeWrong` whether the size alone refused it.
inline bool validateRecord(const TexRecord& t, uint32_t vtableVA, const char* name, uint16_t w, uint16_t h, std::string& why, int* scale = nullptr, bool* sizeWrong = nullptr) {
    char b[200];
    if (scale) *scale = 0;
    if (sizeWrong) *sizeWrong = false;
    if (!vtableVA || t.vtable != vtableVA) { _snprintf_s(b, sizeof b, _TRUNCATE, "record %08X: vtable %08X is not CYyTex's %08X", unsigned(t.rec), t.vtable, vtableVA); why = b; return false; }
    if (std::memcmp(t.name, name, 16) != 0) { _snprintf_s(b, sizeof b, _TRUNCATE, "record %08X: name \"%.16s\" is not \"%.16s\"", unsigned(t.rec), t.name, name); why = b; return false; }
    const int s = sheetScale(t.w, t.h, w, h);
    if (!s) {
        _snprintf_s(b, sizeof b, _TRUNCATE, "record %08X: +24/+26 %ux%u, not the %ux%u the tables are for (nor 2x or 4x it, the same both ways)", unsigned(t.rec), t.w, t.h, w, h);
        why = b;
        if (sizeWrong) *sizeWrong = true;
        return false;
    }
    if (t.tex40 < 0x10000) { _snprintf_s(b, sizeof b, _TRUNCATE, "record %08X: +40 holds no texture (%08X)", unsigned(t.rec), unsigned(t.tex40)); why = b; return false; }
    if (scale) *scale = s;
    return true;
}

inline bool readRecord(const MemReader& m, uintptr_t node, TexRecord& t) {
    t = TexRecord{};
    t.rec = node;
    bool ok = plausible(node) && m.get(node, t.vtable) && m.bytes(node + kRecName, t.name, 16);
    ok = ok && m.get(node + kRecWidth, t.w) && m.get(node + kRecHeight, t.h) && m.get(node + 0x2A, t.b2A) && m.get(node + kRecFlags, t.flags);
    uint32_t a = 0, b = 0;
    ok = ok && m.get(node + kTexPrimary, a) && m.get(node + kTexAlternate, b);
    t.tex40 = a;
    t.tex44 = b;
    t.name[16] = 0;
    return ok;
}

// Scan all requested names in one bounded walk. False if the list cannot be traversed completely.
inline bool scanCache(const MemReader& m, uintptr_t cacheGlobal, uint32_t vtableVA, CacheScan& s, int limit = 65536) {
    s = CacheScan{};
    for (int i = 0; i < kSpriteRecCount; i++) { s.rec[i].name = kSpriteRecs[i].name; s.rec[i].wantW = kSpriteRecs[i].w; s.rec[i].wantH = kSpriteRecs[i].h; }
    s.ustatshd.name = kUstatshd.name;
    s.ustatshd.wantW = kUstatshd.w;
    s.ustatshd.wantH = kUstatshd.h;
    uint32_t cache = 0, node = 0;
    if (!m.get(cacheGlobal, cache) || !plausible(cache) || !m.get(cache + kCacheListHead, node)) { s.fault = true; }
    while (!s.fault && node) {
        if (s.walked >= limit) { s.limitHit = true; break; }
        if (!plausible(node)) { s.fault = true; break; }
        ++s.walked;
        TexRecord t;
        if (!readRecord(m, node, t)) { s.fault = true; break; }
        for (int i = 0; i <= kSpriteRecCount; i++) {   // every record, then ustatshd
            RecordFind* f = i < kSpriteRecCount ? &s.rec[i] : &s.ustatshd;
            if (std::memcmp(t.name, f->name, 16) != 0) continue;
            if (t.skipped()) { ++f->skippedHits; continue; }
            if (f->hits++ == 0) f->first = t;
        }
        uint32_t next = 0;
        if (!m.get(node + kRecNext, next)) { s.fault = true; break; }
        node = next;
    }
    for (int i = 0; i <= kSpriteRecCount; i++) {
        RecordFind* f = i < kSpriteRecCount ? &s.rec[i] : &s.ustatshd;
        if (s.fault || s.limitHit) { f->why = s.fault ? "the texture cache list could not be read" : "the texture cache list did not end (limit reached)"; continue; }
        if (f->hits != 1) { f->why = f->hits ? std::to_string(f->hits) + " records have the name (want 1)" : "not in the texture cache"; continue; }
        f->valid = validateRecord(f->first, vtableVA, f->name, f->wantW, f->wantH, f->why, &f->scale, &f->sizeRefused);
    }
    return !s.fault && !s.limitHit;
}

inline std::string describe(const RecordFind& f, uintptr_t imageBase) {
    char b[320];
    if (!f.hits) {
        _snprintf_s(b, sizeof b, _TRUNCATE, "record \"%.16s\": %s (skipped by the lookup: %d)", f.name, f.why.c_str(), f.skippedHits);
        return b;
    }
    const TexRecord& t = f.first;
    char scaled[40] = "";
    if (f.valid && f.scale > 1) _snprintf_s(scaled, sizeof scaled, _TRUNCATE, " (a %dx scaled sheet)", f.scale);
    _snprintf_s(b, sizeof b, _TRUNCATE, "record \"%.16s\": hits %d (skipped %d), %08X vtable RVA %06X, +24/+26 %ux%u, +2A %u, +2C %08X, +40 %08X, +44 %08X: %s%s", f.name, f.hits, f.skippedHits,
                unsigned(t.rec), unsigned(t.vtable - imageBase), t.w, t.h, t.b2A, t.flags, unsigned(t.tex40), unsigned(t.tex44), f.valid ? "valid" : f.why.c_str(), scaled);
    return b;
}

}  // namespace tf
