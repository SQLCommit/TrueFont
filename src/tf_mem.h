// Pattern scanning and guarded game-memory access.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace tf {

struct Module { uintptr_t base = 0; size_t size = 0; bool contains(uintptr_t a, size_t n = 1) const { return a >= base && a + n <= base + size; } };

inline std::vector<int> parsePattern(const char* text) {
    std::vector<int> out;
    for (const char* p = text; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { out.push_back(-1); p += 2; continue; }
        char hex[3] = {p[0], p[1], 0};
        out.push_back(int(strtol(hex, nullptr, 16)));
        p += 2;
    }
    return out;
}

inline bool matchesAt(const uint8_t* data, size_t size, size_t at, const std::vector<int>& pat) {
    if (at > size || size - at < pat.size()) return false;
    for (size_t i = 0; i < pat.size(); i++)
        if (pat[i] >= 0 && data[at + i] != uint8_t(pat[i])) return false;
    return true;
}

inline std::vector<size_t> findAll(const uint8_t* data, size_t size, const std::vector<int>& pat, size_t limit = 2) {
    std::vector<size_t> hits;
    if (pat.empty() || size < pat.size()) return hits;
    for (size_t i = 0; i + pat.size() <= size && hits.size() < limit; i++)
        if (matchesAt(data, size, i, pat)) hits.push_back(i);
    return hits;
}

// The signature search: a match must start inside [lo, hi) and may run past it; within [lo, hi) the hits are findAll's.
inline std::vector<size_t> findIn(const uint8_t* data, size_t size, const std::vector<int>& pat, size_t lo, size_t hi, size_t limit = 2) {
    std::vector<size_t> hits;
    if (pat.empty() || size < pat.size()) return hits;
    size_t anchor = 0;
    while (anchor < pat.size() && pat[anchor] < 0) anchor++;
    hi = (std::min)(hi, size - pat.size() + 1);   // the last start whose match fits the image
    if (anchor == pat.size()) {   // all wildcards: every start matches
        for (size_t i = lo; i < hi && hits.size() < limit; i++) hits.push_back(i);
        return hits;
    }
    const int first = pat[anchor];
    for (size_t i = lo; i < hi && hits.size() < limit;) {
        const void* p = std::memchr(data + i + anchor, first, hi - i);
        if (!p) break;
        const size_t at = size_t(static_cast<const uint8_t*>(p) - data) - anchor;
        if (matchesAt(data, size, at, pat)) hits.push_back(at);
        i = at + 1;
    }
    return hits;
}
// Search .text first, then other executable sections to detect ambiguous matches.
// Images without usable section headers fall back to a full-image scan.
struct CodeRange {
    size_t lo = 0, hi = 0;
    bool fromHeaders = false;
    std::vector<std::pair<size_t, size_t>> others;
};
inline CodeRange codeRange(const uint8_t* image, size_t size) {
    CodeRange r{0, size, false, {}};
    if (!image || size < 0x40) return r;
    uint32_t pe = 0;
    std::memcpy(&pe, image + 0x3C, 4);
    if (uint64_t(pe) + 24 > size || std::memcmp(image + pe, "PE\0\0", 4) != 0) return r;
    uint16_t sections = 0, optSize = 0;
    std::memcpy(&sections, image + pe + 6, 2);
    std::memcpy(&optSize, image + pe + 20, 2);
    const uint64_t table = uint64_t(pe) + 24 + optSize;
    size_t lo = size, hi = 0;
    CodeRange text;
    std::vector<std::pair<size_t, size_t>> exec;
    for (uint16_t i = 0; i < sections; i++) {
        const uint64_t at = table + uint64_t(i) * 40;
        if (at + 40 > size) return r;
        char name[9] = {};
        uint32_t vsize = 0, va = 0, flags = 0;
        std::memcpy(name, image + at, 8);
        std::memcpy(&vsize, image + at + 8, 4);
        std::memcpy(&va, image + at + 12, 4);
        std::memcpy(&flags, image + at + 36, 4);
        if (!(flags & (0x00000020u | 0x20000000u)) || !vsize || va >= size) continue;
        const size_t end = (std::min)(size, size_t(va) + size_t(vsize));
        if (std::strcmp(name, ".text") == 0 && !text.fromHeaders) text = CodeRange{size_t(va), end, true, {}};
        else exec.emplace_back(size_t(va), end);
        lo = (std::min)(lo, size_t(va));
        hi = (std::max)(hi, end);
    }
    if (text.fromHeaders) {
        text.others = std::move(exec);
        return text;
    }
    if (hi <= lo) return r;
    return CodeRange{lo, hi, true, {}};
}
// Fall back to a full-image scan only when no executable section contains a match.
inline std::vector<size_t> findSig(const uint8_t* data, size_t size, const std::vector<int>& pat, const CodeRange& code, size_t limit) {
    std::vector<size_t> hits = findIn(data, size, pat, code.lo, code.hi, limit);
    for (const auto& sec : code.others) {
        if (hits.size() >= limit) break;
        for (const size_t h : findIn(data, size, pat, sec.first, sec.second, limit - hits.size())) hits.push_back(h);
    }
    if (hits.empty() && code.fromHeaders) hits = findIn(data, size, pat, 0, size, limit);
    return hits;
}

inline bool readRaw(uintptr_t address, void* dest, size_t size) {
    if (!address || uint64_t(address) + size > 0x100000000ull) return false;
    __try { std::memcpy(dest, reinterpret_cast<void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool writeRaw(uintptr_t address, const void* src, size_t size) {
    if (!address || uint64_t(address) + size > 0x100000000ull) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address), src, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <typename T> bool readValue(uintptr_t address, T& out) { return readRaw(address, &out, sizeof(T)); }

// Reads a value from game memory; `def` when the address is 0 or unreadable (SEH-guarded).
template <typename T> T rd(uintptr_t at, T def = T()) {
    T v = def;
    if (!at || !readValue(at, v)) return def;
    return v;
}
// Reads a value; false when the address is 0 or unreadable.
template <typename T> bool rdok(uintptr_t at, T& v) { return at && readValue(at, v); }

// SEH guard for calls into game code or through pointers read from game memory: false when it faulted.
template <class F> bool seh(F&& f) {
    __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A verified write: only when the current value is `expectedBefore` (bitwise), then read back.
enum class WriteResult { Ok, Unreadable, NotExpected, WriteFailed, ReadBackDiffers };
inline const char* writeResultText(WriteResult r) {
    switch (r) {
    case WriteResult::Ok: return "ok";
    case WriteResult::Unreadable: return "unreadable";
    case WriteResult::NotExpected: return "not the expected value; left alone";
    case WriteResult::WriteFailed: return "write faulted";
    case WriteResult::ReadBackDiffers: return "read back differs";
    }
    return "?";
}
template <typename T> WriteResult wrVerifiedWhy(uintptr_t at, const T& value, const T& expectedBefore) {
    T now{};
    if (!rdok(at, now)) return WriteResult::Unreadable;
    if (std::memcmp(&now, &expectedBefore, sizeof(T)) != 0) return WriteResult::NotExpected;
    if (!writeRaw(at, &value, sizeof(T))) return WriteResult::WriteFailed;
    T back{};
    if (!rdok(at, back) || std::memcmp(&back, &value, sizeof(T)) != 0) return WriteResult::ReadBackDiffers;
    return WriteResult::Ok;
}
template <typename T> bool wrVerified(uintptr_t at, const T& value, const T& expectedBefore) { return wrVerifiedWhy(at, value, expectedBefore) == WriteResult::Ok; }

inline std::string fmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, f, ap);
    va_end(ap);
    return b;
}

}  // namespace tf
