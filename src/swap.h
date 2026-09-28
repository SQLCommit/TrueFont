// Chat/menu atlas installation through CYyTex+40; render thread only except DataOnly release.
// The worker builds copied inputs, and the render thread uploads and swaps complete textures.
// One atlas serves moji and high through normalized UVs; active font high requires at least 32 px cells.
// Keep one owning texture reference plus one per installed record. Never release an unowned texture.
// Preserve foreign replacements as originals; stop reapplying after sustained contention.
// Hardware limits, rebuild memory and free address-space blocks constrain Sharpness.
// Chat claims its budget first after enabling; sprite fonts may shrink to leave room.
#pragma once
#include "tf_mem.h"
#include <psapi.h>
#include "sigs.h"
#include "atlas.h"
#include "dxt.h"
#include "swaplogic.h"
#include "datfallback.h"
#include "fonts.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "psapi.lib")

namespace tf {

// Logging sink: set by truefont.cpp (the per-character log and the chat line)
struct Sink {
    void (*info)(const std::string&) = nullptr;
    void (*warn)(const std::string&) = nullptr;
    void (*err)(const std::string&) = nullptr;
    void (*chat)(const std::string&, unsigned char) = nullptr;
};
inline Sink sink;
inline void info(const std::string& s) { if (sink.info) sink.info(s); }
inline void warn(const std::string& s) { if (sink.warn) sink.warn(s); }
inline void err(const std::string& s) { if (sink.err) sink.err(s); }
inline void chat(const std::string& s, unsigned char colour = 0x6A) { if (sink.chat) sink.chat(s, colour); }
// Warn once for an explicit FreeType request that falls back; Auto fallback is log-only.
// Both installers report completed builds on the render thread.
inline bool engineFallbackSaid = false;
inline void engineFallbackChat(Engine setting, const std::string& groups, bool faulted) {
    if (setting != Engine::FreeType || engineFallbackSaid) return;
    engineFallbackSaid = true;
    chat(faulted ? "FreeType failed on the font of " + groups + " during a build, so Windows draws the rest of it. See the log."
                 : "FreeType could not open the font of " + groups + ", so Windows draws it. See the log.",
         0x68);
}

inline std::string formatName(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_A8: return "A8";
    case D3DFMT_DXT1: return "DXT1";
    case D3DFMT_DXT2: return "DXT2";
    case D3DFMT_DXT3: return "DXT3";
    case D3DFMT_DXT4: return "DXT4";
    case D3DFMT_DXT5: return "DXT5";
    default: return fmt("format %u", unsigned(f));
    }
}
inline const char* poolName(D3DPOOL p) {
    switch (p) {
    case D3DPOOL_DEFAULT: return "DEFAULT";
    case D3DPOOL_MANAGED: return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    default: return "?";
    }
}

static_assert(kGridCols % 4 == 0 && kGridRows % 4 == 0, "DXT3 blocks are 4 x 4: the atlas (64 x 128 cells) is a multiple of 4 each way at any cell size");
static_assert(kGridCols == kChatGridCols && kGridRows == kChatGridRows, "texfmt.h's chatPeak counts the atlas grid (64 x 128 cells)");
// The next cell size down (48 skipped when the card wants power-of-two textures); 0 below 16.
inline int nextCellDown(int c, bool nonPow2) { return c > 48 ? (nonPow2 ? 48 : 32) : c > 32 ? 32 : c > 16 ? 16 : 0; }
enum class ChatLimit : uint8_t { None, Card, Pow2, Memory, Session };
// Largest supported cell <= want, with a 16 px floor. Zero caps mean unknown.
// Count otherBytes against the shared budget and report the first limiting constraint.
inline int fitChatCell(int want, unsigned long maxW, unsigned long maxH, bool nonPow2, TexFormat f, uint64_t readbackBytes, uint64_t shapeCells, std::string& why,
                       uint64_t otherBytes = 0, ChatLimit* limit = nullptr) {
    why.clear();
    ChatLimit lim = ChatLimit::None;
    int c = want == 64 || want == 48 || want == 32 ? want : 16;
    if (c == 48 && !nonPow2) {
        why = "the card wants power-of-two textures (D3DPTEXTURECAPS_POW2), and 48 px cells make a 3072 x 6144 one";
        lim = ChatLimit::Pow2;
        c = 32;
    }
    for (; c > 16; c = nextCellDown(c, nonPow2)) {
        const unsigned long w = static_cast<unsigned long>(kGridCols * c), h = static_cast<unsigned long>(kGridRows * c);
        if (maxW && (w > maxW || h > maxH)) {
            if (why.empty()) {
                why = fmt("%d px cells need a %lu x %lu texture; this card's largest is %lu x %lu", c, w, h, maxW, maxH);
                lim = ChatLimit::Card;
            }
            continue;
        }
        const ChatPeak p = chatPeak(c, f, readbackBytes, shapeCells, otherBytes);
        if (p.total() > kMemoryLimit) {
            if (why.empty()) {
                why = fmt("%d px cells would hold about %d MB while they build (as %s: the texture in place %.0f, the new one %.0f, both counted %s as MANAGED; the worker's "
                          "image %.0f%s, the glyph shapes %.1f, the copy of the game's font %.0f, the menu and HUD fonts %.1f), over %.0f MB",
                          c, mbUp(p.total()), texFormatName(f), double(p.installed) / 1048576.0, double(p.texture) / 1048576.0, kManagedCountText, double(p.image) / 1048576.0,
                          p.encode ? fmt(", its A8L8 source %.0f", double(p.encode) / 1048576.0).c_str() : "", double(p.shapes) / 1048576.0, double(p.readback) / 1048576.0,
                          double(p.others) / 1048576.0, double(kMemoryLimit) / 1048576.0);
                lim = ChatLimit::Memory;
            }
            continue;
        }
        break;
    }
    if (limit) *limit = c < want ? lim : ChatLimit::None;
    return c;
}

// Shared sizing inputs for builds, status and UI.
struct ChatBudget {
    unsigned long maxW = 0, maxH = 0;
    bool nonPow2 = true;
    TexFormat plain = TexFormat::A8L8, packed = TexFormat::Dxt3;   // the upload format without and with Compress
    bool compress = false;
    bool high = false;                  // the game's high-resolution font is active (32 px at least)
    uint64_t readback = 8ull << 20;     // the copy of the game's font a build holds
    uint64_t cells = 256;               // the cells a build draws (its kept shapes)
    uint64_t latinCells = 256;          // ... with the Japanese Font Off
    uint64_t others = 0;                // the menu and HUD fonts' bytes
    int cap = 0;                        // cellCap_: the largest size since one above it could not be made (0: none)
    bool capAddress = false;            // ... because the game had no free block large enough
    TexFormat format() const { return compress ? packed : plain; }
};
struct ChatFit {
    int want = 16;                      // the cell asked for (font high applied)
    int cell = 16;                      // the cell built
    ChatLimit limit = ChatLimit::None;
    std::string why;                    // Diagnostic reason
    ChatPeak peak;                      // at `want`
    bool fits() const { return cell >= want; }
};
inline ChatFit fitChat(const ChatBudget& b, int wantCell) {
    ChatFit r;
    r.want = b.high ? (std::max)(32, wantCell) : wantCell;
    r.cell = fitChatCell(r.want, b.maxW, b.maxH, b.nonPow2, b.format(), b.readback, b.cells, r.why, b.others, &r.limit);
    if (b.cap && r.cell > b.cap) {
        r.cell = b.cap;
        if (r.limit == ChatLimit::None) r.limit = ChatLimit::Session;
    }
    if (b.high) r.cell = (std::max)(32, r.cell);
    r.peak = chatPeak(r.want, b.format(), b.readback, b.cells, b.others);
    return r;
}
inline std::string sharpName(int sharp) { return clampSharp(sharp) ? std::to_string(clampSharp(sharp)) + "x" : std::string("Auto"); }
// A built cell size in the panel's words: 16 px is Auto's, 32, 48 and 64 px are 2x, 3x and 4x (the chat lines).
inline std::string cellSharpText(int cell) { return cell <= 16 ? std::string("Auto") : std::to_string(chatSharpX(cell)) + "x"; }
// Explain an unavailable Sharpness choice; empty if it fits.
inline std::string chatSharpDenied(const ChatBudget& b, int sharp) {
    const ChatFit f = fitChat(b, chatCellFor(sharp));
    if (f.fits()) return std::string();
    const std::string x = sharpName(sharp);
    switch (f.limit) {
    case ChatLimit::Card: return "This card's largest texture is too small for " + x + ".";
    case ChatLimit::Pow2: return "This card takes only power-of-two texture sizes, so it can't make " + x + ".";
    case ChatLimit::Session:
        return x + (b.capAddress ? " could not be made this session: the game has no free block of memory large enough for it. /tfont on tries again."
                                 : " could not be made this session. /tfont on tries again.");
    default: break;
    }
    // Memory: what makes it too big, named only when leaving it out would make it fit.
    const auto fitsWith = [&](bool noHigh, bool noJp, bool noOthers) {
        ChatBudget x2 = b;
        if (noHigh) x2.readback = (std::min)(x2.readback, uint64_t(8ull << 20));
        if (noJp) x2.cells = (std::min)(x2.cells, x2.latinCells);
        if (noOthers) x2.others = 0;
        return fitChat(x2, chatCellFor(sharp)).fits();
    };
    const bool hi = b.high && b.readback > (8ull << 20), jp = b.cells > b.latinCells, ot = b.others > 0;
    std::string ctx;
    if (fitsWith(hi, jp, ot)) {
        const char* names[3] = {"the game's high-resolution font on", "a Japanese Font", "the other groups' fonts"};
        const bool present[3] = {hi, jp, ot};
        for (int i = 0; i < 3 && ctx.empty(); i++)
            if (present[i] && fitsWith(i == 0, i == 1, i == 2)) ctx = std::string(" with ") + names[i];
        if (ctx.empty()) {
            std::vector<std::string> list;
            for (int i = 0; i < 3; i++) if (present[i]) list.push_back(names[i]);
            for (size_t i = 0; i < list.size(); i++) ctx += (i == 0 ? " with " : i + 1 == list.size() ? " and " : ", ") + list[i];
        }
    }
    std::string t = "Too big" + ctx + ": " + x + " needs about " + std::to_string(mbUp(f.peak.total())) + " MB; TrueFont's limit is " + std::to_string(mbUp(kMemoryLimit)) + " MB.";
    if (!b.compress && b.packed != b.plain) {
        ChatBudget z = b;
        z.compress = true;
        const ChatFit c = fitChat(z, chatCellFor(sharp));
        if (c.cell > f.cell) t += " Tick Compress (Settings tab) for " + std::to_string(chatSharpX(c.cell)) + "x.";
    }
    return t;
}
// The note under Chat's Sharpness when the saved value cannot be built now: the size actually used. "" when it can.
inline std::string chatSharpNote(const ChatBudget& b, int sharp) {
    const ChatFit f = fitChat(b, chatCellFor(sharp));
    if (f.fits()) return std::string();
    return sharpName(sharp) + " can't be used here, so TrueFont uses " + std::to_string(chatSharpX(f.cell)) + "x (point at " + sharpName(sharp) + " for why).";
}
// The Chat Sharpness tooltip with this client's figures: what TrueFont holds while each size builds, as the rule counts it.
inline std::string chatSharpTip(const ChatBudget& b) {
    ChatBudget p = b, z = b;
    p.compress = false;
    z.compress = true;
    const auto mb = [](const ChatBudget& x, int sharp) { return mbUp(fitChat(x, chatCellFor(sharp)).peak.total()); };
    return fmt("How much detail each character is drawn with. Auto: 16 px (32 px with the game's high-resolution font); 2x, 3x and 4x are 32, 48 and 64 px. While Chat/Items "
               "builds, TrueFont holds about %d, %d, %d and %d MB here (%d, %d, %d and %d MB with Compress), the other groups' fonts included; its limit is %d MB, so a "
               "Sharpness that would pass it is greyed. Chat/Items is also limited by the game's Menu Resolution: set it to your screen size.",
               mb(p, 0), mb(p, 2), mb(p, 3), mb(p, 4), mb(z, 0), mb(z, 2), mb(z, 3), mb(z, 4), mbUp(kMemoryLimit));
}
// Large textures need contiguous address space even when total free memory is sufficient.
inline uint64_t largestFreeRegion() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t at = uintptr_t(si.lpMinimumApplicationAddress);
    const uintptr_t end = uintptr_t(si.lpMaximumApplicationAddress);
    uint64_t best = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (at < end && VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_FREE) best = (std::max)(best, uint64_t(mbi.RegionSize));
        const uintptr_t next = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) break;
        at = next;
    }
    return best;
}
inline uint64_t (*largestFreeFn)() = &largestFreeRegion;

// Sample commit and free address space around large allocations and uploads, never per frame.
// Bound the VirtualQuery walk by region count and elapsed time.
struct MemSample {
    bool ok = false;                    // GetProcessMemoryInfo answered
    uint64_t priv = 0, pagefile = 0;    // PrivateUsage, PagefileUsage
    uint64_t largest = 0, free = 0;     // the largest free block, and all free address space (as far as the walk got)
    int regions = 0;
    bool capped = false;                // the walk stopped at its cap: `largest` and `free` are partial
    double ms = 0;                      // the sample's own cost
};
inline constexpr int kMemWalkRegions = 32768;
inline constexpr double kMemWalkMs = 20.0;
inline constexpr uint64_t kMemProbeBytes = 4ull << 20;
inline MemSample memSampleNow() {
    const auto t0 = std::chrono::steady_clock::now();
    const auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    MemSample m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        m.ok = true;
        m.priv = uint64_t(pmc.PrivateUsage);
        m.pagefile = uint64_t(pmc.PagefileUsage);
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t at = uintptr_t(si.lpMinimumApplicationAddress);
    const uintptr_t end = uintptr_t(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (at < end) {
        if (m.regions >= kMemWalkRegions || ((m.regions & 255) == 255 && ms() > kMemWalkMs)) { m.capped = true; break; }
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) != sizeof mbi) break;
        ++m.regions;
        if (mbi.State == MEM_FREE) {
            m.free += uint64_t(mbi.RegionSize);
            m.largest = (std::max)(m.largest, uint64_t(mbi.RegionSize));
        }
        const uintptr_t next = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) break;
        at = next;
    }
    m.ms = ms();
    return m;
}
inline MemSample (*memSampleFn)() = &memSampleNow;   // replaceable
inline double mib(uint64_t bytes) { return double(bytes) / 1048576.0; }
inline MemSample memLog(const std::string& event) {
    const MemSample m = memSampleFn ? memSampleFn() : MemSample{};
    info(fmt("memory: %s: private %.1f MB, pagefile %.1f MB, largest free block %.1f MB, free address space %.1f MB (%d regions%s, %.2f ms)%s", event.c_str(), mib(m.priv),
             mib(m.pagefile), mib(m.largest), mib(m.free), m.regions, m.capped ? ", the walk capped: partial" : "", m.ms, m.ok ? "" : " (GetProcessMemoryInfo failed)"));
    return m;
}
// Measure address-space consumption; fall back to private-byte growth if either walk was incomplete.
inline double memTakenMB(const MemSample& a, const MemSample& b, bool* byFree = nullptr) {
    const bool f = !a.capped && !b.capped && a.regions > 0 && b.regions > 0;
    if (byFree) *byFree = f;
    return f ? (double(a.free) - double(b.free)) / 1048576.0 : (double(b.priv) - double(a.priv)) / 1048576.0;
}
// The upload's slice count for `bytes`: at least `least`, and at most kUploadSliceBytes a frame.
inline constexpr uint64_t kUploadSliceBytes = 4ull << 20;
inline int uploadSlices(int least, uint64_t bytes) { return (std::max)(least, int((bytes + kUploadSliceBytes - 1) / kUploadSliceBytes)); }
inline std::string mbText(uint64_t bytes) { return fmt("%.1f MB", double(bytes) / 1048576.0); }
inline D3DFORMAT d3dFormat(TexFormat f) { return f == TexFormat::Dxt3 ? D3DFMT_DXT3 : f == TexFormat::A8L8 ? D3DFMT_A8L8 : D3DFMT_A8R8G8B8; }

// What CheckDeviceFormat accepts for a texture on this device; both installers probe it.
struct FormatSupport {
    bool checked = false, argb = true, a8l8 = false, dxt3 = false;
    TexFormat pick(bool compress) const { return compress && dxt3 ? TexFormat::Dxt3 : a8l8 ? TexFormat::A8L8 : TexFormat::A8R8G8B8; }
    // What a build would use: the probed answer, or before the probe the usual one (the panel's note).
    TexFormat expected(bool compress) const { return checked ? pick(compress) : compress ? TexFormat::Dxt3 : TexFormat::A8L8; }
};
// `text`: each format's HRESULT, for the log.
inline FormatSupport probeFormats(IDirect3DDevice8* dev, std::string& text) {
    FormatSupport fs;
    IDirect3D8* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE dm{};
    const HRESULT h1 = dev ? dev->GetDirect3D(&d3d) : E_POINTER;
    const HRESULT h2 = dev ? dev->GetCreationParameters(&cp) : E_POINTER, h3 = dev ? dev->GetDisplayMode(&dm) : E_POINTER;
    text.clear();
    if (d3d && SUCCEEDED(h2) && SUCCEEDED(h3)) {
        fs.checked = true;
        for (const D3DFORMAT f : {D3DFMT_A8R8G8B8, D3DFMT_A8L8, D3DFMT_DXT3, D3DFMT_DXT5}) {
            const HRESULT hr = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, 0, D3DRTYPE_TEXTURE, f);
            text += fmt(" %s 0x%08X%s", formatName(f).c_str(), unsigned(hr), SUCCEEDED(hr) ? " (ok)" : " (REFUSED)");
            if (f == D3DFMT_A8R8G8B8) fs.argb = SUCCEEDED(hr);
            if (f == D3DFMT_A8L8) fs.a8l8 = SUCCEEDED(hr);
            if (f == D3DFMT_DXT3) fs.dxt3 = SUCCEEDED(hr);
        }
        text = fmt("(adapter %u, type %d, display %s):", cp.AdapterOrdinal, int(cp.DeviceType), formatName(dm.Format).c_str()) + text;
    } else text = fmt("not checked (GetDirect3D 0x%08X, GetCreationParameters 0x%08X, GetDisplayMode 0x%08X)", unsigned(h1), unsigned(h2), unsigned(h3));
    if (d3d) d3d->Release();
    return fs;
}
inline bool uploadTex(IDirect3DTexture8* t, const TexImage& img, std::string& why) {
    if (!img.valid()) { why = "no converted atlas"; return false; }
    D3DLOCKED_RECT lr{};
    const HRESULT hr = t->LockRect(0, &lr, nullptr, 0);
    if (FAILED(hr) || !lr.pBits) { why = fmt("LockRect 0x%08X", unsigned(hr)); return false; }
    const size_t row = img.rowBytes();
    if (size_t(lr.Pitch) < row) { t->UnlockRect(0); why = fmt("pitch %d < %zu", lr.Pitch, row); return false; }
    copyRows(static_cast<uint8_t*>(lr.pBits), size_t(lr.Pitch), img.bytes(), row, row, size_t(img.rows()));
    t->UnlockRect(0);
    return true;
}
// One slice of the upload: rows [r0, r1) (DXT3: block rows) through a LockRect of just those rows. Nothing draws with the
// texture yet, so the slices can come on successive frames.
inline bool uploadRows(IDirect3DTexture8* t, const TexImage& img, int r0, int r1, std::string& why) {
    if (!img.valid() || r0 < 0 || r1 > img.rows() || r0 >= r1) { why = "no converted atlas"; return false; }
    const int per = img.format == TexFormat::Dxt3 ? 4 : 1;   // texel rows a row of the image holds
    RECT rc{0, LONG(r0 * per), LONG(img.w), LONG(r1 * per)};
    D3DLOCKED_RECT lr{};
    const HRESULT hr = t->LockRect(0, &lr, &rc, 0);
    if (FAILED(hr) || !lr.pBits) { why = fmt("LockRect 0x%08X", unsigned(hr)); return false; }
    const size_t row = img.rowBytes();
    if (size_t(lr.Pitch) < row) { t->UnlockRect(0); why = fmt("pitch %d < %zu", lr.Pitch, row); return false; }
    copyRows(static_cast<uint8_t*>(lr.pBits), size_t(lr.Pitch), img.bytes() + size_t(r0) * row, row, row, size_t(r1 - r0));
    t->UnlockRect(0);
    return true;
}
// A fine clock for the render thread's own costs (GetTickCount64 steps 15.6 ms).
inline double qpcMs() {
    static const double perMs = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart) / 1000.0; }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return perMs > 0 ? double(c.QuadPart) / perMs : 0.0;
}

// Allocate and fault in readback pages on a helper thread. Access the buffer only after joining it,
// so the render-thread copy writes warm pages.
class BufferPrep {
public:
    BufferPrep() = default;
    BufferPrep(const BufferPrep&) = delete;
    BufferPrep& operator=(const BufferPrep&) = delete;
    ~BufferPrep() { wait(); }
    // Starts preparing `n` bytes unless ready or under way. False: no thread could start (the copy then allocates itself).
    bool request(size_t n) {
        discard_ = false;
        if (!n) return false;
        if (t_.joinable()) {
            if (!done_.load()) return true;   // under way (another size: its take() misses and the copy allocates itself)
            t_.join();
        }
        if (buf_.size() == n) return true;
        std::vector<uint8_t>().swap(buf_);
        n_ = n;
        done_.store(false);
        try {
            t_ = std::thread([this, n] {
                try {
                    std::vector<uint8_t> v(n);   // zero-filled: every page touched here, not on the render thread
                    buf_ = std::move(v);
                } catch (const std::exception&) {}   // Let the copy attempt report allocation failure.
                done_.store(true);
            });
        } catch (const std::exception&) {
            done_.store(true);
            return false;
        }
        return true;
    }
    // Nothing to wait for: `n` bytes are ready, or the helper asked for them finished (even without memory).
    bool readyFor(size_t n) const { return t_.joinable() ? done_.load() && n_ == n : buf_.size() == n; }
    bool busy() const { return t_.joinable() && !done_.load(); }
    std::vector<uint8_t> take(size_t n) {
        if (t_.joinable()) {
            if (!done_.load()) return {};
            t_.join();
        }
        if (buf_.size() != n) return {};
        std::vector<uint8_t> out;
        out.swap(buf_);
        return out;
    }
    // Frees a buffer nobody took (now, or when its thread ends: poll()).
    void discard() {
        discard_ = true;
        poll();
    }
    void poll() {
        if (!discard_ || busy()) return;
        if (t_.joinable()) t_.join();
        std::vector<uint8_t>().swap(buf_);
        discard_ = false;
    }
    void wait() { if (t_.joinable()) t_.join(); }

private:
    std::thread t_;
    std::atomic<bool> done_{true};
    std::vector<uint8_t> buf_;
    size_t n_ = 0;
    bool discard_ = false;
};

// The game's texture level 0 as the render thread copied it: DXT3 block rows or A8R8G8B8 rows, tight.
struct RawTexture {
    bool dxt3 = false;
    int w = 0, h = 0;
    std::vector<uint8_t> bytes;
    std::string source;   // Diagnostic reason
};
inline bool decodeRaw(const RawTexture& raw, Readback& rb, std::string& why) {
    const auto t0 = std::chrono::steady_clock::now();
    const size_t row = raw.dxt3 ? size_t(raw.w / 4) * 16 : size_t(raw.w) * 4, rows = raw.dxt3 ? size_t(raw.h / 4) : size_t(raw.h);
    if (raw.w <= 0 || raw.h <= 0 || raw.bytes.size() < row * rows) { why = "the copied texture is incomplete"; return false; }
    rb.w = raw.w;
    rb.h = raw.h;
    rb.px.clear();
    rb.la.clear();
    const size_t n = size_t(raw.w) * size_t(raw.h);
    if (raw.dxt3) {
        rb.px.assign(n, 0);
        if (!decodeDxt3(raw.bytes.data(), raw.w, raw.h, int(row), rb.px.data())) { why = "the copied DXT3 texture could not be decoded"; return false; }
        rb.compactIfGrey();
    } else {
        // A grey A8R8G8B8 texture (font high is) goes straight to 2 bytes a texel.
        const uint32_t* src = reinterpret_cast<const uint32_t*>(raw.bytes.data());
        bool grey = true;
        for (size_t i = 0; i < n && grey; i++) {
            const uint32_t p = src[i];
            grey = ((p >> 16) & 255) == ((p >> 8) & 255) && ((p >> 8) & 255) == (p & 255);
        }
        if (grey) {
            rb.la.resize(n);
            for (size_t i = 0; i < n; i++) rb.la[i] = uint16_t(((src[i] >> 24) << 8) | (src[i] & 255));
        } else {
            rb.px.resize(n);
            std::memcpy(rb.px.data(), src, n * 4);
        }
    }
    if (!rb.valid()) { why = fmt("%dx%d is not a 64 x 128 grid", rb.w, rb.h); return false; }
    rb.ink = countInk(rb);
    rb.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// The retry the panel's warnings offer.
inline constexpr const char* kRetryText = ". Untick and tick TrueFont, or type /tfont on, to try again.";
// The panel's warning for a Chat and menus refusal. `restart`: only a game restart clears it.
inline std::string chatDownWarning(bool sideOff, const std::string& failure, bool restart) {
    return std::string(sideOff ? "TrueFont" : "Chat/Items") + " could not turn on: " + failure + (restart ? "." : kRetryText);
}

class Installer {
public:
    enum class Phase { Off, Waiting, Building, Installed };

    Installer() = default;
    Installer(const Installer&) = delete;
    Installer& operator=(const Installer&) = delete;

    void setClient(const Module& client, const Resolved& R, std::shared_ptr<const CharMap> map, std::vector<int8_t> widths, std::wstring datPath) {
        client_ = client;
        R_ = R;
        map_ = std::move(map);
        widths_ = std::move(widths);
        datPath_ = std::move(datPath);
        std::vector<uint8_t> head(0x1000);
        if (readRaw(client_.base, head.data(), head.size())) peSection(head.data(), head.size(), ".rdata", rdataLo_, rdataHi_);
        info(fmt("installer: FFXiMain .rdata %06X..%06X%s; width table %zu bytes; fallback atlas %s", rdataLo_, rdataHi_, rdataHi_ ? "" : " (NOT FOUND: the record check will refuse)",
                 widths_.size(), datPath_.empty() ? "(unknown)" : toUtf8(datPath_).c_str()));
    }
    void setDevice(IDirect3DDevice8* d) { device_ = d; }
    // Require the resolved CYyTex vtable when available; otherwise accept a vtable in .rdata.
    void setRecordVtable(uint32_t rva) {
        recVtable_ = rva;
        info(rva ? fmt("installer: the moji and high records must carry CYyTex's vtable %06X", rva) : std::string("installer: CYyTex's vtable is not known: a record's vtable must lie in .rdata"));
    }
    // New options: a rebuild after a short quiet time (a running one restarts). True when one was scheduled.
    bool setOptions(const AtlasOptions& o) {
        // Compared at the cell size actually built (under font high and the size rule a Sharpness change may build nothing new).
        AtlasOptions a = o, b = opts_;
        a.cell = cellFor(o.cell);
        b.cell = cellFor(opts_.cell);
        const bool same = sameOptions(a, b);
        opts_ = o;
        if (same) return false;
        growTried_ = 0;   // a new Sharpness or font may fit where the last one did not
        growPending_ = quietNext_ = false;   // the rebuild due (if any) is this change's
        // A refusal is worth another try with other options: the main loop turns it on again.
        if (!failure_.empty()) { info("options changed: the refusal is retried"); clearLatches(); return false; }
        if (enabled_ && (phase_ == Phase::Installed || phase_ == Phase::Building)) {
            rebuildAt_ = nowMs_ + kRebuildQuietMs;
            info(fmt("options changed: a rebuild in %llu ms", static_cast<unsigned long long>(kRebuildQuietMs)));
            prepareCopy();   // a build that copies the game's font (Keep original off) gets its buffer meanwhile
            return true;
        }
        return false;
    }
    // A discrete change (a box, a radio, a font) rebuilds on the next frame; a slider keeps the quiet time. True when a
    // rebuild was due.
    bool hurryRebuild() {
        if (!rebuildAt_) return false;
        if (rebuildAt_ > nowMs_) rebuildAt_ = nowMs_;
        return true;
    }
    // This frame copied the game's font (the menu fonts' first look avoids sharing such a frame).
    bool heavyThisFrame() const { return frame_ && heavyFrame_ == frame_; }
    // A refusal that logging in again may clear (not the card's limits or the game's records).
    bool failureRetriableAtLogin() const { return !failure_.empty() && !failurePersistent_; }
    // The cell size built (at least 32 while the font high record is active; the largest that fits).
    int effectiveCell() const { return cellFor(opts_.cell); }
    int cellFor(int want) const { return fitChat(budget(), want).cell; }
    // Current inputs for the shared Sharpness calculation.
    ChatBudget budget() const {
        ChatBudget b;
        b.maxW = maxTexW_;
        b.maxH = maxTexH_;
        b.nonPow2 = nonPow2_;
        b.plain = formats_.expected(false);
        b.packed = formats_.expected(true);
        b.compress = opts_.compress;
        b.high = highActive_;
        b.readback = originalBytes();
        b.cells = shapeCells();
        b.latinCells = shapeCells(false);
        b.others = claim_ ? otherFloor_ : otherBytes_;   // sized first after an on: against their floor
        b.cap = cellCap_;
        b.capAddress = capWhy_.find("address space") != std::string::npos;
        return b;
    }
    // Account for sprite textures and in-flight allocations. otherFloor is their minimum after shrinking.
    void setOtherBytes(uint64_t b, bool otherBuilding = false, uint64_t otherPending = 0, uint64_t otherFloor = UINT64_MAX) {
        otherBytes_ = b;
        otherBuilding_ = otherBuilding;
        otherPending_ = otherBuilding ? otherPending : 0;
        otherFloor_ = (std::min)(otherFloor, b);
    }
    // The first look after an on is sized first: against the menu and HUD fonts' floor, not their bytes now.
    bool claiming() const { return claim_; }
    // Budget coverage for every allowed glyph; before the map is ready, use conservative script counts.
    uint64_t shapeCells() const { return shapeCells(opts_.allScripts); }
    uint64_t shapeCells(bool all) const {
        if (!map_) return all ? uint64_t(kGridCells) : 256u;
        const int i = all ? 1 : 0;
        if (allowedMap_ != map_.get()) {
            allowedMap_ = map_.get();
            allowedCells_[0] = allowedCells_[1] = -1;
        }
        if (allowedCells_[i] < 0) {
            int n = 0;
            for (int g = 0; g < kGridCells; g++) n += map_->allowed(g, i == 1) ? 1 : 0;
            allowedCells_[i] = n;
        }
        return uint64_t(allowedCells_[i]);
    }
    // What a build at `cell` holds at its peak now, the menu and HUD fonts included (the chat line).
    ChatPeak peakAt(int cell) const { return chatPeak(cell, formats_.expected(opts_.compress), originalBytes(), shapeCells(), otherBytes_); }
    // Reserve the chat rebuild peak from the shared sprite budget.
    uint64_t budgetBytes() const { return peakAt(effectiveCell()).own(); }
    bool highActive() const { return highActive_; }
    const AtlasStats& stats() const { return stats_; }
    int builtCell() const { return builtCell_; }
    bool building() const { return building_ || upload_; }
    bool uploading() const { return upload_ != nullptr; }   // a finished build's texture is being filled
    bool rebuildPending() const { return rebuildAt_ != 0 || highCopyPending_; }   // a rebuild is due (its quiet time, its copy's buffer, font high's copy)
    int progress() const { return worker_.progress(); }
    unsigned swaps() const { return swaps_; }                 // installs and rebuild swaps so far (the DEV self-check waits on it)
    const std::string& diagT1() const { return t1_; }
    const std::string& diagT2() const { return t2_; }
    const std::string& diagT12() const { return t12_; }
    const std::string& readbackSource() const { return readbackSource_; }
    std::shared_ptr<const Readback> readback() const { return rb_; }
    unsigned reads() const { return reads_; }                 // readbacks adopted since load (texture or DAT)
    // The Identity build's texture compared byte for byte with the readback's cells in its format (DEV).
    const std::string& identityTextureCheck() const { return identityCheck_; }
    D3DSURFACE_DESC originalDesc() const { return origDesc_; }
    const char* poolText() const { return oursPool_; }
    TexFormat textureFormat() const { return oursFormat_; }   // the installed texture's (valid while textureBytes() > 0)
    const IDirect3DTexture8* texture() const { return ours_; }
    int textureWidth() const { return ours_ ? oursW_ : 0; }
    int textureHeight() const { return ours_ ? oursH_ : 0; }
    const FormatSupport& formats() const { return formats_; }
    uint64_t textureBytes() const { return ours_ ? uint64_t(texBytes(oursFormat_, oursW_, oursH_)) : 0; }
    // The readback's bytes: the kept one's, else the last one's, else the usual (8 MB, or 16 MB for the grey high font).
    uint64_t originalBytes() const { return rb_ ? uint64_t(rb_->bytes()) : rbBytes_ ? rbBytes_ : highActive_ ? 16ull << 20 : 8ull << 20; }
    // The copy of the game's font held now: the readback, and a copy waiting for the next build to decode.
    uint64_t keptBytes() const { return (rb_ ? uint64_t(rb_->bytes()) : 0) + (raw_ ? uint64_t(raw_->bytes.size()) : 0); }
    uint64_t shapesBytes() const { return shapes_ ? uint64_t(shapes_->bytes()) : 0; }
    // Enabling Keep Original copies immediately; failure leaves the setting unchanged.
    // Disabling releases the readback once the worker is finished. Skip copying if chat is also turning off.
    bool setKeepOriginal(bool keep, bool copyNow = true) {
        if (keep == keepOriginal_) return true;
        keepOriginal_ = keep;
        if (!keep && phase_ != Phase::Building) { dropReadback(); return true; }
        if (!keep || !copyNow || phase_ != Phase::Installed || rb_ || raw_) return true;
        const bool needRead = needRead_;
        const std::string rbWhy = readbackWhy_;
        if (copyOriginal("Keep Original, ticked on", true) || !copyOutOfMemory_) return true;
        keepOriginal_ = false;   // as it was before the tick: the next build copies the game's font again
        needRead_ = needRead;
        readbackWhy_ = rbWhy;
        warn("readback: Keep Original stays off (the next build reads the game's font as before): there was not enough memory to copy it");
        return false;
    }
    bool keepOriginal() const { return keepOriginal_; }

    Phase phase() const { return phase_; }
    bool enabled() const { return enabled_; }
    bool installed() const { return phase_ == Phase::Installed; }
    bool autoOff() const { return !autoOffReason_.empty(); }
    const std::string& autoOffReason() const { return autoOffReason_; }
    const std::string& failure() const { return failure_; }
    // The refusal lasts until the game restarts: no retry can clear it.
    bool failureNeedsRestart() const { return failureRestart_; }
    // /tfont on has nothing to do here: on, no refusal, and no size cap holding the Sharpness down.
    bool onAsAsked() const { return enabled_ && failure_.empty() && !capLimits(); }
    // cellCap_ keeps the cell below the one the rule would give without it.
    bool capLimits() const {
        if (!cellCap_) return false;
        ChatBudget b = budget();
        b.cap = 0;
        return fitChat(b, opts_.cell).cell > cellCap_;
    }
    bool showingOriginal() const { return showOriginal_ || (originalAtInstall_ && enabled_ && phase_ != Phase::Installed); }   // asked before the install counts
    int reapplies() const { return keep_.total(); }
    bool gameGone() const { return gameGone_; }

    // Clears the automatic off and a refusal without turning on; rejected records are validated afresh.
    void clearLatches() {
        if (!autoOffReason_.empty()) info("clears the automatic off: " + autoOffReason_);
        if (!failure_.empty()) info("clears the earlier refusal: " + failure_);
        autoOffReason_.clear();
        failure_.clear();
        keep_.clearWindow();
        highRejected_ = mojiRejected_ = 0;
        if (phase_ == Phase::Off) enabled_ = false;   // a refusal left it enabled but Off: the main loop turns it on again
    }
    // Announce the next manual rebuild in chat.
    void announceNextRebuild() { announceRebuild_ = true; }
    // Retry installation and clear failure latches; announce also repeats prior refusals.
    void turnOn(bool announce = true) {
        if (!autoOffReason_.empty()) info("on: clears the automatic off: " + autoOffReason_);
        if (!failure_.empty()) info("on: clears the earlier refusal: " + failure_);
        autoOffReason_.clear();
        failure_.clear();
        keep_.clearWindow();
        highRejected_ = mojiRejected_ = 0;   // a requested retry validates the records afresh
        bool capped = false;
        if (announce) {
            lastSaidFailure_.clear();
            capped = capLimits();
            if (cellCap_) info(fmt("on: the %d px cap on the cell size is cleared (I16): the size Sharpness asks for is tried again", cellCap_));
            cellCap_ = 0;
            capWhy_.clear();
            cellSaid_.clear();
            growTried_ = 0;
        }
        enabled_ = true;
        announce_ = announce;
        if (phase_ == Phase::Off) {
            phase_ = Phase::Waiting;
            waitLogged_ = 0;
            claim_ = true;   // sized first; the menu and HUD fonts step down for it
            claimHoldSince_ = 0;
        } else if (capped && phase_ == Phase::Installed && !gameGone_) {   // the cap held the size down: rebuilt now, the outcome said
            rebuildAt_ = nowMs_;
            growPending_ = quietNext_ = false;
            announceRebuild_ = true;
            prepareCopy();
            info(fmt("on: rebuilding at %d px cells (the size Sharpness asks for)", effectiveCell()));
        }
    }
    // /tfont off (and the automatic off): the full restore, on the render thread.
    OffReport turnOff(const char* why) {
        info(std::string("off: ") + why);
        enabled_ = false;
        announce_ = false;
        announceRebuild_ = false;
        memSettleAt_ = 0;           // the 1 s sample would measure the off, not the texture
        claim_ = false;
        growPending_ = quietNext_ = false;
        ++gen_;                     // a build that outlives the stop is never installed
        stopWorker(2000);
        cancelUpload("TrueFont is turned off", true);
        // Font high's copy still waiting for its buffer is made now and kept for the next on.
        if (highCopyPending_ && keepOriginal_ && !raw_ && !gameGone_) copyHigh();
        highCopyPending_ = false;
        OffReport rep = restoreAll(true);
        phase_ = Phase::Off;
        showOriginal_ = false;
        originalAtInstall_ = false;
        offFrees();
        shapes_.reset();
        prep_.discard();
        return rep;
    }
    // Turning Chat off releases the readback regardless of Keep Original. Stop the worker first.
    void freeOriginal() {
        if (phase_ == Phase::Building) return;
        if (raw_) info(fmt("readback: a waiting copy freed (%s): Chat/Items is switched off", mbText(raw_->bytes.size()).c_str()));
        raw_.reset();
        dropReadback("Chat/Items is switched off");
        prep_.discard();
        shapes_.reset();
    }
    // DEV: every record TrueFont wrote holds its original again (after an off); `detail` for the log.
    bool originalsBack(std::string& detail) const {
        bool ok = true;
        int n = 0;
        for (const Slot* s : {&moji_, &high_}) {
            if (!s->rec || !s->original) continue;
            ++n;
            const uint32_t now = rd<uint32_t>(s->rec + kTexPrimary);
            detail += fmt("%s%s record %08X +40 %08X (its original %08X)", detail.empty() ? "" : "; ", s->name, unsigned(s->rec), now, unsigned(s->original));
            ok = ok && !s->installed && now == s->original;
        }
        if (!n) detail = "no record was written";
        return ok && n > 0;
    }
    bool rebuild(std::string& why) {
        const bool ok = rebuildNow(why);
        if (!ok) announceRebuild_ = false;
        return ok;
    }
    bool rebuildNow(std::string& why) {
        if (!enabled_) { why = "TrueFont is off"; return false; }
        if (gameGone_) { why = "the game is closing"; return false; }
        if (phase_ != Phase::Installed) { why = phase_ == Phase::Building ? "the first build is still running" : "it is not installed yet"; return false; }
        if (building_) { stopWorker(2000); info("rebuild: the running build is cancelled and started again"); }
        cancelUpload("a newer build replaces it", true);
        growPending_ = quietNext_ = false;   // asked for: its outcome is said
        // A build that copies the game's font first (Keep Original off) starts once its buffer is made (tick).
        if (waitForCopyBuffer()) {
            rebuildAt_ = nowMs_;
            return true;
        }
        return startBuild(why);
    }
    bool setShowOriginal(bool on, std::string& why) {
        if (enabled_ && (phase_ == Phase::Waiting || phase_ == Phase::Building)) {   // the install applies it
            if (on != originalAtInstall_) info(on ? "show original: kept until the font is in place" : "show original off: TrueFont's font goes in place when built");
            originalAtInstall_ = on;
            return true;
        }
        if (on == showOriginal_) {
            if (!on && phase_ != Phase::Installed) { why = "TrueFont's font is not installed"; return false; }
            return true;
        }
        if (phase_ != Phase::Installed || !ours_) { why = "TrueFont's font is not installed"; return false; }
        if (on) {
            const OffReport rep = restoreSlots(true);
            showOriginal_ = true;
            info("show original: " + (rep.clean() ? std::string("the game's own texture is back; TrueFont's stays loaded") : "left: " + rep.leftText()));
            if (!rep.clean()) warn("show original: switched, but " + rep.leftText());   // the state did switch
            return true;
        }
        const uintptr_t renderer = liveRenderer();
        if (!renderer) { why = "the game's text renderer is gone"; return false; }
        std::string w;
        if (!installSlot(moji_, rd<uint32_t>(renderer + kRendMoji), kMojiName, w)) { why = w; return false; }   // still showing the original
        showOriginal_ = false;
        installHighIfPresent(renderer);
        info("show original off: TrueFont's texture is back");
        return true;
    }

    void tick(uint64_t frame, uint64_t nowMs, bool inGame) {
        frame_ = frame;
        nowMs_ = nowMs;
        prep_.poll();
        if (memSettleAt_ && nowMs >= memSettleAt_) memSettle();
        if (building_ && shapes_ && worker_.shapesRefused()) {   // the worker could not use the kept shapes: never both sets at once
            info(fmt("build: the kept glyph shapes (%s) are freed: the worker could not use them (%s), so it draws its own", mbText(shapesBytes()).c_str(),
                     worker_.shapesRefusedWhy()));
            shapes_.reset();
        }
        if (building_ && worker_.state() != AtlasWorker::Running) finishBuild();
        else if (upload_) continueUpload();   // the next slice of a finished build's texture
        if (gameGone_ || !enabled_) return;
        if (growPending_ && rebuildAt_ && nowMs >= rebuildAt_ && effectiveCell() <= builtCell_) {   // the room went again before it was due
            info(fmt("build: the growth to %d px cells is dropped: the menu and HUD fonts hold that room again, so %d px is the size that fits (I17)", growTried_, builtCell_));
            rebuildAt_ = 0;
            growPending_ = quietNext_ = false;
            if (!highCopyPending_) prep_.discard();   // its copy's buffer, if one was made
        }
        if (rebuildAt_ && nowMs >= rebuildAt_ && !highCopyPending_ && !waitForCopyBuffer() && !holdForOthers()) {   // after font high's copy
            rebuildAt_ = 0;
            std::string why;
            if (phase_ == Phase::Building || phase_ == Phase::Installed) {
                if (building_) stopWorker(2000);
                if (!building_) cancelUpload("a newer build replaces it", true);
                if (building_) rebuildAt_ = nowMs + kRebuildQuietMs;   // the worker did not stop yet: retry, never drop the newest options
                else if (!startBuild(why)) {
                    if (phase_ == Phase::Building) { fail("the font could not be built", why); return; }
                    rebuildFailed("the font could not be built", why);
                }
            }
        }
        if (phase_ == Phase::Waiting) { if (!building_ && !upload_ && !holdForOthers()) begin(inGame); return; }
        if (phase_ == Phase::Installed) highCopyTick();
        if (phase_ == Phase::Installed) growTick(nowMs);
        if (phase_ == Phase::Installed && !showOriginal_) keep();
    }

    // Full: pointers back and ours released (render thread, outside a frame). DataOnly: pointers back only, no Direct3D.
    // `workerStopped` false: the caller pins the DLL and leaks this object.
    OffReport release(ReleaseMode mode, bool& workerStopped) {
        workerStopped = stopWorker(mode == ReleaseMode::Full ? 2000 : 500);
        // What the game had left when Release came (the renderer and texture cache globals).
        info(fmt("release: %s; worker %s; renderer %08X, texture cache %08X, installed %d", releaseModeText(mode), workerStopped ? "stopped" : "STILL RUNNING (the DLL is pinned)",
                 unsigned(liveRenderer()), unsigned(R_.texCache ? rd<uint32_t>(abs(R_.texCache)) : 0), phase_ == Phase::Installed));
        prep_.wait();   // the helper thread ends within milliseconds; nothing of it outlives the DLL
        prep_.discard();   // a prepared buffer is not left held (the object may be leaked on a pinned unload)
        claim_ = false;
        if (mode == ReleaseMode::Nothing) return OffReport{};
        cancelUpload("the plugin is unloading", mode == ReleaseMode::Full);
        OffReport rep = restoreAll(mode == ReleaseMode::Full);
        // An earlier off could not put a record's original back: that texture of TrueFont's may still be drawn.
        if (leftover_ && rep.clean()) rep.left.push_back("a record may still hold a texture of TrueFont's (an earlier off could not put the game's own back)");
        enabled_ = false;
        phase_ = Phase::Off;
        originalAtInstall_ = false;
        return rep;
    }
    bool leftover() const { return leftover_; }

    std::string statusLine() const {
        if (gameGone_) return "the game is closing; TrueFont touches nothing more.";
        if (!enabled_) return autoOff() ? "off (automatically: " + autoOffReason_ + "; /tfont on to try again)." : std::string("off.");
        if (!failure_.empty()) return "Chat/Items could not turn on: " + failure_ + (failureRestart_ ? "." : ". /tfont on to try again.");
        switch (phase_) {
        case Phase::Off: return "off.";
        case Phase::Waiting: return "on, waiting for the character to be in game and for the game's first text.";
        case Phase::Building: return "on, building the font...";
        case Phase::Installed: break;
        }
        const AtlasStats& s = stats_;
        const std::string jp = !opts_.allScripts ? std::string("Japanese Font Off") : opts_.jpFamily.empty() ? std::string("Japanese Font Same as Font") : "Japanese Font " + toUtf8(opts_.jpFamily);
        return fmt("on: %s %s%s, %d px cells, %s; %d characters from the font, %d kept native; re-applied %d times%s%s.", toUtf8(s.face).c_str(), weightName(opts_.weight), opts_.italic ? " italic" : "",
                   builtCell_, jp.c_str(), s.rendered(), s.kept(), keep_.total(), showOriginal_ ? "; showing the game's own font" : "", building_ ? "; rebuilding" : "");
    }
    void logStatus() const {
        info("-- status --");
        info(fmt("enabled %d, phase %d, building %d, show original %d, game gone %d; auto-off \"%s\"; failure \"%s\"", enabled_, int(phase_), building_, showOriginal_, gameGone_,
                 autoOffReason_.c_str(), failure_.c_str()));
        const uintptr_t renderer = liveRenderer();
        info(fmt("renderer %08X; moji record %08X (+40 %08X), high record %08X (+40 %08X); ours %08X (%s %s, %dx%d); readback %s%s", unsigned(renderer),
                 unsigned(renderer ? rd<uint32_t>(renderer + kRendMoji) : 0), unsigned(slotNow(moji_)), unsigned(currentHigh(renderer)),
                 unsigned(currentHigh(renderer) ? rd<uint32_t>(currentHigh(renderer) + kTexPrimary) : 0),
                 unsigned(uintptr_t(ours_)), texFormatName(oursFormat_), oursPool_, oursW_, oursH_, rb_ ? readbackSource_.c_str() : needRead_ ? "freed (Keep Original is off)" : "none yet",
                 keepOriginal_ ? "" : "; Keep Original off"));
        for (const Slot* s : {&moji_, &high_})
            info(fmt("slot %s: installed %d, record %08X, original %08X, installs %u", s->name, s->installed, unsigned(s->rec), unsigned(s->original), s->installs));
        info(fmt("keep: %d re-applies this session (chat said %d)", keep_.total(), keep_.chatSaid()));
    }

private:
    struct Slot {
        const char* name = "";
        uintptr_t rec = 0;        // the record TrueFont wrote into
        uintptr_t original = 0;   // what +40 held before (the value a restore writes back)
        bool installed = false;   // +40 was left holding ours
        bool originalRef = false; // TrueFont holds a reference on `original` (an adopted foreign one)
        unsigned installs = 0;
    };
    // A record is written only while it is still the record it was: vtable in .rdata and its name.
    bool sameRecord(uintptr_t rec, const char* name) const {
        if (rec < 0x10000) return false;
        const uintptr_t vt = rd<uint32_t>(rec);
        char nm[16] = {};
        return vtableOk(vt) && readRaw(rec + 0x0C, nm, 16) && std::memcmp(nm, name, 16) == 0;
    }
    // CYyTex's exact vtable when known, else any vtable inside FFXiMain .rdata.
    bool vtableOk(uintptr_t vt) const {
        if (recVtable_) return vt == client_.base + recVtable_;
        return rdataHi_ && vt >= client_.base + rdataLo_ && vt < client_.base + rdataHi_;
    }
    static const char* nameOf(const Slot& s) { return std::strcmp(s.name, "moji") == 0 ? kMojiName : kHighName; }
    bool isRetired(uintptr_t t) const { for (const uintptr_t r : retired_) if (r == t) return true; return false; }
    // The game is tearing down once the renderer global or the texture cache global is 0.
    bool gameAlive() const { return liveRenderer() != 0 && (!R_.texCache || rd<uint32_t>(abs(R_.texCache)) != 0); }

    static const char* weightName(int w) { return w >= 700 ? "Bold" : w >= 600 ? "Semibold" : "Regular"; }
    uintptr_t abs(uint32_t rva) const { return rva ? client_.base + rva : 0; }
    uintptr_t liveRenderer() const { return R_.renderer ? rd<uint32_t>(abs(R_.renderer)) : 0; }
    static uintptr_t currentHigh(uintptr_t renderer) {
        const uintptr_t alt = renderer ? rd<uint32_t>(renderer + kRendAlt) : 0;
        return alt ? rd<uint32_t>(alt + kAltRecord) : 0;
    }
    static uintptr_t slotNow(const Slot& s) { return s.rec ? rd<uint32_t>(s.rec + kTexPrimary) : 0; }
    uintptr_t oursAddr() const { return reinterpret_cast<uintptr_t>(ours_); }

    // Report each refusal once in chat. Persistent failures survive login; restart failures also reject manual retries.
    void fail(const std::string& outcome, const std::string& detail = std::string(), bool persistent = false, bool restart = false) {
        failure_ = outcome;
        memSettleAt_ = 0;
        claim_ = false;
        growPending_ = quietNext_ = false;
        failurePersistent_ = persistent;
        failureRestart_ = restart;
        announceRebuild_ = false;
        ++gen_;   // a build still running belongs to the refused attempt
        err("could not turn on: " + outcome + (detail.empty() ? std::string() : " (" + detail + ")"));
        if (outcome != lastSaidFailure_) {
            lastSaidFailure_ = outcome;
            chat("Chat/Items could not turn on: " + outcome + ". See the log.", 0x68);
        }
        stopWorker(2000);
        cancelUpload("the refusal", true);
        const OffReport rep = restoreAll(true);
        if (!rep.clean()) warn("after the refusal, left: " + rep.leftText());
        phase_ = Phase::Off;
        originalAtInstall_ = false;
        highCopyPending_ = false;
        offFrees();
        shapes_.reset();
        prep_.discard();
    }

    // A cell size below the one Sharpness asks for: the reason in the log each build, one chat line per outcome.
    void sayCellShortfall(int got) {
        const int want = highActive_ ? (std::max)(32, opts_.cell) : opts_.cell;
        if (got >= want) { cellSaid_.clear(); return; }
        std::string why = fitChat(budget(), want).why;
        if (why.empty()) why = capWhy_;
        warn(fmt("build: Chat/Items' Sharpness asks for %d px cells, built at %d px: %s (I16)", want, got, why.c_str()));
        if (buildQuiet_) return;   // Automatic growth logs its outcome without repeating chat warnings.
        const std::string outcome = fmt("%d>%d", want, got);
        if (outcome == cellSaid_) return;
        cellSaid_ = outcome;
        if (why.find("while they build") != std::string::npos && why.find("could not be made") == std::string::npos) {
            chat(fmt("Chat/Items Sharpness %s needs about %d MB; TrueFont's limit is %d MB, so it uses %s.", cellSharpText(want).c_str(), mbUp(peakAt(want).total()), mbUp(kMemoryLimit),
                     cellSharpText(got).c_str()),
                 0x68);
            return;
        }
        const char* reason = why.find("address space") != std::string::npos ? "the game has no free block of memory large enough for it"
                             : why.find("power-of-two") != std::string::npos ? "this card takes only power-of-two texture sizes"
                             : why.find("largest") != std::string::npos    ? "this card's largest texture is too small for it"
                             : why.find(" MB") != std::string::npos && why.find("could not be made") == std::string::npos ? "it would use too much memory"
                                                                                                                          : "the larger texture could not be made";
        chat(fmt("Chat/Items Sharpness %s can't be used here: %s, so it uses %s. See the log.", cellSharpText(want).c_str(), reason, cellSharpText(got).c_str()), 0x68);
    }
    // When budget frees up, retry a larger cell once after the quiet period. Reset the latch when it stops fitting.
    void growTick(uint64_t nowMs) {
        if (building_ || upload_ || rebuildAt_ || highCopyPending_ || otherBuilding_ || !failure_.empty() || !builtCell_ || gameGone_) return;
        const int want = effectiveCell();
        if (want <= builtCell_) { growTried_ = 0; return; }   // no larger size fits now
        if (want == growTried_) return;
        growTried_ = want;
        growPending_ = quietNext_ = true;
        rebuildAt_ = nowMs + kRebuildQuietMs;
        prepareCopy();
        info(fmt("build: %d px cells fit now (built at %d px while the other groups' fonts held more); a rebuild in %llu ms (I17)", want, builtCell_,
                 static_cast<unsigned long long>(kRebuildQuietMs)));
    }
    // Wait if simultaneous builds would exceed the budget. After enabling, allow kClaimWaitMs for sprites
    // to shrink to Chat's initial claim; on timeout, size Chat against their actual usage.
    bool holdForOthers() {
        if (claim_ && claimHoldSince_ && nowMs_ >= claimHoldSince_ + kClaimWaitMs) {
            claim_ = false;
            claimHoldSince_ = 0;
            holdSaid_ = false;
            warn(fmt("build: the menu and HUD fonts did not step down within %llu s: Chat/Items is sized beside them as they are (%d px cells, I19)",
                     static_cast<unsigned long long>(kClaimWaitMs / 1000), effectiveCell()));
        }
        const uint64_t own = peakAt(effectiveCell()).own(), with = own + otherBytes_ + otherPending_;
        const bool claimWait = claim_ && with > kMemoryLimit, buildWait = otherBuilding_ && with > kMemoryLimit;
        if (!claimWait) claimHoldSince_ = 0;
        else if (!claimHoldSince_) claimHoldSince_ = nowMs_ ? nowMs_ : 1;
        if (!claimWait && !buildWait) { holdSaid_ = false; return false; }
        if (!holdSaid_) {
            holdSaid_ = true;
            if (buildWait)
                info(fmt("build: waits for the menu and HUD fonts' build: its peak (%s) with their new textures beside their old ones (%s) would pass %s (I18)", mbText(own).c_str(),
                         mbText(otherBytes_ + otherPending_).c_str(), mbText(kMemoryLimit).c_str()));
            else
                info(fmt("build: sized first at %d px cells: waits for the menu and HUD fonts to step down (its peak %s with their %s would pass %s; I19)", effectiveCell(),
                         mbText(own).c_str(), mbText(otherBytes_).c_str(), mbText(kMemoryLimit).c_str()));
        }
        return true;
    }
    // Copy newly active font high after the helper buffer is ready, then rebuild at its effective cell size.
    void highCopyTick() {
        if (!highCopyPending_) return;
        if (!highActive_ || raw_) { highCopyPending_ = false; return; }
        const size_t need = nextCopyBytes();
        if (need && !prep_.readyFor(need) && prep_.request(need) && !prep_.readyFor(need)) {
            if (!highPrepSaid_) {
                highPrepSaid_ = true;
                info(fmt("keep: font high: its copy's buffer (%s) is made off the render thread first", mbText(need).c_str()));
            }
            return;
        }
        highCopyPending_ = false;
        copyHigh();
        if (builtCell_ != effectiveCell() || raw_) {
            rebuildAt_ = nowMs_ + 1;
            growPending_ = quietNext_ = false;
            info(fmt("keep: font high: its texture is copied; rebuilding at %d px cells from it (I11)", effectiveCell()));
        }
    }
    // Retry a failed allocation at the next smaller cell, never below 32 px while font high is active.
    bool stepDown(int cell, const std::string& why) {
        const int lower = nextCellDown(cell, nonPow2_);
        if (!enabled_ || gameGone_ || !lower || (highActive_ && lower < 32)) return false;
        cellCap_ = cellCap_ ? (std::min)(cellCap_, lower) : lower;
        capWhy_ = fmt("a %d px atlas could not be made (%s)", cell, why.c_str());
        warn("build: " + capWhy_ + fmt("; building at %d px instead (I16)", cellCap_));
        rebuildAt_ = nowMs_;
        growPending_ = false;
        quietNext_ = buildQuiet_;   // the step down after an automatic growth is as quiet as it
        return true;
    }

    void rebuildFailed(const std::string& outcome, const std::string& detail = std::string()) {
        announceRebuild_ = false;
        err("rebuild: " + outcome + (detail.empty() ? std::string() : " (" + detail + ")") + "; the installed font stays");
        if (buildQuiet_) return;   // an automatic growth's failure is the log's only
        if (outcome != lastSaidFailure_) {   // the same outcome again: the log only (as fail())
            lastSaidFailure_ = outcome;
            chat("the rebuild of Chat/Items failed: " + outcome + "; the current font stays. See the log.", 0x68);
        }
    }

    bool stopWorker(unsigned long ms) {
        if (!building_) return true;
        const bool ok = worker_.stop(ms);
        if (ok) {
            building_ = false;
            worker_.take();
            if (auto loaded = worker_.takeLoaded()) adoptLoadedReadback(std::move(loaded));
        }
        return ok;
    }

    // Validates a record: vtable inside FFXiMain .rdata, the 16-byte name, +40 a texture of a 64 x 128 grid.
    bool validate(uintptr_t rec, const char* name, D3DSURFACE_DESC& d, std::string& why) const {
        if (rec < 0x10000) { why = fmt("no record (%08X)", unsigned(rec)); return false; }
        const uintptr_t vt = rd<uint32_t>(rec);
        if (!vtableOk(vt)) {
            why = recVtable_ ? fmt("record %08X: its vtable %08X is not CYyTex's (%08X)", unsigned(rec), unsigned(vt), unsigned(client_.base + recVtable_))
                             : fmt("record %08X: its vtable %08X is not in FFXiMain .rdata", unsigned(rec), unsigned(vt));
            return false;
        }
        char nm[17] = {};
        if (!readRaw(rec + 0x0C, nm, 16) || std::memcmp(nm, name, 16) != 0) {
            why = fmt("record %08X: its name is \"%.16s\", not \"%.16s\"", unsigned(rec), nm, name);
            return false;
        }
        const uintptr_t t = rd<uint32_t>(rec + kTexPrimary);
        if (t < 0x10000) { why = fmt("record %08X: +40 holds no texture (%08X)", unsigned(rec), unsigned(t)); return false; }
        return describe(t, d, why);
    }
    // GetLevelDesc of a texture pointer read from game memory (SEH-guarded), and the grid check.
    bool describe(uintptr_t t, D3DSURFACE_DESC& d, std::string& why) const {
        auto* tex = reinterpret_cast<IDirect3DTexture8*>(t);
        HRESULT hr = E_FAIL;
        DWORD levels = 0;
        IDirect3DDevice8* dev = nullptr;
        d = D3DSURFACE_DESC{};
        if (!seh([&] { hr = tex->GetLevelDesc(0, &d); levels = tex->GetLevelCount(); if (tex->GetDevice(&dev) != D3D_OK) dev = nullptr; })) {
            why = fmt("texture %08X: GetLevelDesc faulted (not a texture)", unsigned(t));
            return false;
        }
        const bool sameDevice = dev == device_;
        if (dev) seh([&] { dev->Release(); });
        if (FAILED(hr)) { why = fmt("texture %08X: GetLevelDesc failed (0x%08X)", unsigned(t), unsigned(hr)); return false; }
        if (!sameDevice) warn(fmt("texture %08X: its device %08X is not the plugin's %08X (I8: logged, not refused)", unsigned(t), unsigned(uintptr_t(dev)), unsigned(uintptr_t(device_))));
        if (!gridCell(d.Width, d.Height)) { why = fmt("texture %08X is %ux%u, not a 64 x 128 grid of square cells", unsigned(t), d.Width, d.Height); return false; }
        lastLevels_ = levels;
        return true;
    }
    std::string descText(const D3DSURFACE_DESC& d) const {
        return fmt("%s %ux%u, %lu level(s), pool %s, usage %lu", formatName(d.Format).c_str(), d.Width, d.Height, lastLevels_, poolName(d.Pool), d.Usage);
    }

    // Once: what the game made and what the card offers.
    void logDiagnostics(uintptr_t renderer, uintptr_t moji, const D3DSURFACE_DESC& d) {
        const uintptr_t vt = rd<uint32_t>(moji);
        origDesc_ = d;
        t1_ = fmt("renderer [%06X] = %08X; +74 moji record %08X: vtable RVA %06X, name \"%.16s\", +24/+26 %ux%u, +2A %u; +40 %08X: %s", R_.renderer, unsigned(renderer),
                  unsigned(moji), unsigned(vt - client_.base), readName(moji).c_str(), rd<uint16_t>(moji + 0x24), rd<uint16_t>(moji + 0x26), rd<uint8_t>(moji + 0x2A),
                  rd<uint32_t>(moji + kTexPrimary), descText(d).c_str());
        info("T1: " + t1_);
        const uintptr_t alt = rd<uint32_t>(renderer + kRendAlt), high = currentHigh(renderer);
        std::string hd = "none";
        if (high) {
            D3DSURFACE_DESC hdsc{};
            std::string why;
            hd = fmt("%08X name \"%s\" +40 %08X: ", unsigned(high), readName(high).c_str(), rd<uint32_t>(high + kTexPrimary)) +
                 (describe(rd<uint32_t>(high + kTexPrimary), hdsc, why) ? descText(hdsc) : why);
        }
        t2_ = fmt("0036 config %s; renderer +80 = %u; +78 = %08X; high record %s",
                  R_.altConfig ? fmt("[%06X] = %d", R_.altConfig, int(rd<uint32_t>(abs(R_.altConfig)))).c_str() : "not resolved (S5 is a supporting signature)", rd<uint8_t>(renderer + kRendAltOn),
                  unsigned(alt), hd.c_str());
        info("T2: " + t2_);
        if (!device_) { warn("T12: no Direct3D device yet"); return; }
        D3DCAPS8 caps{};
        std::string formats;
        formats_ = probeFormats(device_, formats);
        const HRESULT h4 = device_->GetDeviceCaps(&caps);
        if (SUCCEEDED(h4)) {
            maxTexW_ = caps.MaxTextureWidth;
            maxTexH_ = caps.MaxTextureHeight;
            maxAspect_ = caps.MaxTextureAspectRatio;
            squareOnly_ = (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0;
            nonPow2_ = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) == 0;   // 48 px cells (3072 x 6144) only then
        }
        t12_ = fmt("CheckDeviceFormat %s; caps 0x%08X (%s): TextureCaps %08X POW2 %d SQUAREONLY %d NONPOW2CONDITIONAL %d, max texture %lux%lu, max aspect %lu", formats.c_str(),
                 unsigned(h4), SUCCEEDED(h4) ? "ok" : "FAILED", caps.TextureCaps,
                 (caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0, (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0, (caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0,
                 caps.MaxTextureWidth, caps.MaxTextureHeight, caps.MaxTextureAspectRatio);
        info("T12: " + t12_);
    }
    static std::string readName(uintptr_t rec) {
        char nm[17] = {};
        readRaw(rec + 0x0C, nm, 16);
        for (char& c : nm) if (c && (c < 0x20 || c > 0x7E)) c = '?';
        return nm;
    }

    // Waiting: the character in game, the renderer and +74 set; then validate, read back and start the build.
    void begin(bool inGame) {
        const uintptr_t renderer = inGame ? liveRenderer() : 0;
        const uintptr_t moji = renderer ? rd<uint32_t>(renderer + kRendMoji) : 0;
        const int stage = !inGame ? 1 : !renderer ? 2 : !moji ? 3 : 0;
        if (stage) {
            if (stage != waitLogged_) {
                waitLogged_ = stage;
                info(stage == 1 ? "waiting for the character to be in game" : stage == 2 ? "waiting for the game's text renderer" : "waiting for the game's first text draw (+74)");
            }
            return;
        }
        // The copy's buffer is made on a helper thread first (a frame or two), so the frame that copies only copies.
        if (waitForCopyBuffer()) return;
        D3DSURFACE_DESC d{};
        std::string why;
        if (!validate(moji, kMojiName, d, why)) { fail("the game's font record did not check out", why, true); return; }
        if (!diagnosed_) { diagnosed_ = true; logDiagnostics(renderer, moji, d); }
        noteHigh(renderer);
        // The font high texture while it is active (also after an off, when noteHigh has nothing new to note), else moji.
        if (!rb_ && !raw_) copyOriginal("the first build, or on again");
        if (!startBuild(why)) { fail("the font could not be built", why); return; }
        phase_ = Phase::Building;
    }

    // Active font high supplies native cells for the shared atlas, falling back to scaled moji.
    // Defer installed copies through highCopyTick to prepare the buffer off-thread.
    void noteHigh(uintptr_t renderer, bool defer = false) {
        const uintptr_t high = currentHigh(renderer);
        if (highActive_ || !high || rd<uint8_t>(renderer + kRendAltOn) != 1 || high == highRejected_) return;
        D3DSURFACE_DESC hd{};
        std::string why;
        if (!validate(high, kHighName, hd, why)) {
            highRejected_ = high;
            warn("font high: not used (" + why + ")");
            return;
        }
        highActive_ = true;
        if (defer) {
            if (!raw_ && hd.Width == 2048) highCopyPending_ = true;
            info(fmt("font high is active (renderer +80 = 1): the atlas is built at %d px cells (I11); its texture is copied once a buffer is made off the render thread",
                     effectiveCell()));
            return;
        }
        auto raw = std::make_shared<RawTexture>();
        std::string rbWhy;
        if (hd.Width == 2048 && copyTexture(rd<uint32_t>(high + kTexPrimary), hd, "the font high texture", *raw, rbWhy)) {
            raw_ = raw;   // the next build decodes it and replaces the moji readback
            info(fmt("font high is active (renderer +80 = 1): the atlas is built at %d px cells (I11); its native cells come from ", effectiveCell()) + raw_->source);
        } else {
            info(fmt("font high is active (renderer +80 = 1): the atlas is built at %d px cells (I11); its texture is not read back (", effectiveCell()) +
                 (rbWhy.empty() ? fmt("%ux%u", hd.Width, hd.Height) : rbWhy) + "), so the native cells come from the moji readback");
        }
    }

    // Predict the next copy size for buffer preparation; a wrong estimate falls back to direct allocation.
    size_t nextCopyBytes() const {
        const uintptr_t renderer = liveRenderer();
        if (!renderer) return 0;
        const uintptr_t high = currentHigh(renderer);
        const bool highOn = highActive_ || (high && high != highRejected_ && rd<uint8_t>(renderer + kRendAltOn) == 1);
        for (const bool h : {true, false}) {
            if (h && !highOn) continue;
            const uintptr_t t = h ? originalTexture(high_, high, kHighName) : originalTexture(moji_, rd<uint32_t>(renderer + kRendMoji), kMojiName);
            D3DSURFACE_DESC d{};
            HRESULT hr = E_FAIL;
            if (!t || !seh([&] { hr = reinterpret_cast<IDirect3DTexture8*>(t)->GetLevelDesc(0, &d); }) || FAILED(hr)) continue;
            if (h && d.Width != 2048) continue;
            const bool dxt3 = d.Format == D3DFMT_DXT3;
            if (d.Pool == D3DPOOL_DEFAULT || (!dxt3 && d.Format != D3DFMT_A8R8G8B8) || d.Width > 2048 || !gridCell(d.Width, d.Height)) continue;
            return dxt3 ? size_t(d.Width / 4) * 16 * size_t(d.Height / 4) : size_t(d.Width) * 4 * size_t(d.Height);
        }
        return 0;
    }
    // True while a copy the next step makes waits for its buffer (asked for now if it was not).
    bool waitForCopyBuffer() {
        if (rb_ || raw_ || (phase_ != Phase::Waiting && !needRead_)) return false;
        const size_t need = nextCopyBytes();
        if (!need || prep_.readyFor(need)) return false;
        if (!prep_.request(need)) return false;
        if (!prepSaid_) {
            prepSaid_ = true;
            info(fmt("readback: the copy's buffer (%s) is made off the render thread first", mbText(need).c_str()));
        }
        return !prep_.readyFor(need);
    }
    void prepareCopy() {
        if (rb_ || raw_ || !needRead_) return;
        if (const size_t need = nextCopyBytes()) prep_.request(need);
    }

    // The original's level 0 copied as it is (LockRect read-only, rows at its pitch, unlock); the worker decodes it.
    bool copyTexture(uintptr_t t, const D3DSURFACE_DESC& d, const char* what, RawTexture& raw, std::string& why) {
        if (d.Pool == D3DPOOL_DEFAULT) { why = "it is in D3DPOOL_DEFAULT, which cannot be locked"; return false; }
        const bool dxt3 = d.Format == D3DFMT_DXT3, argb = d.Format == D3DFMT_A8R8G8B8;
        if (!dxt3 && !argb) { why = "its format " + formatName(d.Format) + " is not one TrueFont decodes (DXT3, A8R8G8B8)"; return false; }
        // The DAT fallback is the stock 1024 x 2048, so it is refused for a wider moji too.
        if (d.Width > 2048) { why = fmt("it is %u wide, larger than 2048 (a font mod?)", d.Width); return false; }
        if (!gridCell(d.Width, d.Height) || (dxt3 && ((d.Width & 3) || (d.Height & 3)))) { why = fmt("%ux%u is not a 64 x 128 grid", d.Width, d.Height); return false; }
        const double q0 = qpcMs();
        heavyFrame_ = frame_;
        raw.dxt3 = dxt3;
        raw.w = int(d.Width);
        raw.h = int(d.Height);
        const size_t row = dxt3 ? size_t(raw.w / 4) * 16 : size_t(raw.w) * 4, rows = dxt3 ? size_t(raw.h / 4) : size_t(raw.h);
        // The helper's warm buffer; without one it is allocated here.
        raw.bytes = prep_.take(row * rows);
        const bool prepared = raw.bytes.size() == row * rows;
        if (!prepared) {
            try {
                raw.bytes.assign(row * rows, 0);   // up to 32 MiB in one piece (font high)
            } catch (const std::bad_alloc&) {
                why = "there was not enough memory for the copy (" + mbText(row * rows) + ")";
                copyOutOfMemory_ = true;
                return false;
            }
        }
        const double q1 = qpcMs();
        auto* tex = reinterpret_cast<IDirect3DTexture8*>(t);
        D3DLOCKED_RECT lr{};
        HRESULT hr = E_FAIL;
        if (!seh([&] { hr = tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY); })) { why = "LockRect faulted"; return false; }
        if (FAILED(hr) || !lr.pBits) {
            if (SUCCEEDED(hr)) seh([&] { tex->UnlockRect(0); });
            why = fmt("LockRect failed (0x%08X)", unsigned(hr));
            return false;
        }
        bool ok = size_t(lr.Pitch) >= row;
        const bool ran = !ok || seh([&] { copyRows(raw.bytes.data(), row, static_cast<const uint8_t*>(lr.pBits), size_t(lr.Pitch), row, rows); });
        seh([&] { tex->UnlockRect(0); });
        const double q2 = qpcMs();
        if (!ok || !ran) { why = !ok ? fmt("pitch %d < %zu", lr.Pitch, row) : "reading the locked data faulted"; raw.bytes.clear(); return false; }
        raw.source = std::string(what) + " (" + formatName(d.Format) + fmt(" %ux%u, LockRect read-only)", d.Width, d.Height);
        info(fmt("readback: %s copied on the render thread in %.2f ms (%s; %s %.2f ms, lock and copy %.2f ms); the worker decodes it", raw.source.c_str(), q2 - q0,
                 mbText(raw.bytes.size()).c_str(), prepared ? "its buffer made off the render thread, taken in" : "its buffer allocated here in", q1 - q0, q2 - q1));
        return true;
    }

    void setReadback(std::shared_ptr<const Readback> rb) {
        rb_ = std::move(rb);
        rbBytes_ = rb_ ? uint64_t(rb_->bytes()) : 0;
    }
    // Keep original off (or Chat and menus off), once TrueFont's texture is in place.
    void dropReadback(const char* why = "Keep Original is off") {
        if (!rb_) return;
        info(fmt("readback: freed (%s): %s; the next build reads the game's font again", why, mbText(rbBytes_).c_str()));
        rb_.reset();
        needRead_ = true;
    }
    // Preserve pending copies only with Keep Original enabled; the next build can still use them.
    void offFrees() {
        if (keepOriginal_) return;
        raw_.reset();
        dropReadback();
    }
    // Font high became active while TrueFont is in place: its texture copied for the next build (as noteHigh does).
    void copyHigh() {
        const uintptr_t t = originalTexture(high_, currentHigh(liveRenderer()), kHighName);
        D3DSURFACE_DESC d{};
        std::string w;
        auto raw = std::make_shared<RawTexture>();
        if (t && describe(t, d, w) && d.Width == 2048 && copyTexture(t, d, "the font high texture", *raw, w)) raw_ = raw;
        else info("keep: the font high texture is not copied (" + (w.empty() ? fmt("%ux%u", d.Width, d.Height) : w) + "); the native cells stay the moji readback's");
    }
    // Return the saved original while ours is installed, otherwise a native +40 value; zero if neither is usable.
    uintptr_t originalTexture(const Slot& s, uintptr_t rec, const char* name) const {
        if (!rec || !sameRecord(rec, name)) return 0;
        const uintptr_t now = rd<uint32_t>(rec + kTexPrimary);
        if (now == oursAddr() || isRetired(now)) return rec == s.rec ? s.original : 0;
        return now;
    }
    // Prefer active font high, otherwise moji. The build worker can fall back to DAT if both fail.
    // A Keep Original toggle cannot use that fallback; report allocation failure so the caller can undo the toggle.
    bool copyOriginal(const char* why, bool keepTick = false) {
        needRead_ = false;
        copyOutOfMemory_ = false;
        const uintptr_t renderer = liveRenderer();
        std::string w;
        for (const bool high : {true, false}) {
            if (high && !highActive_) continue;
            const uintptr_t t = high ? originalTexture(high_, currentHigh(renderer), kHighName)
                                     : originalTexture(moji_, renderer ? rd<uint32_t>(renderer + kRendMoji) : 0, kMojiName);
            D3DSURFACE_DESC d{};
            if (!t) { w = fmt("the %s record's own texture is not at hand", high ? "high" : "moji"); continue; }
            if (!describe(t, d, w) || (high && d.Width != 2048)) continue;
            std::shared_ptr<RawTexture> raw;
            try { raw = std::make_shared<RawTexture>(); } catch (const std::bad_alloc&) { w = "there was not enough memory for the copy"; copyOutOfMemory_ = true; continue; }
            if (!copyTexture(t, d, high ? "the font high texture" : "the texture", *raw, w)) continue;
            raw_ = raw;
            info(std::string("readback: for ") + why);
            return true;
        }
        readbackWhy_ = w;
        if (keepTick && copyOutOfMemory_) warn("readback: the texture cannot be copied (" + w + ")");
        else warn("readback: the texture cannot be read back (" + w + "); the worker reads the game's ROM\\272\\120.DAT instead (I1: the game's stock PNG copy, so the kept "
                  "cells show stock art; a font mod's look is not carried)");
        return false;
    }

    // The worker's readback (a decoded copy, or the DAT): it replaces the one held.
    void adoptLoadedReadback(std::shared_ptr<const Readback> loaded) {
        if (!loaded) return;
        setReadback(std::move(loaded));
        needRead_ = false;
        ++reads_;
        if (loadIsDat_) {
            char narrow[MAX_PATH * 2] = {};
            WideCharToMultiByte(CP_ACP, 0, datPath_.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
            readbackSource_ = std::string("the game's ") + narrow + fmt(" (the stock PNG copy: the kept cells show stock art, a font mod's look is not carried; %dx%d%s%s; the texture could not be read back: ",
                                                                        rb_->w, rb_->h, rb_->note.empty() ? "" : ", ", rb_->note.c_str()) + readbackWhy_ + ")";
        } else readbackSource_ = loadSource_;
        info(fmt("readback: from %s; %s on the worker in %.0f ms; %d of 8192 cells have ink", readbackSource_.c_str(), loadIsDat_ ? "read" : "decoded", rb_->ms, rb_->ink));
    }

    bool startBuild(std::string& why) {
        buildQuiet_ = quietNext_;   // an automatic growth's outcome goes to the log only
        quietNext_ = growPending_ = false;
        if (!map_ || widths_.size() < kWidthTableLen) { why = "the character map or the width table is missing"; return false; }
        memSettleAt_ = 0;   // the last texture's 1 s sample would measure this build's image
        if (!rb_ && !raw_ && needRead_) copyOriginal(keepOriginal_ ? "a build (no copy was held)" : "a build (Keep Original is off)");
        std::shared_ptr<const Readback> rb = rb_;
        AtlasWorker::ReadbackLoader load;
        if (raw_) {   // decoded on the worker; it replaces the readback held (font high noted after the install)
            rb = nullptr;
            // The readback it replaces goes now, not after the new one is decoded (never both at once).
            if (rb_) {
                info(fmt("readback: the held copy (%s) is freed before the new one is decoded", mbText(uint64_t(rb_->bytes())).c_str()));
                rb_.reset();
            }
            std::shared_ptr<const RawTexture> raw = std::move(raw_);
            raw_.reset();
            loadIsDat_ = false;
            loadSource_ = raw->source;
            load = [raw](Readback& r, std::string& w) mutable {
                const bool ok = decodeRaw(*raw, r, w);
                raw.reset();   // the copy goes as soon as it is decoded
                if (!ok) w = "the original texture could not be read back (" + w + ")";
                return ok;
            };
            needRead_ = true;   // until the decoded readback is adopted
        } else if (!rb_) {
            const std::wstring path = datPath_;
            const std::string rbWhy = readbackWhy_;
            const UINT ew = origDesc_.Width, eh = origDesc_.Height;   // the DAT must be the size of the moji texture it stands in for
            loadIsDat_ = true;
            load = [path, rbWhy, ew, eh](Readback& r, std::string& w) {
                const auto t0 = std::chrono::steady_clock::now();
                if (loadDatReadback(path, r, w, ew, eh)) {
                    r.ink = countInk(r);
                    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    return true;
                }
                w = "the original texture could not be read back (" + rbWhy + ") and the fallback could not be read (" + w + ")";
                return false;
            };
            needRead_ = true;
        }
        AtlasOptions o = opts_;
        o.cell = effectiveCell();
        if (claim_) info(fmt("build: sized first (I19): %d px cells beside the menu and HUD fonts' %s", o.cell, mbText(otherBytes_).c_str()));
        claim_ = false;   // from here the size rule counts their bytes as they are (they fit beside this size now)
        claimHoldSince_ = 0;
        const TexFormat f = uploadFormat();
        // Check contiguous address space before allocating large textures; require twice the texture size.
        while (o.cell >= 48) {
            const uint64_t tex = uint64_t(texBytes(f, kGridCols * o.cell, kGridRows * o.cell)), free = largestFreeFn ? largestFreeFn() : 0;
            if (free >= 2 * tex) break;
            const int lower = nextCellDown(o.cell, nonPow2_);
            if (!lower || (highActive_ && lower < 32)) break;
            cellCap_ = cellCap_ ? (std::min)(cellCap_, lower) : lower;
            capWhy_ = fmt("a %d px build needs a free block of %.0f MB of address space (twice its texture); the largest is %.0f MB", o.cell, double(2 * tex) / 1048576.0,
                          double(free) / 1048576.0);
            warn("build: " + capWhy_ + fmt("; building at %d px instead (I16)", lower));
            o.cell = lower;
        }
        sayCellShortfall(o.cell);
        // Kept shapes this build cannot re-use go before it draws new ones.
        if (shapes_ && (!sameShapeOptions(o, shapes_->o) || shapes_->fontGen != fontGate().generation())) {
            info(fmt("build: the kept glyph shapes (%s) are freed: this build draws its own", mbText(shapesBytes()).c_str()));
            shapes_.reset();
        }
        if (!worker_.start(o, map_, widths_, rb, std::move(load), f, shapes_)) { why = "the atlas worker could not start"; return false; }
        buildCell_ = o.cell;
        buildGen_ = gen_;
        building_ = true;
        buildStartMs_ = nowMs_;
        info(fmt("build: started (%s %s%s, %d px cells%s, %s%s%s, faux bold %d, gamma %.2f, engine %s, hinting %d; upload %s)", toUtf8(o.family).c_str(), weightName(o.weight),
                 o.italic ? " italic" : "", o.cell,
                 o.cell > opts_.cell ? " (font high is active, I11)" : o.cell < opts_.cell ? fmt(" (Sharpness asks for %d px, I16)", opts_.cell).c_str() : "", o.allScripts ? "Japanese Font on" : "Japanese Font Off",
                 o.allScripts && !o.jpFamily.empty() ? (" (kana and kanji " + toUtf8(o.jpFamily)).c_str() : "", o.allScripts && !o.jpFamily.empty() ? ")" : "", o.fauxBold,
                 o.gamma, engineName(o.engine), o.hinting, texFormatName(f)));
        memStart_ = memLog(fmt("build start (Chat/Items, %d px, %s)", o.cell, texFormatName(f)));
        return true;
    }
    // The format this build uploads in. A refused A8L8 or DXT3 is logged once since load.
    TexFormat uploadFormat() {
        if (opts_.mode == AtlasOptions::Mode::Coverage) return TexFormat::A8R8G8B8;   // DEV Coverage's magenta
        if (!formats_.checked && device_) {   // the first probe had no device yet
            std::string text;
            formats_ = probeFormats(device_, text);
            info("texture: CheckDeviceFormat " + text);
        }
        const TexFormat f = formats_.pick(opts_.compress);
        if (f == TexFormat::A8R8G8B8 && !formatSaid_) {
            formatSaid_ = true;
            warn(fmt("texture: CheckDeviceFormat %s A8L8, so the textures are A8R8G8B8 (4 bytes a pixel)", formats_.checked ? "refused" : "could not check"));
        }
        if (opts_.compress && f != TexFormat::Dxt3 && !compressSaid_) {
            compressSaid_ = true;
            warn(fmt("texture: Compress is on, but CheckDeviceFormat %s DXT3, so the textures are %s", formats_.checked ? "refused" : "could not check", texFormatName(f)));
        }
        return f;
    }

    // A build ended (render thread): create the texture and install it, or swap it for the old one.
    void finishBuild() {
        const AtlasWorker::State st = worker_.state();
        worker_.wait();
        building_ = false;
        std::string why;
        std::unique_ptr<Atlas> a = worker_.take(&why);
        const bool outOfMemory = worker_.outOfMemory();
        adoptLoadedReadback(worker_.takeLoaded());
        if (buildGen_ != gen_) { info("build: discarded (it was started before an off or a refusal)"); return; }
        const bool rebuilding = phase_ == Phase::Installed;   // a failed rebuild keeps the installed font
        if (!a) {
            if (st == AtlasWorker::Cancelled) { info("build: cancelled"); return; }
            if (!enabled_ || gameGone_) return;
            const bool unreadable = why.rfind("the original texture could not be read back", 0) == 0;
            const bool modSize = unreadable && why.find("larger than 2048 (a font mod?)") != std::string::npos;
            const char* outcome = modSize ? "the game's font is not the stock size (a font mod?)" : unreadable ? "the game's own font could not be read" : "the font could not be built";
            // Only running out of memory steps down; a GDI or build error is reported as it is and caps nothing.
            if (!unreadable && outOfMemory && buildCell_ > 16 && stepDown(buildCell_, why)) return;
            if (rebuilding) { rebuildFailed(outcome, why); return; }
            fail(outcome, why, unreadable);
            return;
        }
        info(fmt("build: %s, %s (em %d px, 'H' %d px%s), %d rendered, %d kept native (fixed %d, unmapped %d, other scripts %d, native empty %d, not in the font %d, blank %d, "
                 "engine error %d); condensed %d, squeezed %d, descenders %d, clipped %d; %.0f ms on the worker (%s), converted to %s in %.0f ms; %llu ms since the start",
                 toUtf8(a->stats.face).c_str(), engineName(a->stats.engine), a->stats.emPx, a->stats.capPx, a->stats.substituted ? ", SUBSTITUTED by GDI" : "", a->stats.rendered(),
                 a->stats.kept(),
                 a->stats.count[int(CellOutcome::KeptFixed)], a->stats.count[int(CellOutcome::KeptUnmapped)], a->stats.count[int(CellOutcome::KeptScript)],
                 a->stats.count[int(CellOutcome::KeptEmpty)], a->stats.count[int(CellOutcome::KeptNoGlyph)], a->stats.count[int(CellOutcome::KeptBlank)],
                 a->stats.count[int(CellOutcome::KeptError)], a->stats.condensed, a->stats.squeezed, a->stats.descenders, a->stats.clipped, a->stats.ms,
                 a->stats.shapesReused ? "the kept shapes re-mapped, no glyph drawn: only Brightness or Compress changed" : fmt("%ld glyph rasters", a->stats.rasters).c_str(),
                 texFormatName(a->tex.format), a->tex.ms, static_cast<unsigned long long>(nowMs_ - buildStartMs_)));
        if (!a->stats.fullFace.empty())
            info(fmt("build: kana and kanji %s, %s (em %d px, reference ideograph %d px, baseline row %d)%s", toUtf8(a->stats.fullFace).c_str(), engineName(a->stats.fullEngine),
                     a->stats.fullEmPx, a->stats.fullRefPx, a->stats.fullBase, a->stats.fullSubstituted ? ", SUBSTITUTED by GDI" : ""));
        if (!a->stats.engineNote.empty()) {
            warn("build: engine: " + a->stats.engineNote);
            engineFallbackChat(opts_.engine, "Chat/Items", a->stats.engineFaulted);
        }
        if (!enabled_ || gameGone_) { info("build: discarded (TrueFont was turned off meanwhile)"); return; }
        startUpload(std::move(a), rebuilding);
    }

    // Upload across frames, bounded by kUploadSliceBytes and at least kUploadSlices slices.
    // Install only after every row is ready.
    static constexpr int kUploadSlices = 4;
    struct Upload {
        std::unique_ptr<Atlas> atlas;
        IDirect3DTexture8* tex = nullptr;   // the texture being filled (MANAGED), or the DEFAULT one
        IDirect3DTexture8* sys = nullptr;   // D3DPOOL_DEFAULT: the SYSTEMMEM texture being filled
        const char* pool = "MANAGED";
        int next = 0, frames = 0;
        bool rebuilding = false, triedDefault = false;
        double createMs = 0, worstMs = 0, fillMs = 0;
        uint64_t startFrame = 0;
        MemSample mem0;                     // before CreateTexture (the memory samples)
    };
    void startUpload(std::unique_ptr<Atlas> a, bool rebuilding) {
        if (upload_) cancelUpload("a newer build replaces it (it was still pending)", true);   // never overwrite one
        auto u = std::make_unique<Upload>();
        u->atlas = std::move(a);
        u->rebuilding = rebuilding;
        u->startFrame = frame_;
        std::string why;
        if (!createTexture(*u, false, why)) {
            if (stepDown(u->atlas->cell, why)) return;   // a smaller atlas next
            if (rebuilding) rebuildFailed("the font texture could not be created", why);
            else fail("the font texture could not be created", why, capsRefused_);
            return;
        }
        upload_ = std::move(u);
        continueUpload();
    }
    // DataOnly release retains unpublished upload textures to avoid off-thread Direct3D calls.
    void cancelUpload(const char* why, bool d3d) {
        if (!upload_) return;
        if (d3d) {
            if (upload_->sys) upload_->sys->Release();
            if (upload_->tex) upload_->tex->Release();
        }
        info(fmt("texture: the unfinished upload (%d of %d rows) is dropped: %s%s", upload_->next, upload_->atlas ? upload_->atlas->tex.rows() : 0, why,
                 d3d ? "" : " (its texture is left: no Direct3D call on this path)"));
        upload_.reset();
    }
    void continueUpload() {
        if (gameGone_) { cancelUpload("the game is closing (I9)", false); return; }
        Upload& u = *upload_;
        const TexImage& img = u.atlas->tex;
        const int rows = img.rows(), slices = uploadSlices(kUploadSlices, uint64_t(img.rowBytes()) * uint64_t(rows)), per = (rows + slices - 1) / slices, r1 = (std::min)(rows, u.next + per);
        std::string why;
        const double t0 = qpcMs();
        const bool ok = uploadRows(u.sys ? u.sys : u.tex, img, u.next, r1, why);
        const double ms = qpcMs() - t0;
        if (!ok) {
            // A MANAGED texture that cannot be filled is tried once as DEFAULT through SYSTEMMEM.
            info(fmt("texture: filling the %s texture failed at row %d (%s)", u.pool, u.next, why.c_str()));
            const bool retry = !u.sys && !u.triedDefault && u.next == 0;
            if (u.tex) u.tex->Release();
            if (u.sys) u.sys->Release();
            u.tex = u.sys = nullptr;
            std::string w2;
            if (retry && createTexture(u, true, w2)) return;   // the next frame fills the DEFAULT path's texture
            std::unique_ptr<Upload> gone = std::move(upload_);
            const std::string detail = retry ? w2 : "the upload failed: " + why;
            if (stepDown(gone->atlas->cell, detail)) return;
            if (gone->rebuilding) rebuildFailed("the font texture could not be created", detail);
            else fail("the font texture could not be created", detail, capsRefused_);
            return;
        }
        u.next = r1;
        ++u.frames;
        u.fillMs += ms;
        u.worstMs = (std::max)(u.worstMs, ms);
        if (u.next < rows) return;
        std::unique_ptr<Upload> done = std::move(upload_);
        IDirect3DTexture8* t = done->tex;
        if (done->sys) {
            const HRESULT hu = device_->UpdateTexture(done->sys, t);
            done->sys->Release();
            done->sys = nullptr;
            info(fmt("texture: UpdateTexture 0x%08X", unsigned(hu)));
            if (FAILED(hu)) {
                t->Release();
                const std::string detail = fmt("UpdateTexture 0x%08X", unsigned(hu));
                if (stepDown(done->atlas->cell, detail)) return;
                if (done->rebuilding) rebuildFailed("the font texture could not be created", detail);
                else fail("the font texture could not be created", detail, capsRefused_);
                return;
            }
        }
        // Sample allocation/upload usage now and again after the driver has drawn with the new texture.
        const MemSample memUp = memLog(fmt("after the upload (%s, %d slices)", done->pool, done->frames));
        const MemSample memC0 = done->mem0;
        const uint64_t memTex = uint64_t(texBytes(img.format, done->atlas->w, done->atlas->h)), memImage = uint64_t(img.words.capacity()) * 4u;
        const uint64_t memOld = phase_ == Phase::Installed ? textureBytes() : 0;
        const char* memPool = done->pool;
        const Atlas& a = *done->atlas;
        oursPool_ = done->pool;
        oursW_ = a.w;
        oursH_ = a.h;
        oursFormat_ = img.format;
        info(fmt("texture: %08X ready: CreateTexture %.2f ms; filled in %d slices over %llu frames, the longest %.2f ms, all %.2f ms (QPC, render thread)%s", unsigned(uintptr_t(t)),
                 done->createMs, done->frames, static_cast<unsigned long long>(frame_ - done->startFrame + 1), done->worstMs, done->fillMs,
                 std::strcmp(done->pool, "DEFAULT") == 0 ? " (D3DPOOL_DEFAULT: it would not survive a device Reset; the game never resets)" : ""));
        if (opts_.mode == AtlasOptions::Mode::Identity) identityCheck_ = checkIdentity(t, a.cell, a.tex.format);
        stats_ = a.stats;
        builtCell_ = a.cell;
        if (growTried_ && builtCell_ >= growTried_) growTried_ = 0;   // the growth is reached
        shapes_ = a.shapes;   // the next Brightness or Compress change re-maps these
        done.reset();         // the converted bytes are no longer needed (memory: one atlas + one readback)
        const double s0 = qpcMs();
        const bool swap = phase_ == Phase::Installed;
        if (swap) swapIn(t);
        else install(t);
        info(fmt("texture: %s took %.2f ms (QPC, render thread)", swap ? "the swap" : "the install", qpcMs() - s0));
        const MemSample memEnd = memLog(fmt("build end (Chat/Items, after the %s)", swap ? "swap" : "install"));
        bool byFree = false;
        const double taken = memTakenMB(memC0, memUp, &byFree);
        info(fmt("memory: texture %.1f MB, process address space %+.1f MB (%s; before CreateTexture to after the upload, by %s: %.2f copies of the texture; %+.1f MB "
                 "from the build's start to its end; the limit counts a MANAGED texture %s)",
                 mib(memTex), taken, memPool, byFree ? "the free address space" : "private bytes (a walk was capped)", memTex ? taken * 1048576.0 / double(memTex) : 0.0,
                 memTakenMB(memStart_, memEnd), kManagedCountText));
        memUploaded_ = memUp;
        memImage_ = memImage;
        memOld_ = memOld;
        memSettleAt_ = nowMs_ + kMemSettleMs;
        // A buffer prepared for a copy that did not happen is not kept, unless a rebuild already due may take it.
        if (!rebuildAt_ && !highCopyPending_) prep_.discard();
    }

    // Sample 1 s after installation to capture driver allocations on first draw.
    // The worker image and replaced texture have been released by then.
    static constexpr uint64_t kMemSettleMs = 1000;
    void memSettle() {
        memSettleAt_ = 0;
        const MemSample m = memLog("1 s after the new texture went in");
        bool byFree = false;
        const double taken = memTakenMB(memUploaded_, m, &byFree);
        info(fmt("memory: %+.1f MB of address space from after the upload to 1 s later (by %s; the worker's image, %.1f MB, %s freed meanwhile%s)", taken,
                 byFree ? "the free address space" : "private bytes (a walk was capped)", mib(memImage_),
                 memOld_ ? fmt("and the old texture, %.1f MB, were", mib(memOld_)).c_str() : "was", showOriginal_ ? "; Show Original is on, so it was not drawn" : ""));
    }

    // The converted atlas's format, MANAGED; else DEFAULT through a SYSTEMMEM texture (`defaultPool`: that path at once).
    // Creates only; the fill follows in slices.
    bool createTexture(Upload& u, bool defaultPool, std::string& why) {
        const Atlas& a = *u.atlas;
        if (!device_) { why = "no Direct3D device"; return false; }
        capsRefused_ = false;
        if (maxTexW_ && (unsigned(a.w) > maxTexW_ || unsigned(a.h) > maxTexH_)) {
            capsRefused_ = true;   // the card: retried only when the options change
            why = fmt("the card's largest texture is %lux%lu, the atlas is %dx%d", maxTexW_, maxTexH_, a.w, a.h);
            return false;
        }
        if (squareOnly_ || (maxAspect_ && maxAspect_ < 2)) {   // a 1:2 texture is impossible on this card
            capsRefused_ = true;
            why = fmt("the card cannot make a 1:2 texture (SQUAREONLY %d, max aspect %lu)", squareOnly_, maxAspect_);
            return false;
        }
        const TexImage& img = a.tex;
        if (!img.valid()) { why = "no converted atlas"; return false; }
        const D3DFORMAT df = d3dFormat(img.format);
        const char* fn = texFormatName(img.format);
        u.mem0 = memLog(fmt("before CreateTexture %dx%d %s (%.1f MB, Chat/Items)", a.w, a.h, fn, mib(texBytes(img.format, a.w, a.h))));
        const double t0 = qpcMs();
        IDirect3DTexture8* t = nullptr;
        HRESULT hm = S_OK;
        if (!defaultPool && (img.format != TexFormat::A8R8G8B8 || formats_.argb)) {
            hm = device_->CreateTexture(UINT(a.w), UINT(a.h), 1, 0, df, D3DPOOL_MANAGED, &t);
            info(fmt("texture: CreateTexture %dx%d %s MANAGED 1 level: 0x%08X", a.w, a.h, fn, unsigned(hm)));
            if (SUCCEEDED(hm) && t) {
                u.tex = t;
                u.pool = "MANAGED";
                u.createMs += qpcMs() - t0;
                memLog(fmt("after CreateTexture %dx%d %s MANAGED", a.w, a.h, fn));
                return true;
            }
            t = nullptr;
        } else if (!defaultPool) info("texture: CheckDeviceFormat refused A8R8G8B8 (or it was not checked): trying D3DPOOL_DEFAULT");
        u.triedDefault = true;
        IDirect3DTexture8* sys = nullptr;
        const HRESULT hd = device_->CreateTexture(UINT(a.w), UINT(a.h), 1, 0, df, D3DPOOL_DEFAULT, &t);
        const HRESULT hs = SUCCEEDED(hd) ? device_->CreateTexture(UINT(a.w), UINT(a.h), 1, 0, df, D3DPOOL_SYSTEMMEM, &sys) : E_FAIL;
        info(fmt("texture: %s DEFAULT create 0x%08X, SYSTEMMEM create 0x%08X", fn, unsigned(hd), unsigned(hs)));
        if (SUCCEEDED(hd) && t && SUCCEEDED(hs) && sys) {
            u.tex = t;
            u.sys = sys;
            u.pool = "DEFAULT";
            u.createMs += qpcMs() - t0;
            memLog(fmt("after CreateTexture %dx%d %s DEFAULT and SYSTEMMEM", a.w, a.h, fn));
            return true;
        }
        if (sys) sys->Release();
        if (t) t->Release();
        why = fmt("CreateTexture %s MANAGED 0x%08X, DEFAULT 0x%08X, SYSTEMMEM 0x%08X", fn, unsigned(hm), unsigned(hd), unsigned(hs));
        return false;
    }

    // DEV: compare uploaded Identity texels with the packed native readback on the render thread.
    std::string checkIdentity(IDirect3DTexture8* t, int cell, TexFormat f) const {
        if (!rb_) return "not checked: no readback";
        const auto t0 = std::chrono::steady_clock::now();
        Atlas e;
        e.cell = cell;
        e.w = kGridCols * cell;
        e.h = kGridRows * cell;
        e.px.assign(size_t(e.w) * size_t(e.h), 0);
        for (int g = 0; g < kGridCells; g++) copyNativeCell(*rb_, g, e);
        TexImage img;
        if (!toTexImage(e.px, e.w, e.h, f, img)) return "not checked: the readback could not be converted";
        D3DLOCKED_RECT lr{};
        const HRESULT hr = t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !lr.pBits) return fmt("not checked: LockRect 0x%08X (pool %s)", unsigned(hr), oursPool_);
        size_t differ = 0;
        int first = -1;
        for (int r = 0; r < img.rows(); r++)
            if (std::memcmp(static_cast<const uint8_t*>(lr.pBits) + size_t(r) * size_t(lr.Pitch), img.bytes() + size_t(r) * img.rowBytes(), img.rowBytes()) != 0) {
                if (first < 0) first = r;
                ++differ;
            }
        t->UnlockRect(0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const std::string c = differ ? fmt("FAIL: %zu of %d %s rows differ from the readback's cells (first %d)", differ, img.rows(), texFormatName(f), first)
                                     : fmt("PASS: all %d rows of the %s texture equal the readback's cells, byte for byte", img.rows(), texFormatName(f));
        info(fmt("identity: %s (%.0f ms)", c.c_str(), ms));
        return c;
    }

    // Writes ours into a record's +40 (verified; the record gets its own reference on ours). False with why.
    bool installSlot(Slot& s, uintptr_t rec, const char* name, std::string& why) {
        D3DSURFACE_DESC d{};
        if (!validate(rec, name, d, why)) return false;
        const uintptr_t now = rd<uint32_t>(rec + kTexPrimary);
        if (now == oursAddr()) {   // already ours: only with the original known for THIS record
            if (rec == s.rec && s.original) { s.installed = true; return true; }
            for (auto it = abandoned_.begin(); it != abandoned_.end(); ++it)
                if (it->rec == rec && it->original) {   // a record the renderer named before and names again
                    s = *it;
                    s.installed = true;
                    abandoned_.erase(it);
                    info(fmt("install: the %s record %08X still holds ours; its own original %08X is taken back", s.name, unsigned(rec), unsigned(s.original)));
                    return true;
                }
            why = fmt("record %08X already holds TrueFont's texture but its original is unknown; nothing written", unsigned(rec));
            return false;
        }
        if (isRetired(now)) { retiredRefused_ = true; why = fmt("record %08X holds an earlier TrueFont texture; nothing written", unsigned(rec)); return false; }
        ours_->AddRef();
        const WriteResult r = wrVerifiedWhy<uint32_t>(rec + kTexPrimary, uint32_t(oursAddr()), uint32_t(now));
        if (r != WriteResult::Ok) {
            ours_->Release();
            why = fmt("record %08X: writing +40 %s", unsigned(rec), writeResultText(r));
            return false;
        }
        if (s.originalRef && s.original) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });
        s.rec = rec;
        s.original = now;
        s.originalRef = false;
        s.installed = true;
        ++s.installs;
        info(fmt("install: %s record %08X +40 %08X -> ours %08X (the original was %s)", s.name, unsigned(rec), unsigned(now), unsigned(oursAddr()), descText(d).c_str()));
        return true;
    }
    void installHighIfPresent(uintptr_t renderer) {
        const uintptr_t high = currentHigh(renderer);
        if (!high || high == highRejected_ || high_.installed) return;
        std::string why;
        if (!installSlot(high_, high, kHighName, why)) {
            highRejected_ = high;
            warn("install: the font high record is not used: " + why + " (while 0036 is active the game draws from it, so TrueFont would not show)");
        }
    }

    void install(IDirect3DTexture8* t) {
        ours_ = t;
        const uintptr_t renderer = liveRenderer();
        std::string why;
        retiredRefused_ = false;
        if (!renderer || !installSlot(moji_, rd<uint32_t>(renderer + kRendMoji), kMojiName, why)) {
            // A record still holding an old texture of TrueFont's stays so until the game restarts: say so.
            if (renderer && retiredRefused_)
                fail("the game's font record still holds an old texture of TrueFont's; restart the game to use Chat/Items", why, true, true);
            else fail(renderer ? "the font texture could not be put in place" : "the game's text renderer is gone", why);
            return;
        }
        installHighIfPresent(renderer);
        // While the game draws from font high, a rejected high record means nothing changes: refuse.
        if (rd<uint8_t>(renderer + kRendAltOn) == 1 && currentHigh(renderer) && !high_.installed) {
            fail("the game's high-resolution font is in use and could not be replaced", fmt("high record %08X rejected", unsigned(currentHigh(renderer))), true);
            return;
        }
        phase_ = Phase::Installed;
        showOriginal_ = false;
        ++swaps_;
        info(fmt("installed: moji %s, high %s", moji_.installed ? "yes" : "no", high_.installed ? "yes" : "no"));
        if (originalAtInstall_) {   // back to the originals before the game draws a frame with ours
            originalAtInstall_ = false;
            const OffReport rep = restoreSlots(true);
            showOriginal_ = true;
            info("installed; showing the original (Show Original was on)");
            if (!rep.clean()) warn("show original: switched, but " + rep.leftText());
        }
        if (announce_ || !onSaid_ || announceRebuild_) {
            onSaid_ = true;
            chat(statusLine() + " /tfont off turns it off.");
        }
        announce_ = false;
        announceRebuild_ = false;
        lastSaidFailure_.clear();   // Allow a future refusal to be reported again.
        if (!keepOriginal_) dropReadback();
    }

    // Rebuild: the new texture into every record that holds the old one, then the old one released.
    void swapIn(IDirect3DTexture8* t) {
        IDirect3DTexture8* old = ours_;
        const uintptr_t oldAddr = oursAddr();
        ours_ = t;
        bool oldHeld = false;
        const uintptr_t renderer = liveRenderer();
        for (Slot* s : {&moji_, &high_}) {
            if (!s->installed || showOriginal_) continue;
            const uintptr_t rec = s == &moji_ ? (renderer ? rd<uint32_t>(renderer + kRendMoji) : 0) : currentHigh(renderer);
            if (rec != s->rec) { s->installed = false; info(fmt("rebuild: the %s record changed (%08X -> %08X); the keep check takes over", s->name, unsigned(s->rec), unsigned(rec))); continue; }
            uint32_t now = 0;
            if (!rdok(rec + kTexPrimary, now)) { oldHeld = true; warn(fmt("rebuild: the %s record's +40 is unreadable; the old texture is kept", s->name)); continue; }
            if (now != oldAddr) { info(fmt("rebuild: the %s record holds %08X, not the old texture; the keep check takes over", s->name, now)); continue; }
            t->AddRef();
            const WriteResult r = wrVerifiedWhy<uint32_t>(rec + kTexPrimary, uint32_t(uintptr_t(t)), now);
            if (r == WriteResult::Ok) {
                old->Release();   // the record's reference on the old texture
                info(fmt("rebuild: %s record %08X +40 %08X -> %08X", s->name, unsigned(rec), now, unsigned(uintptr_t(t))));
            } else {
                t->Release();
                oldHeld = true;
                warn(fmt("rebuild: the %s record's +40 was not written (%s); the old texture is kept", s->name, writeResultText(r)));
            }
        }
        if (!oldHeld) old->Release();   // its own reference: released only after the swap
        else {
            retired_.push_back(oldAddr);   // never adopted as an "original"
            warn("rebuild: a record may still hold the old texture, so it is not released (one texture leaks until the game closes)");
        }
        ++swaps_;
        lastSaidFailure_.clear();   // Allow a future failure to be reported again.
        info("rebuild: " + statusLine());
        if (announceRebuild_ && opts_.mode == AtlasOptions::Mode::Normal) chat("rebuilt: " + statusLine());
        announceRebuild_ = false;
        if (!keepOriginal_) dropReadback();
    }

    // The per-frame keep check: re-apply, adopt a foreign value, leave a record the renderer no longer names.
    void keep() {
        const uintptr_t renderer = liveRenderer();
        if (!renderer) {
            gameGone_ = true;
            warn("keep: the renderer global is 0 (game teardown); TrueFont touches nothing more (I9)");
            return;
        }
        // +80 is re-read while the high record is installed; a change rebuilds at the cell size that now applies.
        if (high_.installed) {
            const bool on = rd<uint8_t>(renderer + kRendAltOn) == 1;
            if (on != highActive_) {
                highActive_ = on;
                info(fmt("keep: renderer +80 is now %d: font high %s; rebuilding at %d px cells", on ? 1 : 0, on ? "active" : "no longer active", effectiveCell()));
                // Its copy waits for a buffer made off the render thread (highCopyTick); the rebuild after it.
                if (on && !raw_) highCopyPending_ = true;
                else if (builtCell_ != effectiveCell() || raw_) {
                    rebuildAt_ = nowMs_ + 1;
                    growPending_ = quietNext_ = false;
                }
            }
        }
        bool reapplied = false;
        for (Slot* s : {&moji_, &high_}) {
            const bool isMoji = s == &moji_;
            const uintptr_t rec = isMoji ? rd<uint32_t>(renderer + kRendMoji) : currentHigh(renderer);
            if (!s->installed) {
                if (isMoji) {
                    if (rec && rec != mojiRejected_) {
                        std::string why;
                        if (installSlot(moji_, rec, kMojiName, why)) reapplied = true;
                        else { mojiRejected_ = rec; warn("keep: the new moji record is not used: " + why); }
                    }
                } else if (rec && rec != highRejected_) {
                    const bool wasActive = highActive_;
                    noteHigh(renderer, true);   // the copy waits for its buffer (the saved original is read then)
                    std::string why;
                    if (installSlot(high_, rec, kHighName, why)) info("keep: the font high record appeared; TrueFont's texture is in it too");
                    else {
                        highRejected_ = rec;
                        warn("keep: the font high record is not used: " + why);
                        if (rd<uint8_t>(renderer + kRendAltOn) == 1) {
                            fail("the game's high-resolution font is in use and could not be replaced", why, true);
                            return;
                        }
                    }
                    if (!wasActive && highActive_ && !highCopyPending_ && (builtCell_ != effectiveCell() || raw_)) {   // a waiting copy is built at once too
                        rebuildAt_ = nowMs_ + 1;
                        growPending_ = quietNext_ = false;
                        info(fmt("keep: font high is active now: rebuilding at %d px cells from its texture (I11)", effectiveCell()));
                    }
                }
                continue;
            }
            if (rec != s->rec) {   // a record the renderer no longer names is never written again
                logKeep(fmt("keep: the renderer names another %s record (%08X -> %08X); the old one is not touched", s->name, unsigned(s->rec), unsigned(rec)));
                s->installed = false;
                if (abandoned_.size() < 8) abandoned_.push_back(*s);   // its original, should it come back
                s->original = 0;
                s->originalRef = false;
                std::string why;
                if (rec && installSlot(*s, rec, isMoji ? kMojiName : kHighName, why)) reapplied = true;
                else if (rec) { (isMoji ? mojiRejected_ : highRejected_) = rec; warn(std::string("keep: the new ") + s->name + " record is not used: " + why); }
                continue;
            }
            uint32_t now = 0;
            if (!rdok(rec + kTexPrimary, now)) { logKeep(fmt("keep: the %s record's +40 is unreadable", s->name)); continue; }
            if (now == oursAddr()) continue;
            if (!now) { logKeep(fmt("keep: the %s record's +40 is empty; nothing written", s->name)); continue; }
            if (!sameRecord(rec, nameOf(*s))) {   // freed and reused for another texture: never written again
                logKeep(fmt("keep: the %s record %08X is no longer that record (vtable or name changed); it is dropped", s->name, unsigned(rec)));
                s->installed = false;
                (isMoji ? mojiRejected_ : highRejected_) = rec;
                continue;
            }
            const bool retired = isRetired(now);
            if (!retired && now != s->original) {   // a foreign value: adopted, with a reference held on it
                logKeep(fmt("keep: the %s record's +40 holds %08X (neither ours nor the original %08X): it becomes the original", s->name, now, unsigned(s->original)));
                if (s->originalRef && s->original) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s->original)->Release(); });
                s->original = now;
                s->originalRef = seh([&] { reinterpret_cast<IDirect3DTexture8*>(uintptr_t(now))->AddRef(); });
            }
            ours_->AddRef();
            const WriteResult r = wrVerifiedWhy<uint32_t>(rec + kTexPrimary, uint32_t(oursAddr()), now);
            if (r == WriteResult::Ok) {
                reapplied = true;
                if (retired) reinterpret_cast<IDirect3DTexture8*>(uintptr_t(now))->Release();   // the record's reference on the retired one
                logKeep(fmt("keep: re-applied ours to the %s record (it held %08X%s)", s->name, now, retired ? ", an earlier TrueFont texture" : ""));
            } else {
                ours_->Release();
                logKeep(fmt("keep: re-applying to the %s record failed (%s)", s->name, writeResultText(r)));
            }
        }
        if (!reapplied) return;
        const KeepMonitor::Event e = keep_.reapplied(frame_, nowMs_);
        if (e.chat) chat(fmt("something else replaced a font texture %d times; TrueFont put it back. See the log.", e.total), 0x68);
        if (e.autoOff) {
            autoOffReason_ = fmt("something else replaced the font texture %d times within %d s", e.inWindow, int(KeepMonitor::kAutoOffWindowMs / 1000));
            err("keep: " + autoOffReason_ + "; turning TrueFont off until /tfont on");
            const OffReport rep = turnOff("the automatic off");
            chat("something else keeps replacing the font texture, so TrueFont turned itself off" + (rep.clean() ? std::string() : " (left: " + rep.leftText() + ")") +
                     ". /tfont on to try again.",
                 0x44);
        }
    }
    void logKeep(const std::string& s) {
        if (++keepLines_ <= 40 || keepLines_ % 500 == 0) info(s + (keepLines_ > 40 ? fmt(" [%u keep lines]", keepLines_) : std::string()));
    }

    // The originals back into every installed record; `d3d`: the records' references on ours are dropped. Ours stays.
    OffReport restoreSlots(bool d3d) {
        OffReport rep;
        const uintptr_t renderer = liveRenderer();
        if (!renderer && (moji_.installed || high_.installed)) rep.gameGone = true;
        for (Slot* s : {&moji_, &high_}) {
            if (!s->installed) continue;
            rep.wasOn = true;
            const uintptr_t rec0 = s == &moji_ ? (renderer ? rd<uint32_t>(renderer + kRendMoji) : 0) : currentHigh(renderer);
            const bool same = rec0 == s->rec && sameRecord(s->rec, nameOf(*s));
            uint32_t now = 0;
            const bool readOk = same && rdok(s->rec + kTexPrimary, now);
            // An earlier TrueFont texture still in the record is restored like ours.
            const uintptr_t mine = (readOk && now && isRetired(now)) ? uintptr_t(now) : oursAddr();
            const SlotRestore dcs = decideSlotRestore(renderer != 0, same, readOk, now, mine, s->original);
            bool wrote = false;
            WriteResult r = WriteResult::Ok;
            if (dcs == SlotRestore::WriteOriginal) {
                r = wrVerifiedWhy<uint32_t>(s->rec + kTexPrimary, uint32_t(s->original), now);
                wrote = r == WriteResult::Ok;
                if (wrote && d3d) reinterpret_cast<IDirect3DTexture8*>(mine)->Release();   // the record's reference
            }
            // The reference held on an adopted original: dropped once it is back (Direct3D path), else left.
            if (s->originalRef && d3d && (wrote || dcs == SlotRestore::AlreadyOriginal)) {
                seh([&] { reinterpret_cast<IDirect3DTexture8*>(s->original)->Release(); });
                s->originalRef = false;
            }
            const bool held = slotMayHoldOurs(dcs, wrote);
            // Clear ownership before formatting logs, which may allocate and throw.
            if (held) heldByRecord_ = true;
            s->installed = false;
            if (dcs == SlotRestore::WriteOriginal && !wrote) warn(fmt("restore: %s record %08X +40 not written back (%s)", s->name, unsigned(s->rec), writeResultText(r)));
            if (held) rep.left.push_back(fmt("the %s record still holds TrueFont's texture", s->name));
            info(fmt("restore: %s record %08X: %s%s", s->name, unsigned(s->rec), slotRestoreText(dcs), dcs == SlotRestore::WriteOriginal ? (wrote ? fmt(" (%08X)", unsigned(s->original)).c_str() : " (FAILED)") : ""));
        }
        return rep;
    }
    // Everything back, then ours released (with `d3d`, while no record may hold it, and not after teardown).
    OffReport restoreAll(bool d3d) {
        OffReport rep = showOriginal_ ? OffReport{} : restoreSlots(d3d);
        showOriginal_ = false;
        if (!gameAlive() && ours_) rep.gameGone = true;   // the texture cache global counts too
        if (!ours_) return rep;
        rep.wasOn = true;
        if (heldByRecord_ && rep.clean()) rep.left.push_back("a record may still hold TrueFont's texture (an earlier restore could not put the original back)");
        if (d3d && !heldByRecord_ && !rep.gameGone) {
            ours_->Release();
            info(fmt("restore: TrueFont's texture %08X released", unsigned(oursAddr())));
        } else {
            info(fmt("restore: TrueFont's texture %08X is kept (%s)", unsigned(oursAddr()),
                     !d3d ? "no Direct3D call on this path" : heldByRecord_ ? "a record may still hold it" : "the game is closing (I9)"));
        }
        // Retain textures that records may still reference; never adopt them as native on a later retry.
        if (heldByRecord_) {
            leftover_ = true;
            if (!isRetired(oursAddr())) retired_.push_back(oursAddr());
        }
        ours_ = nullptr;
        heldByRecord_ = false;   // a leaked texture stays where it is; the next install starts clean
        return rep;
    }


    Module client_;
    Resolved R_;
    std::shared_ptr<const CharMap> map_;
    mutable const CharMap* allowedMap_ = nullptr;   // shapeCells' count is for this map
    mutable int allowedCells_[2] = {-1, -1};        // ... Latin only, all scripts (-1: not counted)
    MemSample memStart_, memUploaded_;              // the memory samples: the build's start, the last upload's end
    uint64_t memImage_ = 0, memOld_ = 0;            // ... the image and the old texture freed after that upload
    uint64_t memSettleAt_ = 0;                      // ... when memSettle samples (0: none due)
    std::vector<int8_t> widths_;
    std::wstring datPath_;
    uint32_t rdataLo_ = 0, rdataHi_ = 0;
    uint32_t recVtable_ = 0;         // CYyTex's vtable RVA (setRecordVtable; 0: the .rdata check)
    IDirect3DDevice8* device_ = nullptr;
    AtlasOptions opts_;
    AtlasWorker worker_;
    std::shared_ptr<const Readback> rb_;
    uint64_t rbBytes_ = 0;           // the last readback's size (the panel's "Off saves N MB")
    bool keepOriginal_ = true;
    bool needRead_ = false;          // the next build reads the original again (it was freed, or its read did not land)
    bool copyOutOfMemory_ = false;   // copyOriginal's last try failed for want of memory (Keep original's tick is undone)
    std::shared_ptr<const RawTexture> raw_;   // copied, for the next build to decode
    BufferPrep prep_;                // the next copy's buffer, made off the render thread
    bool prepSaid_ = false;
    uint64_t heavyFrame_ = 0;        // the frame that last copied the game's font
    std::shared_ptr<const AtlasShapes> shapes_;   // the last build's shapes
    std::unique_ptr<Upload> upload_;              // a finished build's texture being filled
    bool loadIsDat_ = false;
    std::string loadSource_, identityCheck_;
    unsigned reads_ = 0;
    std::string readbackSource_, readbackWhy_;
    AtlasStats stats_;
    IDirect3DTexture8* ours_ = nullptr;
    const char* oursPool_ = "-";
    int oursW_ = 0, oursH_ = 0;
    TexFormat oursFormat_ = TexFormat::A8R8G8B8;
    FormatSupport formats_;
    bool formatSaid_ = false, compressSaid_ = false;
    Slot moji_{"moji"}, high_{"high"};
    std::vector<Slot> abandoned_;       // records the renderer stopped naming, with their originals
    std::vector<uintptr_t> retired_;    // earlier TrueFont textures a record may still hold
    uintptr_t mojiRejected_ = 0, highRejected_ = 0;
    KeepMonitor keep_;
    Phase phase_ = Phase::Off;
    bool enabled_ = false;         // turnOn once the character's settings say on
    bool highActive_ = false;
    int builtCell_ = 0;
    uint64_t rebuildAt_ = 0;       // a debounced rebuild is due at this time (0 = none)
    unsigned swaps_ = 0;
    bool announceRebuild_ = false;
    bool failurePersistent_ = false, capsRefused_ = false;
    bool failureRestart_ = false;    // the refusal lasts until the game restarts
    bool retiredRefused_ = false;    // the last installSlot refused a record holding an earlier TrueFont texture
    std::string lastSaidFailure_;
    uint64_t gen_ = 0, buildGen_ = 0;
    std::string t1_, t2_, t12_;
    D3DSURFACE_DESC origDesc_{};
    static constexpr uint64_t kRebuildQuietMs = 300;
    bool building_ = false, showOriginal_ = false, gameGone_ = false, diagnosed_ = false;
    bool originalAtInstall_ = false;   // Show original asked for before the install
    bool heldByRecord_ = false;    // a restore left ours in a record: never release it
    bool leftover_ = false;        // a restore since load left a texture of ours in a record (retired_; Release reports it)
    bool announce_ = false, onSaid_ = false;
    unsigned long maxTexW_ = 0, maxTexH_ = 0, maxAspect_ = 0;
    bool squareOnly_ = false;
    bool nonPow2_ = true;          // the card takes a texture that is not a power of two (until the caps are read: assumed)
    bool highCopyPending_ = false; // font high became active while installed; its copy waits for its buffer
    bool highPrepSaid_ = false;    // Warning already emitted.
    int cellCap_ = 0;              // the largest cell size since one above it could not be made (0: none)
    uint64_t otherBytes_ = 0;      // the menu and HUD fonts' bytes
    bool otherBuilding_ = false;   // ... they are building
    uint64_t otherPending_ = 0;    // ... the new textures their build holds beside the old ones
    uint64_t otherFloor_ = 0;      // ... the least they would hold after stepping down (their bytes when they cannot)
    bool holdSaid_ = false;        // this wait was logged
    int growTried_ = 0;            // the larger cell size last tried (0: none)
    bool growPending_ = false;     // the rebuild due is a growth (dropped when its size no longer fits once due)
    bool quietNext_ = false;       // the rebuild due is automatic (a growth, or a step down after one): its outcome to the log only
    bool buildQuiet_ = false;      // ... the build running (or last started) is
    bool claim_ = false;           // the first look after an on sizes this font first (the menu and HUD fonts step down)
    uint64_t claimHoldSince_ = 0;  // ... when its wait for them began (0: not waiting)
    static constexpr uint64_t kClaimWaitMs = 5000;   // ... the longest wait before it is sized beside them as they are
    std::string capWhy_;           // Reason for the size cap.
    std::string cellSaid_;         // Last Sharpness warning; suppress duplicates.
    int buildCell_ = 16;           // the cell size of the build running (or last started)
    mutable DWORD lastLevels_ = 0;
    int waitLogged_ = 0;
    unsigned keepLines_ = 0;
    std::string autoOffReason_, failure_;
    uint64_t frame_ = 0, nowMs_ = 0, buildStartMs_ = 0;
};

}  // namespace tf
