// FreeType backend using GDI's selected face, coverage and simulated styles.
// Auto selects FreeType for CFF/CFF2 faces and Wine; other faces use Windows.
// Each build owns its library and faces on one thread. Font bytes are shared read-only, capped at 64 MB per file.
// Guarded FreeType faults abandon the library and faces; affected files use Windows for subsequent builds.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <malloc.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;   // Linker-provided image base for the SEH filter.

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_FONT_FORMATS_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_IDS_H

namespace tf {

enum class Engine : uint8_t { Windows = 0, FreeType, Auto };
inline const char* engineKey(Engine e) { return e == Engine::FreeType ? "freetype" : e == Engine::Auto ? "auto" : "windows"; }
inline const char* engineName(Engine e) { return e == Engine::FreeType ? "FreeType" : e == Engine::Auto ? "Auto" : "Windows"; }
inline bool engineFromKey(const std::string& k, Engine& e) {
    if (k == "windows") e = Engine::Windows;
    else if (k == "freetype") e = Engine::FreeType;
    else if (k == "auto") e = Engine::Auto;
    else return false;
    return true;
}

// Wine: ntdll exports wine_get_version. g_wineForTest overrides the answer (-1: ask ntdll).
inline std::atomic<int> g_wineForTest{-1};
inline bool underWine() {
    const int t = g_wineForTest.load();
    if (t >= 0) return t != 0;
    static const bool wine = [] {
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
    }();
    return wine;
}
// The engine a face draws with.
inline Engine pickEngine(Engine setting, bool cffOutlines, bool wine) {
    if (setting == Engine::Auto) return cffOutlines || wine ? Engine::FreeType : Engine::Windows;
    return setting;
}

namespace ftdetail {
inline constexpr DWORD tag(char a, char b, char c, char d) { return DWORD(uint8_t(a)) | (DWORD(uint8_t(b)) << 8) | (DWORD(uint8_t(c)) << 16) | (DWORD(uint8_t(d)) << 24); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline constexpr size_t kMaxFontBytes = 64u << 20;    // fontsdir.h's cap for a folder font; a larger file stays Windows'
inline constexpr FT_Fixed kShearTrueType = 0x5700;    // GDI's oblique on a TrueType face: 0.33984 (measured)
inline constexpr FT_Fixed kShearCff = 11556;          // on a CFF face: tan 10 deg = 0.17633 (measured)
// The selected face has PostScript outlines (the CFF / CFF2 table GDI's NTM_PS_OPENTYPE stands for).
inline bool gdiCff(HDC dc) { return GetFontData(dc, tag('C', 'F', 'F', ' '), 0, nullptr, 0) != GDI_ERROR || GetFontData(dc, tag('C', 'F', 'F', '2'), 0, nullptr, 0) != GDI_ERROR; }
struct Selection {   // a DC with the font GDI selects for a request
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ old = nullptr;
    Selection(const std::wstring& family, int emPx, int weight, bool italic) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return;
        font = CreateFontW(-emPx, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, family.c_str());
        if (font) old = SelectObject(dc, font);
    }
    ~Selection() {
        if (dc && old) SelectObject(dc, old);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }
    Selection(const Selection&) = delete;
    Selection& operator=(const Selection&) = delete;
    bool ok() const { return dc && font && old; }
};
inline std::string errText(const char* what, FT_Error e) {
    char b[96];
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s failed (FreeType error 0x%02X)", what, unsigned(e));
    return b;
}

// Catch selected SEH faults only inside this image, which includes FreeType and the CRT.
// Propagate OS faults (possible heap corruption) and C++ exceptions to their normal handlers.
// Read image bounds from __ImageBase without API calls or loader-lock acquisition in the filter.
inline bool inOwnImage(const void* address) {
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + uintptr_t(__ImageBase.e_lfanew));
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    return a >= base && a - base < uintptr_t(nt->OptionalHeader.SizeOfImage);
}
inline int faultFilterFor(unsigned long code, const void* address) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return inOwnImage(address) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
inline int faultFilter(const EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e ? e->ExceptionRecord : nullptr;
    return r ? faultFilterFor(r->ExceptionCode, r->ExceptionAddress) : EXCEPTION_CONTINUE_SEARCH;
}
template <class F> bool guarded(F&& f, unsigned long& code) {
    __try {
        f();
        return true;
    } __except (faultFilter(GetExceptionInformation())) {
        code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return false;
    }
}
inline std::string faultText(const char* what, unsigned long code) {
    char b[128];
    _snprintf_s(b, sizeof b, _TRUNCATE, "FreeType faulted %s (exception %08lX)", what, code);
    return b;
}

// The font files' bytes, one copy per file for the whole process; a file FreeType faulted on, by the same key.
class FontFiles {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    // The copy for `key`: the one a build still holds, else a new buffer of `n` bytes that `fill` reads. Null when `fill`
    // failed, or with `oom` when there was no memory for the buffer.
    template <class Fill> Bytes get(const std::string& key, size_t n, Fill&& fill, bool& oom) {
        oom = false;
        std::lock_guard<std::mutex> lock(m_);   // held while a new copy is read: two builds never read one file twice
        for (auto it = files_.begin(); it != files_.end();) it = it->second.expired() ? files_.erase(it) : std::next(it);
        const auto it = files_.find(key);
        if (it != files_.end())
            if (Bytes b = it->second.lock()) return b;
        std::shared_ptr<std::vector<uint8_t>> b;
        try {
            b = std::shared_ptr<std::vector<uint8_t>>(new std::vector<uint8_t>(n), [this, n](std::vector<uint8_t>* p) {
                delete p;
                live_ -= n;
            });
        } catch (const std::bad_alloc&) {
            oom = true;
            return nullptr;
        }
        live_ += n;
        for (size_t now = live_.load(), was = peak_.load(); now > was && !peak_.compare_exchange_weak(was, now);) {}
        if (!fill(b->data(), n)) return nullptr;
        files_[key] = b;
        return b;
    }
    void markFaulted(const std::string& key) {
        std::lock_guard<std::mutex> lock(m_);
        faulted_.insert(key);
    }
    bool faulted(const std::string& key) {
        std::lock_guard<std::mutex> lock(m_);
        return faulted_.count(key) != 0;
    }
    size_t liveBytes() const { return live_.load(); }
    size_t peakBytes() const { return peak_.load(); }
    void resetPeak() { peak_.store(live_.load()); }

private:
    std::mutex m_;
    std::map<std::string, std::weak_ptr<const std::vector<uint8_t>>> files_;
    std::set<std::string> faulted_;
    std::atomic<size_t> live_{0}, peak_{0};
};
inline FontFiles& fontFiles() {
    static FontFiles f;
    return f;
}
}  // namespace ftdetail

// How much GDI emboldens, in steps: a TrueType face by ceil(em / 50) px; a CFF face in steps at em 27, 64, 101 and 140
// (FOT-Rodin Pro L, the only CFF face measured).
inline int gdiBoldStep(int emPx, bool cff) { return cff ? (emPx < 27 ? 1 : 2 * (emPx - 26) / 75 + 2) : (emPx + 49) / 50; }
// Match GDI emboldening in 26.6 pixels: grow rightward and upward, retaining the baseline.
// Step 1 and CFF widen only; later TrueType steps also grow upward by k-1 pixels.
struct BoldGrowth { FT_Pos x = 0, y = 0; };
inline BoldGrowth boldStrength(int emPx, bool cff) {
    const int k = gdiBoldStep(emPx, cff);
    double x = 0, y = 0;
    if (cff) x = k == 1 ? std::clamp(emPx / 24.0, 0.625, 1.0) : 1.1 * k + 0.2;
    else if (k == 1) x = std::clamp(0.75 + emPx / 40.0, 1.0, 1.3);
    else { x = k + 0.5; y = k - 1.0; }
    return {FT_Pos(std::lround(x * 64.0)), FT_Pos(std::lround(y * 64.0))};
}
// The advance GDI adds, 26.6 px (measured): 1 px to a TrueType face at every size, em / 37 to a CFF face.
inline FT_Pos boldAdvance(int emPx, bool cff) { return cff ? FT_Pos(std::lround(emPx * 64.0 / 37.0)) : 64; }

// What GDI selected for a family, weight and italic, as FreeType reads it; one per request and build (FtEnv).
struct FtSource {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::string fileKey;         // the font file (FontFiles' key; "" for a stand-in source)
    long index = 0;              // the .ttc member
    bool cff = false;            // PostScript outlines
    bool gdiBold = false;        // GDI reports it bold (otmfsSelection bit 5) at tmWeight
    int gdiWeight = 0;
    bool gdiItalic = false;
    std::wstring style;          // GDI's style name (a variable font's instance)
    std::string why;             // "" when FreeType can take it
};

// One per build: the FT_Library (made on the first FreeType face) and the sources read so far.
class FtEnv : public std::enable_shared_from_this<FtEnv> {
public:
    FtEnv() = default;
    FtEnv(const FtEnv&) = delete;
    FtEnv& operator=(const FtEnv&) = delete;
    ~FtEnv() {
        unsigned long code = 0;
        if (lib_ && !dead_) ftdetail::guarded([&] { FT_Done_FreeType(lib_); }, code);   // a dead library is leaked
    }
    FT_Library library(std::string& why) {
        if (dead_) { why = deadWhy_; return nullptr; }
        if (!lib_ && !initTried_) {
            initTried_ = true;
            unsigned long code = 0;
            FT_Error e = 0;
            if (!ftdetail::guarded([&] { e = FT_Init_FreeType(&lib_); }, code)) { kill(ftdetail::faultText("starting", code)); why = deadWhy_; return nullptr; }
            if (e) { lib_ = nullptr; initWhy_ = ftdetail::errText("FT_Init_FreeType", e); }
        }
        if (!lib_) why = initWhy_.empty() ? "FreeType is not available" : initWhy_;
        return lib_;
    }
    // Abandon the faulted library and all its faces without freeing potentially corrupt state; use Windows.
    void kill(const std::string& why) {
        if (dead_) return;
        dead_ = true;
        deadWhy_ = why;
    }
    bool dead() const { return dead_; }
    const std::string& deadWhy() const { return deadWhy_; }
    // Auto's check, without reading the font: the family's selection has a CFF table.
    bool cff(const std::wstring& family, int weight, bool italic) {
        const auto key = std::make_tuple(lower(family), weight, italic);
        const auto it = cff_.find(key);
        if (it != cff_.end()) return it->second;
        ftdetail::Selection s(family, 64, weight, italic);
        const bool c = s.ok() && ftdetail::gdiCff(s.dc);
        cff_[key] = c;
        return c;
    }
    // The source for the request selected into `dc`.
    std::shared_ptr<const FtSource> source(HDC dc, const std::wstring& family, int weight, bool italic) {
        const auto key = std::make_tuple(lower(family), weight, italic);
        const auto it = sources_.find(key);
        if (it != sources_.end()) return it->second;
        auto s = std::make_shared<FtSource>();
        read(dc, *s);
        sources_[key] = s;
        return s;
    }
    // A stand-in source: what a request reads (a damaged font file, say).
    void putSource(const std::wstring& family, int weight, bool italic, std::shared_ptr<const FtSource> s) { sources_[std::make_tuple(lower(family), weight, italic)] = std::move(s); }
    // The font bytes the sources hold, each buffer once.
    size_t bytesHeld() const {
        std::vector<const void*> seen;
        size_t n = 0;
        for (const auto& kv : sources_) {
            const auto& b = kv.second->bytes;
            if (!b || std::find(seen.begin(), seen.end(), b.get()) != seen.end()) continue;
            seen.push_back(b.get());
            n += b->size();
        }
        return n;
    }

private:
    using Key = std::tuple<std::wstring, int, bool>;
    static std::wstring lower(std::wstring s) {
        for (wchar_t& c : s) c = wchar_t(std::towlower(c));
        return s;
    }
    // Key by table type, file size and header data. Collections use the shared header and first member directory
    // so every face in a .ttc shares one byte buffer; standalone fonts also include their head table.
    static bool fileKey(HDC dc, DWORD table, DWORD n, std::string& key) {
        using namespace ftdetail;
        const auto part = [&](DWORD at, DWORD len, uint8_t* to) { return len == 0 || GetFontData(dc, table, at, to, len) == len; };
        size_t len = std::min<size_t>(n, 64);
        if (table == tag('t', 't', 'c', 'f')) {
            uint8_t h[12] = {};
            if (n < 12 || !part(0, 12, h)) return false;
            const uint32_t count = std::min<uint32_t>(be32(h + 8), 1024);
            len = std::min<size_t>(n, 12 + 4 * size_t(count));
            uint8_t o0[4] = {};
            if (count && n >= 16 && part(12, 4, o0)) {
                const uint32_t first = be32(o0);
                uint8_t dh[12] = {};
                if (first <= n && n - first >= 12 && part(first, 12, dh)) len = std::max(len, std::min<size_t>(n, size_t(first) + 12 + 16 * size_t(be16(dh + 4))));
            }
            len = std::min<size_t>(len, 8192);
        }
        key.assign(8 + len + (table ? 0 : 54), '\0');
        std::memcpy(key.data(), &table, 4);
        std::memcpy(key.data() + 4, &n, 4);
        if (!part(0, DWORD(len), reinterpret_cast<uint8_t*>(key.data() + 8))) return false;
        if (!table) GetFontData(dc, tag('h', 'e', 'a', 'd'), 0, key.data() + 8 + len, 54);
        return true;
    }
    // The whole file's bytes: the process's copy when a build holds one, else read now.
    static std::shared_ptr<const std::vector<uint8_t>> fileBytes(HDC dc, DWORD table, DWORD n, const std::string& key, FtSource& s) {
        bool oom = false;
        auto b = ftdetail::fontFiles().get(key, n, [&](uint8_t* to, size_t len) { return GetFontData(dc, table, 0, to, DWORD(len)) == len; }, oom);
        if (!b) s.why = oom ? "there was not enough memory for a copy of the font" : "GetFontData could not read the font";
        return b;
    }
    void read(HDC dc, FtSource& s) {
        try {
            readFile(dc, s);
        } catch (const std::bad_alloc&) {   // Fall back to Windows without reducing cell size.
            s.bytes.reset();
            s.why = "there was not enough memory for a copy of the font";
        }
    }
    void readFile(HDC dc, FtSource& s) {
        using namespace ftdetail;
        s.cff = gdiCff(dc);
        std::vector<uint8_t> otmBuf(4096);
        auto* otm = reinterpret_cast<OUTLINETEXTMETRICW*>(otmBuf.data());
        otm->otmSize = UINT(otmBuf.size());
        if (GetOutlineTextMetricsW(dc, UINT(otmBuf.size()), otm)) {
            s.gdiBold = (otm->otmfsSelection & 0x20) != 0;
            s.gdiWeight = int(otm->otmTextMetrics.tmWeight);
            s.gdiItalic = otm->otmTextMetrics.tmItalic != 0;
            const size_t off = size_t(reinterpret_cast<uintptr_t>(otm->otmpStyleName));
            if (off && off < otmBuf.size()) s.style.assign(reinterpret_cast<const wchar_t*>(otmBuf.data() + off), wcsnlen(reinterpret_cast<const wchar_t*>(otmBuf.data() + off), (otmBuf.size() - off) / 2));
        }
        const DWORD ttcf = tag('t', 't', 'c', 'f');
        DWORD n = GetFontData(dc, ttcf, 0, nullptr, 0);
        const bool collection = n != GDI_ERROR && n > 12;
        const DWORD table = collection ? ttcf : 0;
        if (!collection) n = GetFontData(dc, 0, 0, nullptr, 0);
        if (n == GDI_ERROR || n == 0) { s.why = "GetFontData returned no font data"; return; }
        if (n > kMaxFontBytes) { s.why = "the font file is larger than 64 MB"; return; }
        if (!fileKey(dc, table, n, s.fileKey)) { s.why = collection ? "GetFontData could not read the collection" : "GetFontData could not read the font"; return; }
        if (fontFiles().faulted(s.fileKey)) { s.why = "FreeType faulted on this font earlier this session"; return; }
        std::shared_ptr<const std::vector<uint8_t>> bytes = fileBytes(dc, table, n, s.fileKey, s);
        if (!bytes) return;
        if (collection) {
            // The member: the collection's offset table whose table directory is the one GDI reads for the face.
            uint8_t head[12] = {};
            if (GetFontData(dc, 0, 0, head, sizeof head) != sizeof head) { s.why = "GetFontData could not read the face's table directory"; return; }
            const size_t dirLen = 12 + 16 * size_t(be16(head + 4));
            std::vector<uint8_t> dir(dirLen);
            if (GetFontData(dc, 0, 0, dir.data(), DWORD(dirLen)) != dirLen) { s.why = "GetFontData could not read the face's table directory"; return; }
            const size_t size = bytes->size();
            const uint32_t count = be32(bytes->data() + 8);
            s.index = -1;
            for (uint32_t i = 0; i < count && 12 + 4 * size_t(i) + 4 <= size; i++) {
                const size_t o = be32(bytes->data() + 12 + 4 * size_t(i));
                if (o <= size && dirLen <= size - o && std::memcmp(bytes->data() + o, dir.data(), dirLen) == 0) { s.index = long(i); break; }   // Bounds checked without unsigned wrap.
            }
            if (s.index < 0) { s.why = "the face is not found in its collection"; return; }
        }
        s.bytes = std::move(bytes);
    }

    FT_Library lib_ = nullptr;
    bool initTried_ = false, dead_ = false;
    std::string initWhy_, deadWhy_;
    std::map<Key, std::shared_ptr<const FtSource>> sources_;
    std::map<Key, bool> cff_;
};

// The building thread's FtEnv while a scope is open (nested scopes keep the outer one).
inline thread_local FtEnv* t_ftEnv = nullptr;
class FtScope {
public:
    FtScope() : prev_(t_ftEnv) {
        if (!prev_) {
            env_ = std::make_shared<FtEnv>();
            t_ftEnv = env_.get();
        }
    }
    ~FtScope() {
        if (env_) t_ftEnv = prev_;
    }
    FtScope(const FtScope&) = delete;
    FtScope& operator=(const FtScope&) = delete;

private:
    FtEnv* prev_;
    std::shared_ptr<FtEnv> env_;
};
inline std::shared_ptr<FtEnv> currentFtEnv() {
    if (t_ftEnv) return t_ftEnv->shared_from_this();
    return std::make_shared<FtEnv>();
}

// Restore condensed stems to min(original width, 1 px), preventing light hinting from leaving subpixel gaps.
inline double stemKeep(double stem, double condense) { return std::max(0.0, std::min(stem, 1.0) - stem * std::max(condense, 1.0 / 64)); }

// A face drawn by FreeType, with GdiFace's calls (atlas.h).
class FtFace {
public:
    FtFace() = default;
    FtFace(const FtFace&) = delete;
    FtFace& operator=(const FtFace&) = delete;
    ~FtFace() { close(); }

    bool open(const std::shared_ptr<FtEnv>& env, const std::wstring& family, int emPx, int weight, bool italic, bool hinted) {
        close();
        why_.clear();
        dead_ = false;
        env_ = env;
        sel_ = std::make_unique<ftdetail::Selection>(family, emPx, weight, italic);
        if (!sel_->ok()) { why_ = "CreateFontW failed"; close(); return false; }
        wchar_t name[LF_FACESIZE] = {};
        GetTextFaceW(sel_->dc, LF_FACESIZE, name);
        face_ = name;
        src_ = env_->source(sel_->dc, family, weight, italic);
        if (!src_->why.empty()) { why_ = src_->why; close(); return false; }
        const FT_Library lib = env_->library(why_);
        if (!lib) { close(); return false; }
        bool ok = false;
        unsigned long code = 0;
        if (!ftdetail::guarded([&] { ok = openFt(lib, emPx, italic, hinted); }, code)) {
            fault("opening it", code);
            close();
            return false;
        }
        if (!ok) { close(); return false; }
        return true;
    }
    void close() {
        unsigned long code = 0;
        if (ft_ && !dead()) ftdetail::guarded([&] { FT_Done_Face(ft_); }, code);   // a dead face is leaked
        ft_ = nullptr;
        instance_ = 0;
        stem_ = -1;
        src_.reset();
        sel_.reset();
        env_.reset();
        sizeW_ = sizeH_ = 0;
    }
    // FreeType faulted on this face, or on another face of its build (their library is then left alone).
    bool dead() const { return dead_ || (env_ && env_->dead()); }
    const std::wstring& face() const { return face_; }
    const std::string& why() const { return !dead_ && env_ && env_->dead() ? env_->deadWhy() : why_; }
    bool hinted() const { return hinted_; }
    bool synthBold() const { return synthBold_; }
    bool synthItalic() const { return synthItalic_; }
    int instance() const { return instance_; }
    long rasters() const { return rasters_; }
    const char* format() const {
        const char* f = nullptr;
        unsigned long code = 0;
        if (ft_ && !dead()) ftdetail::guarded([&] { f = FT_Get_Font_Format(ft_); }, code);
        return f ? f : "";
    }
    const char* family() const { return ft_ && !dead() && ft_->family_name ? ft_->family_name : ""; }
    bool has(wchar_t ch) const {
        WORD idx = 0xFFFF;
        return sel_ && GetGlyphIndicesW(sel_->dc, &ch, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) == 1 && idx != 0xFFFF;
    }
    bool metrics(wchar_t ch, GLYPHMETRICS& gm) const {
        std::vector<uint8_t> bits;
        bool blank = false;
        const long n = rasters_;
        const bool ok = raster(ch, 1.0, 1.0, gm, bits, blank);
        rasters_ = n;
        return ok;
    }
    int boxHeight(wchar_t ch) const {
        GLYPHMETRICS gm{};
        return metrics(ch, gm) ? int(gm.gmBlackBoxY) : 0;
    }
    // GdiFace::raster's contract: 0..64 coverage in rows of (w + 3) & ~3, GLYPHMETRICS; false on a FreeType error or fault
    // (the face is then dead, and GlyphFace draws with Windows).
    bool raster(wchar_t ch, double condense, double squeeze, GLYPHMETRICS& gm, std::vector<uint8_t>& bits, bool& blank) const {
        ++rasters_;
        gm = GLYPHMETRICS{};
        bits.clear();
        blank = false;
        if (!ft_ || dead()) return false;
        bool ok = false;
        unsigned long code = 0;
        if (!ftdetail::guarded([&] { ok = rasterFt(ch, condense, squeeze, gm, bits, blank); }, code)) {
            char what[40];
            _snprintf_s(what, sizeof what, _TRUNCATE, "drawing U+%04X", unsigned(ch));
            fault(what, code);
            gm = GLYPHMETRICS{};
            bits.clear();
            blank = false;
            return false;
        }
        return ok;
    }

private:
    // The face (and its build's library) is dead; the font file is remembered, so later builds draw it with Windows.
    void fault(const char* what, unsigned long code) const {
        dead_ = true;
        why_ = ftdetail::faultText(what, code);
        if (env_) env_->kill(why_);
        if (src_ && !src_->fileKey.empty()) ftdetail::fontFiles().markFaulted(src_->fileKey);
    }
    // open()'s FreeType part, under the SEH guard.
    bool openFt(FT_Library lib, int emPx, bool italic, bool hinted) {
        FT_Error e = FT_New_Memory_Face(lib, src_->bytes->data(), FT_Long(src_->bytes->size()), src_->index, &ft_);
        if (e) { ft_ = nullptr; why_ = ftdetail::errText("FT_New_Memory_Face", e); return false; }
        if (!FT_IS_SCALABLE(ft_) || !FT_HAS_HORIZONTAL(ft_)) { why_ = "FreeType finds no scalable outlines in it"; return false; }
        faceWeight_ = ownWeight();
        const bool faceItalic = (ft_->style_flags & FT_STYLE_FLAG_ITALIC) != 0 || (instance_ && (styleSays(L"italic") || styleSays(L"oblique")));
        cff_ = std::strcmp(FT_Get_Font_Format(ft_) ? FT_Get_Font_Format(ft_) : "", "CFF") == 0;
        synthBold_ = src_->gdiBold && src_->gdiWeight >= faceWeight_ + 200;
        synthItalic_ = italic && src_->gdiItalic && !faceItalic;
        em_ = emPx;
        hinted_ = hinted;
        flags_ = hinted ? (FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) : (FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
        bold_ = synthBold_ ? boldStrength(emPx, cff_) : BoldGrowth{};
        boldAdv_ = synthBold_ ? boldAdvance(emPx, cff_) : 0;
        shear_ = synthItalic_ ? (cff_ ? ftdetail::kShearCff : ftdetail::kShearTrueType) : 0;
        return setSize(1.0, 1.0);
    }
    // raster()'s FreeType part, under the SEH guard.
    bool rasterFt(wchar_t ch, double condense, double squeeze, GLYPHMETRICS& gm, std::vector<uint8_t>& bits, bool& blank) const {
        if (!setSize(condense, squeeze)) return false;
        FT_UInt gid = FT_Get_Char_Index(ft_, FT_ULong(ch));
        if (!gid) {   // a character GDI maps and FreeType's cmap does not
            WORD idx = 0xFFFF;
            if (GetGlyphIndicesW(sel_->dc, &ch, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != 1 || idx == 0xFFFF || idx == 0 || FT_Long(idx) >= ft_->num_glyphs) return false;
            gid = idx;
        }
        if (hinted_ && condense < 1.0) stemPx();   // measured before this glyph is loaded (it loads its own)
        if (!setSize(condense, squeeze) || FT_Load_Glyph(ft_, gid, flags_)) return false;
        const FT_GlyphSlot slot = ft_->glyph;
        if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return false;
        if (hinted_) {   // a bottom on the baseline stays on it
            FT_BBox hb{};
            FT_Outline_Get_CBox(&slot->outline, &hb);
            if (hb.yMin != 0 && slot->outline.n_points > 0) {
                const FT_Pos hinted = hb.yMin;
                if (FT_Load_Glyph(ft_, gid, (flags_ & ~FT_LOAD_TARGET_LIGHT) | FT_LOAD_NO_HINTING)) return false;
                FT_BBox ub{};
                FT_Outline_Get_CBox(&slot->outline, &ub);
                const bool onLine = ub.yMin >= -1 && ub.yMin <= 1;
                if (FT_Load_Glyph(ft_, gid, flags_) || slot->format != FT_GLYPH_FORMAT_OUTLINE) return false;
                if (onLine) FT_Outline_Translate(&slot->outline, 0, -hinted);
            }
        }
        const FT_Pos keep = hinted_ && condense < 1.0 ? FT_Pos(std::lround(stemKeep(stemPx(), condense) * 64.0)) : 0;   // Preserve a minimum stem width.
        if ((bold_.x || bold_.y || keep) && FT_Outline_EmboldenXY(&slot->outline, bold_.x + keep, bold_.y)) return false;
        if (shear_) {   // GDI's slant is condensed with the glyph (eM11), not squeezed (eM22), as GDI does
            FT_Matrix m{0x10000, FT_Fixed(std::lround(double(shear_) * std::clamp(condense, 1.0 / 64, 1.0))), 0, 0x10000};
            FT_Outline_Transform(&slot->outline, &m);
        }
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return false;
        const FT_Bitmap& b = slot->bitmap;
        if (b.pixel_mode != FT_PIXEL_MODE_GRAY || b.num_grays != 256) return false;
        gm.gmCellIncX = short((slot->advance.x + boldAdv_ + 32) >> 6);
        // The ink box: the rows and columns whose 0..64 coverage is above 0.
        const auto at = [&](unsigned y, unsigned x) -> int {
            const unsigned char* row = b.pitch >= 0 ? b.buffer + size_t(y) * size_t(b.pitch) : b.buffer + (size_t(b.rows) - 1 - y) * size_t(-b.pitch);
            return (int(row[x]) * 64 + 127) / 255;
        };
        int x0 = int(b.width), x1 = -1, y0 = int(b.rows), y1 = -1;
        for (unsigned y = 0; y < b.rows; y++)
            for (unsigned x = 0; x < b.width; x++)
                if (at(y, x)) { x0 = std::min(x0, int(x)); x1 = std::max(x1, int(x)); y0 = std::min(y0, int(y)); y1 = std::max(y1, int(y)); }
        if (x1 < 0) { blank = true; return true; }
        const int w = x1 - x0 + 1, h = y1 - y0 + 1, stride = (w + 3) & ~3;
        bits.assign(size_t(stride) * size_t(h), 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) bits[size_t(y) * size_t(stride) + size_t(x)] = uint8_t(at(unsigned(y0 + y), unsigned(x0 + x)));
        gm.gmBlackBoxX = UINT(w);
        gm.gmBlackBoxY = UINT(h);
        gm.gmptGlyphOrigin.x = slot->bitmap_left + x0;
        gm.gmptGlyphOrigin.y = slot->bitmap_top - y0;
        return true;
    }
    // The size for a condense / squeeze, quantised as GDI's MAT2 (GdiFace::raster's toFixed).
    bool setSize(double condense, double squeeze) const {
        const auto fixed = [](double v) { return std::lround(std::clamp(v, 1.0 / 64, 1.0) * 65536.0); };
        const FT_F26Dot6 w = std::max<FT_F26Dot6>(1, FT_F26Dot6(std::llround(double(em_) * 64.0 * double(fixed(condense)) / 65536.0)));
        const FT_F26Dot6 h = std::max<FT_F26Dot6>(1, FT_F26Dot6(std::llround(double(em_) * 64.0 * double(fixed(squeeze)) / 65536.0)));
        if (w == sizeW_ && h == sizeH_) return true;
        if (FT_Set_Char_Size(ft_, w, h, 72, 72)) { sizeW_ = sizeH_ = 0; return false; }
        sizeW_ = w;
        sizeH_ = h;
        return true;
    }
    // The width of a vertical stem at this em, in px (0 when the face has none of 'l' 'I' '1'); measured once.
    double stemPx() const {
        if (stem_ >= 0) return stem_;
        stem_ = 0;
        if (!setSize(1.0, 1.0)) return stem_;
        for (const wchar_t c : {L'l', L'I', L'1'}) {
            const FT_UInt g = FT_Get_Char_Index(ft_, FT_ULong(c));
            if (!g || FT_Load_Glyph(ft_, g, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) || ft_->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
            if (FT_Render_Glyph(ft_->glyph, FT_RENDER_MODE_NORMAL)) continue;
            const FT_Bitmap& b = ft_->glyph->bitmap;
            if (b.rows < 3 || b.pixel_mode != FT_PIXEL_MODE_GRAY) continue;
            const unsigned y = b.rows / 2;
            const unsigned char* row = b.pitch >= 0 ? b.buffer + size_t(y) * size_t(b.pitch) : b.buffer + (size_t(b.rows) - 1 - y) * size_t(-b.pitch);
            double sum = 0;
            for (unsigned x = 0; x < b.width; x++) sum += row[x] / 255.0;
            stem_ = std::min(sum, em_ * 0.25);
            break;
        }
        return stem_;
    }
    bool styleSays(const wchar_t* word) const {
        std::wstring s = src_ ? src_->style : std::wstring();
        for (wchar_t& c : s) c = wchar_t(std::towlower(c));
        return s.find(word) != std::wstring::npos;
    }
    // The face's own weight: a variable font's instance GDI named (set here), else OS/2's usWeightClass.
    int ownWeight() {
        if (FT_HAS_MULTIPLE_MASTERS(ft_) && src_ && !src_->style.empty()) {
            FT_MM_Var* mm = nullptr;
            if (!FT_Get_MM_Var(ft_, &mm) && mm) {
                int pick = 0;
                const FT_UInt names = FT_Get_Sfnt_Name_Count(ft_);
                for (FT_UInt i = 0; i < mm->num_namedstyles && !pick; i++)
                    for (FT_UInt k = 0; k < names && !pick; k++) {
                        FT_SfntName nm{};
                        if (FT_Get_Sfnt_Name(ft_, k, &nm) || nm.name_id != mm->namedstyle[i].strid || nm.platform_id != TT_PLATFORM_MICROSOFT) continue;
                        std::wstring s;
                        for (FT_UInt j = 0; j + 1 < nm.string_len; j += 2) s += wchar_t((nm.string[j] << 8) | nm.string[j + 1]);
                        if (_wcsicmp(s.c_str(), src_->style.c_str()) == 0) pick = int(i) + 1;
                    }
                int w = 0;
                if (pick && !FT_Set_Named_Instance(ft_, FT_UInt(pick))) {
                    for (FT_UInt a = 0; a < mm->num_axis; a++)
                        if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) w = int(mm->namedstyle[pick - 1].coords[a] >> 16);
                    instance_ = pick;
                }
                FT_Done_MM_Var(ft_->glyph->library, mm);
                if (w > 0) return w;
            }
        }
        const TT_OS2* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(ft_, FT_SFNT_OS2));
        if (os2 && os2->usWeightClass >= 100 && os2->usWeightClass <= 1000) return os2->usWeightClass;
        return (ft_->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
    }

    std::shared_ptr<FtEnv> env_;
    std::unique_ptr<ftdetail::Selection> sel_;
    std::shared_ptr<const FtSource> src_;
    FT_Face ft_ = nullptr;
    std::wstring face_;
    mutable std::string why_;
    mutable bool dead_ = false;   // FreeType faulted on this face
    int em_ = 0, faceWeight_ = 400, instance_ = 0;
    bool hinted_ = true, cff_ = false, synthBold_ = false, synthItalic_ = false;
    FT_Int32 flags_ = 0;
    BoldGrowth bold_;
    FT_Pos boldAdv_ = 0;
    FT_Fixed shear_ = 0;
    mutable FT_F26Dot6 sizeW_ = 0, sizeH_ = 0;
    mutable double stem_ = -1;
    mutable long rasters_ = 0;
};

}  // namespace tf
