// Process-private .ttf/.otf/.ttc/.otc fonts from config\truefont\fonts (no subfolders).
// Memory resources survive source-file deletion and are removed by handle. Temporary path resources allow
// enumeration; files stay open without delete sharing until those resources are removed.
// FontGate excludes builds during replacement, since removing a selected font makes GDI substitute it.
// Scans run on the disk writer, with at most 256 files of 64 MB each. Installed families take precedence.
#pragma once
#include "fonts.h"
#include "fontgate.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace tf {

// Null list means the scan could not replace the previous list.
struct FontScan {
    std::shared_ptr<const FontList> list;
    size_t files = 0, loaded = 0, failed = 0;   // font files in the folder; loaded; not loaded
    size_t duplicates = 0;                      // families of loaded files that Windows already has (never used: its own wins)
    std::vector<std::string> newFailures;       // the files whose failure this scan logs (UTF-8 name and why)
    std::vector<std::string> newDuplicates;     // the files whose duplicate family this scan logs
    std::vector<std::string> warnings;          // other lines for the log (a font that stays loaded, the file cap)
    uint64_t waitMs = 0, ms = 0;                // waiting for running builds; the whole scan
    bool reloaded = false;                      // the folder's fonts were loaded again, whether or not `list` is new
    std::string why;                            // no list: why
};

class FolderFonts {
public:
    FolderFonts() = default;
    FolderFonts(const FolderFonts&) = delete;
    FolderFonts& operator=(const FolderFonts&) = delete;

    static constexpr size_t kMaxFiles = 256;
    static constexpr uint64_t kMaxBytes = 64ull << 20;          // mapped whole for AddFontMemResourceEx, in a 32-bit process
    // The enumeration (fonts.h); replaceable so a failing one can be supplied.
    using Enumerate = std::shared_ptr<FontList> (*)();
    Enumerate enumerate = &FontList::enumerate;

    static bool isFontFile(const wchar_t* name) {
        const wchar_t* dot = name ? wcsrchr(name, L'.') : nullptr;
        if (!dot) return false;
        for (const wchar_t* e : {L".ttf", L".otf", L".ttc", L".otc"})
            if (_wcsicmp(dot, e) == 0) return true;
        return false;
    }
    // The font files in `dir` (ending with a backslash), by name; not in subfolders.
    static std::vector<std::wstring> listFiles(const std::wstring& dir) {
        std::vector<std::wstring> out;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && isFontFile(fd.cFileName)) out.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        return out;
    }
    // The family names (name ID 1, Windows platform, every language) of a .ttf/.otf or of each face of a .ttc/.otc,
    // read through `read(offset, bytes, out)` (a file or a buffer). Bounds-checked; unknown layouts give none.
    using ReadAt = std::function<bool(uint64_t, size_t, uint8_t*)>;
    static std::vector<std::wstring> familyNames(const ReadAt& read) {
        std::vector<std::wstring> out;
        uint8_t h[12];
        if (!read(0, sizeof h, h)) return out;
        const auto be16 = [](const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; };
        const auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
        std::vector<uint32_t> heads;
        if (be32(h) == 0x74746366) {   // 'ttcf'
            const uint32_t n = (std::min)(be32(h + 8), 64u);
            std::vector<uint8_t> offs(size_t(n) * 4);
            if (n && read(12, offs.size(), offs.data()))
                for (uint32_t i = 0; i < n; i++) heads.push_back(be32(&offs[size_t(i) * 4]));
        } else heads.push_back(0);
        std::vector<std::pair<uint32_t, uint32_t>> seen;
        for (const uint32_t at : heads) {
            uint8_t oh[12];
            if (!read(at, sizeof oh, oh)) continue;
            const uint32_t nt = (std::min)(be16(oh + 4), 256u);
            std::vector<uint8_t> dir(size_t(nt) * 16);
            if (!nt || !read(uint64_t(at) + 12, dir.size(), dir.data())) continue;
            for (uint32_t t = 0; t < nt; t++) {
                const uint8_t* rec = &dir[size_t(t) * 16];
                if (std::memcmp(rec, "name", 4) != 0) continue;
                const uint32_t off = be32(rec + 8), len = be32(rec + 12);
                if (len < 6 || len > (1u << 20) || std::find(seen.begin(), seen.end(), std::make_pair(off, len)) != seen.end()) break;
                seen.emplace_back(off, len);
                std::vector<uint8_t> tb(len);
                if (!read(off, len, tb.data())) break;
                const uint32_t count = be16(&tb[2]), strings = be16(&tb[4]);
                for (uint32_t i = 0; i < count && 6 + size_t(i) * 12 + 12 <= len; i++) {
                    const uint8_t* nr = &tb[6 + size_t(i) * 12];
                    const uint32_t platform = be16(nr), id = be16(nr + 6), bytes = be16(nr + 8), so = be16(nr + 10);
                    if (platform != 3 || id != 1 || !bytes || (bytes & 1) || size_t(strings) + so + bytes > len) continue;
                    std::wstring name;
                    for (uint32_t b = 0; b < bytes; b += 2) name += wchar_t(be16(&tb[size_t(strings) + so + b]));
                    const bool dup = std::any_of(out.begin(), out.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), name.c_str()) == 0; });
                    if (!dup) out.push_back(std::move(name));
                }
                break;
            }
        }
        return out;
    }

    // Reload private fonts and enumerate on the disk writer. waitMs bounds the wait for active builds.
    FontScan scan(const std::wstring& dir, FontGate& gate, unsigned long waitMs = 30000) {
        FontScan r;
        const ULONGLONG t0 = GetTickCount64();
        std::lock_guard<std::mutex> lock(m_);
        std::vector<std::wstring> names = listFiles(dir);
        r.files = names.size();
        if (names.size() > kMaxFiles) {
            r.warnings.push_back("fonts: " + std::to_string(names.size()) + " font files in the folder; only the first " + std::to_string(kMaxFiles) + " are loaded");
            names.resize(kMaxFiles);
        }
        // The families Windows has (memory fonts do not enumerate). A failed enumeration is a failure, before any font is touched.
        std::shared_ptr<FontList> base;
        try { base = enumerate(); } catch (const std::exception&) {}
        if (!base || base->empty()) {
            r.why = "the installed fonts could not be enumerated";
            r.ms = GetTickCount64() - t0;
            return r;
        }
        const ULONGLONG w0 = GetTickCount64();
        if (!gate.lock(waitMs)) {
            r.waitMs = GetTickCount64() - w0;
            r.why = "a font build kept running (or TrueFont is unloading)";
            r.ms = GetTickCount64() - t0;
            return r;
        }
        struct Unlocker { FontGate& g; ~Unlocker() { g.unlock(); } } unlocker{gate};   // the builds wait no longer than this scan, whatever happens
        r.waitMs = GetTickCount64() - w0;
        retryStuck(r);   // under the gate (a build may draw with that family)
        // Each file is opened only now, without delete sharing, and stays open until its path add is removed.
        std::vector<File> files;
        files.reserve(names.size());
        Closer closer{files};
        for (const auto& n : names) {
            files.emplace_back();
            File& f = files.back();
            f.name = n;
            f.path = dir + n;
            f.h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            LARGE_INTEGER sz{};
            if (f.h == INVALID_HANDLE_VALUE) fail(r, n, "it could not be opened (error " + std::to_string(GetLastError()) + ")");
            else if (!GetFileSizeEx(f.h, &sz) || sz.QuadPart <= 0 || uint64_t(sz.QuadPart) > kMaxBytes) {
                CloseHandle(f.h);
                f.h = INVALID_HANDLE_VALUE;
                fail(r, n, sz.QuadPart > 0 ? "it is larger than 64 MB" : "it is empty or its size could not be read");
            } else f.size = uint64_t(sz.QuadPart);
        }
        // Reserved storage prevents allocation between acquiring and recording a font handle.
        // PathAdds removes temporary enumeration resources on every exit.
        mem_.reserve(mem_.size() + files.size());
        stuck_.reserve(stuck_.size() + files.size());
        unloadLocked(r);
        r.reloaded = true;
        PathAdds adds{*this, files, r};
        for (auto& f : files) {
            if (f.h == INVALID_HANDLE_VALUE) continue;
            const HANDLE fh = f.h;
            f.names = familyNames([fh](uint64_t off, size_t n, uint8_t* out) { return readAt(fh, off, n, out); });
            std::string why;
            const HANDLE mem = addMemory(f.h, f.size, why);
            if (!mem) { fail(r, f.name, why); continue; }
            mem_.push_back(mem);   // reserved: cannot throw
            heldCount_ = mem_.size();
            f.pathAdded = AddFontResourceExW(f.path.c_str(), FR_PRIVATE, nullptr) > 0;
            if (!f.pathAdded) {   // GDI read the bytes but not the file: not used either
                RemoveFontMemResourceEx(mem);
                mem_.pop_back();
                heldCount_ = mem_.size();
                fail(r, f.name, "Windows could not read it as a font");
                continue;
            }
            ++r.loaded;
        }
        std::shared_ptr<FontList> full;
        try { full = enumerate(); } catch (const std::exception&) {}
        adds.removeAll();
        if (!full || full->empty()) {   // the previous list stays; the fonts loaded now are the folder's as it is
            r.why = "the fonts folder was loaded again, but the font list could not be read";
            r.ms = GetTickCount64() - t0;
            return r;
        }
        for (const auto& fam : full->families)
            if (base->find(fam).empty()) full->folder.push_back(fam);   // shared with Windows: installed
        // Installed families take precedence; report duplicates once per file/family and count distinct families.
        std::vector<std::wstring> dups;
        for (const auto& f : files) {
            if (!f.pathAdded && !f.loadedOnce) continue;
            for (const auto& n : f.names) {
                const std::wstring have = base->find(n);
                if (have.empty()) continue;
                if (std::none_of(dups.begin(), dups.end(), [&](const std::wstring& d) { return _wcsicmp(d.c_str(), have.c_str()) == 0; })) dups.push_back(have);
                std::wstring key = f.name + L"|" + have;
                for (auto& c : key) c = wchar_t(towlower(c));
                if (dupSaid_.insert(key).second)
                    r.newDuplicates.push_back(toUtf8(f.name) + ": Windows already has '" + toUtf8(have) + "'; the installed one is used");
            }
        }
        r.duplicates = full->duplicates = dups.size();
        r.list = full;
        r.ms = GetTickCount64() - t0;
        return r;
    }

    // Remove owned fonts only when no build is active. Return the number retained until process exit.
    size_t unloadAll(FontGate& gate) {
        std::lock_guard<std::mutex> lock(m_);
        if (mem_.empty() && stuck_.empty()) return 0;
        if (!gate.tryLock()) return mem_.size() + stuck_.size();
        FontScan r;
        unloadLocked(r);
        retryStuck(r);
        gate.unlock();
        return mem_.size() + stuck_.size();
    }
    // The fonts this has loaded; never waits for a scan (the render thread asks).
    size_t held() const { return heldCount_.load(); }

private:
    struct File {
        std::wstring name, path;
        HANDLE h = INVALID_HANDLE_VALUE;
        uint64_t size = 0;
        bool pathAdded = false;      // its enumeration copy is loaded now
        bool loadedOnce = false;     // it was loaded this scan (its enumeration copy since removed)
        std::vector<std::wstring> names;
    };
    struct Closer {   // every file closed on every way out, an exception included
        std::vector<File>& fs;
        ~Closer() {
            for (auto& f : fs)
                if (f.h != INVALID_HANDLE_VALUE) { CloseHandle(f.h); f.h = INVALID_HANDLE_VALUE; }
        }
    };
    // Every enumeration copy removed on every way out; one that will not go is kept for a later try (its room is reserved).
    struct PathAdds {
        FolderFonts& self;
        std::vector<File>& fs;
        FontScan& r;
        ~PathAdds() { removeAll(); }
        void removeAll() {
            for (auto& f : fs) {
                if (!f.pathAdded) continue;
                f.pathAdded = false;
                f.loadedOnce = true;
                if (RemoveFontResourceExW(f.path.c_str(), FR_PRIVATE, nullptr)) continue;
                const DWORD e = GetLastError();
                if (self.stuck_.size() < self.stuck_.capacity()) self.stuck_.push_back(std::move(f.path));   // a move: no allocation
                try {
                    r.warnings.push_back("fonts: " + toUtf8(f.name) + ": its enumeration copy could not be removed (error " + std::to_string(e) +
                                         "); it stays listed until the game closes");
                } catch (const std::exception&) {}
            }
        }
    };
    static bool readAt(HANDLE h, uint64_t off, size_t n, uint8_t* out) {
        OVERLAPPED o{};
        o.Offset = DWORD(off);
        o.OffsetHigh = DWORD(off >> 32);
        DWORD got = 0;
        return n <= 0x7FFFFFFF && ReadFile(h, out, DWORD(n), &got, &o) && got == n;
    }
    // The first failure of a file is logged with its name.
    void fail(FontScan& r, const std::wstring& name, const std::string& why) {
        ++r.failed;
        std::wstring key = name;
        for (auto& c : key) c = wchar_t(towlower(c));
        if (failedSaid_.insert(key).second) r.newFailures.push_back(toUtf8(name) + ": " + why);
    }
    static HANDLE addMemory(HANDLE file, uint64_t size, std::string& why) {
        HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map) { why = "it could not be read (error " + std::to_string(GetLastError()) + ")"; return nullptr; }
        void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        HANDLE mem = nullptr;
        if (!view) why = "it could not be read (error " + std::to_string(GetLastError()) + ")";
        else {
            DWORD n = 0;
            mem = AddFontMemResourceEx(view, DWORD(size), nullptr, &n);
            if (!mem || !n) {
                if (mem) RemoveFontMemResourceEx(mem);
                mem = nullptr;
                why = "Windows could not read it as a font";
            }
            UnmapViewOfFile(view);
        }
        CloseHandle(map);
        return mem;
    }
    // Under the gate: this module's memory fonts removed (a handle that will not go is kept, and said). In place: no
    // allocation, so a handle is never both removed and still listed.
    void unloadLocked(FontScan& r) {
        const auto kept = std::remove_if(mem_.begin(), mem_.end(), [](HANDLE h) { return RemoveFontMemResourceEx(h) != FALSE; });
        mem_.erase(kept, mem_.end());
        heldCount_ = mem_.size();
        if (!mem_.empty()) r.warnings.push_back("fonts: " + std::to_string(mem_.size()) + " of TrueFont's folder fonts could not be removed; they stay loaded until the game closes");
    }
    // An enumeration copy whose remove failed before: tried again (only this module's own path adds are ever removed).
    void retryStuck(FontScan& r) {
        const size_t was = stuck_.size();
        stuck_.erase(std::remove_if(stuck_.begin(), stuck_.end(), [](const std::wstring& p) { return RemoveFontResourceExW(p.c_str(), FR_PRIVATE, nullptr) != FALSE; }), stuck_.end());
        if (stuck_.size() < was) r.warnings.push_back("fonts: " + std::to_string(was - stuck_.size()) + " enumeration copies removed on a later try");
    }

    std::mutex m_;
    std::vector<HANDLE> mem_;              // the AddFontMemResourceEx handles this loaded
    std::atomic<size_t> heldCount_{0};     // mem_.size(), readable without m_
    std::vector<std::wstring> stuck_;      // path adds of this module whose remove failed
    std::set<std::wstring> failedSaid_;    // lower case
    std::set<std::wstring> dupSaid_;       // file|family (lower case)
};

}  // namespace tf
