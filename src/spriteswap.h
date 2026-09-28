// Sprite-font installation through CYyTex+40, independent of the chat installer.
// Validate each record, layout and session art before use; unsupported records remain native.
// Compatible mods use shipped placement metrics while retaining their original non-letter art.
// Render thread only except DataOnly release; no code patches or DAT readback fallback.
//
// Groups needing separate style or detail use private record copies: registry +1C/+30 for
// Nameplates/Damage, leaf +4C for HUD/Job Tags. Never free a copy until verified unreferenced.
// Leaf writes require a fresh registry walk; cached addresses or signatures are not write authority.
// Rebuild only changed records and share the memory budget with Chat, which claims its allocation first.
#pragma once
#include "swap.h"
#include "spritesigs.h"
#include "records.h"
#include "sprites.h"
#include "spriteatlas.h"
#include "fontgate.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace tf {

constexpr bool spriteSizesBlockAligned() {
    for (const auto& r : kSpriteRecs) if (r.w % 4 || r.h % 4) return false;
    return true;
}
static_assert(spriteSizesBlockAligned(), "DXT3 blocks are 4 x 4: every sprite-font texture must be a multiple of 4 each way");
// The readbacks at each record's sheet scale (a scaled sheet is read at its own size).
inline uint64_t spriteReadbackBytes(const int (&s)[kSpriteRecCount]) {
    uint64_t n = 0;
    for (int i = 0; i < kSpriteRecCount; i++) n += uint64_t(kSpriteRecs[i].w) * uint64_t(kSpriteRecs[i].h) * 4u * uint64_t((std::max)(1, s[i])) * uint64_t((std::max)(1, s[i]));
    return n;
}
// The readbacks at the game's own sheet sizes (every scale 1).
inline uint64_t spriteReadbackBytes() {
    int one[kSpriteRecCount];
    for (int& x : one) x = 1;
    return spriteReadbackBytes(one);
}
// The k a texture over a scaled sheet is built at: at least the sheet's scale and a multiple of it (3 on a 2x sheet is 4),
// so its art is copied texel for texel and never downsampled.
inline int sheetFloor(int k, int s) {
    if (s <= 1) return k;
    const int m = (std::max)(k, s);
    return m + (s - m % s) % s;
}

// Nameplates use a separate composite rather than a group in the shared record.
inline bool recordServes(SpriteRec r, Group g) {
    switch (r) {
    case SpriteRec::FontFont: return g == Group::Labels || g == Group::NamesHud || g == Group::Damage;
    case SpriteRec::Menu2fon: return g == Group::Headings || g == Group::JobTags;
    case SpriteRec::Mn10font: return g == Group::JobTags;
    case SpriteRec::News: return g == Group::Compass;
    }
    return false;
}
// A record has a group that is on (else it is not installed).
inline bool recordOn(SpriteRec r, const SpriteOptions& o) {
    for (int g = 1; g < kGroupCount; g++)
        if (recordServes(r, Group(g)) && o.on[g]) return true;
    return false;
}

// Auto uses 2x except for world text, which follows the screen monitors. DEV forceK overrides all groups.
inline int groupK(Group g, const SpriteOptions& o) {
    if (o.forceK) return o.forceK;
    const int s = o.sharp[int(g)];
    if (s >= 2 && s <= 4) return s;
    if (g == Group::Nameplates || g == Group::Damage) {
        const int a = g == Group::Nameplates ? o.autoKPlate : o.autoKDamage;
        return a < 1 ? 1 : a > 4 ? 4 : a;
    }
    return 2;
}
inline Group baseGroup(SpriteRec r, const SpriteOptions& o) {
    switch (r) {
    case SpriteRec::FontFont: return o.on[int(Group::Labels)] ? Group::Labels : o.on[int(Group::NamesHud)] ? Group::NamesHud : o.on[int(Group::Damage)] ? Group::Damage : Group::Labels;
    case SpriteRec::Menu2fon: return o.on[int(Group::Headings)] || !o.on[int(Group::JobTags)] ? Group::Headings : Group::JobTags;
    case SpriteRec::Mn10font: return Group::JobTags;
    case SpriteRec::News: return Group::Compass;
    }
    return Group::Labels;
}
// A record's k, and a copy's (Nameplates, Damage numbers and HUD text copy font font, Job and level tags menu2fon): the
// group's, raised to the record's sheet (sheetFloor).
inline int recordK(SpriteRec r, const SpriteOptions& o) { return sheetFloor(groupK(baseGroup(r, o), o), o.sheet[int(r)]); }
inline int splitK(Group g, const SpriteOptions& o) { return sheetFloor(groupK(g, o), o.sheet[int(g == Group::JobTags ? SpriteRec::Menu2fon : SpriteRec::FontFont)]); }
// The k built once the memory limit applied (0: as asked).
inline int effRecordK(SpriteRec r, const SpriteOptions& o) { return o.kRec[int(r)] ? o.kRec[int(r)] : recordK(r, o); }
inline int effPlateK(const SpriteOptions& o) { return o.kPlate ? o.kPlate : splitK(Group::Nameplates, o); }
inline int effDamageK(const SpriteOptions& o) { return o.kDamage ? o.kDamage : splitK(Group::Damage, o); }
inline int effHudK(const SpriteOptions& o) { return o.kHud ? o.kHud : splitK(Group::NamesHud, o); }
inline int effJobsK(const SpriteOptions& o) { return o.kJobs ? o.kJobs : splitK(Group::JobTags, o); }

// Compare effective styles only for enabled groups; ignore Miss settings when Damage is disabled.
inline bool sameDrawing(SpriteRec r, const SpriteOptions& a, const SpriteOptions& b) {
    if (a.mode != b.mode || a.compress != b.compress || a.hinting != b.hinting || a.engine != b.engine) return false;   // Hinting, Engine: every record
    const int dmg = int(Group::Damage);
    if (r == SpriteRec::FontFont && (a.on[dmg] && a.missRedraw) != (b.on[dmg] && b.missRedraw)) return false;
    for (int g = 1; g < kGroupCount; g++) {
        if (!recordServes(r, Group(g))) continue;
        if (a.on[g] != b.on[g]) return false;
        if (a.on[g] && !sameStyle(a.style[g], b.style[g])) return false;
    }
    return true;
}
// Compare group-local settings; global quality and budget-assigned scales are handled separately.
inline bool groupChanged(const SpriteOptions& a, const SpriteOptions& b, Group g) {
    const int i = int(g);
    if (a.on[i] != b.on[i]) return true;
    if (!b.on[i]) return false;
    return !sameStyle(a.style[i], b.style[i]) || a.sharp[i] != b.sharp[i] || (g == Group::Damage && a.missRedraw != b.missRedraw);
}
// Two option sets draw a record the same: the same pixels at the same k.
inline bool sameForRecord(SpriteRec r, const SpriteOptions& a, const SpriteOptions& b) { return effRecordK(r, a) == effRecordK(r, b) && sameDrawing(r, a, b); }

// Split Nameplates when their enabled state, style or scale differs from the shared HUD/Labels texture.
inline bool plateStyleNeeded(const SpriteOptions& o) {
    const int n = int(Group::Nameplates);
    for (const Group other : {Group::NamesHud, Group::Labels}) {
        const int x = int(other);
        if (o.on[n] != o.on[x]) return true;
        if (o.on[n] && !sameStyle(o.style[n], o.style[x])) return true;
    }
    return false;
}
inline bool plateNeeded(const SpriteOptions& o) { return plateStyleNeeded(o) || (o.on[int(Group::Nameplates)] && effPlateK(o) != effRecordK(SpriteRec::FontFont, o)); }
// Nameplates draw unlike HUD text (their own font or switch), not merely in Menu labels' punctuation.
inline bool plateOwnFont(const SpriteOptions& o) {
    const int n = int(Group::Nameplates), h = int(Group::NamesHud);
    return o.on[n] != o.on[h] || (o.on[n] && !sameStyle(o.style[n], o.style[h]));
}
// Compare through plateOptions, which maps the Nameplates style onto shared HUD rects and marks.
inline bool samePlate(const SpriteOptions& a, const SpriteOptions& b) { return effPlateK(a) == effPlateK(b) && sameDrawing(SpriteRec::FontFont, plateOptions(a), plateOptions(b)); }
// Split Damage only for higher detail; a larger shared texture already satisfies a lower requested scale.
inline bool damageNeeded(const SpriteOptions& o) { return o.on[int(Group::Damage)] && effDamageK(o) > effRecordK(SpriteRec::FontFont, o); }
inline bool sameDamage(const SpriteOptions& a, const SpriteOptions& b) { return effDamageK(a) == effDamageK(b) && sameDrawing(SpriteRec::FontFont, a, b); }
// HUD text (Job and level tags) get their own copy when on at another k than their record's own texture.
inline bool hudNeeded(const SpriteOptions& o) { return o.on[int(Group::NamesHud)] && effHudK(o) != effRecordK(SpriteRec::FontFont, o); }
inline bool jobsNeeded(const SpriteOptions& o) { return o.on[int(Group::JobTags)] && effJobsK(o) != effRecordK(SpriteRec::Menu2fon, o); }
inline bool sameHud(const SpriteOptions& a, const SpriteOptions& b) { return effHudK(a) == effHudK(b) && sameDrawing(SpriteRec::FontFont, a, b); }
inline bool sameJobs(const SpriteOptions& a, const SpriteOptions& b) { return effJobsK(a) == effJobsK(b) && sameDrawing(SpriteRec::Menu2fon, a, b); }
// The same drawing but for Compress: the kept A8L8 composite is re-encoded, no glyph drawn again.
inline bool sameExceptCompress(SpriteRec r, const SpriteOptions& a, const SpriteOptions& b) {
    SpriteOptions x = b;
    x.compress = a.compress;
    return sameForRecord(r, a, x);
}
inline bool samePlateExceptCompress(const SpriteOptions& a, const SpriteOptions& b) {
    SpriteOptions x = b;
    x.compress = a.compress;
    return samePlate(a, x);
}
inline bool sameDamageExceptCompress(const SpriteOptions& a, const SpriteOptions& b) {
    SpriteOptions x = b;
    x.compress = a.compress;
    return sameDamage(a, x);
}
inline bool sameHudExceptCompress(const SpriteOptions& a, const SpriteOptions& b) {
    SpriteOptions x = b;
    x.compress = a.compress;
    return sameHud(a, x);
}
inline bool sameJobsExceptCompress(const SpriteOptions& a, const SpriteOptions& b) {
    SpriteOptions x = b;
    x.compress = a.compress;
    return sameJobs(a, x);
}

// Fit scales to the shared budget by shrinking the largest texture first. Zero means no texture.
struct SpriteKs {
    int rec[kSpriteRecCount] = {};
    int plate = 0, damage = 0, hud = 0, jobs = 0;   // the splits: copies of font font, and menu2fon's for jobs
    int s[kSpriteRecCount] = {1, 1, 1, 1};           // each record's sheet scale: its readback's size, and the floor of its k and its copies'
};
inline uint64_t spriteKsBytes(const SpriteKs& k, TexFormat f) {
    uint64_t n = spriteReadbackBytes(k.s);
    const int ff = int(SpriteRec::FontFont), m2 = int(SpriteRec::Menu2fon);
    const auto one = [&](int w, int h, int x) { return managedBytes(texBytes(f, w * x, h * x)) + (f == TexFormat::A8R8G8B8 ? 0 : uint64_t(texBytes(TexFormat::A8L8, w * x, h * x))); };
    for (int i = 0; i < kSpriteRecCount; i++)
        if (k.rec[i]) n += one(kSpriteRecs[i].w, kSpriteRecs[i].h, k.rec[i]);
    for (const int x : {k.plate, k.damage, k.hud})
        if (x) n += one(kSpriteRecs[ff].w, kSpriteRecs[ff].h, x);
    if (k.jobs) n += one(kSpriteRecs[m2].w, kSpriteRecs[m2].h, k.jobs);
    return n;
}
inline int stepKDown(int k, bool nonPow2) { return k >= 4 ? (nonPow2 ? 3 : 2) : k == 3 ? 2 : k == 2 ? 1 : k; }
// A texture over a scaled sheet never goes below the sheet's scale, nor to a k it does not divide (sheetFloor).
inline SpriteKs fitSpriteKs(SpriteKs k, uint64_t otherBytes, TexFormat f, bool nonPow2, unsigned long maxW, unsigned long maxH, std::string& why) {
    why.clear();
    const int ff = int(SpriteRec::FontFont), m2 = int(SpriteRec::Menu2fon);
    const auto floorOf = [&](int rec) { return (std::max)(1, k.s[rec]); };
    const auto step = [&](int& x, int fl) {
        x = stepKDown(x, nonPow2);
        if (x % fl) x = stepKDown(x, nonPow2);
        x = (std::max)(x, fl);
    };
    const auto fitCaps = [&](int& x, int w, int h, int fl) {
        if (!x) return;
        if (x == 3 && !nonPow2) { x = sheetFloor(2, fl); if (why.empty()) why = "3x makes a texture that is no power of two, and this card wants power-of-two textures (D3DPTEXTURECAPS_POW2)"; }
        while (x > fl && ((maxW && (unsigned long)(w * x) > maxW) || (maxH && (unsigned long)(h * x) > maxH))) {
            if (why.empty()) why = fmt("a %dx%d texture is larger than this card's largest (%lux%lu)", w * x, h * x, maxW, maxH);
            step(x, fl);
        }
    };
    for (int i = 0; i < kSpriteRecCount; i++) fitCaps(k.rec[i], kSpriteRecs[i].w, kSpriteRecs[i].h, floorOf(i));
    fitCaps(k.plate, kSpriteRecs[ff].w, kSpriteRecs[ff].h, floorOf(ff));
    fitCaps(k.damage, kSpriteRecs[ff].w, kSpriteRecs[ff].h, floorOf(ff));
    fitCaps(k.hud, kSpriteRecs[ff].w, kSpriteRecs[ff].h, floorOf(ff));
    fitCaps(k.jobs, kSpriteRecs[m2].w, kSpriteRecs[m2].h, floorOf(m2));
    for (int guard = 0; guard < 64 && otherBytes + spriteKsBytes(k, f) > kMemoryLimit; guard++) {
        int* worst = nullptr;
        int worstFloor = 1;
        uint64_t worstBytes = 0;
        const auto consider = [&](int& x, int w, int h, int fl) {
            if (x <= fl) return;
            const uint64_t b = texBytes(f, w * x, h * x);
            if (b > worstBytes) { worstBytes = b; worst = &x; worstFloor = fl; }
        };
        for (int i = 0; i < kSpriteRecCount; i++) consider(k.rec[i], kSpriteRecs[i].w, kSpriteRecs[i].h, floorOf(i));
        consider(k.plate, kSpriteRecs[ff].w, kSpriteRecs[ff].h, floorOf(ff));
        consider(k.damage, kSpriteRecs[ff].w, kSpriteRecs[ff].h, floorOf(ff));
        consider(k.hud, kSpriteRecs[ff].w, kSpriteRecs[ff].h, floorOf(ff));
        consider(k.jobs, kSpriteRecs[m2].w, kSpriteRecs[m2].h, floorOf(m2));
        if (!worst) break;   // everything at its floor (1x, or its sheet's scale): nothing more to take away
        if (why.empty() || why.find("MB") == std::string::npos) why = fmt("the total would pass %s", mbText(kMemoryLimit).c_str());
        step(*worst, worstFloor);
    }
    return k;
}
enum class SplitKind : uint8_t { None = 0, Plates, Damage, Hud, Jobs };

struct SpriteJob {
    SpriteRec rec = SpriteRec::FontFont;
    std::shared_ptr<const std::vector<GlyphSlot>> slots;
    std::shared_ptr<const SpritePixels> rb;
    TexFormat format = TexFormat::A8R8G8B8;   // its upload format
    SplitKind split = SplitKind::None;        // a split's composite of font font (Nameplates: built with plateOptions)
    std::shared_ptr<const SpriteAtlas> reencode;   // the last build's A8L8 composite, re-encoded instead of drawn
    int k = 2;                                // the scale it is built at
    ShippedInk shipped{nullptr, 0};           // custom letter art: placed by the shipped retail measurements (with its length)
};
struct SpriteBuilt {
    SpriteRec rec = SpriteRec::FontFont;
    SplitKind split = SplitKind::None;
    std::unique_ptr<SpriteAtlas> atlas;   // null: `why`
    std::shared_ptr<const SpriteAtlas> kept;   // the composite as A8L8 (A8L8 and DXT3 uploads), for a later Compress change
    bool reencoded = false;
    std::string why;
};
class SpriteWorker {
public:
    enum State : int { Idle, Running, Done, Cancelled };
    SpriteWorker() = default;
    SpriteWorker(const SpriteWorker&) = delete;
    SpriteWorker& operator=(const SpriteWorker&) = delete;
    ~SpriteWorker() { cancel(); wait(); }

    bool start(std::vector<SpriteJob> jobs, const SpriteOptions& o) {
        if (state_.load() == Running) return false;
        wait();
        cancel_.store(false);
        {
            std::lock_guard<std::mutex> lock(m_);
            results_.clear();
        }
        state_.store(Running);
        try {
            t_ = std::thread([this, jobs = std::move(jobs), o]() {
                std::vector<SpriteBuilt> out;
                try {
                    FontGate::Use fonts(fontGate(), &cancel_);   // no folder font swap while this draws (fontgate.h)
                    const SpriteOptions po = plateOptions(o);
                    for (const SpriteJob& j : jobs) {
                        if (!fonts.in()) break;   // cancelled while a swap ran
                        if (cancel_.load()) break;
                        SpriteBuilt b;
                        b.rec = j.rec;
                        b.split = j.split;
                        auto a = std::make_unique<SpriteAtlas>();
                        std::string why;
                        if (j.reencode) {   // Compress changed only: the kept A8L8 composite in the new format
                            *a = *j.reencode;
                            a->ms = 0;
                            bool ok = true;
                            if (j.format == TexFormat::Dxt3) {
                                std::vector<uint32_t> words = a->tex.words;
                                ok = toTexImageA8L8(words, a->w, a->h, TexFormat::Dxt3, a->tex, &cancel_);
                            }
                            if (ok) { b.atlas = std::move(a); b.kept = j.reencode; b.reencoded = true; }
                            else b.why = cancel_.load() ? "cancelled" : "it could not be converted to " + std::string(texFormatName(j.format));
                        } else if (j.slots && j.rb && [&] {
                                       SpriteOptions jo = j.split == SplitKind::Plates ? po : o;
                                       jo.k = j.k;
                                       return buildSpriteAtlas(j.rec, *j.slots, *j.rb, jo, *a, &cancel_, why, j.shipped);
                                   }()) {
                            bool ok = true;
                            if (j.format == TexFormat::A8R8G8B8) ok = toTexImage(a->px, a->w, a->h, j.format, a->tex, &cancel_);
                            else {   // A8L8 first (kept), then DXT3 from it: the same bytes as from the A8R8G8B8 composite
                                ok = toTexImage(a->px, a->w, a->h, TexFormat::A8L8, a->tex, &cancel_);
                                if (ok) {
                                    b.kept = std::make_shared<const SpriteAtlas>(*a);
                                    if (j.format == TexFormat::Dxt3) {
                                        std::vector<uint32_t> words = a->tex.words;
                                        const double convMs = a->tex.ms;
                                        ok = toTexImageA8L8(words, a->w, a->h, TexFormat::Dxt3, a->tex, &cancel_);
                                        a->tex.ms += convMs;
                                    }
                                }
                            }
                            if (ok) b.atlas = std::move(a);
                            else { b.kept.reset(); b.why = cancel_.load() ? "cancelled" : "it could not be converted to " + std::string(texFormatName(j.format)); }
                        } else b.why = why.empty() ? std::string("no readback") : why;
                        out.push_back(std::move(b));
                    }
                } catch (const std::exception& e) {
                    SpriteBuilt b;
                    b.why = e.what();
                    b.rec = out.size() < jobs.size() ? jobs[out.size()].rec : SpriteRec::FontFont;
                    b.split = out.size() < jobs.size() ? jobs[out.size()].split : SplitKind::None;
                    try { out.push_back(std::move(b)); } catch (const std::exception&) {}
                }
                std::lock_guard<std::mutex> lock(m_);
                results_ = std::move(out);
                state_.store(cancel_.load() ? Cancelled : Done);
            });
        } catch (const std::exception&) {   // std::system_error: no thread
            state_.store(Idle);
            return false;
        }
        return true;
    }
    State state() const { return State(state_.load()); }
    void cancel() { cancel_.store(true); }
    void wait() { if (t_.joinable()) t_.join(); }
    // Cancel, then wait at most `ms`; false: still running (the holder must stay alive and the DLL mapped).
    bool stop(unsigned long ms) {
        cancel();
        if (!t_.joinable()) return true;
        if (WaitForSingleObject(static_cast<HANDLE>(t_.native_handle()), ms) != WAIT_OBJECT_0) return false;
        t_.join();
        return true;
    }
    std::vector<SpriteBuilt> take() {
        std::lock_guard<std::mutex> lock(m_);
        return std::move(results_);
    }

private:
    std::thread t_;
    std::atomic<bool> cancel_{false};
    std::atomic<int> state_{Idle};
    std::mutex m_;
    std::vector<SpriteBuilt> results_;
};

// Count each enabled group's slots once from the texture it actually uses, including private splits.
// Shared HUD/Nameplate slots count once when both draw from the base texture.
// Use cached build counts; Show Original does not change them.
struct GlyphCounts {
    int rendered = 0, kept = 0;
    bool any = false;   // a group that is on is drawn with a texture of TrueFont's (its numbers count, 0 included)
};
struct TexCounts {   // one texture's, by group, from its build
    int rendered[kGroupCount] = {}, native[kGroupCount] = {};
};
inline TexCounts texCounts(const SpriteAtlas& a) {
    TexCounts c;
    for (int g = 1; g < kGroupCount; g++) {
        c.rendered[g] = a.group[g].rendered;
        c.native[g] = a.group[g].native;
    }
    return c;
}
// The textures the menu and HUD side can have in place: the four records' own (by SpriteRec), then the copies.
enum class SpriteTex : uint8_t { FontFont = 0, Menu2fon, Mn10font, News, Plates, Damage, Hud, Jobs };
inline constexpr int kSpriteTexCount = 8;
static_assert(int(SpriteTex::FontFont) == int(SpriteRec::FontFont) && int(SpriteTex::Menu2fon) == int(SpriteRec::Menu2fon) &&
                  int(SpriteTex::Mn10font) == int(SpriteRec::Mn10font) && int(SpriteTex::News) == int(SpriteRec::News) && kSpriteRecCount == 4,
              "a record's own texture is its SpriteRec");
struct SpriteInPlace {
    bool in[kSpriteTexCount] = {};   // in place (or built and kept while Show Original shows the game's own)
    TexCounts c[kSpriteTexCount];    // each texture's, from its build
    bool nameplatesOn = false;       // Nameplates are on: they draw from font font's own texture while their composite is not in place
};
// The groups a texture in place shows (the counting rule above).
inline bool texShows(SpriteTex t, Group g, const SpriteInPlace& p) {
    const auto in = [&](SpriteTex x) { return p.in[int(x)]; };
    switch (t) {
    case SpriteTex::FontFont:
        if (g == Group::Labels) return true;
        if (g == Group::NamesHud) return !in(SpriteTex::Hud) || (p.nameplatesOn && !in(SpriteTex::Plates));
        return g == Group::Damage && !in(SpriteTex::Damage);
    case SpriteTex::Menu2fon: return g == Group::Headings || (g == Group::JobTags && !in(SpriteTex::Jobs));
    case SpriteTex::Mn10font: return g == Group::JobTags;
    case SpriteTex::News: return g == Group::Compass;
    case SpriteTex::Plates: return g == Group::NamesHud || g == Group::Nameplates;   // the names (Nameplates' style in HUD text's rects) and their marks
    case SpriteTex::Damage: return g == Group::Damage;
    case SpriteTex::Hud: return g == Group::NamesHud;
    case SpriteTex::Jobs: return g == Group::JobTags;
    }
    return false;
}
inline GlyphCounts spriteGlyphCounts(const SpriteInPlace& p) {
    GlyphCounts n;
    for (int t = 0; t < kSpriteTexCount; t++) {
        if (!p.in[t]) continue;
        for (int g = 1; g < kGroupCount; g++) {
            if (!texShows(SpriteTex(t), Group(g), p)) continue;
            const int r = p.c[t].rendered[g], k = p.c[t].native[g];
            n.rendered += r;
            n.kept += k;
            n.any = n.any || r + k > 0;
        }
    }
    return n;
}
// Combine installed Chat and sprite counts. A failed subsystem contributes nothing; the other still counts.
inline GlyphCounts statusCounts(bool enabled, bool chatDrawn, int chatRendered, int chatKept, bool spriteDown, const GlyphCounts& sprites) {
    GlyphCounts n;
    if (!enabled) return n;
    if (chatDrawn) {
        n.rendered += chatRendered;
        n.kept += chatKept;
        n.any = true;
    }
    if (!spriteDown && sprites.any) {
        n.rendered += sprites.rendered;
        n.kept += sprites.kept;
        n.any = true;
    }
    return n;
}

class SpriteInstaller {
public:
    enum class Phase { Off, Waiting, Building, Installed };
    // Native means an unsupported record/layout; Failed means a supported record could not be installed.
    enum class Gate : uint8_t { Unchecked, Ok, Native, Failed };
    enum class GroupState : uint8_t { Pending, Available, Unavailable, Failed };

    SpriteInstaller() = default;
    SpriteInstaller(const SpriteInstaller&) = delete;
    SpriteInstaller& operator=(const SpriteInstaller&) = delete;

    // `rendererRva`: the Chat side's renderer global (0 when unresolved): with the cache global, the game-alive check.
    void setClient(const Module& client, const SpriteResolved& sr, uint32_t rendererRva, MemReader reader = liveReader()) {
        client_ = client;
        SR_ = sr;
        rendererRva_ = rendererRva;
        reader_ = std::move(reader);
        for (int i = 0; i < kSpriteRecCount; i++) {
            recs_[i].id = SpriteRec(i);
            recs_[i].slots = std::make_shared<const std::vector<GlyphSlot>>(slotsFor(SpriteRec(i)));
        }
        std::string counts;
        for (const auto& r : recs_) counts += fmt("%s%s %zu", counts.empty() ? "" : ", ", nameOf(r), r.slots->size());
        info(fmt("sprites: installer ready: CYyTex vtable %06X, texture cache %06X, registry %06X; slots: %s", SR_.texVtable, SR_.texCache, SR_.registry, counts.c_str()));
    }
    void setDevice(IDirect3DDevice8* d) { device_ = d; }
    // `otherBuilding`: Chat and menus is building now (a rebuild whose peak would pass the limit waits for it).
    // otherClaiming gives Chat priority for its initial allocation.
    void setOtherBytes(uint64_t b, bool otherBuilding = false, bool otherClaiming = false) {
        if (b > otherBytes_) growCheckMs_ = 0;   // Chat and menus' share rose: whether these fonts must step down is checked at once
        otherBytes_ = b;
        otherBuilding_ = otherBuilding;
        otherClaiming_ = otherClaiming;
    }
    // Minimum sprite allocation after shrinking, or current usage when rebuilding is unavailable.
    uint64_t floorBytes() const {
        const uint64_t now = memoryBytes();
        if (!enabled_ || gameGone_ || phase_ != Phase::Installed || !failure_.empty() || !batched_ || !formats_.checked) return now;
        SpriteKs want, got;
        std::string limit;
        const TexFormat f = planFormat(opts_);
        planKs(opts_, f, want, got, limit);
        for (int i = 0; i < kSpriteRecCount; i++) want.rec[i] = want.rec[i] ? sheetFloor(1, want.s[i]) : 0;   // 1x, or the sheet's scale
        for (int* x : {&want.plate, &want.damage, &want.hud}) *x = *x ? sheetFloor(1, want.s[int(SpriteRec::FontFont)]) : 0;
        want.jobs = want.jobs ? sheetFloor(1, want.s[int(SpriteRec::Menu2fon)]) : 0;
        // Omit splits that become identical to the shared texture at 1x.
        const SpriteKs built = builtKs(withKs(opts_, want), want);
        if (!built.plate) want.plate = 0;
        if (!built.damage) want.damage = 0;
        if (!built.hud) want.hud = 0;
        if (!built.jobs) want.jobs = 0;
        return (std::min)(now, spriteKsBytes(want, f));
    }
    // The new textures and kept composites a batch in flight holds beside the old ones (0 while none builds).
    uint64_t pendingBytes() const { return building_ ? batchNewBytes_ : 0; }
    bool setOptions(const SpriteOptions& o) {
        const bool same = sameOptions(o, opts_);
        const SpriteOptions was = opts_;
        opts_ = o;
        for (int i = 0; i < kSpriteRecCount; i++) opts_.sheet[i] = was.sheet[i];   // the records' own (read at the look), never the caller's
        if (same) return false;
        batchQuietNext_ = false;   // the rebuild due (if any) is this change's
        if (!failure_.empty()) { info("sprites: options changed: the refusal is retried"); clearLatches(); return false; }
        if (enabled_ && (phase_ == Phase::Installed || phase_ == Phase::Building)) {
            rebuildAt_ = nowMs_ + kRebuildQuietMs;
            info(fmt("sprites: options changed: a rebuild in %llu ms", static_cast<unsigned long long>(kRebuildQuietMs)));
            return true;
        }
        return false;
    }
    // A discrete change (a box, a radio, a font) rebuilds on the next frame; a slider keeps the quiet time.
    bool hurryRebuild() {
        if (!rebuildAt_) return false;
        if (rebuildAt_ > nowMs_) rebuildAt_ = nowMs_;
        return true;
    }

    Phase phase() const { return phase_; }
    bool enabled() const { return enabled_; }
    bool installed() const { return phase_ == Phase::Installed; }
    bool building() const { return building_ || phase_ == Phase::Building; }
    bool autoOff() const { return !autoOffReason_.empty(); }
    const std::string& autoOffReason() const { return autoOffReason_; }
    const std::string& failure() const { return failure_; }
    bool failureRetriableAtLogin() const { return !failure_.empty() && !failurePersistent_; }
    bool showingOriginal() const { return showOriginal_ || (originalAtInstall_ && enabled_ && phase_ != Phase::Installed); }   // asked before the first batch counts
    int reapplies() const { return keep_.total(); }
    unsigned swaps() const { return swaps_; }
    const FormatSupport& formats() const { return formats_; }
    Gate gate(SpriteRec r) const { return rec(r).gate; }
    // The game's language (Unknown until read) and the letter layouts in use.
    ClientLang clientLang() const { return lang_; }
    bool clientLangRead() const { return langRead_; }
    TableSet tableSet() const { return tables_; }
    // Return the unsupported-language reason, empty before detection or for supported layouts.
    std::string languageRefusal() const {
        TableSet ts = TableSet::English;
        return !langRead_ || tableSetFor(lang_, ts) ? std::string() : std::string(clientLangName(lang_)) + " menus aren't supported yet";
    }
    bool languageRefused() const { return !failure_.empty() && failure_ == languageRefusal(); }
    // Retry records that disappeared or changed art, accepting a new art pin.
    // A layout rejected at initial install cannot be repaired by rescanning the same data.
    bool wantsFreshLook() const {
        for (const auto& s : recs_)
            if (s.wentNative || (s.gate != Gate::Ok && !(s.gate == Gate::Native && s.layoutNative))) return true;
        return plate_.adoptGaveUp;
    }
    uintptr_t recordAddress(SpriteRec r) const { return rec(r).rec; }
    const IDirect3DTexture8* texture(SpriteRec r) const { return rec(r).ours; }
    // Cached texture/count state for the panel. Include textures retained during Show Original.
    SpriteInPlace inPlace() const {
        SpriteInPlace p;
        for (int i = 0; i < kSpriteRecCount; i++) {
            const Rec& s = recs_[i];
            p.in[i] = s.built && s.ours && (s.installed || (showOriginal_ && s.gate == Gate::Ok && !s.wentNative));
            p.c[i] = s.counts;
        }
        const auto split = [&](SpriteTex t, const Split& sp) {   // the page slot names the copy
            p.in[int(t)] = sp.built && sp.ours && (sp.installed || (showOriginal_ && sp.copy));
            p.c[int(t)] = sp.counts;
        };
        const auto leaf = [&](SpriteTex t, const LeafSplit& ls) {   // leaves bound to the copy
            p.in[int(t)] = ls.built && ls.ours && (!ls.bound.empty() || (showOriginal_ && ls.copy));
            p.c[int(t)] = ls.counts;
        };
        split(SpriteTex::Plates, plate_);
        split(SpriteTex::Damage, dmg_);
        leaf(SpriteTex::Hud, hud_);
        leaf(SpriteTex::Jobs, jobs_);
        p.nameplatesOn = opts_.on[int(Group::Nameplates)];
        return p;
    }
    GlyphCounts glyphCounts() const { return enabled_ ? spriteGlyphCounts(inPlace()) : GlyphCounts{}; }
    // What the DEV self-check reports per record.
    struct RecordReport {
        Gate gate = Gate::Unchecked;
        std::string why, d1;
        RectCheck check;
        bool checked = false;
        uint32_t pin = 0, shippedPin = 0, sessionPin = 0;
        bool customArt = false;           // the rects are the tables', the art is not (installed, placed by the shipped metrics)
        CodeMapCheck codes;               // the fontshp / dmgnum code -> rect map (font font only)
        bool codesChecked = false;
        int peak = 0, w = 0, h = 0, k = 0;
        int scale = 1;                    // the record's sheet scale (2 or 4: a scaled sheet)
        TexFormat format = TexFormat::A8R8G8B8;   // TrueFont's texture's (while w > 0)
        const IDirect3DTexture8* texture = nullptr;
        bool installed = false;
    };
    RecordReport report(SpriteRec r) const {
        const Rec& s = rec(r);
        RecordReport o;
        o.gate = s.gate; o.why = s.why; o.d1 = s.d1; o.check = s.check; o.checked = s.checked; o.pin = s.pin; o.shippedPin = tableFor(r, tables_).pin;
        o.sessionPin = s.sessionPin; o.customArt = s.customArt; o.codes = s.codes; o.codesChecked = s.codesChecked;
        o.peak = s.peak; o.w = s.ours ? s.w : 0; o.h = s.ours ? s.h : 0; o.k = s.ours ? s.k : 0; o.installed = s.installed; o.scale = s.s;
        o.format = s.format; o.texture = s.ours;
        return o;
    }
    // DEV: compare a group's texture against packed native pixels, excluding other groups' owned areas.
    // DXT3 compares whole 4x4 blocks; render thread only.
    struct SlotCompare { int compared = 0, differ = 0; std::string why; };
    SlotCompare compareSlots(SpriteRec r, Group g) const {
        SlotCompare out;
        const Rec& s = rec(r);
        if (!s.ours || !s.rb || !s.built || s.k < 1) { out.why = "no texture of TrueFont's for this record"; return out; }
        const int k = s.k, w = s.w, h = s.h, d = k / (std::max)(1, s.s);   // d: texels per readback texel
        if (k % (std::max)(1, s.s) || w * s.s != s.rb->w * k || h * s.s != s.rb->h * k) { out.why = "the texture is not the table's size times k over the readback's sheet"; return out; }
        // The mask: g's slot unions, less every slot drawn for another group.
        const std::vector<GlyphSlot>& slots = *s.slots;
        std::vector<uint8_t> mask(size_t(w) * size_t(h), 0);
        const auto paint = [&](const Rect& u, uint8_t v) {
            for (int y = u.v * k; y < u.y1() * k; y++)
                for (int x = u.u * k; x < u.x1() * k; x++) mask[size_t(y) * size_t(w) + size_t(x)] = v;
        };
        for (const GlyphSlot& gs : slots) if (gs.group == g) paint(gs.uni, 1);
        for (const GlyphSlot& gs : slots) if (gs.group != g && slotDrawn(gs, s.builtOpts)) paint(gs.uni, 0);
        std::vector<uint32_t> nat(size_t(w) * size_t(h));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) nat[size_t(y) * size_t(w) + size_t(x)] = s.rb->at(x / d, y / d);
        TexImage want;
        if (!toTexImage(nat, w, h, s.format, want)) { out.why = "the readback could not be packed"; return out; }
        auto* tex = s.ours;
        D3DLOCKED_RECT lr{};
        HRESULT hr = E_FAIL;
        if (!seh([&] { hr = tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY); }) || FAILED(hr) || !lr.pBits) { out.why = fmt("LockRect 0x%08X (pool %s)", unsigned(hr), s.pool); return out; }
        const auto* got = static_cast<const uint8_t*>(lr.pBits);
        const size_t pitch = size_t(lr.Pitch), row = want.rowBytes();
        const bool ran = pitch >= row && seh([&] {
            if (s.format == TexFormat::Dxt3) {
                for (int by = 0; by < h / 4; by++)
                    for (int bx = 0; bx < w / 4; bx++) {
                        bool inside = true;
                        for (int y = 0; y < 4 && inside; y++)
                            for (int x = 0; x < 4 && inside; x++) inside = mask[size_t(by * 4 + y) * size_t(w) + size_t(bx * 4 + x)] != 0;
                        if (!inside) continue;
                        ++out.compared;
                        if (std::memcmp(got + size_t(by) * pitch + size_t(bx) * 16, want.bytes() + size_t(by) * row + size_t(bx) * 16, 16) != 0) ++out.differ;
                    }
            } else {
                const size_t bpp = s.format == TexFormat::A8L8 ? 2 : 4;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        if (!mask[size_t(y) * size_t(w) + size_t(x)]) continue;
                        ++out.compared;
                        if (std::memcmp(got + size_t(y) * pitch + size_t(x) * bpp, want.bytes() + size_t(y) * row + size_t(x) * bpp, bpp) != 0) ++out.differ;
                    }
            }
        });
        seh([&] { tex->UnlockRect(0); });
        if (!ran) { out.why = pitch < row ? fmt("pitch %zu < %zu", pitch, row) : std::string("reading the locked texture faulted"); out.compared = out.differ = 0; }
        return out;
    }
    // Textures of TrueFont's in a record now (the release line); those built, and their bytes (the panel's Memory).
    int texturesInPlace() const {
        int n = 0;
        for (const auto& s : recs_) n += s.installed ? 1 : 0;
        return n;
    }
    int texturesLoaded() const {
        int n = (plate_.ours ? 1 : 0) + (dmg_.ours ? 1 : 0) + (hud_.ours ? 1 : 0) + (jobs_.ours ? 1 : 0);   // the splits' composites count
        for (const auto& s : recs_) n += s.ours ? 1 : 0;
        return n;
    }
    // The A8L8 composites kept for a Compress change, every record's and split's.
    uint64_t keptBytes() const {
        uint64_t n = 0;
        const auto add = [&](const std::shared_ptr<const SpriteAtlas>& k) { if (k) n += uint64_t(k->tex.words.capacity()) * 4u + uint64_t(k->px.capacity()) * 4u; };
        for (const auto& s : recs_) add(s.kept);
        for (const Split* sp : {&plate_, &dmg_}) add(sp->kept);
        for (const LeafSplit* ls : {&hud_, &jobs_}) add(ls->kept);
        return n;
    }
    uint64_t textureBytes() const {
        uint64_t n = 0;
        for (const Split* sp : {&plate_, &dmg_})
            if (sp->ours) n += texBytes(sp->format, sp->w, sp->h);
        for (const LeafSplit* ls : {&hud_, &jobs_})
            if (ls->ours) n += texBytes(ls->format, ls->w, ls->h);
        for (const auto& s : recs_)
            if (s.ours) n += texBytes(s.format, s.w, s.h);
        return n;
    }
    // Shared-budget usage: managed textures, cached A8L8 composites and native readbacks.
    uint64_t memoryBytes() const {
        uint64_t n = keptBytes();
        const auto tex = [&](IDirect3DTexture8* t, const char* pool, TexFormat f, int w, int h) {
            if (!t) return;
            const uint64_t b = texBytes(f, w, h);
            n += std::strcmp(pool, "MANAGED") == 0 ? managedBytes(b) : b;
        };
        for (const Split* sp : {&plate_, &dmg_}) tex(sp->ours, sp->pool, sp->format, sp->w, sp->h);
        for (const LeafSplit* ls : {&hud_, &jobs_}) tex(ls->ours, ls->pool, ls->format, ls->w, ls->h);
        for (const auto& s : recs_) {
            tex(s.ours, s.pool, s.format, s.w, s.h);
            if (s.rb) n += uint64_t(s.rb->px.size()) * 4u;
        }
        return n;
    }
    // Derive group availability from its backing record; Nameplates also requires the page slot.
    GroupState groupState(Group g) const {
        if (g == Group::Nameplates && !plateAvailable()) return GroupState::Unavailable;
        const SpriteRec main = g == Group::Headings || g == Group::JobTags ? SpriteRec::Menu2fon : g == Group::Compass ? SpriteRec::News : SpriteRec::FontFont;
        switch (rec(main).gate) {
        case Gate::Unchecked: return GroupState::Pending;
        case Gate::Ok: return GroupState::Available;
        case Gate::Failed: return GroupState::Failed;
        case Gate::Native: break;
        }
        return GroupState::Unavailable;
    }

    // Its record stays native for its letter layout (a DAT mod's rects, code map or sheet size are not the tables'), not for
    // the game's language or version.
    bool groupLayoutUnknown(Group g) const {
        if (g == Group::Chat || (g == Group::Nameplates && !plateAvailable()) || !languageRefusal().empty()) return false;
        const SpriteRec main = g == Group::Headings || g == Group::JobTags ? SpriteRec::Menu2fon : g == Group::Compass ? SpriteRec::News : SpriteRec::FontFont;
        return rec(main).gate == Gate::Native && rec(main).layoutNative && !rec(main).artNative;
    }
    // The group's record still holds an earlier texture of TrueFont's: only a game restart clears it.
    bool groupNeedsRestart(Group g) const {
        if (g == Group::Chat) return false;
        const SpriteRec main = g == Group::Headings || g == Group::JobTags ? SpriteRec::Menu2fon : g == Group::Compass ? SpriteRec::News : SpriteRec::FontFont;
        return rec(main).gate == Gate::Failed && rec(main).retiredHeld;
    }
    // Distinguish changed art from unsupported layouts; /tfont on can accept a new art pin.
    bool groupArtReplaced(Group g) const {
        if (g == Group::Chat || (g == Group::Nameplates && !plateAvailable())) return false;
        const SpriteRec main = g == Group::Headings || g == Group::JobTags ? SpriteRec::Menu2fon : g == Group::Compass ? SpriteRec::News : SpriteRec::FontFont;
        return rec(main).gate == Gate::Native && rec(main).artNative;
    }
    // The nameplate split, as the DEV self-check and the log read it.
    struct PlateReport {
        bool available = false;        // PR resolved (the slot is known)
        bool wanted = false;           // plateNeeded, and font font is usable
        bool adoptGaveUp = false;      // the page slot's record was given up
        bool built = false, installed = false, pinned = false;
        uintptr_t slotAddr = 0, slot = 0, copy = 0, native = 0;   // the slot, what it holds now, our copy, font font's record
        const IDirect3DTexture8* texture = nullptr;
        int w = 0, h = 0, k = 0;
        TexFormat format = TexFormat::A8R8G8B8;
        bool copyOk = false;           // the copy: font font's vtable, name and size, +40 our composite, +38 and +44 0
        unsigned installs = 0;
    };
    // The same for either split (the Damage split's `available` is DR's, `wanted` damageNeeded's).
    PlateReport splitReport(SplitKind kind) const {
        const Split& sp = kind == SplitKind::Damage ? dmg_ : plate_;
        PlateReport o;
        o.available = splitAvailable(sp);
        o.wanted = splitWanted(sp, opts_);
        o.built = sp.built;
        o.installed = sp.installed;
        o.pinned = sp.pinned;
        o.adoptGaveUp = sp.adoptGaveUp;
        o.slotAddr = splitSlot(sp);
        uint32_t now = 0;
        if (o.slotAddr && rdok(o.slotAddr, now)) o.slot = now;
        o.copy = uintptr_t(sp.copy);
        o.native = rec(SpriteRec::FontFont).rec;
        o.texture = sp.ours;
        o.w = sp.ours ? sp.w : 0;
        o.h = sp.ours ? sp.h : 0;
        o.k = sp.ours ? sp.k : 0;
        o.format = sp.format;
        o.installs = sp.installs;
        if (sp.copy) {
            TexRecord t;
            const SpriteRecInfo& in = kSpriteRecs[int(SpriteRec::FontFont)];
            const int sc = (std::max)(1, rec(SpriteRec::FontFont).s);   // the copy keeps the record's own +24/+26
            o.copyOk = readRecord(reader_, o.copy, t) && t.vtable == vtableVA() && std::memcmp(t.name, in.name, 16) == 0 && t.w == in.w * sc && t.h == in.h * sc &&
                       t.tex40 == uintptr_t(sp.ours) && t.tex44 == 0 && rd<uint32_t>(o.copy + kRecNext) == 0;
        }
        return o;
    }
    PlateReport plateReport() const { return splitReport(SplitKind::Plates); }
    PlateReport damageReport() const { return splitReport(SplitKind::Damage); }
    // The k each texture was built at, and what they were asked for (the DEV self-check's line).
    struct KReport {
        int rec[kSpriteRecCount] = {}, plate = 0, damage = 0, hud = 0, jobs = 0;
        int wantRec[kSpriteRecCount] = {}, wantPlate = 0, wantDamage = 0, wantHud = 0, wantJobs = 0;
        int autoKPlate = 0, autoKDamage = 0;
    };
    // A leaf split's state (the DEV self-check reads it).
    struct LeafReport {
        bool wanted = false, built = false, pinned = false;
        uintptr_t copy = 0, native = 0;
        const IDirect3DTexture8* texture = nullptr;
        int w = 0, h = 0, k = 0;
        size_t bound = 0;
        int boundNamingCopy = 0;       // of the bound leaves, those whose +4C names the copy now
        std::vector<uintptr_t> addrs;  // the bound leaves (valid while the registry is unchanged: the DEV self-check reads them back)
        unsigned binds = 0, rebinds = 0;
        unsigned refused = 0;          // leaves naming the record that failed the identity check before a write
    };
    LeafReport leafReport(SplitKind kind) const {
        const LeafSplit& ls = kind == SplitKind::Jobs ? jobs_ : hud_;
        LeafReport r;
        r.wanted = leafWanted(ls, opts_);
        r.built = ls.built;
        r.pinned = ls.pinned;
        r.copy = uintptr_t(ls.copy);
        r.native = rec(ls.base).rec;
        r.texture = ls.ours;
        r.w = ls.ours ? ls.w : 0;
        r.h = ls.ours ? ls.h : 0;
        r.k = ls.ours ? ls.k : 0;
        r.bound = ls.bound.size();
        r.addrs = ls.bound;
        for (const uintptr_t a : ls.bound) {
            uint32_t v = 0;
            if (rdok(a + kLeafRecord, v) && v == uint32_t(r.copy)) ++r.boundNamingCopy;
        }
        r.binds = ls.binds;
        r.rebinds = ls.rebinds;
        r.refused = ls.refused;
        return r;
    }
    KReport kReport() const {
        KReport r;
        for (int i = 0; i < kSpriteRecCount; i++) { r.rec[i] = recs_[i].ours ? recs_[i].k : 0; r.wantRec[i] = recordK(SpriteRec(i), opts_); }
        r.plate = plate_.ours ? plate_.k : 0;
        r.damage = dmg_.ours ? dmg_.k : 0;
        r.wantPlate = groupK(Group::Nameplates, opts_);
        r.wantDamage = groupK(Group::Damage, opts_);
        r.hud = hud_.ours ? hud_.k : 0;
        r.jobs = jobs_.ours ? jobs_.k : 0;
        r.wantHud = groupK(Group::NamesHud, opts_);
        r.wantJobs = groupK(Group::JobTags, opts_);
        r.autoKPlate = opts_.autoKPlate;
        r.autoKDamage = opts_.autoKDamage;
        return r;
    }
    // DEV: compare nameplate glyphs, marks and unowned pixels against the shared texture or native readback.
    // DXT3 compares whole 4x4 blocks; render thread only.
    struct PlateCompare { int inside = 0, insideDiffer = 0, marks = 0, marksDiffer = 0, outside = 0, outsideDiffer = 0; std::string why; };
    PlateCompare comparePlate() const {
        PlateCompare out;
        const Rec& ff = rec(SpriteRec::FontFont);
        if (!plate_.ours || !plate_.built) { out.why = "no nameplate composite"; return out; }
        const int k = plate_.k, w = plate_.w, h = plate_.h, sc = (std::max)(1, ff.s), d = k / sc;   // d: texels per readback texel
        if (!ff.rb || k % sc || w * sc != ff.rb->w * k || h * sc != ff.rb->h * k) { out.why = "the composite is not the table's size times k over the readback's sheet"; return out; }
        std::vector<uint8_t> mask(size_t(w) * size_t(h), 0);   // 1 Names and HUD, 2 a plate mark
        for (const GlyphSlot& gs : *ff.slots)
            if (gs.group == Group::NamesHud || gs.plateMark)
                for (int y = gs.uni.v * k; y < gs.uni.y1() * k; y++)
                    for (int x = gs.uni.u * k; x < gs.uni.x1() * k; x++) mask[size_t(y) * size_t(w) + size_t(x)] = gs.plateMark ? 2 : 1;
        // The reference: font font's own texture when it matches, else the readback packed.
        const bool baseTex = ff.ours && ff.built && ff.w == w && ff.h == h && ff.format == plate_.format;
        TexImage packed;
        if (!baseTex) {
            std::vector<uint32_t> nat(size_t(w) * size_t(h));
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) nat[size_t(y) * size_t(w) + size_t(x)] = ff.rb->at(x / d, y / d);
            if (!toTexImage(nat, w, h, plate_.format, packed)) { out.why = "the readback could not be packed"; return out; }
        }
        D3DLOCKED_RECT lp{}, lb{};
        HRESULT hp = E_FAIL, hb = E_FAIL;
        auto* pt = plate_.ours;
        auto* bt = baseTex ? ff.ours : nullptr;
        if (!seh([&] { hp = pt->LockRect(0, &lp, nullptr, D3DLOCK_READONLY); }) || FAILED(hp) || !lp.pBits) { out.why = fmt("LockRect (nameplates) 0x%08X", unsigned(hp)); return out; }
        if (bt && (!seh([&] { hb = bt->LockRect(0, &lb, nullptr, D3DLOCK_READONLY); }) || FAILED(hb) || !lb.pBits)) {
            seh([&] { pt->UnlockRect(0); });
            out.why = fmt("LockRect (font font) 0x%08X", unsigned(hb));
            return out;
        }
        const size_t row = plate_.format == TexFormat::Dxt3 ? size_t(w / 4) * 16 : size_t(w) * (plate_.format == TexFormat::A8L8 ? 2u : 4u);
        const uint8_t* a = static_cast<const uint8_t*>(lp.pBits);
        const uint8_t* b = bt ? static_cast<const uint8_t*>(lb.pBits) : packed.bytes();
        const size_t pa = size_t(lp.Pitch), pb = bt ? size_t(lb.Pitch) : packed.rowBytes();
        const bool ran = pa >= row && pb >= row && seh([&] {
            if (plate_.format == TexFormat::Dxt3) {
                for (int by = 0; by < h / 4; by++)
                    for (int bx = 0; bx < w / 4; bx++) {
                        uint8_t in = 0;
                        for (int y = 0; y < 4; y++)
                            for (int x = 0; x < 4; x++) {
                                const uint8_t m = mask[size_t(by * 4 + y) * size_t(w) + size_t(bx * 4 + x)];
                                if (m == 1 || (m == 2 && !in)) in = m;
                            }
                        const bool d = std::memcmp(a + size_t(by) * pa + size_t(bx) * 16, b + size_t(by) * pb + size_t(bx) * 16, 16) != 0;
                        if (in == 1) { ++out.inside; out.insideDiffer += d; }
                        else if (in == 2) { ++out.marks; out.marksDiffer += d; }
                        else { ++out.outside; out.outsideDiffer += d; }
                    }
            } else {
                const size_t bpp = plate_.format == TexFormat::A8L8 ? 2 : 4;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        const bool d = std::memcmp(a + size_t(y) * pa + size_t(x) * bpp, b + size_t(y) * pb + size_t(x) * bpp, bpp) != 0;
                        const uint8_t m = mask[size_t(y) * size_t(w) + size_t(x)];
                        if (m == 1) { ++out.inside; out.insideDiffer += d; }
                        else if (m == 2) { ++out.marks; out.marksDiffer += d; }
                        else { ++out.outside; out.outsideDiffer += d; }
                    }
            }
        });
        seh([&] { pt->UnlockRect(0); });
        if (bt) seh([&] { bt->UnlockRect(0); });
        if (!ran) { out = PlateCompare{}; out.why = "the pitches are too small or reading the locked textures faulted"; }
        return out;
    }

    void clearLatches() {
        if (!autoOffReason_.empty()) info("sprites: clears the automatic off: " + autoOffReason_);
        if (!failure_.empty()) info("sprites: clears the earlier refusal: " + failure_);
        autoOffReason_.clear();
        failure_.clear();
        keep_.clearWindow();
        if (phase_ == Phase::Off) enabled_ = false;
    }
    // Retry refused/disabled records. Manual retries may repeat a refusal, except the language warning
    // already emitted by the command handler.
    void turnOn(bool announce = false) {
        if (announce) lastSaidFailure_ = languageRefusal();
        if (!autoOffReason_.empty()) info("sprites: on: clears the automatic off: " + autoOffReason_);
        if (!failure_.empty()) info("sprites: on: clears the earlier refusal: " + failure_);
        autoOffReason_.clear();
        failure_.clear();
        keep_.clearWindow();
        enabled_ = true;
        batchQuietNext_ = false;
        if (phase_ == Phase::Off) {
            phase_ = Phase::Waiting;
            waitLogged_ = 0;
            firstLookMs_ = 0;
            nextLookMs_ = 0;
            plateRefound();   // a fresh look at the page slot's record too
            for (auto& s : recs_) {   // a fresh look at every record (it may have changed while off)
                s.gate = Gate::Unchecked;
                s.why.clear();
                s.rb.reset();
                s.rec = 0;
                s.wentNative = s.artNative = s.layoutNative = s.retiredHeld = false;
                s.customArt = s.codesChecked = false;   // the art and the code map are read afresh (a new pin)
                s.sessionPin = 0;
                s.s = 1;                                 // and the sheet's scale
                opts_.sheet[int(s.id)] = 1;
            }
        }
    }
    OffReport turnOff(const char* why) {
        info(std::string("sprites: off: ") + why);
        enabled_ = false;
        announceRebuild_ = false;
        ++gen_;
        stopWorker(2000);
        OffReport rep = restoreAll(true);
        phase_ = Phase::Off;
        showOriginal_ = false;
        originalAtInstall_ = false;
        return rep;
    }
    // Announce the next manual rebuild in chat.
    void announceNextRebuild() { announceRebuild_ = true; }
    bool rebuild(std::string& why) {
        const bool ok = rebuildNow(why);
        if (!ok || !building_) announceRebuild_ = false;
        return ok;
    }
    bool rebuildNow(std::string& why) {
        if (!enabled_) { why = "TrueFont is off"; return false; }
        if (gameGone_) { why = "the game is closing"; return false; }
        if (phase_ != Phase::Installed) { why = phase_ == Phase::Building ? "the first build is still running" : "they are not installed yet"; return false; }
        if (building_) stopWorker(2000);
        if (building_) { why = "the running build did not stop"; return false; }
        bool any = false;
        for (auto& s : recs_) {
            if (s.gate == Gate::Failed && s.rb) { s.gate = Gate::Ok; s.why.clear(); info(fmt("sprites: rebuild: %s is tried again", nameOf(s))); }
            any = any || (s.gate == Gate::Ok && s.rb && !s.wentNative && recordOn(s.id, opts_));
        }
        any = any || plateWanted(opts_) || splitWanted(dmg_, opts_) || leafWanted(hud_, opts_) || leafWanted(jobs_, opts_);   // a split's composite alone
        if (!any) {
            bool parked = false;
            for (const auto& s : recs_) parked = parked || (s.gate == Gate::Ok && !recordOn(s.id, opts_));
            why = parked ? "every menu and HUD font group in place is switched off" : "no menu or HUD font texture is in place (see /tfont status)";
            return false;
        }
        forceAll_ = true;
        batchQuietNext_ = false;   // asked for: a failure is said
        return startBatch(why);
    }
    bool setShowOriginal(bool on, std::string& why) {
        if (enabled_ && (phase_ == Phase::Waiting || phase_ == Phase::Building)) {   // the first batch applies it
            if (on != originalAtInstall_)
                info(on ? "sprites: show original: kept until the fonts are in place" : "sprites: show original off: TrueFont's fonts go in place when built");
            originalAtInstall_ = on;
            return true;
        }
        if (on == showOriginal_) {
            if (!on && phase_ != Phase::Installed) { why = "TrueFont's fonts are not installed"; return false; }
            return true;
        }
        if (phase_ != Phase::Installed) { why = "TrueFont's fonts are not installed"; return false; }
        if (on) {
            const OffReport rep = restoreSlots(true);
            showOriginal_ = true;
            info("sprites: show original: " + (rep.clean() ? std::string("the game's own textures are back; TrueFont's stay loaded") : "left: " + rep.leftText()));
            if (!rep.clean()) warn("sprites: show original: switched, but " + rep.leftText());
            return true;
        }
        if (!gameAlive()) { why = "the game's texture cache is gone"; return false; }
        showOriginal_ = false;
        bool any = false, tried = false;
        for (auto& s : recs_) {
            if (s.ours && !s.installed && s.gate == Gate::Ok && !s.wentNative) {
                tried = true;
                std::string w;
                if (!installSlot(s, s.rec, w)) {   // the record may have been re-made meanwhile: looked for again
                    warn(fmt("sprites: show original off: %s: %s; it is looked for again", nameOf(s), w.c_str()));
                    s.lost = true;
                    s.lostTries = 0;
                    rescanAt_ = nowMs_ + 1;
                }
            }
            any = any || s.installed;
        }
        keepLeafSplits();
        // Nothing to put back because every record is parked is not a failure.
        if (!any && (tried || !anyParked())) { why = "none of their textures could be put back (see the log)"; return false; }
        info(any ? "sprites: show original off: TrueFont's textures are back" : "sprites: show original off: nothing to put back (every record's groups are switched off)");
        return true;
    }

    // Delay initial sprite readback if Chat copied this frame, keeping the two costs on separate frames.
    void tick(uint64_t frame, uint64_t nowMs, bool inGame, bool holdBegin = false) {
        frame_ = frame;
        nowMs_ = nowMs;
        if (building_ && worker_.state() != SpriteWorker::Running) finishBatch();
        if (!gameGone_ && inGame && !langRead_ && nowMs >= langPollMs_ && phase_ != Phase::Waiting) {   // the language poll (a first look reads it itself)
            langPollMs_ = nowMs + 1000;
            if (gameAlive() && readLanguage()) lateLanguage();
        }
        if (gameGone_ || !enabled_) return;
        if (rebuildAt_ && nowMs >= rebuildAt_ && !holdForChat()) {
            rebuildAt_ = 0;
            if (phase_ == Phase::Building || phase_ == Phase::Installed) {
                if (building_) stopWorker(2000);
                if (building_) rebuildAt_ = nowMs + kRebuildQuietMs;   // the worker did not stop yet: retry, never drop the newest options
                else {
                    for (auto& s : recs_)   // a record that could not be built or put in place is tried again
                        if (s.gate == Gate::Failed && s.rb) {
                            s.gate = Gate::Ok;
                            s.why.clear();
                            s.built = false;
                            s.kept.reset();
                            info(fmt("sprites: options changed: %s is tried again", nameOf(s)));
                        }
                    std::string why;
                    if (!startBatch(why)) {
                        if (phase_ == Phase::Building) { fail("the menu and HUD fonts could not be built", why); return; }
                        err("sprites: rebuild: " + why + "; the installed fonts stay");
                    }
                    if (!building_) announceRebuild_ = false;   // a typed rebuild's batch was replaced by none
                    if (!building_ && phase_ == Phase::Building) {   // every group switched off during the first build
                        phase_ = Phase::Installed;
                        showOriginal_ = originalAtInstall_;
                        originalAtInstall_ = false;
                        info("sprites: installed: nothing to build (every record's groups are switched off)");
                    }
                }
            }
        }
        if (phase_ == Phase::Waiting) {
            if (!building_ && nowMs >= nextLookMs_) {
                if (!holdBegin) begin(inGame);
                else if (!holdSaid_) { holdSaid_ = true; info("sprites: the first look waits a frame (Chat/Items copied the game's font this frame)"); }
            }
            return;
        }
        if (phase_ == Phase::Installed) {
            if (!showOriginal_) keep();
            if (nowMs >= growCheckMs_) growTick();
            if (rescanAt_ && nowMs >= rescanAt_) rescan();
            if (nowMs >= regPollMs_) {
                if (!building_) pollRegistry();   // never while a batch builds
                else {   // the leaf splits' binds keep their second (they walk their sets afresh)
                    regPollMs_ = nowMs + 1000;
                    keepLeafSplits();
                }
            }
        }
    }

    OffReport release(ReleaseMode mode, bool& workerStopped) {
        workerStopped = stopWorker(mode == ReleaseMode::Full ? 2000 : 500);
        info(fmt("sprites: release: %s; worker %s; texture cache %08X; %d record(s) hold TrueFont's texture; the nameplate slot %s, the damage-number slot %s", releaseModeText(mode),
                 workerStopped ? "stopped" : "STILL RUNNING (the DLL is pinned)", unsigned(cacheNow()), texturesInPlace(), plate_.installed ? "names TrueFont's copy" : "is the game's",
                 dmg_.installed ? "names TrueFont's copy" : "is the game's"));
        info(fmt("sprites: release: HUD text's copy bound to %zu leaves, Job and level tags' to %zu", hud_.bound.size(), jobs_.bound.size()));
        if (mode == ReleaseMode::Nothing) return OffReport{};
        OffReport rep = restoreAll(mode == ReleaseMode::Full);
        // An earlier off could not put a record's original back: that texture of TrueFont's may still be drawn.
        if (leftover_ && rep.clean()) rep.left.push_back("a menu or HUD font record may still hold a texture of TrueFont's (an earlier off could not put the game's own back)");
        enabled_ = false;
        phase_ = Phase::Off;
        originalAtInstall_ = false;
        return rep;
    }

    // One clause for the chat status line.
    std::string statusText() const {
        if (!enabled_) return autoOff() ? "the menu and HUD fonts are off (automatically: " + autoOffReason_ + ")" : std::string("the menu and HUD fonts are off");
        if (!failure_.empty()) return "the menu and HUD fonts could not turn on: " + failure_;
        switch (phase_) {
        case Phase::Off: return "the menu and HUD fonts are off";
        case Phase::Waiting: return "the menu and HUD fonts are waiting for the game";
        case Phase::Building: return "the menu and HUD fonts are being built";
        case Phase::Installed: break;
        }
        int ok = 0;
        std::string native, parked;
        for (const auto& s : recs_) {
            if (s.installed || (showOriginal_ && s.ours)) ++ok;
            else if (s.gate == Gate::Ok && !s.wentNative && !recordOn(s.id, opts_)) parked += std::string(parked.empty() ? "" : ", ") + kSpriteRecs[int(s.id)].shortName;
            else native += std::string(native.empty() ? "" : ", ") + kSpriteRecs[int(s.id)].shortName;
        }
        // Mention Nameplates only when they have a distinct font, not a punctuation-only composite.
        const bool own = plateOwnFont(opts_);
        const char* plates = plate_.installed && own                                                            ? "; nameplates have their own font"
                             : own && plateWanted(opts_) && plate_.ours && !showOriginal_ && !plate_.adoptGaveUp ? "; nameplates get their own font at the next one drawn"
                             : plateWanted(opts_) && plate_.adoptGaveUp                                          ? "; nameplates keep the game's own font (see the log)"
                                                                                                                 : "";
        return fmt("the menu and HUD fonts: %d of %d textures in place%s%s%s%s", ok, kSpriteRecCount, native.empty() ? "" : (" (native: " + native + ")").c_str(),
                   parked.empty() ? "" : (" (switched off: " + parked + ")").c_str(), plates, showOriginal_ ? "; showing the game's own fonts" : "");
    }
    // Report engines for enabled, built groups; mixed means some classes fell back.
    bool groupEngine(Group g, Engine& e, bool* mixed = nullptr) const {
        const int i = int(g);
        if (i <= 0 || i >= kGroupCount || !engineBuilt_[i] || !opts_.on[i]) return false;
        e = engine_[i];
        if (mixed) *mixed = engineMixed_[i];
        return true;
    }
    void logStatus() const {
        info(fmt("sprites: enabled %d, phase %d, building %d, show original %d, game gone %d; auto-off \"%s\"; failure \"%s\"; texture cache %08X", enabled_, int(phase_), building_,
                 showOriginal_, gameGone_, autoOffReason_.c_str(), failure_.c_str(), unsigned(cacheNow())));
        info(fmt("sprites: the game's language: %s%s; %s letter layouts", clientLangName(lang_), langRead_ ? "" : " (not read yet)", tableSetName(tables_)));
        for (const auto& s : recs_)
            info(fmt("sprites: %-9s gate %d%s%s; record %08X (+40 %08X), original %08X, installed %d, installs %u; ours %08X (%s %s %dx%d, k %d); retired %zu, abandoned %zu",
                     kSpriteRecs[int(s.id)].shortName, int(s.gate), s.why.empty() ? "" : ": ", s.why.c_str(), unsigned(s.rec), unsigned(s.rec ? rd<uint32_t>(s.rec + kTexPrimary) : 0),
                     unsigned(s.original), s.installed, s.installs, unsigned(uintptr_t(s.ours)), texFormatName(s.format), s.pool, s.w, s.h, s.k, s.retired.size(), s.abandoned.size()));
        info(fmt("sprites: keep: %d re-applies this session (chat said %d)", keep_.total(), keep_.chatSaid()));
        for (const Split* sp : {&plate_, &dmg_}) {
            const PlateReport pr = splitReport(sp->kind);
            info(fmt("sprites: %s (%s): page slot %s %08X holds %08X; wanted %d, built %d, installed %d, installs %u, pinned %d; copy %08X (%s); ours %08X (%s %s %dx%d, k %d)",
                     splitName(*sp), sp->kind == SplitKind::Plates ? "R13" : "R15", pr.available ? "at" : "NOT RESOLVED,", unsigned(pr.slotAddr), unsigned(pr.slot), pr.wanted, pr.built,
                     pr.installed, pr.installs, pr.pinned, unsigned(pr.copy), pr.copy ? (pr.copyOk ? "checks out" : "DOES NOT CHECK OUT") : "none", unsigned(uintptr_t(sp->ours)),
                     texFormatName(sp->format), sp->pool, pr.w, pr.h, pr.k));
        }
        const KReport kr = kReport();
        info(fmt("sprites: Sharpness (R16): font font k %d (asked %d), menu2fon %d (%d), mn10font %d (%d), news %d (%d); nameplates %d (%d), damage numbers %d (%d), HUD text %d (%d), "
                 "Job and level tags %d (%d); Auto's window k %d (Nameplates), %d (Damage numbers)",
                 kr.rec[0], kr.wantRec[0], kr.rec[1], kr.wantRec[1], kr.rec[2], kr.wantRec[2], kr.rec[3], kr.wantRec[3], kr.plate, kr.wantPlate, kr.damage, kr.wantDamage, kr.hud, kr.wantHud,
                 kr.jobs, kr.wantJobs, kr.autoKPlate, kr.autoKDamage));
        for (const LeafSplit* ls : {&hud_, &jobs_})
            info(fmt("sprites: %s (R18): copy %08X, ours %08X (%s %s %dx%d, k %d); %zu leaves bound, %u bound in all, %u again after a re-resolve; pinned %d", leafName(*ls),
                     unsigned(uintptr_t(ls->copy)), unsigned(uintptr_t(ls->ours)), texFormatName(ls->format), ls->pool, ls->w, ls->h, ls->k, ls->bound.size(), ls->binds, ls->rebinds, ls->pinned));
    }

private:
    struct Abandoned { uintptr_t rec = 0, original = 0; bool originalRef = false; };
    struct Rec {
        SpriteRec id = SpriteRec::FontFont;
        Gate gate = Gate::Unchecked;
        std::string why;                 // Native/Failed reason
        std::shared_ptr<const std::vector<GlyphSlot>> slots;
        std::shared_ptr<const SpritePixels> rb;
        uintptr_t rec = 0;               // the record TrueFont writes into
        uintptr_t original = 0;          // what +40 held before
        bool installed = false;          // +40 was left holding ours
        bool wentNative = false;         // dropped or given up since load: native until /tfont on
        bool artNative = false;          // native because a record re-created since load holds art other than the pinned art; cleared by /tfont on
        bool layoutNative = false;       // native at install because its rects or code map are not the tables'
        bool retiredHeld = false;        // failed because its record still holds an earlier texture of ours
        bool originalRef = false;        // TrueFont holds a reference on `original` (an adopted foreign one)
        unsigned installs = 0;
        IDirect3DTexture8* ours = nullptr;
        const char* pool = "-";
        TexFormat format = TexFormat::A8R8G8B8;
        int w = 0, h = 0, k = 0;
        bool built = false;
        SpriteOptions builtOpts;
        TexCounts counts;                // its build's From Font and Native by group
        std::shared_ptr<const SpriteAtlas> kept;   // its composite as A8L8 (A8L8 / DXT3 uploads), for a Compress change
        std::vector<uintptr_t> retired;  // earlier textures of ours a record may still hold
        std::vector<Abandoned> abandoned;
        bool heldByRecord = false;       // a restore left ours in a record: never release it
        bool lost = false;               // the record is being looked for again
        int lostTries = 0;
        std::string d1;                  // the install's log line
        RectCheck check;
        bool checked = false;
        uint32_t pin = 0;
        int peak = 0;
        uint32_t sessionPin = 0;         // the pin read at this on's first install (a re-created record must match it)
        bool customArt = false;          // that pin is not the shipped one: custom letter art, placed by the shipped metrics
        CodeMapCheck codes;              // the fontshp / dmgnum code -> rect map (font font only)
        bool codesChecked = false;
        int s = 1;                       // the sheet's scale read at this on's look (2 or 4: a scaled sheet, placed as custom letter art)
    };
    Rec& rec(SpriteRec r) { return recs_[int(r)]; }
    const Rec& rec(SpriteRec r) const { return recs_[int(r)]; }
    static const char* nameOf(const Rec& s) { return kSpriteRecs[int(s.id)].shortName; }
    // Use group and Sharpness in chat; texture names and failure details stay in the log.
    static std::string groupFontText(Group g, int k) { return fmt("the %s font at %dx", groupName(g), k); }
    std::string recordFontText(const Rec& s) const { return groupFontText(baseGroup(s.id, batchOpts_), effRecordK(s.id, batchOpts_)); }
    std::string splitFontText(bool plates) const {
        return plates ? groupFontText(Group::Nameplates, effPlateK(batchOpts_)) : groupFontText(Group::Damage, effDamageK(batchOpts_));
    }
    // List changed groups once in tab order. Global quality changes and explicit rebuilds list all affected
    // groups; shared-texture rebuilds caused by another group do not count as a local setting change.
    struct RebuiltLine {
        struct Item {
            Group g;
            int k;
            bool changed;
        };
        std::vector<Item> items;
        bool all = false;   // a typed rebuild: every item named
        bool empty() const { return items.empty(); }
        // `before`: the options the texture last showed this group with (nullptr: it is new, so it changed).
        void add(Group g, int k, const SpriteOptions* before, const SpriteOptions& now) { items.push_back(Item{g, k, !before || groupChanged(*before, now, g)}); }
        std::string text() const {
            bool any = false;
            if (!all)
                for (const Item& i : items) any = any || i.changed;
            std::string out;
            for (const Group g : kGroupOrder)
                for (const Item& i : items) {
                    if (i.g != g || (any && !i.changed)) continue;
                    const std::string t = fmt("%s at %dx", groupName(i.g), i.k);
                    if ((", " + out + ", ").find(", " + t + ", ") == std::string::npos) out += (out.empty() ? "" : ", ") + t;
                }
            return out;
        }
    };
    // Exclude groups drawing from private splits when describing the shared texture.
    bool recordShows(SpriteRec r, Group g) const {
        if (!recordServes(r, g) || !batchOpts_.on[int(g)]) return false;
        if (r == SpriteRec::FontFont && g == Group::NamesHud) return !leafWanted(hud_, batchOpts_);
        if (r == SpriteRec::FontFont && g == Group::Damage) return !splitWanted(dmg_, batchOpts_);
        if (r == SpriteRec::Menu2fon && g == Group::JobTags) return !leafWanted(jobs_, batchOpts_);
        return true;
    }

    // Private font-font record and composite, reached through registry +1C (Nameplates) or +30 (Damage).
    struct Split {
        explicit Split(SplitKind k) : kind(k) {}
        SplitKind kind;
        IDirect3DTexture8* ours = nullptr;   // the composite (our own reference; the copy's +40 borrows it)
        std::shared_ptr<const SpriteAtlas> kept;   // the composite as A8L8, for a Compress change
        const char* pool = "-";
        TexFormat format = TexFormat::A8R8G8B8;
        int w = 0, h = 0, k = 0;
        bool built = false;
        SpriteOptions builtOpts;
        TexCounts counts;                    // its build's From Font and Native by group
        uint8_t* copy = nullptr;             // kRecordBytes on the process heap: what the slot names while installed
        bool installed = false;              // the slot names the copy
        bool lost = false;                   // it did, and something else replaced it: the next install is a re-apply
        bool pinned = false;                 // a restore could not be verified: the copy and `ours` are never freed
        unsigned installs = 0;
        int waitLogged = 0;
        uint64_t adoptAt = 0;                // adoption (Nameplates only): the next look at a record the slot names that font font's is not
        int adoptTries = 0;
        bool adoptGaveUp = false;            // 30 looks failed (or the art differs); nameplates keep the game's own
    };
    Split plate_{SplitKind::Plates}, dmg_{SplitKind::Damage};
    Split& split(SplitKind k) { return k == SplitKind::Damage ? dmg_ : plate_; }
    static const char* splitName(const Split& sp) { return sp.kind == SplitKind::Plates ? "nameplates" : "damage numbers"; }
    // font font's record found again (rescan, or a fresh look): the page slot's record is looked at afresh.
    void plateRefound() {
        plate_.adoptTries = 0;
        plate_.adoptGaveUp = false;
        plate_.adoptAt = 0;
    }
    bool splitAvailable(const Split& sp) const { return SR_.registry != 0 && (sp.kind == SplitKind::Plates ? SR_.pageRead : SR_.pageDmgRead) != 0; }
    uintptr_t splitSlot(const Split& sp) const { return splitAvailable(sp) ? abs(SR_.registry) + (sp.kind == SplitKind::Plates ? kPageFontshpSlot : kPageDmgnumSlot) : 0; }
    bool plateAvailable() const { return splitAvailable(plate_); }
    // The split needs its own composite now: its slot is known, font font is usable, and plateNeeded / damageNeeded.
    bool splitWanted(const Split& sp, const SpriteOptions& o) const {
        const Rec& ff = rec(SpriteRec::FontFont);
        return splitAvailable(sp) && ff.gate == Gate::Ok && ff.rb && !ff.wentNative && (sp.kind == SplitKind::Plates ? plateNeeded(o) : damageNeeded(o));
    }
    bool plateWanted(const SpriteOptions& o) const { return splitWanted(plate_, o); }
    // samples()' alias: a record's leaves may name its leaf split's copy; the page slots' copies are never in a leaf.
    uintptr_t aliasOf(const Rec& s) const { return s.id == SpriteRec::FontFont ? uintptr_t(hud_.copy) : s.id == SpriteRec::Menu2fon ? uintptr_t(jobs_.copy) : 0; }
    void splitWait(Split& sp, int stage, const std::string& what) {
        if (stage == sp.waitLogged) return;
        sp.waitLogged = stage;
        info(std::string("sprites: ") + splitName(sp) + ": " + what);
    }
    // Rebind only after the game fills the page slot with the live base record. Return true for reapplications.
    bool splitKeep(Split& sp) {
        if (!splitAvailable(sp)) return false;
        const uintptr_t slot = splitSlot(sp);
        uint32_t now = 0;
        if (!rdok(slot, now)) { splitWait(sp, 9, fmt("the page slot %08X cannot be read; nothing is written", unsigned(slot))); return false; }
        const uint32_t copy = uint32_t(uintptr_t(sp.copy));
        if (sp.installed) {
            if (copy && now == copy) {
                // Park an unwanted split unless a prior failed restore pinned it; do not retry that failure every frame.
                if (!splitWanted(sp, opts_) && !sp.pinned && !rebuildAt_ && !building_)
                    parkSplit(sp, sp.kind == SplitKind::Plates ? "Nameplates no longer need their own composite" : "Damage numbers no longer need their own composite");
                return false;
            }
            sp.installed = false;
            sp.lost = true;
            logKeep(fmt("sprites: keep: the %s slot holds %08X, not TrueFont's copy %08X%s", sp.kind == SplitKind::Plates ? "nameplate" : "damage-number", now, copy,
                        now == 0 ? (sp.kind == SplitKind::Plates ? " (emptied: the game fills it again at the next nameplate drawn)" : " (emptied: the game fills it again at the next damage number)")
                                 : ""));
        }
        if (copy && now == copy) {   // a copy a failed restore left in the slot is still ours
            sp.installed = true;
            return false;
        }
        if (!sp.ours || !sp.built || !splitWanted(sp, opts_)) return false;
        const Rec& ff = rec(SpriteRec::FontFont);
        if (!now) {
            splitWait(sp, 1, sp.kind == SplitKind::Plates ? "the page slot is empty (no nameplate drawn yet): the split goes in at the first one"
                                                         : "the page slot is empty (no damage number drawn yet): the split goes in at the first one");
            return false;
        }
        if (ff.lost) { splitWait(sp, 2, fmt("the font font record is being looked for again; the page slot (%08X) is left alone", now)); return false; }
        if (sp.kind == SplitKind::Plates && now != ff.rec && !ff.installed && !adoptPlateRecord(now)) return false;
        if (now != ff.rec || !sameRecord(ff.rec, SpriteRec::FontFont)) {
            splitWait(sp, 3, fmt("the page slot holds %08X, not the font font record %08X: left alone", now, unsigned(ff.rec)));
            return false;
        }
        // The slot does not name the copy now, so the copy may be written: a fresh one of the live record.
        uint8_t buf[kRecordBytes];
        if (!readRaw(ff.rec, buf, sizeof buf)) { splitWait(sp, 4, fmt("the font font record %08X could not be read to copy it", unsigned(ff.rec))); return false; }
        if (!sp.copy) {
            sp.copy = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kRecordBytes));
            if (!sp.copy) { splitWait(sp, 5, "the record copy could not be allocated"); return false; }
        }
        const uint32_t zero = 0, tex = uint32_t(uintptr_t(sp.ours));
        std::memcpy(buf + kRecNext, &zero, 4);
        std::memcpy(buf + kTexPrimary, &tex, 4);
        std::memcpy(buf + kTexAlternate, &zero, 4);
        std::memcpy(sp.copy, buf, sizeof buf);
        sp.pinned = false;   // the slot named something else: nothing refers to the copy but us
        const uint32_t c = uint32_t(uintptr_t(sp.copy));
        const WriteResult w = wrVerifiedWhy<uint32_t>(slot, c, now);
        if (w != WriteResult::Ok) { splitWait(sp, 6, fmt("writing the page slot %s", writeResultText(w))); return false; }
        sp.installed = true;
        ++sp.installs;
        sp.waitLogged = 0;
        const bool again = sp.lost;
        sp.lost = false;
        info(fmt("sprites: %s: page slot %08X: font font record %08X -> TrueFont's copy %08X (+40 %08X, %dx%d k %d)%s", splitName(sp), unsigned(slot), now, c, tex, sp.w, sp.h, sp.k,
                 again ? " (re-applied)" : ""));
        return again;
    }
    // Adopt a replacement page-slot record only if cache lookup and the session art pin validate it.
    bool adoptPlateRecord(uint32_t now) {
        if (plate_.adoptGaveUp || nowMs_ < plate_.adoptAt) return false;
        plate_.adoptAt = nowMs_ + 2000;
        Rec& ff = rec(SpriteRec::FontFont);
        std::string why;
        bool artDiffers = false;
        if (!sameRecord(now, SpriteRec::FontFont)) why = "it is not a font font record";
        else {
            CacheScan cs;
            scanCache(reader_, abs(SR_.texCache), vtableVA(), cs);
            const RecordFind& f = cs.rec[int(SpriteRec::FontFont)];
            if (!f.valid) why = "the texture cache has no one font font record (" + f.why + ")";
            else if (f.first.rec != now) why = fmt("the texture cache's font font record is %08X", unsigned(f.first.rec));
            else {
                SpritePixels px;
                const uint32_t pin = ff.pin;
                const int peak = ff.peak;
                if (readAndPin(ff, rd<uint32_t>(now + kTexPrimary), px, why, false, &artDiffers)) {
                    info(fmt("sprites: nameplates: the game re-created the font font record (%08X -> %08X) while font font is switched off; it is font font's now", unsigned(ff.rec), now));
                    ff.rec = now;
                    plate_.adoptTries = 0;
                    return true;
                }
                ff.pin = pin;   // the record in use keeps its own
                ff.peak = peak;
            }
        }
        // Art that is not the pinned art does not come back by looking again (such a record is dropped at once): given up now.
        if (++plate_.adoptTries >= kLostTries || artDiffers) {
            plate_.adoptGaveUp = true;
            warn(fmt("sprites: nameplates: the page slot holds %08X, a record TrueFont cannot use as font font's (%s)%s; nameplates keep the game's own font %s", now, why.c_str(),
                     artDiffers ? "" : fmt(" after %d looks", plate_.adoptTries).c_str(), artDiffers ? "until font font's record is found again" : "until /tfont on"));
        } else splitWait(plate_, 7, fmt("the page slot holds %08X, not the font font record %08X (%s); looked at again every 2 s", now, unsigned(ff.rec), why.c_str()));
        return false;
    }
    // Clear our page slot and verify it no longer names the copy. Pin the copy and texture on failure.
    bool restoreSplit(Split& sp, OffReport& rep) {
        if (!sp.copy) { sp.installed = false; return true; }
        rep.wasOn = true;
        const uintptr_t slot = splitSlot(sp);
        const uint32_t copy = uint32_t(uintptr_t(sp.copy));
        uint32_t now = 0, refilled = 0;
        std::string left;
        const bool read = slot && rdok(slot, now);
        if (!read) left = "the page slot could not be read";
        else if (now != copy) {}   // not ours now: nothing to write
        else if (!gameAlive()) left = "the game is closing (it zeroes the slot itself in its teardown; nothing is written)";
        else {
            const WriteResult w = wrVerifiedWhy<uint32_t>(slot, 0u, copy);
            // Off-thread, the game may refill the cleared slot before readback; any value except our copy is safe.
            if (w == WriteResult::ReadBackDiffers && rdok(slot, refilled) && refilled != copy) {}
            else if (w != WriteResult::Ok) left = std::string("writing it ") + writeResultText(w);
        }
        const char* slotName = sp.kind == SplitKind::Plates ? "nameplate" : "damage-number";
        if (!left.empty()) {
            sp.installed = !read || now == copy;
            sp.pinned = true;
            rep.left.push_back(fmt("the %s slot still names TrueFont's copy of the font font record", slotName));
            if (!gameAlive()) {   // no leftover: the game takes the slot, the copy and the texture with it
                rep.gameGone = true;
                info(fmt("sprites: restore: %s slot %08X: %s; the copy %08X and its texture %08X stay and go with the game", slotName, unsigned(slot), left.c_str(), copy,
                         unsigned(uintptr_t(sp.ours))));
            } else
                warn(fmt("sprites: restore: %s slot %08X: %s; the copy %08X and its texture %08X are kept for good (the DLL is pinned)", slotName, unsigned(slot), left.c_str(), copy,
                         unsigned(uintptr_t(sp.ours))));
            return false;
        }
        info(fmt("sprites: restore: %s slot %08X: %s", slotName, unsigned(slot),
                 now != copy  ? fmt("holds %08X, not the copy: left alone", now).c_str()
                 : refilled   ? fmt("TrueFont's copy -> 0, and the game filled it again at once (%08X)", refilled).c_str()
                              : "TrueFont's copy -> 0 (the game fills it again by name)"));
        sp.installed = false;
        sp.lost = false;
        sp.pinned = false;
        return true;
    }
    // Free only unreferenced copies on the Direct3D release path. DataOnly draws may still hold pointers.
    void freeSplit(Split& sp, bool d3d) {
        if (sp.pinned || sp.installed || !d3d) return;
        if (sp.ours) {
            if (gameAlive()) {
                sp.ours->Release();
                info(fmt("sprites: restore: TrueFont's %s composite %08X released", sp.kind == SplitKind::Plates ? "nameplate" : "damage-number", unsigned(uintptr_t(sp.ours))));
            } else info(fmt("sprites: restore: TrueFont's %s composite %08X is kept (the game is closing)", sp.kind == SplitKind::Plates ? "nameplate" : "damage-number", unsigned(uintptr_t(sp.ours))));
            sp.ours = nullptr;
        }
        if (sp.copy) {
            HeapFree(GetProcessHeap(), 0, sp.copy);
            sp.copy = nullptr;
        }
        sp.built = false;
        sp.kept.reset();
        sp.w = sp.h = sp.k = 0;
        sp.waitLogged = 0;
    }
    // The split's group goes back to drawing from font font's own record (nothing to split any more, font font dropped).
    void parkSplit(Split& sp, const char* why) {
        if (!gameAlive() || (!sp.copy && !sp.ours)) return;
        OffReport rep;
        const bool ok = restoreSplit(sp, rep);
        freeSplit(sp, true);
        if (ok) info(std::string("sprites: ") + splitName(sp) + ": " + why + ": the page slot is the game's again, the composite released");
        else warn(std::string("sprites: ") + splitName(sp) + ": " + why + ", but " + rep.leftText());
    }
    void parkSplits(const char* why) {
        parkSplit(plate_, why);
        parkSplit(dmg_, why);
    }
    // Install the rebuilt split. baseBefore describes the shared texture used before a split first existed.
    void finishSplit(Split& sp, SpriteBuilt& b, RebuiltLine& rebuilt, const SpriteOptions* baseBefore, std::string& firstErr) {
        const bool plates = sp.kind == SplitKind::Plates;
        const char* comp = plates ? "the nameplate composite" : "the damage-number composite";
        if (!splitWanted(sp, opts_)) {
            info(fmt("sprites: build: %s discarded (%s draw as font font's own texture now, or font font went native)", comp, plates ? "nameplates" : "damage numbers"));
            return;
        }
        if (!b.atlas) {
            err(fmt("sprites: build: %s could not be built (%s)%s", comp, b.why.c_str(), sp.ours ? "; the current one stays" : ""));
            if (firstErr.empty()) firstErr = splitFontText(plates) + " could not be made";
            return;
        }
        const SpriteAtlas& a = *b.atlas;
        if (plates) {
            if (!batchOpts_.on[int(Group::Nameplates)]) info("sprites: build: nameplates: switched off (the game's own art in the Names and HUD rects)");
            else {
                const SpriteGroupStats& gs = a.group[int(Group::NamesHud)];
                info(fmt("sprites: build: nameplates (font font's Names and HUD rects): %s%s, %s, %d slots: %d drawn, %d native (%d not in the font, %d no native ink, %d errors); condensed %d, "
                         "squeezed %d, shifted %d, clipped %d, rim trimmed %d",
                         toUtf8(gs.face).c_str(), gs.substituted ? " (SUBSTITUTED by GDI)" : "", groupEngineText(gs.engine, gs.engineMixed).c_str(), gs.slots, gs.rendered, gs.native, gs.noGlyph, gs.noInk, gs.errors, gs.condensed, gs.squeezed,
                         gs.shifted, gs.clipped, gs.trimmed));
            }
            if (batchOpts_.on[int(Group::Nameplates)]) {
                const SpriteGroupStats& ms = a.group[int(Group::Nameplates)];
                info(fmt("sprites: build: nameplates' marks (the Menu labels punctuation and tiny digits the fontshp page draws): %s, %s, %d slots: %d drawn, %d native",
                         toUtf8(ms.face).c_str(), groupEngineText(ms.engine, ms.engineMixed).c_str(), ms.slots, ms.rendered, ms.native));
            }
        } else {
            const SpriteGroupStats& gs = a.group[int(Group::Damage)];
            info(fmt("sprites: build: damage numbers (their own composite, at their own Sharpness): %s, %s, %d slots: %d drawn, %d native", toUtf8(gs.face).c_str(), groupEngineText(gs.engine, gs.engineMixed).c_str(),
                     gs.slots, gs.rendered, gs.native));
        }
        info(fmt("sprites: build: %s %dx%d (k %d) in %.0f ms on the worker, converted to %s in %.0f ms", comp, a.w, a.h, a.k, a.ms, texFormatName(a.tex.format), a.tex.ms));
        std::string why;
        IDirect3DTexture8* t = createTexture(a, why);
        if (!t) {
            err(fmt("sprites: build: %s's texture could not be created (%s)%s", comp, why.c_str(), sp.ours ? "; the current one stays" : ""));
            if (firstErr.empty()) firstErr = splitFontText(plates) + " could not be made";
            return;
        }
        const Group shows = plates ? Group::Nameplates : Group::Damage;
        // A disabled Nameplates group may still need a native-art composite while HUD or Labels is enabled.
        if (batchOpts_.on[int(shows)]) rebuilt.add(shows, a.k, sp.built ? &sp.builtOpts : baseBefore, batchOpts_);   // before builtOpts is replaced
        IDirect3DTexture8* old = sp.ours;
        sp.ours = t;
        sp.w = a.w;
        sp.h = a.h;
        sp.k = a.k;
        sp.pool = poolOf_;
        sp.format = formatOf_;
        sp.built = true;
        sp.builtOpts = batchOpts_;
        sp.counts = texCounts(a);
        noteEngines(a);
        sp.kept = b.kept;
        b.atlas.reset();
        if (sp.copy) {   // the copy names the new one before the old one goes (a draw binds +40 when it draws)
            const uint32_t tex = uint32_t(uintptr_t(t));
            std::memcpy(sp.copy + kTexPrimary, &tex, 4);
        }
        if (old) {
            old->Release();   // nothing names it now (D3D holds its own reference on a bound texture)
            info(fmt("sprites: rebuild: %s %08X -> %08X", comp, unsigned(uintptr_t(old)), unsigned(uintptr_t(t))));
        }
    }

    // Private records reached through leaf +4C: fontshp for HUD, pname for Job Tags.
    // The copy's +40 borrows its split's composite texture.
    struct LeafSplit {
        LeafSplit(SplitKind k, SpriteRec b, const char* res) : kind(k), base(b), resName(res) {}
        SplitKind kind;
        SpriteRec base;
        const char* resName;                 // the sprite set whose leaves are bound (16 bytes)
        IDirect3DTexture8* ours = nullptr;   // the composite (our own reference; the copy's +40 borrows it)
        std::shared_ptr<const SpriteAtlas> kept;
        const char* pool = "-";
        TexFormat format = TexFormat::A8R8G8B8;
        int w = 0, h = 0, k = 0;
        bool built = false;
        SpriteOptions builtOpts;
        TexCounts counts;                    // its build's From Font and Native by group
        uint8_t* copy = nullptr;             // kRecordBytes on the process heap: what the bound leaves' +4C name
        std::vector<uintptr_t> bound;        // the leaves whose +4C named the copy at the last bind pass (a fresh walk's)
        bool pinned = false;                 // a restore could not be verified: the copy and `ours` are never freed
        unsigned binds = 0, rebinds = 0;     // leaves bound; leaves bound again after the game re-resolved them
        unsigned refused = 0;                // leaves naming the record that failed the identity check
        int waitLogged = 0;
        std::string lastWalkWhy;             // the last bind pass's walk failure (logged once per reason)
    };
    LeafSplit hud_{SplitKind::Hud, SpriteRec::FontFont, kFontshpName}, jobs_{SplitKind::Jobs, SpriteRec::Menu2fon, kPnameName};
    static const char* leafName(const LeafSplit& ls) { return ls.kind == SplitKind::Hud ? "HUD text" : "Job and level tags"; }
    std::string leafFontText(const LeafSplit& ls) const {   // groupFontText's words for a leaf split
        return ls.kind == SplitKind::Hud ? groupFontText(Group::NamesHud, effHudK(batchOpts_)) : groupFontText(Group::JobTags, effJobsK(batchOpts_));
    }
    bool leafWanted(const LeafSplit& ls, const SpriteOptions& o) const {
        const Rec& b = rec(ls.base);
        return SR_.registry && SR_.recordBytesOk && b.gate == Gate::Ok && b.rb && !b.wentNative && recordOn(ls.base, o) && (ls.kind == SplitKind::Hud ? hudNeeded(o) : jobsNeeded(o));
    }
    void leafWait(LeafSplit& ls, int stage, const std::string& what) {
        if (stage == ls.waitLogged) return;
        ls.waitLogged = stage;
        info(std::string("sprites: ") + leafName(ls) + ": " + what);
    }
    // Find exactly one live sprite set and walk it immediately before writing.
    // Duplicate names are unsafe: unloading by name frees every matching set.
    bool freshSetLeaves(const LeafSplit& ls, std::vector<LiveLeaf>& out, std::string& why) const {
        out.clear();
        std::vector<uint32_t> res;
        bool limitHit = false;
        if (!resourcesNamed(reader_, abs(SR_.registry), ls.resName, res, limitHit)) {
            why = limitHit ? "the sprite registry did not end (limit)" : "the sprite registry could not be read";
            return false;
        }
        if (res.size() != 1) {
            why = res.empty() ? fmt("\"%.16s\" is not loaded", ls.resName) : fmt("%zu sets are named \"%.16s\"; none is bound", res.size(), ls.resName);
            return false;
        }
        const Rec& b = rec(ls.base);
        const char* name = kSpriteRecs[int(ls.base)].name;
        const uintptr_t copy = uintptr_t(ls.copy), native = b.rec;
        RegistryScan rs;
        const bool ok = walkResource(reader_, res[0], [&](const LiveLeaf& l) { return samples(l, native, name, copy); }, rs, RegistryLimits{});
        if (!ok || rs.limitHit) {
            why = rs.limitHit ? fmt("\"%.16s\" did not end (limit)", ls.resName) : fmt("\"%.16s\" could not be read", ls.resName);
            return false;
        }
        out = std::move(rs.kept);
        return true;
    }
    // The leaf read once more just before its write: the walk's rect (one the tables accept), the record's texture name, +4C.
    enum class LeafId { Unreadable, Differs, Ok };
    static LeafId leafIdentity(const LiveLeaf& l, SpriteRec base, TableSet ts, int scale, uint32_t& v) {
        v = 0;
        uint8_t head[kLeafRecord + 4];
        if (!readRaw(l.addr, head, sizeof head)) return LeafId::Unreadable;
        Rect r;
        std::memcpy(&r.w, head + kLeafW, 2);
        std::memcpy(&r.h, head + kLeafH, 2);
        std::memcpy(&r.u, head + kLeafU, 2);
        std::memcpy(&r.v, head + kLeafV, 2);
        std::memcpy(&v, head + kLeafRecord, 4);
        bool exact = false;
        const Rect t = scaledDown(r, scale, exact);   // the table's rect (the record's sheet scale)
        if (!(r == l.r) || !exact || !rectKnown(base, t, ts)) return LeafId::Differs;
        const char* want = kSpriteRecs[int(base)].name;
        for (int i = 0; i < 16; i++) {
            char a = char(head[kLeafName + i]), c = want[i];
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (a != c) return LeafId::Differs;
        }
        return LeafId::Ok;
    }
    // Bind only native record pointers and preserve foreign values. Count game re-resolution separately.
    void leafBind(LeafSplit& ls) {
        const Rec& b = rec(ls.base);
        if (!ls.ours || !ls.built || b.lost || !sameRecord(b.rec, ls.base) || !gameAlive()) return;
        if (!ls.copy) {
            uint8_t buf[kRecordBytes];
            if (!readRaw(b.rec, buf, sizeof buf)) { leafWait(ls, 4, fmt("the %s record %08X could not be read to copy it", nameOf(b), unsigned(b.rec))); return; }
            ls.copy = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kRecordBytes));
            if (!ls.copy) { leafWait(ls, 5, "the record copy could not be allocated"); return; }
            const uint32_t zero = 0, tex = uint32_t(uintptr_t(ls.ours));
            std::memcpy(buf + kRecNext, &zero, 4);
            std::memcpy(buf + kTexPrimary, &tex, 4);
            std::memcpy(buf + kTexAlternate, &zero, 4);
            std::memcpy(ls.copy, buf, sizeof buf);
        }
        std::vector<LiveLeaf> fresh;
        std::string why;
        if (!freshSetLeaves(ls, fresh, why)) {
            if (why != ls.lastWalkWhy) {
                ls.lastWalkWhy = why;
                warn(fmt("sprites: %s: %s; nothing is bound this pass (checked again in a second)", leafName(ls), why.c_str()));
            }
            return;
        }
        ls.lastWalkWhy.clear();
        const uint32_t copy = uint32_t(uintptr_t(ls.copy)), native = uint32_t(b.rec);
        std::vector<uintptr_t> now;
        int wrote = 0, again = 0, left = 0, failed = 0, refused = 0;
        for (const LiveLeaf& l : fresh) {
            uint32_t v = 0;
            const LeafId id = leafIdentity(l, ls.base, tables_, b.s, v);
            if (id == LeafId::Unreadable) { ++failed; continue; }
            if (v == copy) { now.push_back(l.addr); continue; }   // bound already (the restore walks every set for the copy anyway)
            if (v != native) { ++left; continue; }                 // 0 (resolved lazily by name at its next draw) or foreign: next time
            if (id != LeafId::Ok) { ++refused; continue; }        // names the record but is not the leaf the walk found: never written
            if (wrVerifiedWhy<uint32_t>(l.addr + kLeafRecord, copy, native) != WriteResult::Ok) { ++failed; continue; }
            now.push_back(l.addr);
            ++wrote;
            if (std::find(ls.bound.begin(), ls.bound.end(), l.addr) != ls.bound.end()) ++again;
        }
        ls.bound = std::move(now);
        ls.binds += unsigned(wrote - again);
        ls.rebinds += unsigned(again);
        if (refused) {
            ls.refused += unsigned(refused);
            if (ls.waitLogged != 7) {
                ls.waitLogged = 7;
                warn(fmt("sprites: %s: %d leaves of \"%.16s\" name the %s record but are not the leaf the walk found (rect or texture name differ); they are not written", leafName(ls),
                         refused, ls.resName, nameOf(b)));
            }
        }
        if (wrote) {
            if (!refused) ls.waitLogged = 0;
            info(fmt("sprites: %s: %d leaves of \"%.16s\" bound to TrueFont's copy %08X of the %s record %08X (+40 %08X, %dx%d k %d)%s; %zu bound in all%s", leafName(ls), wrote,
                     ls.resName, copy, nameOf(b), native, unsigned(uintptr_t(ls.ours)), ls.w, ls.h, ls.k,
                     again ? fmt(", %d of them again (the game re-resolved its leaves: a frame-style change)", again).c_str() : "", ls.bound.size(),
                     left || failed ? fmt(" (%d left alone, %d not written)", left, failed).c_str() : ""));
        } else if (ls.bound.empty()) leafWait(ls, 1, fmt("no leaf of \"%.16s\" names the %s record now (%d left alone); bound at the next check", ls.resName, nameOf(b), left));
    }
    // Restore by value across the entire live registry, then verify with a second complete walk.
    // An incomplete restore pins the copy and texture. Pointer writes only; safe for DataOnly release.
    bool leafRestore(LeafSplit& ls, OffReport& rep) {
        if (!ls.copy) { ls.bound.clear(); return true; }
        rep.wasOn = true;
        const Rec& b = rec(ls.base);
        std::string left;
        int wrote = 0, stayed = 0, visited = 0;
        size_t outside = 0;   // of them, leaves of another set than the split's own (a stray bind)
        const uint32_t copy = uint32_t(uintptr_t(ls.copy));
        const double t0 = qpcMs();
        if (!gameAlive()) left = "the game is closing (its leaves go with it; nothing is written)";
        else {
            uint32_t native = sameRecord(b.rec, ls.base, true) ? uint32_t(b.rec) : 0;
            if (!native) {   // the record was re-created: the one its name finds now (0: the lazy path finds it at the next draw)
                CacheScan cs;
                scanCache(reader_, abs(SR_.texCache), vtableVA(), cs);
                if (cs.rec[int(ls.base)].valid) native = uint32_t(cs.rec[int(ls.base)].first.rec);
            }
            std::vector<uintptr_t> naming;
            bool limitHit = false;
            if (!leavesNaming(reader_, abs(SR_.registry), copy, naming, visited, limitHit)) left = limitHit ? "the sprite registry did not end (limit)" : "the sprite registry could not be read";
            else {
                for (const uintptr_t at : naming) {
                    if (std::find(ls.bound.begin(), ls.bound.end(), at) == ls.bound.end()) ++outside;
                    if (wrVerifiedWhy<uint32_t>(at + kLeafRecord, native, copy) == WriteResult::Ok) ++wrote;
                    else ++stayed;
                }
                if (stayed) left = fmt("%d leaves could not be written back", stayed);
                else {   // the free waits for a second complete walk that finds none
                    std::vector<uintptr_t> still;
                    int again = 0;
                    if (!leavesNaming(reader_, abs(SR_.registry), copy, still, again, limitHit))
                        left = limitHit ? "the sprite registry did not end at the check (limit)" : "the sprite registry could not be read at the check";
                    else if (!still.empty()) left = fmt("%zu leaves still name the copy after the restore", still.size());
                }
            }
        }
        if (!left.empty()) {
            ls.pinned = true;
            rep.left.push_back(fmt("%s's leaves may still name TrueFont's copy of the %s record", leafName(ls), nameOf(b)));
            if (!gameAlive()) {   // no leftover: the game takes its leaves, the copy and the texture with it
                rep.gameGone = true;
                info(fmt("sprites: restore: %s: %s; the copy %08X and its texture %08X stay and go with the game", leafName(ls), left.c_str(), copy, unsigned(uintptr_t(ls.ours))));
            } else
                warn(fmt("sprites: restore: %s: %s; the copy %08X and its texture %08X are kept for good (the DLL is pinned)", leafName(ls), left.c_str(), copy, unsigned(uintptr_t(ls.ours))));
            return false;
        }
        info(fmt("sprites: restore: %s: %d leaves named TrueFont's copy (%zu outside the ones bound in \"%.16s\"): back to the %s record; none left in %d leaves (both walks "
                 "%.2f ms, QPC)",
                 leafName(ls), wrote, outside, ls.resName, nameOf(b), visited, qpcMs() - t0));
        ls.bound.clear();
        ls.pinned = false;
        return true;
    }
    void leafFree(LeafSplit& ls, bool d3d) {
        if (ls.pinned || !ls.bound.empty() || !d3d) return;
        if (ls.ours) {
            if (gameAlive()) {
                ls.ours->Release();
                info(fmt("sprites: restore: TrueFont's %s composite %08X released", leafName(ls), unsigned(uintptr_t(ls.ours))));
            } else info(fmt("sprites: restore: TrueFont's %s composite %08X is kept (the game is closing)", leafName(ls), unsigned(uintptr_t(ls.ours))));
            ls.ours = nullptr;
        }
        if (ls.copy) {
            HeapFree(GetProcessHeap(), 0, ls.copy);
            ls.copy = nullptr;
        }
        ls.built = false;
        ls.kept.reset();
        ls.w = ls.h = ls.k = 0;
        ls.waitLogged = 0;
    }
    void leafPark(LeafSplit& ls, const char* why) {
        if (!gameAlive() || (!ls.copy && !ls.ours)) return;
        OffReport rep;
        const bool ok = leafRestore(ls, rep);
        leafFree(ls, true);
        if (ok) info(std::string("sprites: ") + leafName(ls) + ": " + why + ": its leaves name the record again, the composite released");
        else warn(std::string("sprites: ") + leafName(ls) + ": " + why + ", but " + rep.leftText());
    }
    // Rebind once per second, including during builds. Park unwanted splits only between batches.
    void keepLeafSplits() {
        if (showOriginal_) return;
        for (LeafSplit* ls : {&hud_, &jobs_}) {
            if (!ls->ours && !ls->copy) continue;
            if (!leafWanted(*ls, opts_)) {
                if (!ls->pinned && !rebuildAt_ && !building_) leafPark(*ls, ls->kind == SplitKind::Hud ? "HUD text no longer needs its own copy" : "Job and level tags no longer need their own copy");
                continue;
            }
            leafBind(*ls);
        }
    }
    // A leaf split's composite built: into the copy's +40 and in place of the old one; bound now.
    void finishLeafSplit(LeafSplit& ls, SpriteBuilt& b, RebuiltLine& rebuilt, const SpriteOptions* baseBefore, std::string& firstErr) {
        const char* comp = ls.kind == SplitKind::Hud ? "the HUD text composite" : "the Job and level tags composite";
        if (!leafWanted(ls, opts_)) { info(fmt("sprites: build: %s discarded (the group draws from its record's own texture now)", comp)); return; }
        if (!b.atlas) {
            err(fmt("sprites: build: %s could not be built (%s)%s", comp, b.why.c_str(), ls.ours ? "; the current one stays" : ""));
            if (firstErr.empty()) firstErr = leafFontText(ls) + " could not be made";
            return;
        }
        const SpriteAtlas& a = *b.atlas;
        info(fmt("sprites: build: %s %dx%d (k %d) in %.0f ms on the worker, converted to %s in %.0f ms", comp, a.w, a.h, a.k, a.ms, texFormatName(a.tex.format), a.tex.ms));
        std::string why;
        IDirect3DTexture8* t = createTexture(a, why);
        if (!t) {
            err(fmt("sprites: build: %s's texture could not be created (%s)%s", comp, why.c_str(), ls.ours ? "; the current one stays" : ""));
            if (firstErr.empty()) firstErr = leafFontText(ls) + " could not be made";
            return;
        }
        const Group shows = ls.kind == SplitKind::Hud ? Group::NamesHud : Group::JobTags;
        if (batchOpts_.on[int(shows)]) rebuilt.add(shows, a.k, ls.built ? &ls.builtOpts : baseBefore, batchOpts_);   // only while on; before builtOpts is replaced
        IDirect3DTexture8* old = ls.ours;
        ls.ours = t;
        ls.w = a.w;
        ls.h = a.h;
        ls.k = a.k;
        ls.pool = poolOf_;
        ls.format = formatOf_;
        ls.built = true;
        ls.builtOpts = batchOpts_;
        ls.counts = texCounts(a);
        noteEngines(a);
        ls.kept = b.kept;
        b.atlas.reset();
        if (ls.copy) {   // the copy names the new one before the old one goes
            const uint32_t tex = uint32_t(uintptr_t(t));
            std::memcpy(ls.copy + kTexPrimary, &tex, 4);
        }
        if (old) {
            old->Release();
            info(fmt("sprites: rebuild: %s %08X -> %08X", comp, unsigned(uintptr_t(old)), unsigned(uintptr_t(t))));
        }
        if (!showOriginal_ && gameAlive()) leafBind(ls);
    }

    uintptr_t abs(uint32_t rva) const { return rva ? client_.base + rva : 0; }
    uintptr_t cacheNow() const { return SR_.texCache ? rd<uint32_t>(abs(SR_.texCache)) : 0; }
    // The game is tearing down once the texture cache global (or v1's renderer global, when known) is 0.
    bool gameAlive() const { return cacheNow() != 0 && (!rendererRva_ || rd<uint32_t>(abs(rendererRva_)) != 0); }
    uint32_t vtableVA() const { return SR_.texVtable ? uint32_t(client_.base + SR_.texVtable) : 0; }
    // Revalidate identity before writing. A skipped record may still be restored to its original texture.
    bool sameRecord(uintptr_t r, SpriteRec id, bool restoring = false) const {
        if (r < 0x10000) return false;
        TexRecord t;
        if (!readRecord(reader_, r, t)) return false;
        const SpriteRecInfo& in = kSpriteRecs[int(id)];
        const int sc = (std::max)(1, rec(id).s);   // this on's sheet scale: a record at another scale is not this one
        return vtableVA() && t.vtable == vtableVA() && std::memcmp(t.name, in.name, 16) == 0 && t.w == in.w * sc && t.h == in.h * sc && (restoring || !t.skipped());
    }
    static bool isRetired(const Rec& s, uintptr_t t) { for (const uintptr_t r : s.retired) if (r == t) return true; return false; }
    static bool isOurs(const Rec& s, uintptr_t t) { return t && (t == uintptr_t(s.ours) || isRetired(s, t)); }

    // One read of the resident sets' names; true once the language is known (then never read again).
    bool readLanguage() {
        if (langRead_) return true;
        if (!SR_.registry) return false;
        LangVote v;
        bool limit = false;
        const bool ok = readClientLang(reader_, abs(SR_.registry), v, limit);
        if (!ok || v.lang() == ClientLang::Unknown) {
            if (ok && v.mixed && !langMixedSaid_) {
                langMixedSaid_ = true;
                warn(fmt("sprites: the registry holds the window-frame sprite sets of two languages (the first \"%s\"): the game's language is not read from them", v.bank.c_str()));
            }
            return false;
        }
        lang_ = v.lang();
        langRead_ = true;
        TableSet ts = TableSet::English;
        const bool known = tableSetFor(lang_, ts);
        info(fmt("sprites: the game's language: %s (the resident sprite set \"%s\"); %s", clientLangName(lang_), v.bank.c_str(),
                 known ? (std::string(tableSetName(ts)) + " letter layouts").c_str() : "no letter layouts for it (the menu and HUD fonts stay the game's own)"));
        if (known && !tablesTaken()) useTableSet(ts);   // the look (begin) or lateLanguage takes them otherwise
        return true;
    }
    // A record was checked, or something of TrueFont's built or held, with the table set in use.
    bool tablesTaken() const {
        for (const auto& r : recs_)
            if (r.gate != Gate::Unchecked || r.ours || r.built) return true;
        return false;
    }
    // If language detection follows installation, restore unsupported layouts or restart with the Japanese tables.
    // When disabled, defer switching until the next enable.
    void lateLanguage() {
        TableSet ts = TableSet::English;
        const bool known = tableSetFor(lang_, ts);
        const bool looked = enabled_ && (phase_ == Phase::Building || phase_ == Phase::Installed);
        if (!known) {
            if (looked) {
                info(fmt("sprites: the game's language was read after the first look (%s): what that look put in place goes back", clientLangName(lang_)));
                refuseLanguage();
            }
            return;
        }
        if (ts == tables_) return;
        if (!tablesTaken()) { useTableSet(ts); return; }
        if (looked || (enabled_ && phase_ == Phase::Off && !failure_.empty())) {
            const bool original = showingOriginal();
            info(fmt("sprites: the game's language was read after the first look (%s): a fresh look takes the %s letter layouts", clientLangName(lang_), tableSetName(ts)));
            const OffReport rep = turnOff("a fresh look for the game's language");
            if (!rep.clean()) warn("sprites: the fresh look for the game's language left: " + rep.leftText());
            turnOn(false);
            originalAtInstall_ = original;   // Show original holds across it
            return;
        }
        info(fmt("sprites: the %s letter layouts are taken at the next look (the menu and HUD fonts are off)", tableSetName(ts)));
    }
    // The table set, and every record's slots from it, while nothing has been checked or built with the other.
    void useTableSet(TableSet ts) {
        if (ts == tables_) return;
        for (const auto& r : recs_)
            if (r.gate != Gate::Unchecked || r.ours || r.built) {
                warn(fmt("sprites: the %s letter layouts are not taken: %s was already checked with the %s ones (/tfont off, then /tfont on, uses them)", tableSetName(ts),
                         nameOf(r), tableSetName(tables_)));
                return;
            }
        tables_ = ts;
        std::string counts;
        for (auto& r : recs_) {
            r.slots = std::make_shared<const std::vector<GlyphSlot>>(slotsFor(r.id, ts));
            counts += fmt("%s%s %zu", counts.empty() ? "" : ", ", nameOf(r), r.slots->size());
        }
        info(fmt("sprites: %s letter layouts in use; slots: %s", tableSetName(ts), counts.c_str()));
    }
    // Reject unsupported languages and restore any textures installed before language detection.
    void refuseLanguage() {
        const char* name = clientLangName(lang_);
        if (!deFrSaid_) {
            deFrSaid_ = true;
            warn(fmt("sprites: German/French layouts are not supported yet (the game is %s): the menu and HUD fonts keep the game's own; Chat/Items is not affected", name));
        }
        for (auto& s : recs_) {   // every record, a checked one too: no group is left half TrueFont's
            s.gate = Gate::Native;
            s.why = fmt("the game's language is %s, which has no letter layouts in TrueFont", name);
            s.layoutNative = true;
            s.artNative = s.wentNative = false;
            s.rb.reset();
        }
        fail(languageRefusal(), std::string(), true);
    }
    void fail(const std::string& outcome, const std::string& detail = std::string(), bool persistent = false) {
        failure_ = outcome;
        failurePersistent_ = persistent;
        ++gen_;
        announceRebuild_ = false;
        err("sprites: could not turn on: " + outcome + (detail.empty() ? std::string() : " (" + detail + ")"));
        if (outcome != lastSaidFailure_) {   // once per outcome, as swap.h fail()
            lastSaidFailure_ = outcome;
            chat("the menu and HUD fonts could not turn on: " + outcome + ". See the log.", 0x68);
        }
        stopWorker(2000);
        const OffReport rep = restoreAll(true);
        if (!rep.clean()) warn("sprites: after the refusal, left: " + rep.leftText());
        phase_ = Phase::Off;
        originalAtInstall_ = false;
    }
    void setGate(Rec& s, Gate g, const std::string& why) {
        s.gate = g;
        s.why = why;
        if (g == Gate::Native) warn(fmt("sprites: %s stays native on this client: %s", nameOf(s), why.c_str()));
        else if (g == Gate::Failed) err(fmt("sprites: %s stays native: %s", nameOf(s), why.c_str()));
    }
    bool stopWorker(unsigned long ms) {
        if (!building_) return true;
        const bool ok = worker_.stop(ms);
        if (ok) {
            building_ = false;
            worker_.take();
        }
        return ok;
    }

    // GetLevelDesc of a texture pointer read from game memory (SEH-guarded).
    bool describeTex(uintptr_t t, D3DSURFACE_DESC& d, std::string& why) const {
        auto* tex = reinterpret_cast<IDirect3DTexture8*>(t);
        HRESULT hr = E_FAIL;
        d = D3DSURFACE_DESC{};
        if (!seh([&] { hr = tex->GetLevelDesc(0, &d); })) { why = fmt("texture %08X: GetLevelDesc faulted (not a texture)", unsigned(t)); return false; }
        if (FAILED(hr)) { why = fmt("texture %08X: GetLevelDesc failed (0x%08X)", unsigned(t), unsigned(hr)); return false; }
        return true;
    }
    static std::string descText(const D3DSURFACE_DESC& d) { return fmt("%s %ux%u pool %s", formatName(d.Format).c_str(), d.Width, d.Height, poolName(d.Pool)); }
    // The record's own texture into A8R8G8B8 at its size: the table's times the sheet's scale `sc`.
    bool readSprite(uintptr_t t, const D3DSURFACE_DESC& d, const SpriteRecInfo& in, int sc, SpritePixels& out, std::string& why) const {
        if (d.Width != in.w * unsigned(sc) || d.Height != in.h * unsigned(sc)) { why = fmt("its texture is %ux%u, not the record's %ux%u", d.Width, d.Height, in.w * unsigned(sc), in.h * unsigned(sc)); return false; }
        if (d.Pool == D3DPOOL_DEFAULT) { why = "its texture is in D3DPOOL_DEFAULT, which cannot be locked"; return false; }
        const bool dxt3 = d.Format == D3DFMT_DXT3, argb = d.Format == D3DFMT_A8R8G8B8;
        if (!dxt3 && !argb) { why = "its texture format " + formatName(d.Format) + " is not one TrueFont decodes (DXT3, A8R8G8B8)"; return false; }
        out.w = int(d.Width);
        out.h = int(d.Height);
        out.px.assign(size_t(out.w) * size_t(out.h), 0);
        auto* tex = reinterpret_cast<IDirect3DTexture8*>(t);
        D3DLOCKED_RECT lr{};
        HRESULT hr = E_FAIL;
        if (!seh([&] { hr = tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY); })) { why = "LockRect faulted"; return false; }
        if (FAILED(hr) || !lr.pBits) {
            if (SUCCEEDED(hr)) seh([&] { tex->UnlockRect(0); });
            why = fmt("LockRect failed (0x%08X)", unsigned(hr));
            return false;
        }
        bool ok = false;
        const bool ran = seh([&] {
            if (dxt3) ok = decodeDxt3(static_cast<const uint8_t*>(lr.pBits), out.w, out.h, lr.Pitch, out.px.data());
            else if (size_t(lr.Pitch) >= size_t(out.w) * 4) {
                copyRows(reinterpret_cast<uint8_t*>(out.px.data()), size_t(out.w) * 4, static_cast<const uint8_t*>(lr.pBits), size_t(lr.Pitch), size_t(out.w) * 4, size_t(out.h));
                ok = true;
            }
        });
        seh([&] { tex->UnlockRect(0); });
        if (!ran || !ok) { why = ran ? "the locked data could not be decoded" : "reading the locked data faulted"; out.px.clear(); return false; }
        return true;
    }

    // Pin art on the first readback of each enable; recreated records must match that pin.
    bool readAndPin(Rec& s, uintptr_t t, SpritePixels& px, std::string& why, bool first, bool* artDiffers = nullptr) {
        if (artDiffers) *artDiffers = false;
        D3DSURFACE_DESC d{};
        if (!describeTex(t, d, why) || !readSprite(t, d, kSpriteRecs[int(s.id)], s.s, px, why)) { why = "its texture could not be read back: " + why; return false; }
        const uint32_t pin = texturePin(s.id, px.px.data(), px.w, px.h, tables_, s.s), shipped = tableFor(s.id, tables_).pin;
        int peak = 0;
        for (const uint32_t p : px.px) peak = (std::max)(peak, int(p >> 24));
        s.pin = pin;
        s.peak = peak;
        info(fmt("sprites: %s: texture %08X %s: read back; peak alpha %02X; texture pin %08X (shipped %08X%s)", nameOf(s), unsigned(t), descText(d).c_str(), peak, pin, shipped,
                 first ? "" : fmt(", session %08X", s.sessionPin).c_str()));
        if (first) {
            s.sessionPin = pin;
            s.customArt = pin != shipped || s.s > 1;   // a scaled sheet is redrawn art: placed by the table's metrics
            if (s.s > 1)
                info(fmt("sprites: %s: %dx scaled layout (a DAT mod); installing as custom letter art (pin %08X over its %dx%d sheet, shipped %08X): TrueFont's letters sit as on "
                         "retail, everything it does not own keeps this art at its own density",
                         nameOf(s), s.s, pin, px.w, px.h, shipped));
            else if (s.customArt)
                info(fmt("sprites: %s: custom letter art (pin %08X, shipped %08X%s); installing; metrics from the table (TrueFont's letters sit as on retail; everything it "
                         "does not own keeps this art)",
                         nameOf(s), pin, shipped, tables_ == TableSet::Japanese && s.id == SpriteRec::Menu2fon ? "; the Japanese client's sheet re-encodes the same letters" : ""));
            return true;
        }
        if (pin == s.sessionPin) return true;
        if (artDiffers) *artDiffers = true;
        why = fmt("its glyph art is not the art read at this install (texture pin %08X, session pin %08X; shipped %08X)", pin, s.sessionPin, shipped);
        return false;
    }

    void waitLog(int stage, const char* what) {
        if (stage == waitLogged_) return;
        waitLogged_ = stage;
        info(std::string("sprites: ") + what);
    }

    // Waiting: find the records, check their rects, read them back, start the first batch.
    void begin(bool inGame) {
        nextLookMs_ = nowMs_ + 1000;
        regCache_.clear();   // a fresh look walks afresh (a leaf rebound while off keeps no stale status)
        if (!inGame) { waitLog(1, "waiting for the character to be in game"); return; }
        if (!gameAlive()) { waitLog(2, "waiting for the game's texture cache"); return; }
        if (!firstLookMs_) firstLookMs_ = nowMs_;
        const bool patient = nowMs_ - firstLookMs_ < kFindWaitMs;
        if (!readLanguage()) {   // the language gets the records' patience (60 s); then the English tables
            if (patient) { waitLog(4, "waiting for the game's language (the window-frame sprite set)"); return; }
            if (!langUnreadSaid_) {
                langUnreadSaid_ = true;
                warn("sprites: the game's language could not be read in 60 s (no window-frame sprite set of one language in the registry); the English letter layouts are "
                     "used, every record's rects are still checked against them, and a later read redoes this look (L4)");
            }
        }
        {
            TableSet ts = TableSet::English;
            if (!tableSetFor(lang_, ts)) { refuseLanguage(); return; }
            useTableSet(ts);   // a look after an off takes the language's set (a no-op when it is in use)
        }
        const ULONGLONG t0 = GetTickCount64();
        CacheScan cs;
        scanCache(reader_, abs(SR_.texCache), vtableVA(), cs);
        if (!d1Logged_) {
            d1Logged_ = true;
            info(fmt("sprites: D1: texture cache walked: %d records%s%s in %llu ms", cs.walked, cs.fault ? ", FAULT" : "", cs.limitHit ? ", LIMIT" : "", GetTickCount64() - t0));
            for (int i = 0; i < kSpriteRecCount; i++) {
                recs_[i].d1 = describe(cs.rec[i], client_.base);
                info("sprites: D1: " + recs_[i].d1);
            }
            info("sprites: D1: " + describe(cs.ustatshd, client_.base) + " (logged only)");
        }
        bool waiting = false;
        std::vector<Rec*> found;
        for (auto& s : recs_) {
            if (s.gate != Gate::Unchecked) continue;
            const RecordFind& f = cs.rec[int(s.id)];
            if ((cs.fault || cs.limitHit || f.hits == 0) && patient) { waiting = true; continue; }
            if (cs.fault || cs.limitHit) { setGate(s, Gate::Failed, f.why); continue; }
            if (!f.valid) {
                s.layoutNative = f.sizeRefused;   // a sheet size no scale of the table's: the letter layout is not one TrueFont knows
                setGate(s, Gate::Native, "its texture record did not check out: " + f.why);
                continue;
            }
            s.rec = f.first.rec;
            s.s = f.scale;
            opts_.sheet[int(s.id)] = f.scale;
            found.push_back(&s);
        }
        if (waiting) {
            waitLog(3, "waiting for the menu font textures to be loaded (60 s at most)");
            if (found.empty()) return;
            // Some are here: wait for the others first (one registry walk for all).
            for (Rec* s : found) { s->rec = 0; s->s = 1; opts_.sheet[int(s->id)] = 1; }
            return;
        }
        if (!found.empty()) {
            // One walk of the registry, filtered to the leaves that sample one of the found records.
            const double t1 = qpcMs();
            RegistryScan rs;
            const auto keepLeaf = [&](const LiveLeaf& l) {
                for (const Rec* s : found)
                    if (samples(l, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s))) return true;
                return false;
            };
            // The walk cache starts here (with this look's filter; the next poll's first walk is usually a full one).
            const bool ok = scanRegistryIncremental(reader_, abs(SR_.registry), keepLeaf, filterKey(found), regCache_, rs);
            watchRegistry(rs);
            info(fmt("sprites: registry walked: %d resources, %d composites, %d leaves, %zu sample the records, %.2f ms%s%s", rs.resources, rs.composites, rs.leaves, rs.kept.size(),
                     qpcMs() - t1, rs.fault ? ", FAULT" : "", rs.limitHit ? ", LIMIT" : ""));
            for (Rec* s : found) {
                if (!ok) { setGate(*s, Gate::Failed, rs.limitHit ? "the sprite registry did not end (limit reached)" : "the sprite registry could not be read"); continue; }
                std::vector<Rect> offGrid;
                const std::vector<Rect> live = rectsOf(rs, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s), s->s, &offGrid);
                const RectCheck c = checkRects(s->id, live, tables_);
                s->check = c;
                s->checked = true;
                info(fmt("sprites: D9: %s: %d live rects: %d owned (of %d shipped), %d foreign, %d outside, %zu unknown; hash %08X, shipped %08X (%s)%s", nameOf(*s), c.rects, c.owned,
                         c.ownedShipped, c.foreign, c.outside, c.unknown.size(), c.liveHash, c.shippedHash, c.complete() ? "complete" : c.ok() ? "partial set, all known" : "MISMATCH",
                         s->s > 1 ? fmt("; a %dx sheet: its rects divided by %d, %zu off its grid", s->s, s->s, offGrid.size()).c_str() : ""));
                // Rects off the grid: harmless where they read texels TrueFont copies unchanged or where a set left at
                // the unscaled layout reads the mod's letters anyway; any other one means the letters are not where the
                // table's layout scaled puts them.
                const OffGridCheck og = checkOffGrid(s->id, offGrid, s->s, tables_);
                const auto named = [&](const Rect& r) {
                    return fmt("%d,%d %dx%d in \"%s\"", r.u, r.v, r.w, r.h, setSampling(rs, r, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s)).c_str());
                };
                if (!og.clear.empty())
                    info(fmt("sprites: %s: %zu of its live rects are off its %dx sheet's grid but touch no glyph TrueFont draws (first %s): they read the sheet's own texels, left as they are",
                             nameOf(*s), og.clear.size(), s->s, named(og.clear[0]).c_str()));
                if (!og.unscaled.empty())
                    info(fmt("sprites: %s: %zu of its live rects are the tables' own rects, unscaled, on its %dx sheet (first %s): a sprite set the DAT mod left at the game's "
                             "unscaled layout; it reads a piece of the mod's letters with or without TrueFont, left as it is",
                             nameOf(*s), og.unscaled.size(), s->s, named(og.unscaled[0]).c_str()));
                if (!og.ok()) {
                    s->layoutNative = true;
                    setGate(*s, Gate::Native,
                            fmt("%zu of its live rects are off its %dx sheet's grid over a glyph (first %s): its letter layout is not the tables' scaled", og.refused.size(), s->s,
                                named(og.refused[0]).c_str()));
                    continue;
                }
                if (!c.ok()) {
                    s->layoutNative = !c.unknown.empty();   // unknown rects are the client's layout; none in use may change
                    setGate(*s, Gate::Native,
                            c.unknown.empty() ? std::string("none of its shipped glyph rects is in use")
                                              : fmt("%zu of its live rects overlap a glyph but are not in the shipped table (first %d,%d %dx%d): this client's layout differs", c.unknown.size(),
                                                    c.unknown[0].u, c.unknown[0].v, c.unknown[0].w, c.unknown[0].h));
                    continue;
                }
                auto pixels = std::make_shared<SpritePixels>();
                std::string why;
                if (isRetired(*s, rd<uint32_t>(s->rec + kTexPrimary))) {   // not the game's art to read back
                    setGate(*s, Gate::Failed, "it still holds an earlier texture of TrueFont's (an off could not put the game's own back); restart the game to use it");
                    s->retiredHeld = true;   // the panel says to restart the game, not to rebuild
                    continue;
                }
                if (s->id == SpriteRec::FontFont) {   // the fontshp / dmgnum code -> rect maps too
                    s->codes = checkCodeMap(rs, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s), tables_, s->s);
                    s->codesChecked = true;
                    info(fmt("sprites: D9 codes: font font: %d fontshp / dmgnum leaves checked against the tables' code -> rect map: %s", s->codes.leaves,
                             s->codes.ok() ? "all as the tables have them" : ("MISMATCH, " + std::to_string(s->codes.mismatches) + ": " + s->codes.first).c_str()));
                    if (!s->codes.ok()) {
                        s->layoutNative = true;
                        setGate(*s, Gate::Native, "its code -> rect map differs from the tables' (" + s->codes.first + "): this client's layout differs");
                        continue;
                    }
                }
                if (!readAndPin(*s, rd<uint32_t>(s->rec + kTexPrimary), *pixels, why, true)) {   // a first read takes any art
                    setGate(*s, Gate::Failed, why);
                    continue;
                }
                s->rb = pixels;
                s->gate = Gate::Ok;
                s->why.clear();
            }
        }
        bool any = false;
        for (const auto& s : recs_) any = any || s.gate == Gate::Ok;
        if (!any) {
            bool onlyNative = true;
            for (const auto& s : recs_) onlyNative = onlyNative && s.gate == Gate::Native;
            fail(onlyNative ? "their textures are not the ones TrueFont knows on this client" : "none of their textures could be used", std::string(), true);
            return;
        }
        std::string why;
        if (!startBatch(why)) { fail("the menu and HUD fonts could not be built", why); return; }
        if (building_) phase_ = Phase::Building;
        else {   // nothing to build: every sprite group is switched off
            phase_ = Phase::Installed;
            showOriginal_ = originalAtInstall_;
            originalAtInstall_ = false;
        }
    }

    // Plan per-texture scales for the batch (zero means absent). Caps must be initialized first.
    // Return requested and fitted scales plus the first limiting constraint.
    void planKs(const SpriteOptions& o, TexFormat f, SpriteKs& want, SpriteKs& got, std::string& limit) const {
        want = SpriteKs{};
        for (int i = 0; i < kSpriteRecCount; i++) {
            const Rec& s = recs_[i];
            want.s[i] = s.rb ? (std::max)(1, o.sheet[i]) : 1;   // a readback held at its sheet's size
            want.rec[i] = s.gate == Gate::Ok && s.rb && !s.wentNative && recordOn(s.id, o) ? recordK(s.id, o) : 0;
        }
        want.plate = splitWanted(plate_, o) ? splitK(Group::Nameplates, o) : 0;
        want.damage = splitWanted(dmg_, o) ? splitK(Group::Damage, o) : 0;
        want.hud = leafWanted(hud_, o) ? splitK(Group::NamesHud, o) : 0;
        want.jobs = leafWanted(jobs_, o) ? splitK(Group::JobTags, o) : 0;
        got = fitSpriteKs(want, otherBytes_, f, nonPow2_, maxTexW_, maxTexH_, limit);
    }
    static bool sameKs(const SpriteKs& a, const SpriteKs& b) {
        for (int i = 0; i < kSpriteRecCount; i++) if (a.rec[i] != b.rec[i]) return false;
        return a.plate == b.plate && a.damage == b.damage && a.hud == b.hud && a.jobs == b.jobs;
    }
    // Any texture `a` gives a larger k than `b`.
    static bool anyLarger(const SpriteKs& a, const SpriteKs& b) {
        for (int i = 0; i < kSpriteRecCount; i++) if (a.rec[i] > b.rec[i]) return true;
        return a.plate > b.plate || a.damage > b.damage || a.hud > b.hud || a.jobs > b.jobs;
    }
    // `o` with the ks of `got` (0 there: the group's own k), as a batch builds it.
    static SpriteOptions withKs(SpriteOptions o, const SpriteKs& got) {
        for (int i = 0; i < kSpriteRecCount; i++) o.kRec[i] = got.rec[i] ? got.rec[i] : recordK(SpriteRec(i), o);
        o.kPlate = got.plate ? got.plate : splitK(Group::Nameplates, o);
        o.kDamage = got.damage ? got.damage : splitK(Group::Damage, o);
        o.kHud = got.hud ? got.hud : splitK(Group::NamesHud, o);
        o.kJobs = got.jobs ? got.jobs : splitK(Group::JobTags, o);
        return o;
    }
    // Omit splits that become identical to the shared texture at the fitted scale.
    SpriteKs builtKs(const SpriteOptions& o, const SpriteKs& got) const {
        SpriteKs built = got;
        built.plate = splitWanted(plate_, o) ? o.kPlate : 0;
        built.damage = splitWanted(dmg_, o) ? o.kDamage : 0;
        built.hud = leafWanted(hud_, o) ? o.kHud : 0;
        built.jobs = leafWanted(jobs_, o) ? o.kJobs : 0;
        return built;
    }
    // Use the actual upload format for budget calculations, including ARGB in DEV Coverage mode.
    TexFormat planFormat(const SpriteOptions& o) const { return o.mode == SpriteOptions::Mode::Coverage ? TexFormat::A8R8G8B8 : formats_.pick(o.compress); }
    // Shrink immediately when Chat's growing share exceeds the budget. Grow after the quiet period,
    // at most once per fitted outcome. Automatic rebuild failures are log-only.
    void growTick() {
        growCheckMs_ = nowMs_ + 500;
        if (building_ || rebuildAt_ || otherBuilding_ || !failure_.empty() || !batched_ || !formats_.checked) return;
        readCaps();
        SpriteKs want, got;
        std::string limit;
        planKs(opts_, planFormat(opts_), want, got, limit);
        // Use actual allocations: a planned split omitted at build time must not force a needless step-down.
        if (otherBytes_ > batchShare_ && otherBytes_ + memoryBytes() > kMemoryLimit && anyLarger(lastGot_, got)) {
            rebuildAt_ = nowMs_;
            batchQuietNext_ = true;
            info(fmt("sprites: memory: Chat/Items' share rose from %s to %s%s; with these fonts' %s it would pass %s, so these fonts step down now (R4)",
                     mbText(batchShare_).c_str(), mbText(otherBytes_).c_str(), otherClaiming_ ? " (it is sized first, swap.h I19)" : "", mbText(memoryBytes()).c_str(),
                     mbText(kMemoryLimit).c_str()));
            return;
        }
        if (!anyLarger(got, lastGot_) || (grew_ && sameKs(got, growTried_))) return;
        grew_ = true;
        growTried_ = got;
        rebuildAt_ = nowMs_ + kRebuildQuietMs;
        batchQuietNext_ = true;
        if (otherBytes_ < batchShare_)
            info(fmt("sprites: memory: Chat/Items' share fell from %s to %s, so these fonts fit at a higher Sharpness than the last batch; a rebuild in %llu ms (R4)",
                     mbText(batchShare_).c_str(), mbText(otherBytes_).c_str(), static_cast<unsigned long long>(kRebuildQuietMs)));
        else
            info(fmt("sprites: memory: these fonts fit at a higher Sharpness than the last batch (a texture it did not build is wanted now; Chat/Items' share is %s); a "
                     "rebuild in %llu ms (R4)",
                     mbText(otherBytes_).c_str(), static_cast<unsigned long long>(kRebuildQuietMs)));
    }
    // Wait if overlapping old/new sprite textures and Chat's build peak would exceed the shared budget.
    bool holdForChat() {
        if (!otherBuilding_ || !formats_.checked) { chatHoldSaid_ = false; return false; }
        SpriteKs want, got;
        std::string limit;
        const TexFormat f = planFormat(opts_);
        planKs(opts_, f, want, got, limit);
        const uint64_t fresh = spriteKsBytes(got, f) - spriteReadbackBytes(got.s);   // the new textures and kept composites
        if (otherBytes_ + memoryBytes() + fresh <= kMemoryLimit) { chatHoldSaid_ = false; return false; }
        if (!chatHoldSaid_) {
            chatHoldSaid_ = true;
            info(fmt("sprites: the rebuild waits for Chat/Items' build: its peak %s, these fonts' textures %s and the new ones %s would pass %s (R4)", mbText(otherBytes_).c_str(),
                     mbText(memoryBytes()).c_str(), mbText(fresh).c_str(), mbText(kMemoryLimit).c_str()));
        }
        return true;
    }
    bool startBatch(std::string& why) {
        SpriteOptions o = opts_;
        const TexFormat f = uploadFormat(o);
        readCaps();
        SpriteKs want, got;
        std::string limit;
        planKs(o, f, want, got, limit);
        lastGot_ = got;
        batchShare_ = otherBytes_;
        batched_ = true;
        batchQuiet_ = batchQuietNext_;   // an automatic batch's failure goes to the log only
        batchQuietNext_ = false;
        if (!sameKs(got, growTried_)) grew_ = false;   // a new outcome: a later rise is tried again
        o = withKs(o, got);
        // At the k they get, a split may now draw exactly as its record's own texture: then it is not built.
        const SpriteKs built = builtKs(o, got);
        const bool plate = built.plate != 0, damage = built.damage != 0, hud = built.hud != 0, jobsSplit = built.jobs != 0;
        const Rec& ff = rec(SpriteRec::FontFont);
        const ShippedInk ffInk = ff.customArt ? shippedInk(SpriteRec::FontFont, tables_) : ShippedInk{nullptr, 0};   // custom letter art
        std::vector<SpriteJob> jobs;
        std::string names;
        if (!plate && (plate_.ours || plate_.copy)) parkSplit(plate_, "nameplates draw as font font's own texture now (as HUD text, their punctuation as Menu labels)");
        if (!damage && (dmg_.ours || dmg_.copy)) parkSplit(dmg_, "damage numbers draw from font font's own texture now (the same k)");
        if (!hud && (hud_.ours || hud_.copy)) leafPark(hud_, "HUD text draws from font font's own texture now (the same k, or switched off)");
        if (!jobsSplit && (jobs_.ours || jobs_.copy)) leafPark(jobs_, "Job and level tags draw from menu2fon's own texture now (the same k, or switched off)");
        if (plate && (forceAll_ || !plate_.built || !samePlate(plate_.builtOpts, o))) {
            const bool reuse = !forceAll_ && plate_.built && plate_.kept && f != TexFormat::A8R8G8B8 && samePlateExceptCompress(plate_.builtOpts, o);
            jobs.push_back(SpriteJob{SpriteRec::FontFont, ff.slots, ff.rb, f, SplitKind::Plates, reuse ? plate_.kept : nullptr, o.kPlate, ffInk});
            names = reuse ? "the nameplate composite (re-encoded)" : "the nameplate composite";
        }
        if (damage && (forceAll_ || !dmg_.built || !sameDamage(dmg_.builtOpts, o))) {
            const bool reuse = !forceAll_ && dmg_.built && dmg_.kept && f != TexFormat::A8R8G8B8 && sameDamageExceptCompress(dmg_.builtOpts, o);
            jobs.push_back(SpriteJob{SpriteRec::FontFont, ff.slots, ff.rb, f, SplitKind::Damage, reuse ? dmg_.kept : nullptr, o.kDamage, ffInk});
            names += std::string(names.empty() ? "" : ", ") + (reuse ? "the damage-number composite (re-encoded)" : "the damage-number composite");
        }
        // The leaf splits' composites (their record's composite at their k).
        for (LeafSplit* ls : {&hud_, &jobs_}) {
            if (!(ls == &hud_ ? hud : jobsSplit)) continue;
            const int lk = ls == &hud_ ? o.kHud : o.kJobs;
            if (!forceAll_ && ls->built && (ls == &hud_ ? sameHud(ls->builtOpts, o) : sameJobs(ls->builtOpts, o))) continue;
            const Rec& b = rec(ls->base);
            const bool reuse = !forceAll_ && ls->built && ls->kept && f != TexFormat::A8R8G8B8 &&
                               (ls == &hud_ ? sameHudExceptCompress(ls->builtOpts, o) : sameJobsExceptCompress(ls->builtOpts, o));
            jobs.push_back(SpriteJob{ls->base, b.slots, b.rb, f, ls->kind, reuse ? ls->kept : nullptr, lk, b.customArt ? shippedInk(ls->base, tables_) : ShippedInk{nullptr, 0}});
            names += std::string(names.empty() ? "" : ", ") + (ls == &hud_ ? "the HUD text composite" : "the Job and level tags composite") + (reuse ? " (re-encoded)" : "");
        }
        for (auto& s : recs_) {
            if (s.gate != Gate::Ok || !s.rb || s.wentNative) continue;
            if (!recordOn(s.id, o)) {   // not built; parked when TrueFont's texture is there
                if (s.ours || s.installed || !s.abandoned.empty()) parkRecord(s);
                continue;
            }
            if (!forceAll_ && s.built && sameForRecord(s.id, s.builtOpts, o)) continue;
            const bool reuse = !forceAll_ && s.built && s.kept && f != TexFormat::A8R8G8B8 && sameExceptCompress(s.id, s.builtOpts, o);
            jobs.push_back(SpriteJob{s.id, s.slots, s.rb, f, SplitKind::None, reuse ? s.kept : nullptr, o.kRec[int(s.id)], s.customArt ? shippedInk(s.id, tables_) : ShippedInk{nullptr, 0}});
            names += std::string(names.empty() ? "" : ", ") + nameOf(s) + (reuse ? " (re-encoded)" : "");
        }
        batchForced_ = forceAll_;   // a typed rebuild's batch names every group that is on
        forceAll_ = false;
        const uint64_t mine = spriteKsBytes(built, f);
        std::string ks;
        for (int i = 0; i < kSpriteRecCount; i++)
            if (built.rec[i]) ks += fmt("%s%s %dx", ks.empty() ? "" : ", ", kSpriteRecs[i].shortName, built.rec[i]);
        if (built.plate) ks += fmt(", nameplates %dx", built.plate);
        if (built.damage) ks += fmt(", damage numbers %dx", built.damage);
        if (built.hud) ks += fmt(", HUD text %dx", built.hud);
        if (built.jobs) ks += fmt(", Job and level tags %dx", built.jobs);
        std::string sheets;   // the scaled sheets: their readbacks are read at their own size
        for (int i = 0; i < kSpriteRecCount; i++)
            if (built.s[i] > 1) sheets += fmt("%s%s's letter sheet is %dx", sheets.empty() ? ", " : "; ", kSpriteRecs[i].shortName, built.s[i]);
        info(fmt("sprites: memory: Chat/Items' build peak %s, these fonts %s (%s; textures as %s, counted %s as MANAGED, and their A8L8 kept copies; readbacks %s%s); total %s "
                 "(limit %s)",
                 mbText(otherBytes_).c_str(),
                 mbText(mine).c_str(), ks.empty() ? "nothing built" : ks.c_str(), texFormatName(f), kManagedCountText, mbText(spriteReadbackBytes(built.s)).c_str(), sheets.c_str(),
                 mbText(otherBytes_ + mine).c_str(), mbText(kMemoryLimit).c_str()));
        {   // a k raised to a scaled sheet's (its art is never downsampled), said once per outcome
            std::string raised;
            const auto up = [&](const char* what, int asked, int k, SpriteRec r) {
                if (!k || k <= asked) return;
                raised += fmt("%s%s raised to %dx (asked %dx): its letter sheet is %ux%u", raised.empty() ? "" : "; ", what, k, asked, kSpriteRecs[int(r)].w * unsigned(built.s[int(r)]),
                              kSpriteRecs[int(r)].h * unsigned(built.s[int(r)]));
            };
            for (int i = 0; i < kSpriteRecCount; i++) up(kSpriteRecs[i].shortName, groupK(baseGroup(SpriteRec(i), o), o), want.rec[i], SpriteRec(i));
            up("nameplates", groupK(Group::Nameplates, o), want.plate, SpriteRec::FontFont);
            up("damage numbers", groupK(Group::Damage, o), want.damage, SpriteRec::FontFont);
            up("HUD text", groupK(Group::NamesHud, o), want.hud, SpriteRec::FontFont);
            up("Job and level tags", groupK(Group::JobTags, o), want.jobs, SpriteRec::Menu2fon);
            if (raised != raisedSaid_ && !raised.empty()) info("sprites: " + raised);
            raisedSaid_ = raised;
        }
        // Report each constrained texture in the log; deduplicate group-level Sharpness warnings in chat.
        std::string lower, groups;
        const auto note = [&](const char* what, Group grp, int w, int g) {
            if (!w || !g || g == w) return;
            lower += fmt("%s%s built at %dx (asked %dx)", lower.empty() ? "" : ", ", what, g, w);
            const std::string said = fmt("%s at %dx, not %dx", groupName(grp), g, w);
            if (groups.find(said) == std::string::npos) groups += (groups.empty() ? "" : "; ") + said;
        };
        for (int i = 0; i < kSpriteRecCount; i++) note(kSpriteRecs[i].shortName, baseGroup(SpriteRec(i), o), want.rec[i], got.rec[i]);
        note("nameplates", Group::Nameplates, want.plate, built.plate);
        note("damage numbers", Group::Damage, want.damage, built.damage);
        note("HUD text", Group::NamesHud, want.hud, built.hud);
        note("Job and level tags", Group::JobTags, want.jobs, built.jobs);
        if (!lower.empty()) {
            warn("sprites: memory / texture limits: " + lower + ": " + limit);
            if (groups != kSaid_) {
                kSaid_ = groups;
                const char* reason = limit.find("power of two") != std::string::npos ? "this card takes only power-of-two texture sizes"
                                     : limit.find("largest") != std::string::npos   ? "this card's largest texture is too small for them"
                                                                                    : "they would pass TrueFont's 160 MB limit";
                chat("some menu and HUD fonts are drawn at a lower Sharpness than asked (" + groups + "): " + reason + ". See the log.", 0x68);
            }
        } else kSaid_.clear();
        if (jobs.empty()) {   // settled as it is: counted as a finished batch, so a wait on swaps() ends (DEV self-check)
            info("sprites: build: nothing changed");
            ++swaps_;
            return true;
        }
        batchNewBytes_ = 0;   // what this batch holds beside the old textures until it is swapped in
        for (const SpriteJob& j : jobs) {
            const int w = int(kSpriteRecs[int(j.rec)].w) * j.k, h = int(kSpriteRecs[int(j.rec)].h) * j.k;
            batchNewBytes_ += managedBytes(texBytes(f, w, h)) + (j.reencode || f == TexFormat::A8R8G8B8 ? 0 : uint64_t(texBytes(TexFormat::A8L8, w, h)));
        }
        if (!worker_.start(std::move(jobs), o)) { why = "the sprite worker thread could not start"; return false; }
        batchOpts_ = o;
        buildGen_ = gen_;
        building_ = true;
        buildStartMs_ = nowMs_;
        info("sprites: build: started (" + names + ")");
        memLog("build start (menu and HUD fonts)");
        return true;
    }
    // The card's texture limits (the largest size, and whether a size that is no power of two may be made), once.
    void readCaps() {
        if (capsRead_ || !device_) return;
        capsRead_ = true;
        D3DCAPS8 caps{};
        if (SUCCEEDED(device_->GetDeviceCaps(&caps))) {
            maxTexW_ = caps.MaxTextureWidth;
            maxTexH_ = caps.MaxTextureHeight;
            nonPow2_ = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) == 0;
        }
    }

    // The format this batch uploads in. A refused A8L8 or DXT3 is logged once since load.
    TexFormat uploadFormat(const SpriteOptions& o) {
        if (o.mode == SpriteOptions::Mode::Coverage) return TexFormat::A8R8G8B8;   // DEV Coverage's colours
        if (!formats_.checked && device_) {
            std::string text;
            formats_ = probeFormats(device_, text);
            info("sprites: CheckDeviceFormat " + text);
        }
        const TexFormat f = formats_.pick(o.compress);
        if (f == TexFormat::A8R8G8B8 && !formatSaid_) {
            formatSaid_ = true;
            warn(fmt("sprites: texture: CheckDeviceFormat %s A8L8, so the textures are A8R8G8B8 (4 bytes a pixel)", formats_.checked ? "refused" : "could not check"));
        }
        if (o.compress && f != TexFormat::Dxt3 && !compressSaid_) {
            compressSaid_ = true;
            warn(fmt("sprites: texture: Compress is on, but CheckDeviceFormat %s DXT3, so the textures are %s", formats_.checked ? "refused" : "could not check", texFormatName(f)));
        }
        return f;
    }

    void finishBatch() {
        const SpriteWorker::State st = worker_.state();
        worker_.wait();
        building_ = false;
        std::vector<SpriteBuilt> results = worker_.take();
        if (buildGen_ != gen_) { info("sprites: build: discarded (it was started before an off or a refusal)"); return; }
        if (st == SpriteWorker::Cancelled) { info("sprites: build: cancelled"); return; }
        if (!enabled_ || gameGone_) { info("sprites: build: discarded (off meanwhile)"); return; }
        // The memory samples (swap.h memSample): the batch's textures made and swapped in, against the address space.
        const MemSample mem0 = memLog("menu and HUD fonts built, before their textures");
        const uint64_t memTex0 = textureBytes();
        uint64_t memImages = 0;
        for (const SpriteBuilt& b : results)
            if (b.atlas) memImages += uint64_t(b.atlas->tex.words.capacity()) * 4u;
        RebuiltLine rebuilt;   // a typed rebuild's outcome
        rebuilt.all = batchForced_;
        std::string firstErr;
        // What each record's texture showed before this batch, taken before any result replaces it.
        std::vector<SpriteOptions> before(kSpriteRecCount);
        bool wasBuilt[kSpriteRecCount] = {};
        for (int i = 0; i < kSpriteRecCount; i++) {
            wasBuilt[i] = recs_[i].built;
            if (wasBuilt[i]) before[size_t(i)] = recs_[i].builtOpts;
        }
        const auto beforeOf = [&](SpriteRec r) -> const SpriteOptions* { return wasBuilt[int(r)] ? &before[size_t(int(r))] : nullptr; };
        // Honor Show Original before first install: retain built textures without exposing them to the game.
        if (phase_ == Phase::Building && originalAtInstall_) showOriginal_ = true;
        for (SpriteBuilt& b : results) {
            if (b.split == SplitKind::Hud || b.split == SplitKind::Jobs) {   // leaf splits
                LeafSplit& ls = b.split == SplitKind::Hud ? hud_ : jobs_;
                finishLeafSplit(ls, b, rebuilt, beforeOf(ls.base), firstErr);
                continue;
            }
            if (b.split != SplitKind::None) { finishSplit(split(b.split), b, rebuilt, beforeOf(SpriteRec::FontFont), firstErr); continue; }   // page slot splits
            Rec& s = rec(b.rec);
            if (s.gate != Gate::Ok) { info(fmt("sprites: build: %s discarded (it went native meanwhile)", nameOf(s))); continue; }
            if (!recordOn(s.id, opts_)) { info(fmt("sprites: build: %s discarded (its groups were switched off meanwhile)", nameOf(s))); continue; }
            if (!b.atlas) {
                if (s.ours) err(fmt("sprites: rebuild of %s failed (%s); the current texture stays", nameOf(s), b.why.c_str()));
                else setGate(s, Gate::Failed, "it could not be built: " + b.why);
                if (firstErr.empty()) firstErr = recordFontText(s) + " could not be made";
                continue;
            }
            const SpriteAtlas& a = *b.atlas;
            for (int g = 1; g < kGroupCount; g++) {
                if (!recordServes(s.id, Group(g))) continue;
                if (!batchOpts_.on[g]) { info(fmt("sprites: build: %s %s: switched off (the game's own art)", nameOf(s), groupName(Group(g)))); continue; }
                const SpriteGroupStats& gs = a.group[g];
                info(fmt("sprites: build: %s %s: %s%s, %s, %d slots: %d drawn, %d native (%d not in the font, %d no native ink, %d errors); condensed %d, squeezed %d, shifted %d, "
                         "clipped %d, rim trimmed %d",
                         nameOf(s), groupName(Group(g)), toUtf8(gs.face).c_str(), gs.substituted ? " (SUBSTITUTED by GDI)" : "", groupEngineText(gs.engine, gs.engineMixed).c_str(), gs.slots, gs.rendered, gs.native, gs.noGlyph, gs.noInk, gs.errors,
                         gs.condensed, gs.squeezed, gs.shifted, gs.clipped, gs.trimmed));
            }
            info(fmt("sprites: build: %s %dx%d (k %d) in %.0f ms on the worker, converted to %s in %.0f ms%s; %llu ms since the start", nameOf(s), a.w, a.h, a.k, a.ms,
                     texFormatName(a.tex.format), a.tex.ms, b.reencoded ? " (Compress changed: the kept composite re-encoded, no glyph drawn)" : "",
                     static_cast<unsigned long long>(nowMs_ - buildStartMs_)));
            std::string why;
            IDirect3DTexture8* t = createTexture(a, why);
            if (!t) {
                if (s.ours) err(fmt("sprites: rebuild of %s: the texture could not be created (%s); the current one stays", nameOf(s), why.c_str()));
                else setGate(s, Gate::Failed, "its texture could not be created: " + why);
                if (firstErr.empty()) firstErr = recordFontText(s) + " could not be made";
                continue;
            }
            RebuiltLine shown;   // Report groups only after successful installation.
            for (const Group g : kGroupOrder)
                if (recordShows(s.id, g)) shown.add(g, a.k, beforeOf(s.id), batchOpts_);
            s.built = true;
            s.builtOpts = batchOpts_;
            s.counts = texCounts(a);
            noteEngines(a);
            s.kept = b.kept;
            s.k = a.k;
            const int w = a.w, h = a.h;
            b.atlas.reset();   // the converted bytes are no longer needed
            if (s.ours) swapIn(s, t, w, h);
            else install(s, t, w, h);
            if (s.gate == Gate::Failed) { if (firstErr.empty()) firstErr = recordFontText(s) + " could not be put in place"; }
            else rebuilt.items.insert(rebuilt.items.end(), shown.items.begin(), shown.items.end());
        }
        // After a partial build failure, drop splits now identical to the surviving shared texture.
        {
            SpriteOptions inPlace = batchOpts_;
            for (int i = 0; i < kSpriteRecCount; i++)
                if (recs_[i].ours) inPlace.kRec[i] = recs_[i].k;
            if ((plate_.ours || plate_.copy) && !splitWanted(plate_, inPlace)) parkSplit(plate_, "font font's own texture stayed at the k it draws at (its larger one could not be made)");
            if ((dmg_.ours || dmg_.copy) && !splitWanted(dmg_, inPlace)) parkSplit(dmg_, "font font's own texture stayed at the k it draws at (its larger one could not be made)");
            if ((hud_.ours || hud_.copy) && !leafWanted(hud_, inPlace)) leafPark(hud_, "font font's own texture stayed at its k (its larger one could not be made)");
            if ((jobs_.ours || jobs_.copy) && !leafWanted(jobs_, inPlace)) leafPark(jobs_, "menu2fon's own texture stayed at its k (its larger one could not be made)");
        }
        {
            const MemSample mem1 = memLog("build end (menu and HUD fonts, after their swaps)");
            bool byFree = false;
            const double taken = memTakenMB(mem0, mem1, &byFree);
            info(fmt("sprites: memory: textures %.1f MB (%.1f MB before), process address space %+.1f MB (by %s; from the batch's end on the worker to its last swap; the "
                     "converted images, %.1f MB, and any textures replaced were freed meanwhile; the limit counts a MANAGED texture %s)",
                     mib(textureBytes()), mib(memTex0), taken, byFree ? "the free address space" : "private bytes (a walk was capped)", mib(memImages), kManagedCountText));
        }
        ++swaps_;
        // A rebuild asked for (typed) while the first batch built is that batch: the flag never outlives it.
        if (phase_ != Phase::Installed) announceRebuild_ = false;
        if (announceRebuild_ && phase_ == Phase::Installed) {
            announceRebuild_ = false;
            if (!firstErr.empty()) {
                lastSaidFailure_ = firstErr;
                chat("the rebuild of the menu and HUD fonts failed: " + firstErr + ". See the log.", 0x68);
            } else if (!rebuilt.empty()) chat("rebuilt: the menu and HUD fonts (" + rebuilt.text() + ").");
        } else if (phase_ == Phase::Installed && !firstErr.empty() && batchQuiet_) {
            info("sprites: rebuild: an automatic one (R4) failed (" + firstErr + "); said in the log only; the current fonts stay");
        } else if (phase_ == Phase::Installed && !firstErr.empty() && firstErr != lastSaidFailure_) {
            // Report failed setting changes once, since the visible font remains unchanged.
            lastSaidFailure_ = firstErr;
            chat("the rebuild of the menu and HUD fonts failed: " + firstErr + "; the current fonts stay. See the log.", 0x68);
        }
        if (phase_ == Phase::Installed && firstErr.empty() && !rebuilt.empty()) lastSaidFailure_.clear();   // Allow a future failure to be reported again.
        if (phase_ == Phase::Building) {
            bool any = false, wanted = false;
            for (const auto& s : recs_) {
                any = any || s.installed || (showOriginal_ && s.ours && s.gate == Gate::Ok);   // built and kept counts
                wanted = wanted || ((s.gate == Gate::Ok || s.gate == Gate::Failed) && recordOn(s.id, opts_));
            }
            // Nothing in place because every record's groups were switched off meanwhile is not a refusal.
            if (!any && wanted) { fail("none of their textures could be put in place", std::string(), false); return; }
            phase_ = Phase::Installed;
            showOriginal_ = originalAtInstall_;
            originalAtInstall_ = false;
            lastSaidFailure_.clear();
            std::string list;
            for (const auto& s : recs_) list += fmt("%s%s %s", list.empty() ? "" : ", ", nameOf(s), s.installed ? "yes" : showOriginal_ && s.ours ? "built (showing the original)" : "native");
            info("sprites: installed: " + list + (showOriginal_ ? "; showing the originals (Show Original was on)" : ""));
        }
    }

    IDirect3DTexture8* createTexture(const SpriteAtlas& a, std::string& why) {
        if (!device_) { why = "no Direct3D device"; return nullptr; }
        readCaps();
        if (maxTexW_ && (unsigned(a.w) > maxTexW_ || unsigned(a.h) > maxTexH_)) { why = fmt("the card's largest texture is %lux%lu, this one is %dx%d", maxTexW_, maxTexH_, a.w, a.h); return nullptr; }
        const TexImage& img = a.tex;
        const D3DFORMAT df = d3dFormat(img.format);
        const char* fn = texFormatName(img.format);
        const uint64_t bytes = texBytes(img.format, a.w, a.h);
        const bool probe = bytes >= kMemProbeBytes;   // a large texture's create and upload, sampled around
        if (probe) memLog(fmt("before CreateTexture %dx%d %s (%.1f MB, a menu font)", a.w, a.h, fn, mib(bytes)));
        IDirect3DTexture8* t = nullptr;
        HRESULT hm = E_FAIL;
        std::string uw;
        if (img.format != TexFormat::A8R8G8B8 || formats_.argb) {
            hm = device_->CreateTexture(UINT(a.w), UINT(a.h), 1, 0, df, D3DPOOL_MANAGED, &t);
            if (SUCCEEDED(hm) && t && uploadTex(t, img, uw)) {
                poolOf_ = "MANAGED";
                formatOf_ = img.format;
                info(fmt("sprites: texture: %08X CreateTexture %dx%d %s MANAGED", unsigned(uintptr_t(t)), a.w, a.h, fn));
                if (probe) memLog(fmt("after CreateTexture and the upload %dx%d %s MANAGED", a.w, a.h, fn));
                return t;
            }
            if (t) { t->Release(); t = nullptr; }
        }
        IDirect3DTexture8* sys = nullptr;
        const HRESULT hd = device_->CreateTexture(UINT(a.w), UINT(a.h), 1, 0, df, D3DPOOL_DEFAULT, &t);
        const HRESULT hs = SUCCEEDED(hd) ? device_->CreateTexture(UINT(a.w), UINT(a.h), 1, 0, df, D3DPOOL_SYSTEMMEM, &sys) : E_FAIL;
        std::string uw2;
        const bool up2 = SUCCEEDED(hs) && sys && uploadTex(sys, img, uw2);
        const HRESULT hu = up2 ? device_->UpdateTexture(sys, t) : E_FAIL;
        if (sys) sys->Release();
        info(fmt("sprites: texture: %s MANAGED 0x%08X%s; DEFAULT 0x%08X, SYSTEMMEM 0x%08X, upload %s, UpdateTexture 0x%08X", fn, unsigned(hm), uw.empty() ? "" : (" (" + uw + ")").c_str(),
                 unsigned(hd), unsigned(hs), up2 ? "ok" : uw2.c_str(), unsigned(hu)));
        if (SUCCEEDED(hu) && t) {
            poolOf_ = "DEFAULT";
            formatOf_ = img.format;
            if (probe) memLog(fmt("after CreateTexture and the upload %dx%d %s DEFAULT", a.w, a.h, fn));
            return t;
        }
        if (t) t->Release();
        why = fmt("CreateTexture %s MANAGED 0x%08X, DEFAULT 0x%08X, SYSTEMMEM 0x%08X, UpdateTexture 0x%08X", fn, unsigned(hm), unsigned(hd), unsigned(hs), unsigned(hu));
        return nullptr;
    }

    // Writes ours into a record's +40 (verified; the record gets its own reference on ours). False with why.
    bool installSlot(Rec& s, uintptr_t r, std::string& why) {
        if (!sameRecord(r, s.id)) { why = fmt("record %08X is no longer \"%s\" (vtable, name or size)", unsigned(r), kSpriteRecs[int(s.id)].name); return false; }
        const uintptr_t now = rd<uint32_t>(r + kTexPrimary);
        if (now < 0x10000) { why = fmt("record %08X: +40 holds no texture (%08X)", unsigned(r), unsigned(now)); return false; }
        if (now == uintptr_t(s.ours)) {
            if (r == s.rec && s.original) { s.installed = true; return true; }
            why = fmt("record %08X already holds TrueFont's texture but its original is unknown; nothing written", unsigned(r));
            return false;
        }
        if (isRetired(s, now)) { why = fmt("record %08X holds an earlier TrueFont texture; nothing written", unsigned(r)); return false; }
        D3DSURFACE_DESC d{};
        if (!describeTex(now, d, why)) return false;
        if (d.Width != kSpriteRecs[int(s.id)].w * unsigned(s.s) || d.Height != kSpriteRecs[int(s.id)].h * unsigned(s.s)) { why = "the record's texture is now " + descText(d) + ", not the size it had"; return false; }
        s.ours->AddRef();
        const WriteResult wr = wrVerifiedWhy<uint32_t>(r + kTexPrimary, uint32_t(uintptr_t(s.ours)), uint32_t(now));
        if (wr != WriteResult::Ok) {
            s.ours->Release();
            why = fmt("record %08X: writing +40 %s", unsigned(r), writeResultText(wr));
            return false;
        }
        if (s.originalRef && s.original && s.rec == r) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });
        s.rec = r;
        s.original = now;
        s.originalRef = false;
        s.installed = true;
        s.lost = false;
        s.lostTries = 0;
        ++s.installs;
        info(fmt("sprites: install: %s record %08X +40 %08X -> ours %08X (the original was %s)", nameOf(s), unsigned(r), unsigned(now), unsigned(uintptr_t(s.ours)), descText(d).c_str()));
        return true;
    }
    void install(Rec& s, IDirect3DTexture8* t, int w, int h) {
        s.ours = t;
        s.w = w;
        s.h = h;
        s.pool = poolOf_;
        s.format = formatOf_;
        s.heldByRecord = false;
        if (showOriginal_) return;
        // A recreated or deleted record must be found again; freed bytes may still pass identity checks.
        uint32_t held = 0;
        const bool itself = sameRecord(s.rec, s.id), emptied = itself && (!rdok(s.rec + kTexPrimary, held) || held < 0x10000);
        if (!itself || emptied) {
            s.installed = false;
            s.lost = true;
            s.lostTries = 0;
            rescanAt_ = nowMs_ + 1;
            info(fmt("sprites: install: the %s record %08X %s; it is looked for again", nameOf(s), unsigned(s.rec),
                     itself ? fmt("has no texture at +40 (%08X: the cache deleted it)", held).c_str() : "is no longer itself"));
            return;
        }
        std::string why;
        if (!installSlot(s, s.rec, why)) {
            setGate(s, Gate::Failed, "its texture could not be put in place: " + why);
            s.ours->Release();   // no record holds it: its own reference
            s.ours = nullptr;
        }
    }
    // Rebuild: the new texture into the record that holds the old one, then the old one released.
    void swapIn(Rec& s, IDirect3DTexture8* t, int w, int h) {
        IDirect3DTexture8* old = s.ours;
        const uintptr_t oldAddr = uintptr_t(old);
        s.ours = t;
        s.w = w;
        s.h = h;
        s.pool = poolOf_;
        s.format = formatOf_;
        bool oldHeld = false;
        if (s.installed && !showOriginal_) {
            uint32_t now = 0;
            if (!sameRecord(s.rec, s.id)) {
                s.installed = false;
                s.lost = true;
                rescanAt_ = nowMs_ + 1;
                info(fmt("sprites: rebuild: the %s record %08X is no longer itself; it is looked for again", nameOf(s), unsigned(s.rec)));
                oldHeld = rd<uint32_t>(s.rec + kTexPrimary) == oldAddr;   // it may still hold it: never release
            } else if (!rdok(s.rec + kTexPrimary, now)) {
                oldHeld = true;
                warn(fmt("sprites: rebuild: the %s record's +40 is unreadable; the old texture is kept", nameOf(s)));
            } else if (now != oldAddr) {
                info(fmt("sprites: rebuild: the %s record holds %08X, not the old texture; the keep check takes over", nameOf(s), now));
            } else {
                t->AddRef();
                const WriteResult r = wrVerifiedWhy<uint32_t>(s.rec + kTexPrimary, uint32_t(uintptr_t(t)), now);
                if (r == WriteResult::Ok) {
                    old->Release();   // the record's reference on the old one
                    info(fmt("sprites: rebuild: %s record %08X +40 %08X -> %08X", nameOf(s), unsigned(s.rec), now, unsigned(uintptr_t(t))));
                } else {
                    t->Release();
                    oldHeld = true;
                    warn(fmt("sprites: rebuild: the %s record's +40 was not written (%s); the old texture is kept", nameOf(s), writeResultText(r)));
                }
            }
        }
        for (const Abandoned& a : s.abandoned)
            if (rd<uint32_t>(a.rec + kTexPrimary) == oldAddr) oldHeld = true;
        if (!oldHeld) old->Release();   // its own reference, only after the swap
        else {
            s.retired.push_back(oldAddr);
            warn(fmt("sprites: rebuild: a record may still hold the old %s texture, so it is not released", nameOf(s)));
        }
    }

    // The per-frame keep check: re-apply, adopt a foreign value, look for a lost record again.
    void keep() {
        if (!gameAlive()) {
            gameGone_ = true;
            warn("sprites: keep: the texture cache or the renderer global is 0 (game teardown); nothing more is touched");
            return;
        }
        bool reapplied = false;
        for (auto& s : recs_) {
            if (!s.installed || !s.ours) continue;
            // +40 first (as swap.h): a record that still holds ours stays tracked, so off and unload restore it.
            uint32_t now = 0;
            if (!rdok(s.rec + kTexPrimary, now)) { logKeep(fmt("sprites: keep: the %s record's +40 is unreadable", nameOf(s))); continue; }
            if (now == uintptr_t(s.ours)) continue;
            if (now && !sameRecord(s.rec, s.id)) {
                logKeep(fmt("sprites: keep: the %s record %08X is no longer itself (vtable, name or size); it is not written again and is looked for anew", nameOf(s), unsigned(s.rec)));
                s.installed = false;
                s.lost = true;
                if (!rescanAt_) rescanAt_ = nowMs_ + 2000;
                continue;
            }
            if (!now) {   // the cache deleted the record (YD: +40 released and cleared); look for its successor
                if (!s.lost) {
                    logKeep(fmt("sprites: keep: the %s record's +40 is empty (the cache deleted it); nothing written, it is looked for anew", nameOf(s)));
                    s.lost = true;
                    if (!rescanAt_) rescanAt_ = nowMs_ + 2000;
                }
                continue;
            }
            const bool retired = isRetired(s, now);
            if (!retired && now != s.original) {   // a foreign value: adopted, with a reference held on it
                logKeep(fmt("sprites: keep: the %s record's +40 holds %08X (neither ours nor the original %08X): it becomes the original", nameOf(s), now, unsigned(s.original)));
                if (s.originalRef && s.original) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });
                s.original = now;
                s.originalRef = seh([&] { reinterpret_cast<IDirect3DTexture8*>(uintptr_t(now))->AddRef(); });
            }
            s.ours->AddRef();
            const WriteResult r = wrVerifiedWhy<uint32_t>(s.rec + kTexPrimary, uint32_t(uintptr_t(s.ours)), now);
            if (r == WriteResult::Ok) {
                reapplied = true;
                if (retired) reinterpret_cast<IDirect3DTexture8*>(uintptr_t(now))->Release();   // the record's reference on the retired one
                logKeep(fmt("sprites: keep: re-applied ours to the %s record (it held %08X%s)", nameOf(s), now, retired ? ", an earlier TrueFont texture" : ""));
            } else {
                s.ours->Release();
                logKeep(fmt("sprites: keep: re-applying to the %s record failed (%s)", nameOf(s), writeResultText(r)));
            }
        }
        if (splitKeep(plate_)) reapplied = true;   // the nameplate slot
        if (splitKeep(dmg_)) reapplied = true;     // the damage-number slot
        if (!reapplied) return;
        const KeepMonitor::Event e = keep_.reapplied(frame_, nowMs_);
        if (e.chat) chat(fmt("something else replaced a font texture %d times; TrueFont put it back. See the log.", e.total), 0x68);
        if (e.autoOff) {
            autoOffReason_ = fmt("something else replaced the menu and HUD font textures %d times within %d s", e.inWindow, int(KeepMonitor::kAutoOffWindowMs / 1000));
            err("sprites: keep: " + autoOffReason_ + "; turning them off until /tfont on");
            const OffReport rep = turnOff("the automatic off");
            chat("something else keeps replacing the menu and HUD font textures, so TrueFont gave them back to the game" +
                     (rep.clean() ? std::string() : " (left: " + rep.leftText() + ")") + ". /tfont on to try again.",
                 0x44);
        }
    }
    void logKeep(const std::string& s) {
        if (++keepLines_ <= 40 || keepLines_ % 500 == 0) info(s + (keepLines_ > 40 ? fmt(" [%u keep lines]", keepLines_) : std::string()));
    }

    // The identity of a walk's leaf filter, so the walk cache is re-used only for it.
    std::vector<uintptr_t> filterKey(const std::vector<Rec*>& recs) const {
        std::vector<uintptr_t> k;
        for (const Rec* s : recs) { k.push_back(uintptr_t(s->id)); k.push_back(s->rec); k.push_back(aliasOf(*s)); }
        return k;
    }
    // Watch resources containing our records and retain their registry signature for change detection.
    void watchRegistry(const RegistryScan& rs) {
        watched_.clear();
        for (const LiveLeaf& l : rs.kept)
            if (std::find(watched_.begin(), watched_.end(), l.resAddr) == watched_.end()) watched_.push_back(l.resAddr);
        regSig_ = registrySignature(reader_, abs(SR_.registry), watched_);
        regPollMs_ = nowMs_ + 1000;
    }
    void pollRegistry() {
        regPollMs_ = nowMs_ + 1000;
        if (!gameAlive()) return;
        const RegistrySig now = registrySignature(reader_, abs(SR_.registry), watched_);
        // Rebind every poll through a fresh walk; an unchanged signature does not authorize writes.
        if (now == regSig_) { keepLeafSplits(); return; }
        if (rechecked_ && nowMs_ < lastRecheckMs_ + kRecheckMs) { keepLeafSplits(); return; }   // throttled (the baseline stays: it fires later)
        for (const auto& s : recs_)
            if (s.gate == Gate::Ok && s.lost) { keepLeafSplits(); return; }   // its address is stale: the baseline stays until rescan settles
        rechecked_ = true;
        lastRecheckMs_ = nowMs_;
        info(fmt("sprites: the sprite registry changed (%u resources, %u composites, %u watched leaves; was %u, %u, %u): the rects are checked again", now.count, now.comps,
                 now.leaves, regSig_.count, regSig_.comps, regSig_.leaves));
        std::vector<Rec*> live;
        for (auto& s : recs_)   // the records in place, and under Show original the ones that go back in at untick; font font
                                // also while only its nameplate composite is built
            if (s.gate == Gate::Ok && !s.lost && !s.wentNative &&
                (s.installed || (showOriginal_ && s.ours) || (s.id == SpriteRec::FontFont && (plate_.ours || dmg_.ours || hud_.ours)) || (s.id == SpriteRec::Menu2fon && jobs_.ours)))
                live.push_back(&s);
        RegistryScan rs;
        const auto keepLeaf = [&](const LiveLeaf& l) {
            for (const Rec* s : live)
                if (samples(l, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s))) return true;
            return false;
        };
        const double t0 = qpcMs();
        const bool ok = scanRegistryIncremental(reader_, abs(SR_.registry), keepLeaf, filterKey(live), regCache_, rs);
        const double walkMs = qpcMs() - t0;
        if (!ok) { warn(fmt("sprites: the registry could not be walked again (%s); the records stay as they are", rs.limitHit ? "limit" : "fault")); return; }
        info(fmt("sprites: registry walked again in %.2f ms (QPC): %d of %d resources walked, the others unchanged; %d composites, %d leaves, %zu sample the records", walkMs,
                 regCache_.walkedLast, rs.resources, rs.composites, rs.leaves, rs.kept.size()));
        watchRegistry(rs);
        for (Rec* s : live) {
            std::vector<Rect> offGrid;
            const RectCheck c = checkRects(s->id, rectsOf(rs, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s), s->s, &offGrid), tables_);
            const OffGridCheck og = checkOffGrid(s->id, offGrid, s->s, tables_);
            s->check = c;
            info(fmt("sprites: D9 again (%.2f ms): %s: %d live rects: %d owned (of %d), %d foreign, %zu unknown (%s)%s", qpcMs() - t0, nameOf(*s), c.rects, c.owned, c.ownedShipped,
                     c.foreign, c.unknown.size(), c.complete() ? "complete" : c.ok() ? "partial set, all known" : "MISMATCH",
                     offGrid.empty() ? "" : fmt("; off its %dx grid: %zu clear of the glyphs, %zu unscaled table rects, %zu over a glyph", s->s, og.clear.size(), og.unscaled.size(),
                                                og.refused.size()).c_str()));
            // The code -> rect maps (a fontshp or dmgnum set loaded or reloaded with its composites reordered).
            if (s->id == SpriteRec::FontFont) {
                s->codes = checkCodeMap(rs, s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s), tables_, s->s);
                if (!s->codes.ok()) {
                    dropRecord(*s, "a sprite set loaded later has a code -> rect map the tables do not (" + s->codes.first + ")");
                    continue;
                }
            }
            // A live rect "not in use now" is not a mismatch; only rects present but unknown (or off a scaled sheet's grid over a glyph) drop the record.
            if (!og.ok())
                dropRecord(*s, fmt("a sprite set loaded later samples it with %zu rects off its %dx sheet's grid over a glyph (first %d,%d %dx%d in \"%s\")", og.refused.size(), s->s,
                                   og.refused[0].u, og.refused[0].v, og.refused[0].w, og.refused[0].h,
                                   setSampling(rs, og.refused[0], s->rec, kSpriteRecs[int(s->id)].name, aliasOf(*s)).c_str()));
            else if (!c.unknown.empty())
                dropRecord(*s, fmt("a sprite set loaded later samples it with %zu rects the tables do not know (first %d,%d %dx%d)", c.unknown.size(), c.unknown[0].u, c.unknown[0].v,
                                   c.unknown[0].w, c.unknown[0].h));
            else if (!c.owned) info(fmt("sprites: %s: no loaded sprite set samples it now; kept as it is", nameOf(*s)));
        }
        keepLeafSplits();   // bound again (the game may have re-resolved its leaves)
    }
    // Disable the record until /tfont on. Retain uncertain references after deletion rather than risk a double release.
    void dropRecord(Rec& s, const std::string& why) {
        if (s.id == SpriteRec::FontFont) parkSplits("font font goes back to native");   // their composites go with it
        if (s.id == hud_.base) leafPark(hud_, "font font goes back to native");
        if (s.id == jobs_.base) leafPark(jobs_, "menu2fon goes back to native");
        OffReport rep;
        if (s.installed) {
            bool ref = s.originalRef;
            if (restoreOne(s, s.rec, s.original, ref, true, "", rep)) s.heldByRecord = true;
            s.originalRef = ref;
            s.installed = false;
        }
        for (Abandoned& a : s.abandoned)
            if (restoreOne(s, a.rec, a.original, a.originalRef, true, " (an older record)", rep)) s.heldByRecord = true;
        s.abandoned.clear();
        if (s.ours && !s.heldByRecord && gameAlive()) {
            s.ours->Release();
            s.ours = nullptr;
        } else if (s.ours) warn(fmt("sprites: TrueFont's %s texture is kept (a record may still hold it); off and unload report it", nameOf(s)));
        if (s.originalRef && s.original) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });
        s.originalRef = false;
        s.built = false;
        s.kept.reset();
        s.lost = false;
        s.wentNative = true;
        setGate(s, Gate::Native, why);
    }

    // Park disabled records with their readbacks intact. If restoration fails, block later rebuilds
    // from releasing a texture the game may still hold.
    void parkRecord(Rec& s) {
        if (!gameAlive()) return;
        if (s.id == hud_.base) leafPark(hud_, "every group of font font is switched off");   // its leaves first
        if (s.id == jobs_.base) leafPark(jobs_, "every group of menu2fon is switched off");
        if (s.lost) {
            if (!rescanAt_) rescanAt_ = nowMs_ + 1;
            info(fmt("sprites: %s: switched off while its record is looked for again; parked once it is found", nameOf(s)));
            return;
        }
        OffReport rep;
        if (s.installed) {
            bool ref = s.originalRef;
            if (restoreOne(s, s.rec, s.original, ref, true, "", rep)) s.heldByRecord = true;
            s.originalRef = ref;
            s.installed = false;
        }
        for (Abandoned& a : s.abandoned)
            if (restoreOne(s, a.rec, a.original, a.originalRef, true, " (an older record)", rep)) s.heldByRecord = true;
        s.abandoned.clear();
        if (s.originalRef && s.original) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });   // an adopted original not written back
        s.originalRef = false;
        s.lost = false;
        s.lostTries = 0;
        s.built = false;
        s.kept.reset();
        if (s.heldByRecord) {
            warn(fmt("sprites: %s: switched off, but %s; TrueFont's texture is kept and the record stays native until /tfont on", nameOf(s), rep.leftText().c_str()));
            s.wentNative = true;
            setGate(s, Gate::Failed, "switched off, but its original could not be put back");
            return;
        }
        if (s.ours) {
            s.ours->Release();
            info(fmt("sprites: %s: every group it serves is switched off: the game's own texture is back, TrueFont's released", nameOf(s)));
        }
        s.ours = nullptr;
        s.w = s.h = s.k = 0;
    }
    bool anyParked() const {
        for (const auto& s : recs_) if (s.gate == Gate::Ok && !s.wentNative && !recordOn(s.id, opts_)) return true;
        return false;
    }

    // A record that is no longer itself, or whose +40 the cache emptied, is looked for again by name.
    void rescan() {
        rescanAt_ = 0;
        if (!gameAlive()) return;
        CacheScan cs;
        scanCache(reader_, abs(SR_.texCache), vtableVA(), cs);
        bool again = false;
        for (auto& s : recs_) {
            if (!s.lost || !s.ours) continue;
            const RecordFind& f = cs.rec[int(s.id)];
            const bool on = recordOn(s.id, opts_);
            if (f.valid && f.first.rec == s.rec && !on) {   // switched off while looked for, the record itself again: parked
                s.lost = false;
                parkRecord(s);
                continue;
            }
            if (f.valid && f.first.rec == s.rec && !s.installed && !showOriginal_) {   // the same record checks out again
                if (reinstallSame(s) || s.gate != Gate::Ok) continue;
                if (++s.lostTries >= kLostTries) {
                    s.lost = false;
                    s.wentNative = true;
                    warn(fmt("sprites: %s: record %08X did not take TrueFont's texture after %d looks; it stays native until /tfont on", nameOf(s), unsigned(s.rec), s.lostTries));
                } else again = true;
                continue;
            }
            if (!f.valid || f.first.rec == s.rec) {
                if (f.valid && s.installed) { s.lost = false; continue; }   // the same record, back to normal
                if (++s.lostTries >= kLostTries) {
                    s.lost = false;
                    s.installed = false;   // the keep check stops looking (the cache's destructor dropped its reference)
                    s.wentNative = true;
                    warn(fmt("sprites: %s: no record of its name to use after %d looks (%s); it stays native until /tfont on", nameOf(s), s.lostTries, f.valid ? "only the old one" : f.why.c_str()));
                    continue;
                }
                again = true;
                continue;
            }
            // A record re-created at another sheet scale is other art: native until /tfont on reads it afresh.
            if (f.scale != s.s) {
                dropRecord(s, fmt("its new record is a %dx sheet, not the %dx one read at this install", f.scale, s.s));
                s.artNative = true;
                continue;
            }
            // Validate replacement art before adopting the new record; retain the old one for restoration if it holds ours.
            {
                SpritePixels px;
                std::string why;
                bool artDiffers = false;
                if (!readAndPin(s, rd<uint32_t>(f.first.rec + kTexPrimary), px, why, false, &artDiffers)) {
                    if (artDiffers) {   // the art changed while the game ran: native until /tfont on reads it afresh
                        dropRecord(s, "its new record's " + why);
                        s.artNative = true;
                        continue;
                    }
                    if (++s.lostTries >= kLostTries) {
                        s.lost = false;
                        s.installed = false;
                        s.wentNative = true;
                        warn(fmt("sprites: %s: the new record is not used: %s; it stays native until /tfont on", nameOf(s), why.c_str()));
                    }
                    else again = true;
                    continue;
                }
            }
            if (sameRecord(s.rec, s.id, true) && isOurs(s, rd<uint32_t>(s.rec + kTexPrimary)) && s.abandoned.size() < 8) s.abandoned.push_back(Abandoned{s.rec, s.original, s.originalRef});
            else if (s.originalRef && s.original) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });
            s.installed = false;
            // Keep uncertain references after deletion to avoid releasing through stale memory.
            s.original = 0;
            s.originalRef = false;
            if (s.id == SpriteRec::FontFont) plateRefound();   // a record found again: the page slot is looked at afresh
            if (!on) {   // every group it serves is switched off: the new record is adopted, not written, and parked
                s.rec = f.first.rec;
                s.lost = false;
                s.lostTries = 0;
                info(fmt("sprites: %s: the game re-created its record (%08X) while its groups are switched off; it is parked", nameOf(s), unsigned(f.first.rec)));
                parkRecord(s);
                continue;
            }
            if (showOriginal_) {   // the originals are showing: Show original off installs into the new record
                s.rec = f.first.rec;
                s.lost = false;
                info(fmt("sprites: %s: the game re-created its record (%08X); TrueFont's texture goes in when Show Original is off", nameOf(s), unsigned(f.first.rec)));
                continue;
            }
            std::string why;
            // The game's own re-creation, not something else replacing ours: not counted as a re-apply.
            if (installSlot(s, f.first.rec, why)) info(fmt("sprites: %s: the game re-created its record (%08X); TrueFont's texture is in it", nameOf(s), unsigned(f.first.rec)));
            else {
                s.rec = f.first.rec;
                if (++s.lostTries >= kLostTries) {
                    s.lost = false;
                    s.wentNative = true;
                    warn(fmt("sprites: %s: the new record is not used: %s; it stays native until /tfont on", nameOf(s), why.c_str()));
                }
                else again = true;
            }
        }
        if (again) rescanAt_ = nowMs_ + 2000;
    }

    // Reapply only after identity and art checks. Release the record's previous TrueFont reference after swapping.
    bool reinstallSame(Rec& s) {
        uint32_t now = 0;
        if (!rdok(s.rec + kTexPrimary, now)) return false;
        if (isRetired(s, now)) {
            s.ours->AddRef();
            if (wrVerifiedWhy<uint32_t>(s.rec + kTexPrimary, uint32_t(uintptr_t(s.ours)), now) != WriteResult::Ok) { s.ours->Release(); return false; }
            reinterpret_cast<IDirect3DTexture8*>(uintptr_t(now))->Release();
            s.installed = true;
            s.lost = false;
            s.lostTries = 0;
            info(fmt("sprites: %s: record %08X checks out again; TrueFont's texture replaces an earlier one of its own", nameOf(s), unsigned(s.rec)));
            return true;
        }
        if (now != uintptr_t(s.ours)) {
            SpritePixels px;
            std::string why;
            bool artDiffers = false;
            if (!readAndPin(s, now, px, why, false, &artDiffers)) {
                if (artDiffers) {
                    dropRecord(s, "its record's " + why);
                    s.artNative = true;
                }
                return false;
            }
        }
        std::string why;
        if (!installSlot(s, s.rec, why)) { info(fmt("sprites: %s: %s; looked at again", nameOf(s), why.c_str())); return false; }
        info(fmt("sprites: %s: record %08X checks out again; TrueFont's texture is back in it", nameOf(s), unsigned(s.rec)));
        return true;
    }

    // One record's restore (swaplogic.h decideSlotRestore), for the record and for an abandoned one.
    bool restoreOne(Rec& s, uintptr_t r, uintptr_t original, bool& originalRef, bool d3d, const char* which, OffReport& rep) {
        const bool alive = gameAlive();
        const bool same = alive && sameRecord(r, s.id, true);
        uint32_t now = 0;
        const bool readOk = same && rdok(r + kTexPrimary, now);
        const uintptr_t mine = (readOk && isOurs(s, now)) ? uintptr_t(now) : uintptr_t(s.ours);
        const SlotRestore dcs = decideSlotRestore(alive, same, readOk, now, mine, original);
        bool wrote = false;
        if (dcs == SlotRestore::WriteOriginal) {
            const WriteResult w = wrVerifiedWhy<uint32_t>(r + kTexPrimary, uint32_t(original), now);
            wrote = w == WriteResult::Ok;
            if (wrote && d3d) reinterpret_cast<IDirect3DTexture8*>(mine)->Release();   // the record's reference
            if (!wrote) warn(fmt("sprites: restore: %s record %08X +40 not written back (%s)", nameOf(s), unsigned(r), writeResultText(w)));
        }
        if (originalRef && d3d && (wrote || dcs == SlotRestore::AlreadyOriginal)) {
            seh([&] { reinterpret_cast<IDirect3DTexture8*>(original)->Release(); });
            originalRef = false;
        }
        const bool held = slotMayHoldOurs(dcs, wrote);
        if (held) rep.left.push_back(fmt("the %s record still holds TrueFont's texture", nameOf(s)));
        if (!alive) rep.gameGone = true;
        info(fmt("sprites: restore: %s%s record %08X: %s%s", nameOf(s), which, unsigned(r), slotRestoreText(dcs), dcs == SlotRestore::WriteOriginal ? (wrote ? fmt(" (%08X)", unsigned(original)).c_str() : " (FAILED)") : ""));
        return held;
    }
    OffReport restoreSlots(bool d3d) {
        OffReport rep;
        restoreSplit(plate_, rep);   // the page slots first (their composites and copies are kept here)
        restoreSplit(dmg_, rep);
        leafRestore(hud_, rep);      // the leaves bound to a copy
        leafRestore(jobs_, rep);
        for (auto& s : recs_) {
            if (s.installed) {
                rep.wasOn = true;
                bool ref = s.originalRef;
                if (restoreOne(s, s.rec, s.original, ref, d3d, "", rep)) s.heldByRecord = true;
                s.originalRef = ref;
                s.installed = false;
            }
            for (Abandoned& a : s.abandoned) {
                rep.wasOn = true;
                if (restoreOne(s, a.rec, a.original, a.originalRef, d3d, " (an older record)", rep)) s.heldByRecord = true;
            }
            s.abandoned.clear();
        }
        return rep;
    }
    // Restore records before releasing textures; skip Direct3D calls on DataOnly release or after game teardown.
    OffReport restoreAll(bool d3d) {
        regCache_.clear();   // off, a refusal and Release forget the walk (the next look walks afresh)
        OffReport rep = restoreSlots(d3d);
        freeSplit(plate_, d3d);   // only once the slot is verified not to name the copy, never on a data-only release
        freeSplit(dmg_, d3d);
        leafFree(hud_, d3d);      // likewise, once no leaf names the copy
        leafFree(jobs_, d3d);
        showOriginal_ = false;
        const bool alive = gameAlive();
        for (auto& s : recs_) {
            s.lost = false;
            s.lostTries = 0;
            if (!s.ours) continue;
            rep.wasOn = true;
            if (!alive) rep.gameGone = true;
            if (s.heldByRecord && rep.clean()) rep.left.push_back(fmt("a record may still hold TrueFont's %s texture", nameOf(s)));
            if (d3d && !s.heldByRecord && alive) {
                s.ours->Release();
                info(fmt("sprites: restore: TrueFont's %s texture %08X released", nameOf(s), unsigned(uintptr_t(s.ours))));
            } else {
                info(fmt("sprites: restore: TrueFont's %s texture %08X is kept (%s)", nameOf(s), unsigned(uintptr_t(s.ours)),
                         !d3d ? "no Direct3D call on this path" : s.heldByRecord ? "a record may still hold it" : "the game is closing"));
            }
            if (s.originalRef && s.original && d3d && alive) seh([&] { reinterpret_cast<IDirect3DTexture8*>(s.original)->Release(); });
            s.originalRef = false;
            // Retained textures remain ours for future detection and require pinning on unload.
            if (s.heldByRecord) {
                leftover_ = true;
                if (!isRetired(s, uintptr_t(s.ours))) s.retired.push_back(uintptr_t(s.ours));
            }
            s.ours = nullptr;
            s.w = s.h = s.k = 0;
            s.built = false;
            s.kept.reset();
            s.heldByRecord = false;
        }
        return rep;
    }

    static constexpr uint64_t kRebuildQuietMs = 300;
    static constexpr uint64_t kFindWaitMs = 60000;
    static constexpr int kLostTries = 30;

    Module client_;
    SpriteResolved SR_;
    uint32_t rendererRva_ = 0;
    MemReader reader_;
    IDirect3DDevice8* device_ = nullptr;
    SpriteOptions opts_ = SpriteOptions::defaults();
    SpriteOptions batchOpts_;
    SpriteWorker worker_;
    Rec recs_[kSpriteRecCount];
    KeepMonitor keep_;
    Phase phase_ = Phase::Off;
    bool enabled_ = false, building_ = false, showOriginal_ = false, gameGone_ = false, forceAll_ = false, batchForced_ = false, d1Logged_ = false, failurePersistent_ = false;
    bool originalAtInstall_ = false;   // Show original asked for before the first batch was in place
    bool capsRead_ = false;
    unsigned long maxTexW_ = 0, maxTexH_ = 0;
    bool nonPow2_ = true;              // the card takes a texture that is no power of two (D3DPTEXTURECAPS_POW2 clear)
    std::string kSaid_;                // Last Sharpness warning; suppress duplicates.
    std::string raisedSaid_;           // the last ks raised to a scaled sheet's (logged once per outcome)
    const char* poolOf_ = "-";
    TexFormat formatOf_ = TexFormat::A8R8G8B8;
    FormatSupport formats_;
    bool formatSaid_ = false, compressSaid_ = false;
    Engine engine_[kGroupCount] = {};   // what each group was last drawn with (groupEngine)
    bool engineBuilt_[kGroupCount] = {}, engineMixed_[kGroupCount] = {};
    // A built composite's engines: kept for /tfont diag; a face FreeType could not open is logged.
    void noteEngines(const SpriteAtlas& a) {
        for (int g = int(Group::Labels); g < kGroupCount; g++) {
            const SpriteGroupStats& gs = a.group[g];
            if (gs.face.empty()) continue;   // not drawn in this composite
            engine_[g] = gs.engine;
            engineMixed_[g] = gs.engineMixed;
            engineBuilt_[g] = true;
            if (gs.engineNote.empty()) continue;
            warn(fmt("sprites: build: engine: %s: %s", groupName(Group(g)), gs.engineNote.c_str()));
            engineFallbackChat(batchOpts_.engine, groupName(Group(g)), gs.engineFaulted);
        }
    }
    uint64_t otherBytes_ = 0;
    bool otherBuilding_ = false;       // Chat and menus is building
    bool otherClaiming_ = false;       // Chat and menus' first look after an on is sized first
    bool chatHoldSaid_ = false;        // a rebuild's wait for it was logged
    uint64_t batchNewBytes_ = 0;       // the batch in flight's new textures and kept composites
    SpriteKs lastGot_, growTried_;     // the ks the last batch fitted; the larger outcome last tried
    bool batched_ = false, grew_ = false;
    uint64_t batchShare_ = 0;          // Chat and menus' share when the last batch was fitted (the growth line's words)
    bool batchQuietNext_ = false;      // the rebuild due is automatic (a growth or a step down): a failure goes to the log only
    bool batchQuiet_ = false;          // ... the batch running (or last started) is
    uint64_t growCheckMs_ = 0;
    uint64_t gen_ = 0, buildGen_ = 0;
    uint64_t frame_ = 0, nowMs_ = 0, buildStartMs_ = 0, rebuildAt_ = 0, rescanAt_ = 0, firstLookMs_ = 0, nextLookMs_ = 0, regPollMs_ = 0;
    std::vector<uintptr_t> watched_;   // Resources whose leaves sample a tracked record.
    RegistrySig regSig_;
    uint64_t lastRecheckMs_ = 0;
    bool rechecked_ = false;
    static constexpr uint64_t kRecheckMs = 5000;
    RegistryCache regCache_;   // the last registry walk, by resource
    bool holdSaid_ = false;
    bool announceRebuild_ = false;
    bool leftover_ = false;            // a restore since load left a texture of ours in a record (Rec::retired; Release reports it)
    std::string lastSaidFailure_;
    unsigned swaps_ = 0, keepLines_ = 0;
    int waitLogged_ = 0;
    std::string autoOffReason_, failure_;
    // The client's language and the table set it picks.
    ClientLang lang_ = ClientLang::Unknown;
    TableSet tables_ = TableSet::English;
    bool langRead_ = false, langUnreadSaid_ = false, langMixedSaid_ = false, deFrSaid_ = false;
    uint64_t langPollMs_ = 0;
};

// Tick Chat first with current sprite usage and in-flight allocations. Its initial claim uses the sprite floor.
inline void chatFrame(Installer& chat, const SpriteInstaller* sprites, uint64_t frame, uint64_t nowMs, bool inGame) {
    const uint64_t b = sprites ? sprites->memoryBytes() : 0;
    chat.setOtherBytes(b, sprites && sprites->building(), sprites ? sprites->pendingBytes() : 0, sprites && chat.claiming() ? sprites->floorBytes() : b);
    chat.tick(frame, nowMs, inGame);
}
// Then tick sprites with Chat's reserved peak. Do not copy both native atlases in the same frame.
inline void spriteFrame(SpriteInstaller& sprites, const Installer* chat, bool chatOn, uint64_t frame, uint64_t nowMs, bool inGame) {
    const bool on = chat && chatOn;
    sprites.setOtherBytes(on ? chat->budgetBytes() : 0, on && chat->building(), on && chat->claiming());
    sprites.tick(frame, nowMs, inGame, chat && chat->heavyThisFrame());
}

}  // namespace tf
