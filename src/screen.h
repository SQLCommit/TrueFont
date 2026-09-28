// Sharpness Auto for world text uses the smallest active rendering target per axis:
// R = min(viewport, background RT, active menu canvas, client area).
// k = pow2ceil(max(0.00111 * R_w, 0.00112 * R_h) / 1.1), clamped to 2..4.
// Apply the tolerance before rounding to avoid undersampling.
//
// Sizes come from the app getters (+10..+1A), menu-manager bounds and GetClientRect.
// Use the menu canvas only while renderer+1E0 and +1E8 are nonzero.
// Poll once per second; accept the first valid size immediately and later changes after 3 s.
#pragma once
#include "sigs.h"
#include "tf_mem.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tf {

inline constexpr const char* kAppGetterPatterns[6] = {
    "A1 ?? ?? ?? ?? 66 8B 40 10 C3", "A1 ?? ?? ?? ?? 66 8B 40 12 C3", "A1 ?? ?? ?? ?? 66 8B 40 14 C3",
    "A1 ?? ?? ?? ?? 66 8B 40 16 C3", "A1 ?? ?? ?? ?? 66 8B 40 18 C3", "A1 ?? ?? ?? ?? 66 8B 40 1A C3",
};
// The mouse-remap manager (Sep-10 0x19AC): operands right (+8), left (+19), top (+49), bottom (+60).
inline constexpr const char* kMenuWordsPattern =
    "E8 ?? ?? ?? ?? 0F BF 0D ?? ?? ?? ?? 89 44 24 18 0F BF 05 ?? ?? ?? ?? 2B C8 89 4C 24 10 DB 44 24 10 DB 44 24 14 D8 F2 DE C9 E8 ?? ?? ?? ?? "
    "0F BF 15 ?? ?? ?? ?? DD D8 8B F0 0F BF 05 ?? ?? ?? ?? 2B C2";
inline constexpr uint32_t kAppViewport = 0x10, kAppMenu = 0x14, kAppBack = 0x18;
inline constexpr uint32_t kRendMenuRt = 0x1E0, kRendMenuRt2 = 0x1E8;

struct ScreenResolved {
    uint32_t appSlot = 0;      // RVA of the app object pointer (Sep-10 4568FC)
    uint32_t menuLeft = 0;     // RVA of the menu-manager word L (Sep-10 6218B4); T, R, B follow at +2, +4, +6
    uint32_t rendererSlot = 0; // RVA of the renderer pointer (sigs.h deviceSlot, Sep-10 45666C)
    bool ok() const { return appSlot != 0; }
};
// Resolve a memory-layout image (offset == RVA). Without rendererSlot, ignore the menu canvas.
inline ScreenResolved resolveScreen(const uint8_t* data, size_t size, uintptr_t base, uint32_t rendererSlot, std::vector<std::string>& log) {
    ScreenResolved r;
    const CodeRange code = codeRange(data, size);
    const auto absAt = [&](size_t at) -> uint32_t {
        const uint32_t v = rd32(data, size, at);
        return v >= base && uint64_t(v) - base < size ? uint32_t(v - base) : 0;
    };
    uint32_t slot = 0;
    bool agree = true;
    std::string where;
    for (int i = 0; i < 6; i++) {
        const auto hits = findSig(data, size, parsePattern(kAppGetterPatterns[i]), code, 3);
        const uint32_t s = hits.size() == 1 ? absAt(hits[0] + 1) : 0;
        where += fmt("%s%06X", i ? " " : "", hits.size() == 1 ? unsigned(hits[0]) : 0u);
        if (!s) agree = false;
        else if (!slot) slot = s;
        else if (s != slot) agree = false;
    }
    r.appSlot = agree ? slot : 0;
    log.push_back(fmt("screen: app getters at %s: app slot %06X (Sep-10 4568FC)%s", where.c_str(), r.appSlot, r.appSlot ? "" : " NOT RESOLVED (each unique, one slot): Sharpness Auto is 2x"));
    const auto hits = findSig(data, size, parsePattern(kMenuWordsPattern), code, 3);
    if (hits.size() == 1) {
        const uint32_t right = absAt(hits[0] + 8), left = absAt(hits[0] + 19), top = absAt(hits[0] + 49), bottom = absAt(hits[0] + 60);
        if (left && top == left + 2 && right == left + 4 && bottom == left + 6) r.menuLeft = left;
    }
    r.rendererSlot = rendererSlot;
    log.push_back(fmt("screen: menu-manager words %06X (Sep-10 6218B4; %zu match(es))%s; renderer slot %06X (Sep-10 45666C)%s", r.menuLeft, hits.size(),
                      r.menuLeft ? "" : " NOT RESOLVED: the menu canvas is not used", r.rendererSlot, r.rendererSlot ? "" : " NOT RESOLVED: the menu RT is taken as absent"));
    return r;
}

struct ScreenSizes {
    int vpW = 0, vpH = 0, menuW = 0, menuH = 0, bgW = 0, bgH = 0, canvasW = 0, canvasH = 0, clientW = 0, clientH = 0;
    bool menuRt = false;
    bool ok() const { return vpW > 0 && vpH > 0; }
    std::string text() const {
        return fmt("viewport %dx%d, menu resolution %dx%d, background %dx%d, menu RT %s%s, client %dx%d", vpW, vpH, menuW, menuH, bgW, bgH, menuRt ? "yes, canvas " : "no",
                   menuRt ? fmt("%dx%d", canvasW, canvasH).c_str() : "", clientW, clientH);
    }
};
inline bool plausibleSize(int w, int h) { return w >= 64 && h >= 64 && w <= 16384 && h <= 16384; }

// An invalid viewport invalidates the entire sample; other invalid sizes are ignored.
inline ScreenSizes readScreen(uintptr_t base, const ScreenResolved& r, HWND hwnd) {
    ScreenSizes s;
    if (!r.ok()) return s;
    const uintptr_t app = rd<uint32_t>(base + r.appSlot);
    if (app < 0x10000) return s;
    const auto pair = [&](uint32_t off, int& w, int& h) {
        const int a = rd<uint16_t>(app + off), b = rd<uint16_t>(app + off + 2);
        if (plausibleSize(a, b)) { w = a; h = b; }
    };
    pair(kAppViewport, s.vpW, s.vpH);
    pair(kAppMenu, s.menuW, s.menuH);
    pair(kAppBack, s.bgW, s.bgH);
    if (!s.ok()) return ScreenSizes{};
    if (r.rendererSlot) {
        const uintptr_t rend = rd<uint32_t>(base + r.rendererSlot);
        s.menuRt = rend >= 0x10000 && rd<uint32_t>(rend + kRendMenuRt) != 0 && rd<uint32_t>(rend + kRendMenuRt2) != 0;
    }
    if (s.menuRt && r.menuLeft) {
        const int L = rd<int16_t>(base + r.menuLeft), T = rd<int16_t>(base + r.menuLeft + 2), R = rd<int16_t>(base + r.menuLeft + 4), B = rd<int16_t>(base + r.menuLeft + 6);
        if (plausibleSize(R - L, B - T)) { s.canvasW = R - L; s.canvasH = B - T; }
    }
    RECT rc{};
    if (hwnd && GetClientRect(hwnd, &rc) && plausibleSize(int(rc.right - rc.left), int(rc.bottom - rc.top))) {
        s.clientW = int(rc.right - rc.left);
        s.clientH = int(rc.bottom - rc.top);
    }
    return s;
}

inline constexpr int kAutoMinK = 2;          // the floor: what these textures always had
inline constexpr double kAutoTolerance = 1.1;   // need / 1.1 before pow2ceil
// Account for applied Aspect and Size: max(0.00111 * widthFactor * R_w, 0.00112 * R_h) / 1.1 * size.
inline int autoPlateK(const ScreenSizes& s, double widthFactor = 1.0, double size = 1.0) {
    if (!s.ok()) return 2;
    int rw = s.vpW, rh = s.vpH;
    const auto cap = [&](int w, int h) {
        if (w > 0 && h > 0) { rw = (std::min)(rw, w); rh = (std::min)(rh, h); }
    };
    cap(s.bgW, s.bgH);
    if (s.menuRt) cap(s.canvasW, s.canvasH);
    cap(s.clientW, s.clientH);
    const double need = (std::max)(0.00111 * widthFactor * rw, 0.00112 * rh) / kAutoTolerance * size;
    const int k = need <= 1.0 ? 1 : need <= 2.0 ? 2 : 4;   // pow2ceil, clamped to 1..4
    return (std::max)(kAutoMinK, k);
}

// One monitor per group because Aspect and Size differ; both share the same screen sample.
struct AutoKMonitor {
    static constexpr uint64_t kPollMs = 1000, kHoldMs = 3000;
    int k = 2;                  // Auto's k in use
    bool seen = false;          // a read has been taken
    int pending = 0;            // a different k seen, since `pendingSince`
    uint64_t pendingSince = 0, nextPoll = 0;
    ScreenSizes last;           // the read the k in use came from
    ScreenSizes latest;         // the newest read
    bool offer(const ScreenSizes& s, uint64_t nowMs, double widthFactor = 1.0, double size = 1.0) {
        latest = s;
        if (!s.ok()) return false;
        const int want = autoPlateK(s, widthFactor, size);
        if (!seen) {
            seen = true;
            last = s;
            pending = 0;
            const bool changed = want != k;
            k = want;
            return changed;
        }
        if (want == k) { pending = 0; last = s; return false; }
        if (want != pending) { pending = want; pendingSince = nowMs; return false; }
        if (nowMs - pendingSince < kHoldMs) return false;
        k = want;
        pending = 0;
        last = s;
        return true;
    }
    bool due(uint64_t nowMs) {
        if (nowMs < nextPoll) return false;
        nextPoll = nowMs + kPollMs;
        return true;
    }
};

}  // namespace tf
