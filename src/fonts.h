// Installed and private font families, enumerated on the disk writer and shared read-only.
// Families are case-insensitive, sorted and exclude vertical faces. Prefer a basic-Latin family for the default.
// Validate saved names against enumeration: GDI may silently substitute missing fonts.
#pragma once
#include <windows.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace tf {

// UTF-8 <-> UTF-16 (the settings, the log and the panel are UTF-8; GDI is UTF-16).
inline std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// Encode chat as CP932, using Windows best-fit substitution, without cutting double-byte characters.
inline std::string chatText(const std::string& utf8, size_t maxBytes) {
    std::string out;
    const std::wstring w = fromUtf8(utf8);
    if (!w.empty()) {
        const int n = WideCharToMultiByte(932, 0, w.c_str(), int(w.size()), nullptr, 0, "?", nullptr);
        out.assign(size_t(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(932, 0, w.c_str(), int(w.size()), out.data(), n, "?", nullptr);
    }
    size_t at = 0;
    while (at < out.size()) {
        const unsigned char c = static_cast<unsigned char>(out[at]);
        const size_t len = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC) ? 2 : 1;   // a Shift-JIS lead byte
        if (at + len > maxBytes || at + len > out.size()) break;
        at += len;
    }
    out.resize(at);
    // A control byte (a hand-edited name's %1E) would reach the chat as an escape: '?'. No Shift-JIS trail byte is below 40h
    // or 7Fh, so this never splits a double-byte character.
    for (char& ch : out)
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = '?';
    return out;
}

// The highlight markers a chat line may carry around a command: \x11 starts the command colour, \x12 goes back to the
// line's own.
inline constexpr char kHlOn = '\x11', kHlOff = '\x12';
inline constexpr unsigned char kColCommand = 0x02;
// A chat line's body: each run between markers as chatText makes it, each marker a two-byte colour escape (the command
// colour, or `color` back). At most `maxBytes` in all, never cutting an escape or a double-byte character.
inline std::string chatBody(const std::string& utf8, unsigned char color, size_t maxBytes) {
    std::string out;
    size_t start = 0;
    for (;;) {
        const size_t at = utf8.find_first_of("\x11\x12", start);
        const std::string run = utf8.substr(start, at == std::string::npos ? std::string::npos : at - start);
        if (!run.empty()) {
            const std::string whole = chatText(run, run.size());   // Shift-JIS is never longer than the UTF-8
            if (out.size() + whole.size() > maxBytes) {             // cut here: nothing after it
                out += chatText(run, maxBytes - out.size());
                break;
            }
            out += whole;
        }
        if (at == std::string::npos || out.size() + 2 > maxBytes) break;
        out += '\x1E';
        out += char(utf8[at] == kHlOn ? kColCommand : color);
        start = at + 1;
    }
    return out;
}
// The same line without its markers (the log's copy).
inline std::string plainText(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == kHlOn || c == kHlOff; }), s.end());
    return s;
}

// Convert CP932 input to UTF-8. Preserve valid UTF-8 from scripts and unconvertible input.
inline std::string fromChatText(const std::string& sjis) {
    bool ascii = true;
    for (const char c : sjis) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    if (ascii) return sjis;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, sjis.c_str(), int(sjis.size()), nullptr, 0) > 0) return sjis;
    const int n = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sjis.c_str(), int(sjis.size()), nullptr, 0);
    if (n <= 0) return sjis;
    std::wstring w(size_t(n), L'\0');
    if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sjis.c_str(), int(sjis.size()), w.data(), n) != n) return sjis;
    return toUtf8(w);
}

class FontList {
public:
    std::vector<std::wstring> families;   // sorted, case-insensitive
    std::vector<std::wstring> japanese;   // the families GDI also reports with SHIFTJIS_CHARSET, sorted the same way
    std::vector<std::wstring> folder;     // the families only the fonts folder has (fontsdir.h), sorted the same way
    std::vector<std::wstring> symbol;     // the families GDI reports with SYMBOL_CHARSET, sorted the same way
    std::wstring firstText;               // the first family that draws basic Latin ("" when none was found: first() is the list's first)
    bool failed = false;                  // the enumeration ran out of memory (enumerate() then returns null)
    size_t duplicates = 0;                // families of folder files that Windows already has (never used)

    // Null when GDI could not be asked (no DC) or listed nothing: a failure, never an empty list.
    static std::shared_ptr<FontList> enumerate() {
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return nullptr;
        std::shared_ptr<FontList> list;
        try { list = std::make_shared<FontList>(); } catch (const std::exception&) { DeleteDC(dc); throw; }
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, &FontList::callback, reinterpret_cast<LPARAM>(list.get()), 0);
        list->finish();
        if (!list->failed) list->firstText = list->pickFirstText([dc](const std::wstring& f) { return drawsBasicLatin(dc, f); });
        DeleteDC(dc);
        return list->failed || list->families.empty() ? nullptr : list;
    }
    void finish() {
        const auto less = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
        const auto same = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; };
        for (auto* f : {&families, &japanese, &symbol}) {
            std::sort(f->begin(), f->end(), less);
            f->erase(std::unique(f->begin(), f->end(), same), f->end());
        }
    }
    std::wstring find(const std::wstring& name) const {
        for (const auto& f : families)
            if (_wcsicmp(f.c_str(), name.c_str()) == 0) return f;
        return std::wstring();
    }
    bool empty() const { return families.empty(); }
    // The new-player default font (a non-empty list): the first family that draws basic Latin, else the list's first.
    const std::wstring& first() const { return firstText.empty() ? families.front() : firstText; }
    bool isSymbol(const std::wstring& name) const {
        for (const auto& f : symbol)
            if (_wcsicmp(f.c_str(), name.c_str()) == 0) return true;
        return false;
    }
    // The first family in list order that is not SYMBOL_CHARSET and that `drawsLatin` accepts; "" when none.
    template <class F> std::wstring pickFirstText(F&& drawsLatin) const {
        for (const auto& f : families)
            if (!isSymbol(f) && drawsLatin(f)) return f;
        return std::wstring();
    }
    // Every one of A-Z a-z 0-9 maps to a glyph (GGI_MARK_NONEXISTING_GLYPHS: 0xFFFF for a missing one), and the face GDI
    // selected is the family asked for (a substitute's glyphs must not answer).
    static bool drawsBasicLatin(HDC dc, const std::wstring& family) {
        if (!dc || family.empty() || family.size() >= LF_FACESIZE) return false;
        LOGFONTW lf{};
        lf.lfHeight = -32;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
        wcsncpy_s(lf.lfFaceName, family.c_str(), _TRUNCATE);
        HFONT font = CreateFontIndirectW(&lf);
        if (!font) return false;
        HGDIOBJ was = SelectObject(dc, font);
        bool ok = false;
        wchar_t face[LF_FACESIZE] = {};
        if (was && GetTextFaceW(dc, LF_FACESIZE, face) > 0 && _wcsicmp(face, family.c_str()) == 0) {
            static const wchar_t kLatin[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            constexpr int n = int(sizeof kLatin / sizeof kLatin[0]) - 1;
            WORD idx[n] = {};
            ok = GetGlyphIndicesW(dc, kLatin, n, idx, GGI_MARK_NONEXISTING_GLYPHS) == DWORD(n);
            for (int i = 0; ok && i < n; i++) ok = idx[i] != 0xFFFF;
        }
        if (was) SelectObject(dc, was);
        DeleteObject(font);
        return ok;
    }
    size_t installedCount() const { return families.size() - (std::min)(folder.size(), families.size()); }
    size_t folderCount() const { return folder.size(); }
    bool fromFolder(const std::wstring& name) const {
        for (const auto& f : folder)
            if (_wcsicmp(f.c_str(), name.c_str()) == 0) return true;
        return false;
    }
    bool isJapanese(const std::wstring& name) const {
        for (const auto& f : japanese)
            if (_wcsicmp(f.c_str(), name.c_str()) == 0) return true;
        return false;
    }

    static bool keep(const wchar_t* face, DWORD fontType, DWORD ntmFlags) {
        if (!face || !*face || face[0] == L'@') return false;
        return (fontType & TRUETYPE_FONTTYPE) != 0 || (ntmFlags & (NTM_PS_OPENTYPE | NTM_TT_OPENTYPE)) != 0;
    }
    // CFF OpenType faces can report DEVICE_FONTTYPE; non-raster faces still expose ntmFlags.
    static bool hasNtmFlags(DWORD fontType) { return (fontType & RASTER_FONTTYPE) == 0; }

    // EnumFontFamiliesExW calls once per family/charset; param is the FontList.
    static int CALLBACK callback(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD fontType, LPARAM param) {
        const auto* ntm = reinterpret_cast<const NEWTEXTMETRICEXW*>(tm);
        const DWORD flags = hasNtmFlags(fontType) ? ntm->ntmTm.ntmFlags : 0;
        if (!keep(lf->lfFaceName, fontType, flags)) return 1;
        auto* list = reinterpret_cast<FontList*>(param);
        try {   // nothing is thrown through GDI's frames: out of memory ends the enumeration as a failure
            list->families.emplace_back(lf->lfFaceName);
            if (lf->lfCharSet == SHIFTJIS_CHARSET) list->japanese.emplace_back(lf->lfFaceName);
            if (lf->lfCharSet == SYMBOL_CHARSET) list->symbol.emplace_back(lf->lfFaceName);
        } catch (const std::exception&) {
            list->failed = true;
            return 0;
        }
        return 1;
    }
};

inline std::string fontsFoundText(const FontList& l) { return std::to_string(l.installedCount()) + " installed, " + std::to_string(l.folderCount()) + " from the folder"; }
// Rescan Fonts' chat line: the same, and " (K duplicates)" when the folder has families Windows already has.
inline std::string fontsFoundLine(const FontList& l) {
    return fontsFoundText(l) + (l.duplicates ? " (" + std::to_string(l.duplicates) + (l.duplicates == 1 ? " duplicate)" : " duplicates)") : std::string());
}

}  // namespace tf
