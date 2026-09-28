// Per-character INI settings, read and written on the disk writer.
// New settings use defaultGroup; files without defaults=2 use legacyGroup for missing keys.
// share_all stays in the character's file and is excluded from resets and presets.
// Font names are UTF-8 escaped to ASCII for the Windows profile API.
#pragma once
#include "persist.h"
#include "atlas.h"
#include "fonttables.h"
#include "spriteatlas.h"
#include "fonts.h"
#include "shape.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tf {

// "6,410" (the panel's en-GB grouping).
inline std::string grouped(int n) {
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return n < 0 ? "-" + s : s;
}

// The Status panel's Fonts readout: "N of 8 custom", or "All native" when every group is switched off.
inline std::string fontsText(int on, int groups) { return on <= 0 ? std::string("All native") : std::to_string(on) + " of " + std::to_string(groups) + " custom"; }

// "uses" after one group whose name is singular (Chat and menus, HUD text, Compass), else "use".
inline const char* useVerb(const std::string& groups) { return groups == "Chat/Items" || groups == "HUD text" || groups == "Compass" ? "uses" : "use"; }
// The chat line for fonts that are not installed: each name, the groups that had it, and what they draw with instead.
struct MissingFont { std::string name, groups, used; };
inline std::string missingFontsText(const std::vector<MissingFont>& m) {
    if (m.empty()) return std::string();
    if (m.size() == 1) return "the font '" + m[0].name + "' is not installed; " + m[0].groups + " " + useVerb(m[0].groups) + " " + m[0].used + ".";
    std::string t = "these fonts are not installed: ";
    for (size_t i = 0; i < m.size(); i++) t += std::string(i ? "; '" : "'") + m[i].name + "' (" + m[i].groups + " " + useVerb(m[i].groups) + " " + m[i].used + ")";
    return t + ".";
}

// Escape UTF-8 bytes and % before the ANSI profile API converts through the system code page.
inline std::string iniEscape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (const unsigned char c : s) {
        if (c >= 0x80 || c == '%') { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
        else o += char(c);
    }
    return o;
}
inline std::string iniUnescape(const std::string& s) {
    const auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hv(s[i + 1]) >= 0 && hv(s[i + 2]) >= 0) {
            o += char(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else o += s[i];
    }
    return o;
}

inline constexpr const char* kDefaultFont = "Segoe UI";   // the fallback for a font that is not installed
inline constexpr const char* kSameChat = "same";           // an older file's "Same as chat" (migration only)
inline constexpr const char* kSameHeadings = "same_headings";   // an older file's "Same as menu headings" (jobs; migration only)
inline constexpr const char* kJpOff = "off";               // chat_jp: kana and kanji stay the game's own (Latin only; Off)
inline constexpr const char* kJpSame = "same";             // chat_jp: the Font draws them too (Same as Font)
inline constexpr const char* kFirstFont = "";   // Empty selects the first basic-Latin family in the sorted list.
inline constexpr const char* kDefaultsKey = "defaults";    // every save writes it: a key the file lacks takes the new-player default
inline constexpr int kDefaultsNew = 2;                     // (a file without it is from before: a key it lacks takes the old default)

// Sharpness: 0=Auto, otherwise 2x/3x/4x. Chat uses 16 px cells per step (Auto: 16 or 32 for font high).
// Sprite Auto is 2x except for nameplates and damage, which follow screen size. Different scales need separate composites.
inline const char* sharpKey(int s) { return s == 2 ? "2" : s == 3 ? "3" : s == 4 ? "4" : "auto"; }
// clampSharp and chatCellFor: texfmt.h.

inline const char* outlineKey(int o) { return o <= 0 ? "off" : o >= 2 ? "thick" : "thin"; }
inline Group groupFromKey(const std::string& k) {
    for (int i = 0; i < kGroupCount; i++) if (k == groupKey(Group(i))) return Group(i);
    return Group::None;
}
// A group's tab title: what the Same Font As drop-down lists and the chat names.
inline const char* groupTitle(Group g) {
    switch (g) {
    case Group::Chat: return "Chat/Items";
    case Group::Labels: return "Labels";
    case Group::NamesHud: return "HUD";
    case Group::Damage: return "Damage";
    case Group::Headings: return "Headings";
    case Group::JobTags: return "Job Tags";
    case Group::Nameplates: return "Nameplates";
    case Group::Compass: return "Compass";
    default: return "";
    }
}
// The group Same Font As names by default: Chat; Labels for Chat; HUD for Nameplates, Headings for Compass (their origins).
inline Group defaultFollow(Group g) {
    switch (g) {
    case Group::Chat: return Group::Labels;
    case Group::Nameplates: return Group::NamesHud;
    case Group::Compass: return Group::Headings;
    default: return Group::Chat;
    }
}
// The groups with Aspect and Size (the game's world text: names and damage numbers, shape.h).
inline bool hasShape(Group g) { return g == Group::Nameplates || g == Group::Damage; }
inline ShapeGroup shapeGroupOf(Group g) { return g == Group::Damage ? ShapeGroup::Damage : ShapeGroup::Plates; }
inline Aspect aspectFromKey(const std::string& v, bool& ok) {
    ok = true;
    if (v == "game") return Aspect::Game;
    if (v == "4:3" || v == "43") return Aspect::FourThree;
    if (v == "true") return Aspect::True;
    ok = false;
    return Aspect::Game;
}
// The group a split-off group was split from (its values in a file from before the split), Group::None for the others.
inline Group splitFrom(Group g) { return g == Group::Nameplates ? Group::NamesHud : g == Group::Compass ? Group::Headings : Group::None; }

struct GroupSettings {
    bool on = true;              // the group draws with TrueFont (off: the game's own font; the values below are kept)
    bool same = false;           // Same font as: draws with the family at the end of its follow chain
    Group follow = Group::Chat;  // the group Same font as names (kept while `same` is clear)
    std::string font;            // the group's own family (UTF-8), kept while it follows
    int weight = 700;            // 400 Regular, 600 Semibold, 700 Bold
    bool fauxBold = false;
    int outline = 1;             // 0 Off, 1 Thin, 2 Thick (the seven sprite groups)
    bool italic = false;
    bool shadow = false;         // Shadow (the seven sprite groups)
    int sharp = 0;               // Sharpness: 0 Auto, 2, 3 or 4 (see sharpKey)
    Aspect aspect = Aspect::Game;   // Aspect (Nameplates and Damage numbers only, shape.h)
    int size = kSizeDefault;     // Size in percent of the game's (the same two groups)
    bool firstFont() const { return font.empty(); }   // the first family in the font list (kFirstFont)
    bool operator==(const GroupSettings& o) const {
        return on == o.on && same == o.same && follow == o.follow && font == o.font && weight == o.weight && fauxBold == o.fauxBold &&
               outline == o.outline && italic == o.italic && shadow == o.shadow && sharp == o.sharp && aspect == o.aspect && size == o.size;
    }
    bool operator!=(const GroupSettings& o) const { return !(*this == o); }
};

struct Settings {
    bool enabled = true;         // TrueFont on (/truefont on | off)
    GroupSettings g[kGroupCount];
    int gammaTenths = 10;        // Chat and menus' brightness 0.6..1.6 in tenths (6..16)
    std::string chatJp = kJpOff;    // Chat and menus' Japanese font (UTF-8): "off", "same" (the Font) or a family for kana and kanji
    bool hinting = true;         // Quality's Hinting: off draws every group's unhinted outlines
    Engine engine = Engine::Windows;   // Quality's Engine (ftface.h): every group's
    bool compress = false;       // Quality's Compress: the textures as DXT3
    bool keepOriginal = true;   // Retain the native Chat atlas readback.
    bool missRedraw = false;     // Redraw "Miss!" (Damage numbers)
    bool migratedFromV1 = false; // load(): the older keys were read (the next save writes the per-group keys)
    bool migratedJp = false;   // Migrated scripts/chat_jp_font to chat_jp.
    bool migratedSame = false;   // Migrated legacy font links to <g>_follow.
    bool migratedSplit = false;   // Migrated Nameplates/Compass from their former parent groups.
    bool migratedCells = false;   // Migrated cells to chat_sharp.
    bool olderFile = false;   // Missing keys use legacy defaults.
    std::string followBroken;    // clamp(): the ticked chains it broke ("jobs -> headings -> jobs"; "" = none); not compared

    // The new-player defaults (a character with no file, Reset, Reset all).
    Settings() { for (int i = 0; i < kGroupCount; i++) g[i] = defaultGroup(Group(i)); }
    // The defaults a file from before defaults=2 was written against (a key such a file lacks takes these).
    static Settings legacy() {
        Settings s;
        for (int i = 0; i < kGroupCount; i++) s.g[i] = legacyGroup(Group(i));
        return s;
    }

    // The new-player defaults: off, on the first font, Same Font As clear; the rest as legacyGroup.
    static GroupSettings defaultGroup(Group grp) {
        GroupSettings d = legacyGroup(grp);
        d.on = false;
        d.same = false;
        d.font = kFirstFont;
        return d;
    }
    // Preserve missing-key defaults for files written before defaults=2.
    static GroupSettings legacyGroup(Group grp) {
        GroupSettings d;
        d.follow = defaultFollow(grp);
        switch (grp) {
        case Group::Chat: d.font = kDefaultFont; d.weight = 600; break;
        case Group::Labels: d.font = kDefaultFont; d.same = true; break;
        case Group::NamesHud: d.font = "Arial"; d.italic = true; break;
        case Group::Damage: d.font = "Arial"; d.italic = true; break;
        case Group::Headings: d.font = "Cambria"; d.italic = true; break;
        case Group::JobTags: d.font = "Cambria"; d.same = true; d.follow = Group::Headings; break;
        case Group::Nameplates: d.font = "Arial"; d.italic = true; d.same = true; break;   // follows HUD (defaultFollow)
        case Group::Compass: d.font = "Cambria"; d.italic = true; d.same = true; break;    // follows Menu headings
        default: break;
        }
        return d;
    }
    // The group `grp` follows while its Same Font As box is ticked, Group::None when clear.
    Group follows(Group grp) const {
        if (int(grp) >= kGroupCount) return Group::None;
        const GroupSettings& x = g[int(grp)];
        return x.same && int(x.follow) < kGroupCount ? x.follow : Group::None;
    }
    bool followsTo(Group id, Group target) const {
        Group x = follows(id);
        for (int n = 0; x != Group::None && n < kGroupCount; x = follows(x), n++) if (x == target) return true;
        return false;
    }
    // The groups `id` may name: not itself, and not one that already follows it (directly or down a chain); in the
    // order the tabs show them (kGroupOrder).
    std::vector<Group> followIds(Group id) const {
        std::vector<Group> v;
        for (const Group x : kGroupOrder) if (x != id && !followsTo(x, id)) v.push_back(x);
        return v;
    }
    // The group whose own font `grp` draws with: the end of its ticked chain (at most eight steps; clamp breaks loops).
    Group followEnd(Group grp) const {
        Group at = grp;
        for (int n = 0; n < kGroupCount && follows(at) != Group::None; n++) at = follows(at);
        return at;
    }
    // `grp`'s own font is drawn: a group that is on ends its chain at `grp` (itself, or one that follows it, even while
    // `grp` is switched off).
    bool ownFontDrawn(Group grp) const {
        for (int h = 0; h < kGroupCount; h++) if (g[h].on && followEnd(Group(h)) == grp) return true;
        return false;
    }
    // The groups drawn with `grp`'s own font, by their full names in tab order ("Menu headings, Job and level tags").
    std::string drawnWithText(Group grp) const {
        std::string t;
        for (const Group h : kGroupOrder)
            if (g[int(h)].on && followEnd(h) == grp) t += std::string(t.empty() ? "" : ", ") + groupName(h);
        return t;
    }
    // Default comparisons and resets include group-specific options but preserve the master enabled flag.
    bool groupIsDefault(Group grp) const {
        const Settings d;
        const int i = int(grp);
        return g[i] == d.g[i] && (grp != Group::Chat || (gammaTenths == d.gammaTenths && chatJp == d.chatJp && keepOriginal == d.keepOriginal)) &&
               (grp != Group::Damage || missRedraw == d.missRedraw);
    }
    // Clear Same Font As if the default target would create a cycle.
    void resetGroup(Group grp) {
        resetGroupValues(grp);
        GroupSettings& x = g[int(grp)];
        if (x.same && followsTo(x.follow, grp)) x.same = false;
    }
    bool qualityIsDefault() const { const Settings d; return engine == d.engine && hinting == d.hinting && compress == d.compress; }
    void resetQuality() {
        const Settings d;
        engine = d.engine;
        hinting = d.hinting;
        compress = d.compress;
    }
    bool allDefault() const {
        for (int i = 0; i < kGroupCount; i++) if (!groupIsDefault(Group(i))) return false;
        return qualityIsDefault();
    }
    void resetAll() {
        for (int i = 0; i < kGroupCount; i++) resetGroupValues(Group(i));   // every default at once: no loop to avoid
        resetQuality();
    }
    int groupsOn() const {
        int n = 0;
        for (const auto& x : g) n += x.on ? 1 : 0;
        return n;
    }
    std::string groupsOffText() const {
        std::string t;
        for (const Group x : kGroupOrder)
            if (!g[int(x)].on) t += std::string(t.empty() ? "" : ", ") + groupName(x);
        return t;
    }
    bool anySpriteGroupOn() const {
        for (int i = int(Group::Labels); i < kGroupCount; i++) if (g[i].on) return true;
        return false;
    }
    GroupSettings& chat() { return g[int(Group::Chat)]; }
    const GroupSettings& chat() const { return g[int(Group::Chat)]; }
    // Chat and menus draws a Japanese Font of its own (not Off, not Same as Font) while it is on.
    bool jpDrawn() const { return chat().on && !jpOff() && !jpSame(); }

    static constexpr const char* kSection = "truefont";
    static int clampWeight(int w) { return w >= 650 ? 700 : w >= 500 ? 600 : 400; }
    static int clampGamma(int t) { return t < 6 ? 6 : t > 16 ? 16 : t; }
    // A group's own font is a family of 1..120 bytes (never the older "same" / "same_headings"); "" is kFirstFont.
    static bool fontAllowed(const std::string& f) { return !f.empty() && f.size() <= 120 && f != kSameChat && f != kSameHeadings; }
    // The Japanese font: "off", "same", or a family of 1..120 bytes.
    static bool jpAllowed(const std::string& f) { return f == kJpOff || f == kJpSame || (!f.empty() && f.size() <= 120 && f != kSameHeadings); }
    bool jpSame() const { return chatJp == kJpSame; }
    bool jpOff() const { return chatJp == kJpOff; }
    bool allScripts() const { return !jpOff(); }
    // The group an older file's font value follows: "same" (any group but Chat) Chat and menus, "same_headings" (Job and
    // level tags) Menu headings; Group::None for anything else, which is not a follow.
    static Group legacyFollow(Group grp, const std::string& f) {
        if (f == kSameChat && grp != Group::Chat) return Group::Chat;
        if (f == kSameHeadings && grp == Group::JobTags) return Group::Headings;
        return Group::None;
    }
    void clamp() {
        for (int i = 0; i < kGroupCount; i++) {
            GroupSettings& x = g[i];
            x.weight = clampWeight(x.weight);
            x.outline = x.outline < 0 ? 0 : x.outline > 2 ? 2 : x.outline;
            x.sharp = clampSharp(x.sharp);
            if (!x.firstFont() && !fontAllowed(x.font)) x.font = kFirstFont;
            if (int(x.follow) >= kGroupCount || x.follow == Group(i)) x.follow = defaultFollow(Group(i));
            if (!hasShape(Group(i)) || int(x.aspect) > int(Aspect::True)) x.aspect = Aspect::Game;
            if (!hasShape(Group(i)) || !sizeAllowed(x.size)) x.size = kSizeDefault;
        }
        // Break cycles in group order for deterministic results.
        for (int i = 0; i < kGroupCount; i++) {
            Group path[kGroupCount + 1] = {Group(i)};
            int n = 1;
            for (Group at = Group(i); g[int(at)].same && n <= kGroupCount; ) {
                const Group next = g[int(at)].follow;
                bool loops = false;
                for (int k = 0; k < n; k++) loops = loops || path[k] == next;
                if (loops) {
                    std::string t;
                    for (int k = 0; k < n; k++) t += std::string(groupKey(path[k])) + " -> ";
                    followBroken += (followBroken.empty() ? "" : "; ") + t + groupKey(next);
                    g[int(at)].same = false;
                    break;
                }
                path[n++] = next;
                at = next;
            }
        }
        gammaTenths = clampGamma(gammaTenths);
        if (!jpAllowed(chatJp)) chatJp = kJpOff;
    }

    bool operator==(const Settings& o) const {
        for (int i = 0; i < kGroupCount; i++) if (g[i] != o.g[i]) return false;
        return enabled == o.enabled && gammaTenths == o.gammaTenths && chatJp == o.chatJp && hinting == o.hinting && engine == o.engine && compress == o.compress &&
               keepOriginal == o.keepOriginal && missRedraw == o.missRedraw;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }

    // Missing files leave settings unchanged. Files without defaults use legacy group defaults for missing keys.
    // Invalid values take the group default; all values are clamped.
    bool load(const std::string& path) {
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        const char* f = path.c_str();
        const auto has = [&](const char* key) {
            char b[8] = {};
            return GetPrivateProfileStringA(kSection, key, "\x01", b, sizeof b, f) > 0 && b[0] != '\x01';
        };
        const auto str = [&](const char* key, const std::string& v) {
            char b[512] = {};
            GetPrivateProfileStringA(kSection, key, v.c_str(), b, sizeof b, f);
            return std::string(b);
        };
        // An integer key: its value when the whole text parses, `bad` when it does not, `v` when the key is absent.
        const auto integer = [&](const char* key, int v, int bad) {
            if (!has(key)) return v;
            const std::string t = str(key, "");
            char* end = nullptr;
            const long n = std::strtol(t.c_str(), &end, 10);
            return (!t.empty() && end && *end == 0) ? int(n) : bad;
        };
        const auto boolean = [&](const char* key, bool v, bool bad) {
            const int n = integer(key, v ? 1 : 0, bad ? 1 : 0);
            return n == 0 ? false : n == 1 ? true : bad;
        };
        enabled = num(f, "enabled", enabled ? 1 : 0) != 0;
        migratedFromV1 = false;
        migratedSame = false;
        migratedSplit = false;
        migratedCells = false;
        olderFile = integer(kDefaultsKey, 0, 0) < kDefaultsNew;
        if (olderFile) {   // the existing file keeps its meaning: what it lacks is the old default
            const Settings l = legacy();
            for (int i = 0; i < kGroupCount; i++) g[i] = l.g[i];
        }
        const auto hasGroup = [&](Group grp) {
            const std::string k = std::string(groupKey(grp)) + "_";
            for (const char* v : {"on", "same", "follow", "font", "weight", "size", "offset", "faux_bold", "outline", "shadow", "italic", "sharp", "aspect"})
                if (has((k + v).c_str())) return true;
            return false;
        };
        bool v1Format = olderFile;
        for (int i = 0; i < kGroupCount && v1Format; i++) v1Format = !hasGroup(Group(i));
        bool splitNew[kGroupCount] = {};
        for (int i = 0; i < kGroupCount; i++) splitNew[i] = splitFrom(Group(i)) != Group::None && !hasGroup(Group(i));
        if (!has("chat_font"))
            for (const char* old : {"font", "weight", "size_adjust", "v_offset", "faux_bold", "gamma"}) migratedFromV1 = migratedFromV1 || has(old);
        for (int i = 0; i < kGroupCount; i++) {
            const Group grp = Group(i);
            const std::string k = groupKey(grp);
            const GroupSettings def = olderFile ? legacyGroup(grp) : defaultGroup(grp);
            GroupSettings& x = g[i];
            const bool v1 = grp == Group::Chat && migratedFromV1;   // the older keys are Chat and menus
            const auto key = [&](const char* v11, const char* old) { return v1 ? std::string(old) : k + "_" + v11; };
            const bool hasFont = has(key("font", "font").c_str());
            Group legacy = Group::None;
            if (hasFont) {
                const std::string fv = iniUnescape(str(key("font", "font").c_str(), ""));
                legacy = legacyFollow(grp, fv);
                x.font = fontAllowed(fv) ? fv : def.font;   // an older "same": the group's own default family
            }
            if (legacy != Group::None) {   // an older Same as chat / Same as menu headings
                x.same = true;
                x.follow = legacy;
                migratedSame = true;
            } else {
                // A font but no <g>_same (a file from before Same font as): the font is the group's own.
                x.same = boolean((k + "_same").c_str(), hasFont ? false : x.same, def.same);
                const std::string fk = k + "_follow";
                if (has(fk.c_str())) {
                    const Group fg = groupFromKey(str(fk.c_str(), ""));
                    x.follow = fg == Group::None ? def.follow : fg;
                }
            }
            x.weight = integer(key("weight", "weight").c_str(), x.weight, def.weight);
            x.fauxBold = boolean(key("faux_bold", "faux_bold").c_str(), x.fauxBold, def.fauxBold);
            if (grp != Group::Chat) {
                const std::string ok = k + "_outline";
                if (has(ok.c_str())) {
                    const std::string o = str(ok.c_str(), "");
                    x.outline = o == "off" || o == "0" ? 0 : o == "thin" || o == "1" ? 1 : o == "thick" || o == "2" ? 2 : def.outline;
                }
                x.shadow = boolean((k + "_shadow").c_str(), x.shadow, def.shadow);
            }
            x.italic = boolean((k + "_italic").c_str(), x.italic, def.italic);
            x.on = boolean((k + "_on").c_str(), x.on, def.on);   // no key (an older file): on
            const std::string sk = k + "_sharp";
            if (has(sk.c_str())) {
                const std::string v = str(sk.c_str(), "");
                x.sharp = v == "auto" ? 0 : v == "2" ? 2 : v == "3" ? 3 : v == "4" ? 4 : def.sharp;
            } else if (grp == Group::Chat && has("cells")) {   // the older Quality Cells: High (32 px) is 2x, Standard Auto
                x.sharp = num(f, "cells", 16) >= 24 ? 2 : 0;
                migratedCells = true;
            }
            if (hasShape(grp)) {   // Aspect and Size: absent (an older file) or not allowed is the default
                const std::string ak = k + "_aspect";
                if (has(ak.c_str())) {
                    bool ok = false;
                    x.aspect = aspectFromKey(str(ak.c_str(), ""), ok);
                    if (!ok) x.aspect = def.aspect;
                } else x.aspect = def.aspect;
                const int sz = integer((k + "_size").c_str(), def.size, def.size);
                x.size = sizeAllowed(sz) ? sz : def.size;   // the removed px Size (-2..+2) is no percent: 100
            }
        }
        // Nameplates and Compass in a file from before the split: they drew with HUD's and Menu headings' values, so they
        // take them (on, font, style) and follow them, and look as they did.
        for (int i = 0; i < kGroupCount; i++) {
            if (!splitNew[i]) continue;
            const Group from = splitFrom(Group(i));
            g[i] = g[int(from)];
            g[i].same = true;
            g[i].follow = from;
            migratedSplit = true;
        }
        // A v1.0 file (no group's keys) had Chat and menus only; the menu and HUD groups it never drew stay the game's.
        if (v1Format)
            for (int i = 0; i < kGroupCount; i++)
                if (Group(i) != Group::Chat) g[i].on = false;
        const char* gk = migratedFromV1 ? "gamma" : "chat_gamma";
        const std::string gt = str(gk, gammaText());
        char* end = nullptr;
        const double gv = std::strtod(gt.c_str(), &end);
        if (end && end != gt.c_str()) gammaTenths = int(std::lround(gv * 10.0));   // text that does not parse keeps the value
        migratedJp = false;
        if (has("chat_jp")) {
            const std::string jp = iniUnescape(str("chat_jp", ""));
            chatJp = jpAllowed(jp) ? jp : std::string(kJpOff);
        } else if (has("scripts") || has("chat_jp_font")) {   // the older Scripts and Japanese font pair
            const bool all = has("scripts") ? str("scripts", "") == "all" : allScripts();
            std::string fam = has("chat_jp_font") ? iniUnescape(str("chat_jp_font", "")) : jpOff() ? std::string(kJpSame) : chatJp;
            if (!jpAllowed(fam) || fam == kJpOff) fam = kJpSame;
            chatJp = all ? fam : std::string(kJpOff);
            migratedJp = true;
        }
        hinting = boolean("hinting", hinting, true);
        {   // absent (an older file) or not one of the three: windows
            Engine e = Engine::Windows;
            engine = has("engine") && engineFromKey(str("engine", ""), e) ? e : Engine::Windows;
        }
        compress = boolean("compress", compress, false);
        keepOriginal = boolean("keep_original", keepOriginal, true);
        missRedraw = boolean("miss_redraw", missRedraw, false);
        clamp();
        return true;
    }
    std::string gammaText() const {
        char b[16];
        _snprintf_s(b, sizeof b, _TRUNCATE, "%d.%d", gammaTenths / 10, gammaTenths % 10);
        return b;
    }
    // Every key (`withEnabled` false leaves the TrueFont switch out), and the older keys' removals.
    IniSnapshot snapshot(bool withEnabled = true) const {
        IniSnapshot v;
        const auto put = [&](const std::string& key, std::string value) { v.push_back(IniValue{kSection, key, std::move(value)}); };
        if (withEnabled) put("enabled", enabled ? "1" : "0");
        for (int i = 0; i < kGroupCount; i++) {
            const Group grp = Group(i);
            const std::string k = std::string(groupKey(grp)) + "_";
            const GroupSettings& x = g[i];
            put(k + "on", x.on ? "1" : "0");
            put(k + "same", x.same ? "1" : "0");
            put(k + "follow", groupKey(x.follow));
            if (x.firstFont()) {   // the first font in the list: no key until the player picks one
                IniValue r{kSection, k + "font", std::string()};
                r.remove = true;
                v.push_back(r);
            } else put(k + "font", iniEscape(x.font));
            put(k + "weight", std::to_string(x.weight));
            for (const char* gone : {"size", "offset"}) {   // the removed Size and Vertical Offset: never written back
                if (hasShape(grp) && std::strcmp(gone, "size") == 0) continue;   // plates_size and damage_size are the new Size
                IniValue r{kSection, k + gone, std::string()};
                r.remove = true;
                v.push_back(r);
            }
            put(k + "faux_bold", x.fauxBold ? "1" : "0");
            if (grp != Group::Chat) put(k + "outline", outlineKey(x.outline));
            if (grp != Group::Chat) put(k + "shadow", x.shadow ? "1" : "0");
            put(k + "italic", x.italic ? "1" : "0");
            put(k + "sharp", sharpKey(x.sharp));
            if (hasShape(grp)) {
                put(k + "aspect", aspectKey(x.aspect));
                put(k + "size", std::to_string(x.size));
            }
        }
        put("chat_gamma", gammaText());
        put("chat_jp", iniEscape(chatJp));
        put("hinting", hinting ? "1" : "0");
        put("engine", engineKey(engine));
        put("compress", compress ? "1" : "0");
        put("keep_original", keepOriginal ? "1" : "0");
        put("miss_redraw", missRedraw ? "1" : "0");
        for (const char* gone : {"on", "same", "follow", "font", "weight", "size", "offset", "faux_bold", "outline", "shadow", "italic", "sharp"}) {
            IniValue r{kSection, std::string("tags_") + gone, std::string()};   // the removed Item tags group's keys (never read)
            r.remove = true;
            v.push_back(r);
        }
        // Remove legacy keys and mark defaults=2 only after all replacement values have been written.
        for (const char* old : {"font", "weight", "size_adjust", "v_offset", "faux_bold", "gamma", "scripts", "chat_jp_font", "cells"}) {
            IniValue r{kSection, old, std::string()};
            r.remove = r.last = true;
            v.push_back(r);
        }
        IniValue d{kSection, kDefaultsKey, std::to_string(kDefaultsNew)};
        d.last = true;
        v.push_back(d);
        return v;
    }
    // `withEnabled` false: not the TrueFont switch (Every Character keeps it per character). `error`: the first failed
    // write's GetLastError (every key is still tried; the `last` entries only when all the others were written).
    bool save(const std::string& path, bool withEnabled = true, DWORD* error = nullptr, IniWriteFn write = &WritePrivateProfileStringA) const {
        DWORD e = 0;
        const bool ok = writeIniEntries(path, snapshot(withEnabled), e, [](const IniValue&) {}, write);
        if (error) *error = e;
        return ok;
    }
    // The Chat and menus atlas options for the family actually used and the Japanese Font's ("" = the same family).
    AtlasOptions options(const std::wstring& family, const std::wstring& jpFamily = std::wstring()) const {
        AtlasOptions o;
        const GroupSettings& c = chat();
        o.family = family;
        o.jpFamily = jpFamily == family ? std::wstring() : jpFamily;
        o.hinting = hinting;
        o.engine = engine;
        o.weight = c.weight;
        o.fauxBold = c.fauxBold;
        o.gamma = gammaTenths / 10.0;
        o.cell = chatCellFor(c.sharp);   // Sharpness (Auto 16; the installer makes Auto 32 under the game's high font)
        o.allScripts = allScripts();
        o.compress = compress;
        o.italic = c.italic;
        return o;
    }
    // The options the seven sprite groups share (Hinting, Engine, Compress, Redraw "Miss!") and each group's Sharpness.
    void spriteQuality(SpriteOptions& o) const {
        o.hinting = hinting;
        o.engine = engine;
        o.compress = compress;
        o.missRedraw = missRedraw;
        for (int i = int(Group::Labels); i < kGroupCount; i++) o.sharp[i] = clampSharp(g[i].sharp);
    }
    GroupStyle style(Group grp, const std::wstring& family) const {
        const GroupSettings& x = g[int(grp)];
        GroupStyle st;
        st.family = family;
        st.weight = x.weight;
        st.italic = x.italic;
        st.fauxBold = x.fauxBold;
        st.outline = Outline(x.outline);
        st.shadow = x.shadow;
        return st;
    }
    std::string text() const {
        std::string t = enabled ? "enabled 1" : "enabled 0";
        for (int i = 0; i < kGroupCount; i++) {
            const GroupSettings& x = g[i];
            char b[300];
            const std::string follow = x.same ? std::string(" same as ") + groupKey(x.follow) + "," : std::string();
            _snprintf_s(b, sizeof b, _TRUNCATE, "; %s%s%s \"%s\" %d%s%s%s%s sharp %s", groupKey(Group(i)), x.on ? "" : " OFF", follow.c_str(),
                        x.firstFont() ? "(the first font)" : x.font.c_str(), x.weight, x.fauxBold ? " faux" : "",
                        i ? (std::string(" ") + outlineKey(x.outline)).c_str() : "", i && x.shadow ? " shadow" : "", x.italic ? " italic" : "", sharpKey(x.sharp));
            t += b;
            if (hasShape(Group(i))) t += std::string(" aspect ") + aspectKey(x.aspect) + " size " + std::to_string(x.size);
        }
        char e[400];
        _snprintf_s(e, sizeof e, _TRUNCATE, "; gamma %s, jp \"%s\", engine %s, hinting %d, compress %d, keep_original %d, miss_redraw %d", gammaText().c_str(), chatJp.c_str(),
                    engineKey(engine), hinting, compress, keepOriginal, missRedraw);
        return t + e;
    }

private:
    void resetGroupValues(Group grp) {
        const Settings d;
        g[int(grp)] = d.g[int(grp)];
        if (grp == Group::Chat) { gammaTenths = d.gammaTenths; chatJp = d.chatJp; keepOriginal = d.keepOriginal; }
        if (grp == Group::Damage) missRedraw = d.missRedraw;
    }
    static int num(const char* f, const char* key, int v) { return int(GetPrivateProfileIntA(kSection, key, v, f)); }
};

// Aspect and Size follow the master switch, independently of font replacement, so they also apply to native fonts.
inline ShapeWant shapeWantOf(const Settings& s, Group grp) {
    const GroupSettings& gs = s.g[int(grp)];
    ShapeWant w;
    w.on = s.enabled;
    w.aspect = gs.aspect;
    w.size = gs.size;
    w.gameFont = !gs.on;
    return w;
}
// Keep Aspect and Size available for native nameplates and damage; Sharpness requires replacement glyphs.
// shape: 0=available, 1=owned by another plugin, 2=unsupported client.
inline constexpr const char* kGameFontNote = "Aspect and Size work with the game's font too.";
inline constexpr const char* kGameFontSharpTip = "Not for the game's font: its letters are pictures, so there is no more detail to draw. Tick TrueFont above to use it.";
inline constexpr const char* kGameFontOnlySharpTip = "Not for the game's font: its letters are pictures, so there is no more detail to draw.";
struct DetailState {
    bool dim = false;        // the Detail section greyed (every control in it)
    bool gameFont = false;   // the letters are the game's: Sharpness greyed (label and radios) with sharpTip
    bool note = false;       // kGameFontNote under Aspect and Size (only while they can be used)
    bool reset = true;       // Reset to Defaults enabled (the character's settings read, and something on the tab works here)
    const char* sharpTip = nullptr;   // Sharpness' tooltip while gameFont
};
inline DetailState detailState(Group grp, bool off, bool available, bool groupOn, int shape) {
    DetailState d;
    const bool shapeHere = hasShape(grp) && shape != 2;       // Aspect and Size exist on this game version
    const bool usable = available || shapeHere;               // something on the tab works here
    const bool gameFont = hasShape(grp) && (!groupOn || !available);
    d.dim = off || !usable || (!groupOn && !gameFont);
    d.gameFont = gameFont;
    d.note = gameFont && shape == 0;                          // not under a refusal of Aspect and Size
    d.reset = !off && usable;
    d.sharpTip = !gameFont ? nullptr : available ? kGameFontSharpTip : kGameFontOnlySharpTip;   // ticking TrueFont helps only where its fonts work
    return d;
}
// The line under a not-available group's note on the Nameplates and Damage numbers tabs, while Aspect and Size can work.
inline constexpr const char* kShapeStillNote = "Aspect and Size still work.";
inline const char* unavailableShapeNote(Group grp, int shape) { return hasShape(grp) && shape == 0 ? kShapeStillNote : ""; }
// /tfont on retries failed sprite records; rebuild does not revalidate them.
inline constexpr const char* kGroupFailedNote = "Could not be built; /tfont on tries again. See the log.";
// /tfont reset's reply. The defaults switch every font group off (defaultGroup), so the game's own fonts come back.
inline std::string resetReplyText(bool enabled) {
    return std::string("reset: every setting is back to its default; every font group is switched off") +
           (enabled ? ", so the game's own fonts are back (tick a group's TrueFont box in /truefont to draw with it)." : " (TrueFont is off).");
}
// Save an explicit selection even when it matches the current default or missing-font substitute.
inline bool fontPickSaves(bool missing, bool firstFont) { return missing || firstFont; }
// The missing-font chat line names a group's own font once something draws with it: TrueFont on, the font drawn, missing,
// and not said yet since load.
inline bool sayMissingFont(const Settings& s, Group g, bool missing, bool said) { return missing && !said && s.enabled && s.ownFontDrawn(g); }
inline bool sayMissingJp(const Settings& s, bool missing, bool said) { return missing && !said && s.enabled && s.jpDrawn(); }

}  // namespace tf
