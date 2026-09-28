// Sprite-font rectangles, characters and groups. Generated tables cover English and Japanese layouts.
// Live rects overlapping owned glyphs must match the language's table or the record stays native.
// Unowned symbols and item tags stay native; Miss! is opt-in. Nameplates reuse HUD rects through a separate composite.
// Glyph variants share a slot: fit within their intersection and clear their union. Shadow-only variants
// do not constrain that intersection. Damage digits share a quad height, so their metrics use screen space.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace tf {

// The values are stored (the shipped table's group byte): Nameplates and Compass come after the first six so no older
// value moves. The order the player sees is kGroupOrder.
enum class Group : uint8_t { Chat = 0, Labels, NamesHud, Damage, Headings, JobTags, Nameplates, Compass, None };
inline constexpr int kGroupCount = 8;   // Chat .. Compass (None is not a group)
inline constexpr Group kGroupOrder[kGroupCount] = {Group::Chat,     Group::Labels,   Group::Nameplates, Group::NamesHud,
                                                   Group::Damage,   Group::Headings, Group::JobTags,    Group::Compass};
// NamesHud is the HUD group ("HUD" tab, "HUD Text" section); its settings keys stay names_.
inline const char* groupName(Group g) {
    switch (g) {
    case Group::Chat: return "Chat/Items";
    case Group::Labels: return "Menu labels";
    case Group::NamesHud: return "HUD text";
    case Group::Damage: return "Damage numbers";
    case Group::Headings: return "Menu headings";
    case Group::JobTags: return "Job and level tags";
    case Group::Nameplates: return "Nameplates";
    case Group::Compass: return "Compass";
    default: return "none";
    }
}
inline const char* groupKey(Group g) {
    switch (g) {
    case Group::Chat: return "chat";
    case Group::Labels: return "labels";
    case Group::NamesHud: return "names";
    case Group::Damage: return "damage";
    case Group::Headings: return "headings";
    case Group::JobTags: return "jobs";
    case Group::Nameplates: return "plates";
    case Group::Compass: return "compass";
    default: return "none";
    }
}

enum class SpriteRec : uint8_t { FontFont = 0, Menu2fon, Mn10font, News };
inline constexpr int kSpriteRecCount = 4;
struct SpriteRecInfo { const char* name; uint16_t w, h; const char* shortName; };
// The records TrueFont replaces: name, the +24/+26 size they must report (51.DAT's sizes). news is DXT3 (peak alpha 88h).
inline constexpr SpriteRecInfo kSpriteRecs[kSpriteRecCount] = {
    {"font    font    ", 256, 256, "font font"},
    {"menu    menu2fon", 256, 256, "menu2fon"},
    {"menu    mn10font", 128, 128, "mn10font"},
    {"menu    news    ", 32, 32, "news"},
};
// Logged only: the icon texture beside them, never replaced.
inline constexpr SpriteRecInfo kUstatshd = {"menu    ustatshd", 256, 256, "ustatshd"};

struct Rect {
    int16_t u = 0, v = 0, w = 0, h = 0;
    int x1() const { return u + w; }
    int y1() const { return v + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    int area() const { return empty() ? 0 : int(w) * int(h); }
    bool operator==(const Rect& o) const { return u == o.u && v == o.v && w == o.w && h == o.h; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
    bool operator<(const Rect& o) const { return v != o.v ? v < o.v : u != o.u ? u < o.u : w != o.w ? w < o.w : h < o.h; }
    bool overlaps(const Rect& o) const { return !empty() && !o.empty() && u < o.x1() && o.u < x1() && v < o.y1() && o.v < y1(); }
    bool contains(const Rect& o) const { return !o.empty() && o.u >= u && o.v >= v && o.x1() <= x1() && o.y1() <= y1(); }
    static Rect fromEdges(int x0, int y0, int x1, int y1) { Rect r; r.u = int16_t(x0); r.v = int16_t(y0); r.w = int16_t(std::max(0, x1 - x0)); r.h = int16_t(std::max(0, y1 - y0)); return r; }
    Rect intersect(const Rect& o) const { return fromEdges(std::max<int>(u, o.u), std::max<int>(v, o.v), std::min(x1(), o.x1()), std::min(y1(), o.y1())); }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return fromEdges(std::min<int>(u, o.u), std::min<int>(v, o.v), std::max(x1(), o.x1()), std::max(y1(), o.y1()));
    }
};

struct FontEntry {
    int16_t u, v, w, h;
    Group group;              // None: a foreign rect (not identified; overlaps an owned glyph)
    uint8_t cls;              // size class
    const wchar_t* text;      // the character or word; nullptr for foreign rects
    Rect rect() const { Rect r; r.u = u; r.v = v; r.w = w; r.h = h; return r; }
    bool owned() const { return group != Group::None && text; }
};

// FNV-1a over the owned entries in table order (u, v, w, h little-endian, group, class, UTF-16 text + 0); hashes a
// table and a generated set alike.
inline uint32_t fnv1a(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
inline uint32_t hashOwned(const FontEntry* e, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        if (!e[i].owned()) continue;
        const uint8_t head[10] = {uint8_t(e[i].u), uint8_t(uint16_t(e[i].u) >> 8), uint8_t(e[i].v), uint8_t(uint16_t(e[i].v) >> 8), uint8_t(e[i].w), uint8_t(uint16_t(e[i].w) >> 8),
                                  uint8_t(e[i].h), uint8_t(uint16_t(e[i].h) >> 8), uint8_t(e[i].group), e[i].cls};
        h = fnv1a(h, head, sizeof head);
        for (const wchar_t* t = e[i].text;; ++t) {
            const uint8_t cu[2] = {uint8_t(uint16_t(*t)), uint8_t(uint16_t(*t) >> 8)};
            h = fnv1a(h, cu, 2);
            if (!*t) break;
        }
    }
    return h;
}

struct Anchor { int16_t x; const wchar_t* text; };   // text nullptr: a glyph TrueFont does not own
struct AnchorRow {
    SpriteRec rec;
    Group group;
    uint8_t cls;
    int16_t y;                 // the row's top as the anchors give it
    int16_t yUp, yDown;        // a rect's top may lie in [y - yUp, y + yDown]
    int16_t xMin, xMax;        // the row's horizontal range (a rect's u must lie inside)
    int16_t tolX;              // nearest anchor within +-tolX
    int16_t minH;              // smaller rects are not this row's glyphs
    const Anchor* a;
    size_t n;
};

namespace anchors {
// font font small bold rows (Menu labels).
inline constexpr Anchor kL0[] = {{0, L" "}, {12, L"!"}, {21, L"\""}, {30, L"#"}, {40, L"$"}, {50, L"%"}, {60, L"&"}, {72, L"'"}, {81, L"("}, {93, L")"}, {100, L"*"}, {111, L"+"}};
inline constexpr Anchor kL13[] = {{1, L","}, {11, L"-"}, {23, L"."}, {30, L"/"}, {40, L"0"}, {51, L"1"}, {61, L"2"}, {70, L"3"}, {80, L"4"}, {91, L"5"}, {101, L"6"}, {111, L"7"}};
inline constexpr Anchor kL26[] = {{0, L"8"}, {9, L"9"}, {19, L":"}, {24, L";"}, {30, L"<"}, {39, L"="}, {48, L">"}, {56, L"?"}, {66, L"@"}, {77, L"A"}, {87, L"B"}, {97, L"C"}, {107, L"D"}, {117, L"\u2026"}};
inline constexpr Anchor kL39[] = {{0, L"E"}, {10, L"F"}, {20, L"G"}, {31, L"H"}, {42, L"I"}, {48, L"J"}, {58, L"K"}, {69, L"L"}, {80, L"M"}, {91, L"N"}, {102, L"O"}, {113, L"P"}};
inline constexpr Anchor kL52[] = {{0, L"Q"}, {11, L"R"}, {22, L"S"}, {31, L"T"}, {41, L"U"}, {52, L"V"}, {62, L"W"}, {76, L"X"}, {88, L"Y"}, {99, L"Z"}, {114, L"["}};
inline constexpr Anchor kL65[] = {{1, L"\\"}, {12, L"]"}, {21, L"^"}, {31, L"_"}, {42, L"`"}, {49, L"a"}, {58, L"b"}, {68, L"c"}, {78, L"d"}, {89, L"e"}, {99, L"f"}, {107, L"g"}};
inline constexpr Anchor kL78[] = {{0, L"h"}, {10, L"i"}, {16, L"j"}, {26, L"k"}, {35, L"l"}, {41, L"m"}, {56, L"n"}, {66, L"o"}, {76, L"p"}, {87, L"q"}, {98, L"r"}, {106, L"s"}, {118, nullptr}};
inline constexpr Anchor kL92[] = {{0, L"t"}, {7, L"u"}, {17, L"v"}, {26, L"w"}, {39, L"x"}, {50, L"y"}, {60, L"z"}, {70, L"{"}, {80, nullptr}, {91, L"}"}, {104, nullptr}, {116, nullptr}};
inline constexpr Anchor kL104[] = {{0, nullptr}, {10, nullptr}};
inline constexpr Anchor kLTiny[] = {{20, L"0"}, {30, L"1"}, {40, L"2"}, {50, L"3"}, {60, L"4"}, {70, L"5"}, {80, L"6"}, {90, L"7"}, {100, L"8"}, {110, L"9"}};
inline constexpr Anchor kL116[] = {{2, L"|"}, {8, L"~"}};
// The fontshp code table's italic rects (codes 30..39, 41..5A, 61..7A, 7F, 80, 83) (Names and HUD numbers).
inline constexpr Anchor kI107[] = {{187, L"\u00FB"}, {207, L"\u00E9"}, {227, nullptr}};
inline constexpr Anchor kI131[] = {{16, L"0"}, {34, L"1"}, {54, L"2"}, {75, L"3"}, {94, L"4"}, {114, L"5"}, {133, L"6"}, {154, L"7"}, {173, L"8"}, {192, L"9"}, {211, L"A"}, {234, L"B"}};
inline constexpr Anchor kI155[] = {{3, L"C"}, {25, L"D"}, {47, L"E"}, {68, L"F"}, {88, L"G"}, {111, L"H"}, {132, L"I"}, {146, L"J"}, {166, L"K"}, {185, L"L"}, {207, L"M"}, {233, L"N"}};
inline constexpr Anchor kI179[] = {{3, L"O"}, {25, L"P"}, {48, L"Q"}, {70, L"R"}, {93, L"S"}, {114, L"T"}, {134, L"U"}, {157, L"V"}, {179, L"W"}, {204, L"X"}, {226, L"Y"}};
inline constexpr Anchor kI203[] = {{1, L"Z"}, {21, L"a"}, {39, L"b"}, {60, L"c"}, {81, L"d"}, {100, L"e"}, {119, L"f"}, {134, L"g"}, {154, L"h"}, {173, L"i"}, {185, L"j"}, {200, L"k"}, {219, L"l"}, {234, L"m"}};
inline constexpr Anchor kI227[] = {{0, L"n"}, {21, L"o"}, {41, L"p"}, {62, L"q"}, {83, L"r"}, {97, L"s"}, {118, L"t"}, {132, L"u"}, {152, L"v"}, {171, L"w"}, {192, L"x"}, {210, L"y"}, {229, L"z"}};
// The dmgnum code table's big digits (codes 30..39) and "Miss!" (3A, 45) (Damage numbers).
inline constexpr Anchor kD0[] = {{130, L"0"}, {159, L"1"}, {177, L"2"}, {204, L"3"}, {228, L"4"}};
inline constexpr Anchor kD37[] = {{129, L"5"}, {155, L"6"}, {180, L"7"}, {203, L"8"}, {229, L"9"}};
inline constexpr Anchor kDMiss[] = {{129, L"Miss!"}};
// menu2fon (Menu headings).
inline constexpr Anchor kH161[] = {{3, L"A"}, {13, L"B"}, {24, L"C"}, {34, L"D"}, {45, L"E"}, {55, L"F"}, {65, L"G"}, {76, L"H"}, {86, L"I"}, {94, L"J"}, {104, L"K"}, {114, L"L"},
                                   {124, L"M"}, {137, L"N"}, {147, L"O"}, {158, L"P"}, {168, L"Q"}, {179, L"R"}, {190, L"S"}, {200, L"T"}, {210, L"U"}, {220, L"V"}, {231, L"W"}, {242, L"X"}};
inline constexpr Anchor kH173[] = {{5, L"Y"}, {14, L"Z"}, {22, L"a"}, {31, L"b"}, {40, L"c"}, {49, L"d"}, {59, L"e"}, {67, L"f"}, {74, L"g"}, {83, L"h"}, {93, L"i"}, {99, L"j"}, {106, L"k"}, {116, L"l"},
                                   {123, L"m"}, {135, L"n"}, {145, L"o"}, {155, L"p"}, {165, L"q"}, {174, L"r"}, {181, L"s"}, {190, L"t"}, {198, L"u"}, {207, L"v"}, {215, L"w"}, {227, L"x"}, {236, L"y"}, {245, L"z"}};
inline constexpr Anchor kH101[] = {{147, L"0"}, {156, L"1"}, {164, L"2"}, {173, L"3"}, {181, L"4"}, {190, L"5"}, {199, L"6"}, {208, L"7"}, {216, L"8"}, {225, L"9"}};
inline constexpr Anchor kH92Big[] = {{145, L"J"}, {154, L"E"}, {162, L"F"}, {171, L"G"}, {180, L"D"}, {189, L"?"}};
inline constexpr Anchor kH92Small[] = {{197, L"J"}, {204, L"E"}, {211, L"F"}, {218, L"G"}, {225, L"D"}, {232, L"?"}};
// pname job, race and level cells and the marks (Job and level tags); mn10font GEO and RUN.
inline constexpr Anchor kJ184[] = {{0, L"WAR"}, {25, L"MNK"}, {50, L"WHM"}, {76, L"BLM"}, {101, L"RDM"}, {127, L"THF"}, {152, L"PLD"}, {177, L"DRK"}, {203, L"BST"}, {228, L"BRD"}};
inline constexpr Anchor kJ194[] = {{0, L"RNG"}, {26, L"SAM"}, {51, L"NIN"}, {76, L"DRG"}, {102, L"SMN"}};
inline constexpr Anchor kJ194Digits[] = {{128, L"0"}, {139, L"1"}, {150, L"2"}, {161, L"3"}, {172, L"4"}, {183, L"5"}, {195, L"6"}, {205, L"7"}, {216, L"8"}, {227, L"9"}};
inline constexpr Anchor kJ204[] = {{0, L"Hum"}, {26, L"Elv"}, {51, L"Tar"}, {76, L"Gal"}, {101, L"Mit"}};
inline constexpr Anchor kJ203Marks[] = {{128, L"/"}, {217, L"["}, {230, L"]"}};
inline constexpr Anchor kJ214[] = {{0, L"BLU"}, {26, L"COR"}, {51, L"PUP"}, {76, L"DNC"}, {102, L"SCH"}};
inline constexpr Anchor kJ118[] = {{0, L"GEO"}, {29, L"RUN"}, {115, nullptr}};
// news, the compass letters, one 16 x 16 cell each (Compass).
inline constexpr Anchor kN0[] = {{0, L"N"}, {16, L"E"}};
inline constexpr Anchor kN16[] = {{0, L"W"}, {16, L"S"}};
}  // namespace anchors

#define TF_ROW(rec, grp, cls, y, up, down, x0, x1, tol, minh, arr) AnchorRow{SpriteRec::rec, Group::grp, cls, y, up, down, x0, x1, tol, minh, arr, sizeof(arr) / sizeof(arr[0])}
inline const AnchorRow kAnchorRows[] = {
    // font font: labels (x < 129, y < 128); the nearest row whose top is at most 3 above the rect's top.
    TF_ROW(FontFont, Labels, 0, 0, 3, 12, 0, 128, 3, 1, anchors::kL0),
    TF_ROW(FontFont, Labels, 0, 13, 3, 12, 0, 128, 3, 1, anchors::kL13),
    TF_ROW(FontFont, Labels, 0, 26, 3, 12, 0, 128, 3, 1, anchors::kL26),
    TF_ROW(FontFont, Labels, 0, 39, 3, 12, 0, 128, 3, 1, anchors::kL39),
    TF_ROW(FontFont, Labels, 0, 52, 3, 12, 0, 128, 3, 1, anchors::kL52),
    TF_ROW(FontFont, Labels, 0, 65, 3, 12, 0, 128, 3, 1, anchors::kL65),
    TF_ROW(FontFont, Labels, 0, 78, 3, 12, 0, 128, 3, 1, anchors::kL78),
    TF_ROW(FontFont, Labels, 0, 92, 3, 11, 0, 128, 3, 1, anchors::kL92),
    TF_ROW(FontFont, Labels, 0, 104, 3, 2, 0, 19, 3, 1, anchors::kL104),
    TF_ROW(FontFont, Labels, 1, 107, 3, 8, 16, 128, 3, 1, anchors::kLTiny),
    TF_ROW(FontFont, Labels, 0, 116, 3, 11, 0, 20, 3, 1, anchors::kL116),
    // font font: the medium italic set (Names and HUD numbers).
    TF_ROW(FontFont, NamesHud, 0, 107, 3, 4, 180, 255, 4, 1, anchors::kI107),
    TF_ROW(FontFont, NamesHud, 0, 131, 3, 4, 0, 255, 4, 1, anchors::kI131),
    TF_ROW(FontFont, NamesHud, 0, 155, 3, 4, 0, 255, 4, 1, anchors::kI155),
    TF_ROW(FontFont, NamesHud, 0, 179, 3, 4, 0, 255, 4, 1, anchors::kI179),
    TF_ROW(FontFont, NamesHud, 0, 203, 3, 4, 0, 255, 4, 1, anchors::kI203),
    TF_ROW(FontFont, NamesHud, 0, 227, 3, 4, 0, 255, 4, 1, anchors::kI227),
    // font font: big digits and the "Miss!" word art (Damage numbers).
    TF_ROW(FontFont, Damage, 0, 0, 1, 2, 125, 255, 4, 20, anchors::kD0),
    TF_ROW(FontFont, Damage, 0, 37, 1, 2, 125, 255, 4, 20, anchors::kD37),
    TF_ROW(FontFont, Damage, 1, 75, 0, 0, 129, 129, 0, 20, anchors::kDMiss),
    // menu2fon: the bold italic serif alphabet, italic digits and language letters (Menu headings).
    TF_ROW(Menu2fon, Headings, 0, 161, 1, 2, 0, 255, 3, 6, anchors::kH161),
    TF_ROW(Menu2fon, Headings, 0, 173, 1, 4, 0, 255, 3, 6, anchors::kH173),
    TF_ROW(Menu2fon, Headings, 1, 101, 1, 1, 140, 235, 2, 6, anchors::kH101),
    TF_ROW(Menu2fon, Headings, 2, 92, 1, 1, 140, 194, 2, 6, anchors::kH92Big),
    TF_ROW(Menu2fon, Headings, 3, 92, 1, 1, 195, 240, 2, 6, anchors::kH92Small),
    // menu2fon: the word cells, level digits and marks (Job and level tags).
    TF_ROW(Menu2fon, JobTags, 0, 184, 0, 0, 0, 255, 0, 10, anchors::kJ184),
    TF_ROW(Menu2fon, JobTags, 0, 194, 0, 0, 0, 120, 0, 10, anchors::kJ194),
    TF_ROW(Menu2fon, JobTags, 1, 194, 0, 0, 125, 240, 0, 10, anchors::kJ194Digits),
    TF_ROW(Menu2fon, JobTags, 0, 204, 0, 0, 0, 120, 0, 10, anchors::kJ204),
    TF_ROW(Menu2fon, JobTags, 2, 203, 0, 1, 125, 240, 0, 10, anchors::kJ203Marks),
    TF_ROW(Menu2fon, JobTags, 0, 214, 0, 0, 0, 120, 0, 10, anchors::kJ214),
    // mn10font: GEO and RUN (Job and level tags); the dash at (115,109) is not owned.
    TF_ROW(Mn10font, JobTags, 0, 118, 0, 0, 0, 60, 0, 10, anchors::kJ118),
    // news: N E / W S (Compass).
    TF_ROW(News, Compass, 0, 0, 0, 0, 0, 16, 0, 10, anchors::kN0),
    TF_ROW(News, Compass, 0, 16, 0, 0, 0, 16, 0, 10, anchors::kN16),
};
#undef TF_ROW

// Layer 1: a rect -> group, size class, text by the nearest row and anchor. False when unclaimed or not owned.
inline bool classify(SpriteRec rec, const Rect& r, Group& group, uint8_t& cls, const wchar_t*& text) {
    const AnchorRow* best = nullptr;
    int bestDy = 1 << 30;
    for (const AnchorRow& row : kAnchorRows) {
        if (row.rec != rec || r.h < row.minH) continue;
        if (r.u < row.xMin || r.u > row.xMax) continue;
        if (r.v < row.y - row.yUp || r.v > row.y + row.yDown) continue;
        const int dy = std::abs(r.v - row.y);
        if (dy < bestDy) { bestDy = dy; best = &row; }
    }
    if (!best) return false;
    const Anchor* hit = nullptr;
    int bestDx = 1 << 30;
    for (size_t i = 0; i < best->n; i++) {
        const int dx = std::abs(r.u - best->a[i].x);
        if (dx <= best->tolX && dx < bestDx) { bestDx = dx; hit = &best->a[i]; }
    }
    if (!hit || !hit->text) return false;
    group = best->group;
    cls = best->cls;
    text = hit->text;
    return true;
}

// Layer 1 over a record's rect set: the owned rects, then the foreign ones overlapping an owned glyph, sorted.
inline std::vector<FontEntry> buildTable(SpriteRec rec, std::vector<Rect> rects) {
    std::sort(rects.begin(), rects.end());
    rects.erase(std::unique(rects.begin(), rects.end()), rects.end());
    std::vector<FontEntry> owned, out;
    std::vector<Rect> rest;
    for (const Rect& r : rects) {
        Group g = Group::None;
        uint8_t c = 0;
        const wchar_t* t = nullptr;
        if (classify(rec, r, g, c, t)) owned.push_back(FontEntry{r.u, r.v, r.w, r.h, g, c, t});
        else rest.push_back(r);
    }
    // Each owned glyph's area: the union of its variants (same group, class and text).
    std::vector<Rect> areas;
    for (const FontEntry& e : owned) {
        Rect a = e.rect();
        for (const FontEntry& o : owned)
            if (o.group == e.group && o.cls == e.cls && std::wcscmp(o.text, e.text) == 0) a = a.unite(o.rect());
        areas.push_back(a);
    }
    out = owned;
    for (const Rect& r : rest)
        for (const Rect& a : areas)
            if (r.overlaps(a)) { out.push_back(FontEntry{r.u, r.v, r.w, r.h, Group::None, 0, nullptr}); break; }
    std::sort(out.begin(), out.end(), [](const FontEntry& a, const FontEntry& b) { return a.rect() < b.rect(); });
    return out;
}

// A leaf of a code-indexed sprite set (fontshp: code 20h + composite; dmgnum: 30h + composite) that samples font font,
// as 51.DAT has it (the generated kCodesFontshp / kCodesDmgnum): its composite and rect.
struct CodeLeaf { int16_t comp; Rect r; };

}  // namespace tf

#include "fonttables_data.h"
#include "fonttables_jp.h"

namespace tf {

// The client's language: which menu DAT the game loaded (Resource_FileId 25B560, index 0), read from the name of its
// window-frame sprite set (the variant-name table 38A8D0, row 0). Each menu DAT ships only its own language's set, and
// the EU fold (DE / FR shown as EN) loads framesus, so the name is the layout actually in use.
enum class ClientLang : uint8_t { Unknown = 0, English, Japanese, German, French };
inline const char* clientLangName(ClientLang l) {
    switch (l) {
    case ClientLang::English: return "English";
    case ClientLang::Japanese: return "Japanese";
    case ClientLang::German: return "German";
    case ClientLang::French: return "French";
    default: return "Unknown";
    }
}
struct LangBank { const char* name16; ClientLang lang; };
inline constexpr LangBank kLangBanks[] = {
    {"menu    framesus", ClientLang::English}, {"menu    frames  ", ClientLang::Japanese}, {"menu    framesde", ClientLang::German}, {"menu    framesfr", ClientLang::French}};
inline ClientLang langOfBank(const char* name16) {
    for (const LangBank& b : kLangBanks)
        if (std::memcmp(name16, b.name16, 16) == 0) return b.lang;
    return ClientLang::Unknown;
}
// The resident sets' names, one at a time: one language's frames set gives it; none, or two languages', give Unknown.
struct LangVote {
    ClientLang first = ClientLang::Unknown;
    int banks = 0;          // frames sets seen
    bool mixed = false;     // two languages' sets
    std::string bank;       // the first one's name
    ClientLang lang() const { return mixed ? ClientLang::Unknown : first; }
    void add(const char* name16) {
        const ClientLang l = langOfBank(name16);
        if (l == ClientLang::Unknown) return;
        if (!banks++) { first = l; bank.assign(name16, 16); }
        else if (l != first) mixed = true;
    }
};

// The layouts TrueFont knows: English (51.DAT's), Japanese (English's plus the Japanese menu DAT's crop variants).
enum class TableSet : uint8_t { English = 0, Japanese };
inline constexpr int kTableSetCount = 2;
inline const char* tableSetName(TableSet t) { return t == TableSet::Japanese ? "Japanese" : "English"; }
// The language guard: false when TrueFont has no layouts for the language (German, French: every sprite-font record
// stays native). Unknown is checked against the English layouts.
inline bool tableSetFor(ClientLang l, TableSet& t) {
    switch (l) {
    case ClientLang::Japanese: t = TableSet::Japanese; return true;
    case ClientLang::German:
    case ClientLang::French: return false;
    default: t = TableSet::English; return true;
    }
}

// The retail sheet's measurements (custom letter art): each slot's native ink on 51.DAT's texture, in slotsFor order
// (generated with the table).
struct ShippedInk { const Rect* r; size_t n; };
#define TF_N(a) (sizeof a / sizeof a[0])
inline ShippedInk shippedInk(SpriteRec rec, TableSet ts = TableSet::English) {
    const bool jp = ts == TableSet::Japanese;
    switch (rec) {
    case SpriteRec::FontFont: return jp ? ShippedInk{tables_jp::kInkFontFont, TF_N(tables_jp::kInkFontFont)} : ShippedInk{tables::kInkFontFont, TF_N(tables::kInkFontFont)};
    case SpriteRec::Menu2fon: return jp ? ShippedInk{tables_jp::kInkMenu2fon, TF_N(tables_jp::kInkMenu2fon)} : ShippedInk{tables::kInkMenu2fon, TF_N(tables::kInkMenu2fon)};
    case SpriteRec::Mn10font: return jp ? ShippedInk{tables_jp::kInkMn10font, TF_N(tables_jp::kInkMn10font)} : ShippedInk{tables::kInkMn10font, TF_N(tables::kInkMn10font)};
    case SpriteRec::News: return jp ? ShippedInk{tables_jp::kInkNews, TF_N(tables_jp::kInkNews)} : ShippedInk{tables::kInkNews, TF_N(tables::kInkNews)};
    }
    return {nullptr, 0};
}
// Slot counts must match generated measurements. Japanese crop variants reuse existing slots.
static_assert(TF_N(tables::kInkFontFont) == 180 && TF_N(tables::kInkMenu2fon) == 99 && TF_N(tables::kInkMn10font) == 2 && TF_N(tables::kInkNews) == 4 &&
                  TF_N(tables_jp::kInkFontFont) == 180 && TF_N(tables_jp::kInkMenu2fon) == 99 && TF_N(tables_jp::kInkMn10font) == 2 && TF_N(tables_jp::kInkNews) == 4,
              "the shipped ink tables (S15) are not the lengths they were generated at: check them against slotsFor");
// The code -> rect maps of fontshp and dmgnum (their font font leaves, by composite).
struct CodeTable { const CodeLeaf* e; size_t n; };
inline CodeTable codesFontshp() { return {tables::kCodesFontshp, sizeof tables::kCodesFontshp / sizeof tables::kCodesFontshp[0]}; }
inline CodeTable codesDmgnum() { return {tables::kCodesDmgnum, sizeof tables::kCodesDmgnum / sizeof tables::kCodesDmgnum[0]}; }

// `pin`: texturePin over the native texture the table was generated from.
struct RecordTable { const FontEntry* e; size_t n; uint32_t hash; uint32_t pin; };
inline RecordTable tableFor(SpriteRec rec, TableSet ts = TableSet::English) {
    if (ts == TableSet::Japanese) {
        switch (rec) {
        case SpriteRec::FontFont: return {tables_jp::kFontFont, TF_N(tables_jp::kFontFont), tables_jp::kHashFontFont, tables_jp::kPinFontFont};
        case SpriteRec::Menu2fon: return {tables_jp::kMenu2fon, TF_N(tables_jp::kMenu2fon), tables_jp::kHashMenu2fon, tables_jp::kPinMenu2fon};
        case SpriteRec::Mn10font: return {tables_jp::kMn10font, TF_N(tables_jp::kMn10font), tables_jp::kHashMn10font, tables_jp::kPinMn10font};
        case SpriteRec::News: return {tables_jp::kNews, TF_N(tables_jp::kNews), tables_jp::kHashNews, tables_jp::kPinNews};
        }
        return {nullptr, 0, 0, 0};
    }
    switch (rec) {
    case SpriteRec::FontFont: return {tables::kFontFont, TF_N(tables::kFontFont), tables::kHashFontFont, tables::kPinFontFont};
    case SpriteRec::Menu2fon: return {tables::kMenu2fon, TF_N(tables::kMenu2fon), tables::kHashMenu2fon, tables::kPinMenu2fon};
    case SpriteRec::Mn10font: return {tables::kMn10font, TF_N(tables::kMn10font), tables::kHashMn10font, tables::kPinMn10font};
    case SpriteRec::News: return {tables::kNews, TF_N(tables::kNews), tables::kHashNews, tables::kPinNews};
    }
    return {nullptr, 0, 0, 0};
}
#undef TF_N

// One glyph (or word) the art builder draws: its variants, their union (cleared), intersection and fit box.
struct GlyphSlot {
    Group group = Group::None;
    uint8_t cls = 0;
    std::wstring text;
    std::vector<Rect> variants;
    Rect uni, safe, fit;
    bool fitTrimmed = false;   // the fit box lost rows or columns to a neighbouring slot
    bool plateMark = false;    // a Menu labels glyph the fontshp page draws too (kPlateMarks): nameplates' punctuation
    std::vector<Rect> foreign; // the table's foreign rects over its union (rows the rim may not take)
    bool word() const { return text.size() > 1; }
};

// The font font rects of Menu labels that the fontshp set samples (51.DAT codes 20..2F, 3A..40, 5B..60, 7B..7E, 84..8D:
// punctuation, the ellipsis, tiny digits). Nameplates draw them through the page slot, so the nameplate composite draws
// them in Nameplates' style whenever that differs from font font's own; font font's texture keeps them as Menu labels.
inline constexpr Rect kPlateMarks[] = {
    {0, 0, 4, 8},     {12, 0, 6, 11},   {21, 0, 8, 5},    {30, 0, 10, 11},  {40, 0, 10, 11},  {50, 0, 10, 11},  {60, 0, 10, 11},  {72, 0, 5, 7},
    {81, 0, 6, 11},   {93, 0, 6, 11},   {100, 0, 10, 11}, {111, 0, 8, 11},  {1, 11, 6, 13},   {11, 13, 8, 8},   {23, 13, 4, 11},  {30, 13, 9, 11},
    {19, 26, 5, 10},  {24, 26, 6, 11},  {30, 26, 8, 11},  {39, 26, 8, 10},  {48, 26, 8, 11},  {56, 26, 9, 11},  {66, 26, 9, 11},  {114, 52, 7, 11},
    {1, 65, 9, 11},   {12, 65, 7, 11},  {21, 65, 8, 7},   {31, 65, 8, 11},  {42, 65, 5, 7},   {70, 92, 8, 11},  {2, 116, 4, 11},  {91, 92, 8, 11},
    {8, 119, 10, 5},  {30, 107, 7, 8},  {40, 107, 7, 8},  {50, 107, 7, 8},  {60, 107, 7, 8},  {70, 107, 7, 8},  {80, 107, 7, 8},  {90, 107, 7, 8},
    {100, 107, 7, 8}, {117, 26, 9, 11}, {110, 107, 7, 8},
};
inline bool isPlateMark(const Rect& r) {
    for (const Rect& m : kPlateMarks)
        if (m == r) return true;
    return false;
}
// The font font rects only the shadow pass draws (51.DAT): the Menu labels g, i, p and y.
inline constexpr Rect kShadowOnly[] = {{107, 67, 9, 10}, {10, 79, 5, 9}, {76, 80, 9, 10}, {50, 93, 9, 10}};
inline bool isShadowOnly(SpriteRec rec, const Rect& r) {
    if (rec != SpriteRec::FontFont) return false;
    for (const Rect& m : kShadowOnly)
        if (m == r) return true;
    return false;
}
// A group whose rects are drawn stretched to one height.
inline bool stretchedRows(Group g) { return g == Group::Damage; }

// The slots of a record from a table (`e`, `n`): overlapping variants of one glyph form one slot. slotsFor: the shipped
// table's; the table generator passes the one it builds.
inline std::vector<GlyphSlot> slotsFrom(SpriteRec rec, const FontEntry* e, size_t n) {
    const RecordTable t{e, n, 0, 0};
    std::vector<GlyphSlot> slots;
    std::vector<int> slotOf(t.n, -1);
    for (size_t i = 0; i < t.n; i++) {
        if (!t.e[i].owned() || slotOf[i] >= 0) continue;
        GlyphSlot s;
        s.group = t.e[i].group;
        s.cls = t.e[i].cls;
        s.text = t.e[i].text;
        const int id = int(slots.size());
        std::vector<size_t> todo{i};
        slotOf[i] = id;
        while (!todo.empty()) {
            const size_t k = todo.back();
            todo.pop_back();
            s.variants.push_back(t.e[k].rect());
            for (size_t j = 0; j < t.n; j++) {
                if (slotOf[j] >= 0 || !t.e[j].owned() || t.e[j].group != s.group || t.e[j].cls != s.cls || s.text != t.e[j].text) continue;
                if (!t.e[j].rect().overlaps(t.e[k].rect())) continue;
                slotOf[j] = id;
                todo.push_back(j);
            }
        }
        std::sort(s.variants.begin(), s.variants.end());
        for (const Rect& r : s.variants) s.plateMark = s.plateMark || (rec == SpriteRec::FontFont && s.group == Group::Labels && isPlateMark(r));
        s.uni = s.variants[0];
        for (const Rect& r : s.variants) s.uni = s.uni.unite(r);
        for (size_t j = 0; j < t.n; j++)
            if (!t.e[j].owned() && t.e[j].rect().overlaps(s.uni)) s.foreign.push_back(t.e[j].rect());
        // The intersection of the variants a main pass draws (all of them when none is).
        bool first = true;
        for (const Rect& r : s.variants) {
            if (isShadowOnly(rec, r)) continue;
            s.safe = first ? r : s.safe.intersect(r);
            first = false;
        }
        if (first) {
            s.safe = s.variants[0];
            for (const Rect& r : s.variants) s.safe = s.safe.intersect(r);
        }
        slots.push_back(std::move(s));
    }
    // The fit box: the intersection, trimmed off every other slot's union (the side that keeps the most area).
    for (size_t i = 0; i < slots.size(); i++) {
        Rect f = slots[i].safe;
        for (size_t j = 0; j < slots.size(); j++) {
            if (i == j || !f.overlaps(slots[j].uni)) continue;
            const Rect o = slots[j].uni;
            const Rect cand[4] = {Rect::fromEdges(f.u, f.v, o.u, f.y1()), Rect::fromEdges(o.x1(), f.v, f.x1(), f.y1()), Rect::fromEdges(f.u, f.v, f.x1(), o.v),
                                  Rect::fromEdges(f.u, o.y1(), f.x1(), f.y1())};
            Rect best;
            for (const Rect& c : cand) if (c.area() > best.area()) best = c;
            f = best;
            slots[i].fitTrimmed = true;
        }
        slots[i].fit = f.empty() ? slots[i].safe : f;
    }
    return slots;
}
inline std::vector<GlyphSlot> slotsFor(SpriteRec rec, TableSet ts = TableSet::English) {
    const RecordTable t = tableFor(rec, ts);
    return slotsFrom(rec, t.e, t.n);
}

// The texture pin: FNV-1a over the alpha nibble of the native texels in every owned glyph's union (outside = 0). The
// nibble is what DXT3 stores, so the pin holds for a DXT3 read (a*17) and for an A8R8G8B8 expansion (a*17 or a<<4).
inline uint32_t texturePinOf(const std::vector<GlyphSlot>& slots, const uint32_t* px, int w, int h) {
    uint32_t hsh = 2166136261u;
    for (const GlyphSlot& s : slots)
        for (int y = s.uni.v; y < s.uni.y1(); y++)
            for (int x = s.uni.u; x < s.uni.x1(); x++) {
                const uint8_t a = (px && x >= 0 && y >= 0 && x < w && y < h) ? uint8_t(px[size_t(y) * size_t(w) + size_t(x)] >> 28) : uint8_t(0);
                hsh = fnv1a(hsh, &a, 1);
            }
    return hsh;
}
inline uint32_t texturePin(SpriteRec rec, const uint32_t* px, int w, int h, TableSet ts = TableSet::English) { return texturePinOf(slotsFor(rec, ts), px, w, h); }

// The run-time check: the live leaf rects of a record against the shipped table.
struct RectCheck {
    int rects = 0;           // unique live rects
    int owned = 0;           // shipped owned rects among them
    int foreign = 0;         // shipped foreign rects among them
    int outside = 0;         // live rects that touch no owned glyph (icons, logos: left native)
    std::vector<Rect> unknown;   // live rects that overlap an owned glyph but are not in the table: the layout differs
    int ownedShipped = 0;    // owned rects in the table
    uint32_t liveHash = 0;   // hashOwned over the owned table entries that were seen live
    uint32_t shippedHash = 0;
    bool ok() const { return unknown.empty() && owned > 0; }
    bool complete() const { return ok() && owned == ownedShipped && liveHash == shippedHash; }
};
inline RectCheck checkRects(SpriteRec rec, std::vector<Rect> live, TableSet ts = TableSet::English) {
    std::sort(live.begin(), live.end());
    live.erase(std::unique(live.begin(), live.end()), live.end());
    const RecordTable t = tableFor(rec, ts);
    RectCheck c;
    c.rects = int(live.size());
    c.shippedHash = t.hash;
    std::vector<Rect> areas;
    for (const GlyphSlot& s : slotsFor(rec, ts)) areas.push_back(s.uni);
    std::vector<FontEntry> seen;
    for (size_t i = 0; i < t.n; i++) if (t.e[i].owned()) ++c.ownedShipped;
    for (const Rect& r : live) {
        const FontEntry* hit = nullptr;
        for (size_t i = 0; i < t.n; i++) if (t.e[i].rect() == r) { hit = &t.e[i]; break; }
        if (hit) {
            if (hit->owned()) { ++c.owned; seen.push_back(*hit); }
            else ++c.foreign;
            continue;
        }
        bool touches = false;
        for (const Rect& a : areas) if (r.overlaps(a)) { touches = true; break; }
        if (touches) c.unknown.push_back(r);
        else ++c.outside;
    }
    c.liveHash = hashOwned(seen.data(), seen.size());
    return c;
}

// A rect a leaf of `rec` may sample before it is bound to a copy: inside the texture and touching no owned glyph, or one
// of the table's rects (checkRects' "unknown" is refused).
inline bool rectKnown(SpriteRec rec, const Rect& r, TableSet ts = TableSet::English) {
    const RecordTable t = tableFor(rec, ts);
    for (size_t i = 0; i < t.n; i++)
        if (t.e[i].rect() == r) return true;
    const SpriteRecInfo& in = kSpriteRecs[int(rec)];
    if (r.empty() || r.u < 0 || r.v < 0 || r.x1() > int(in.w) || r.y1() > int(in.h)) return false;
    const auto unions = [](SpriteRec x, TableSet y) { std::vector<Rect> a; for (const GlyphSlot& s : slotsFor(x, y)) a.push_back(s.uni); return a; };
    static const std::vector<Rect> areas[kTableSetCount][kSpriteRecCount] = {
        {unions(SpriteRec(0), TableSet::English), unions(SpriteRec(1), TableSet::English), unions(SpriteRec(2), TableSet::English), unions(SpriteRec(3), TableSet::English)},
        {unions(SpriteRec(0), TableSet::Japanese), unions(SpriteRec(1), TableSet::Japanese), unions(SpriteRec(2), TableSet::Japanese), unions(SpriteRec(3), TableSet::Japanese)},
    };
    for (const Rect& a : areas[int(ts)][int(rec)])
        if (r.overlaps(a)) return false;
    return true;
}

}  // namespace tf
