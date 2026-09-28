// Sprite-font rect enumeration from the sprite registry (4E1BF8): every loaded leaf, its rect and the record it
// samples. Offsets (pinned in spritesigs.h): list +0 first node, +10 count; node +0 next, +10 data, +14 dead;
// resource +24 name, +20 composites, +3C count; composite +4 leaf list; leaf +08..+0E rect, +1C flags, +2D blend,
// +32 name, +4C record, +C0/+C8 corners. Bounded; every read through the reader.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "records.h"

namespace tf {

struct LiveLeaf {
    char res[17] = {};       // the resource (Msb set) name
    int comp = 0, leaf = 0;  // composite index in the resource, leaf index in the composite
    Rect r;
    uint8_t flags = 0;
    uint8_t blend[4] = {};
    char name[17] = {};      // the texture name
    uintptr_t record = 0;    // +4C
    uintptr_t resAddr = 0;   // the resource the leaf belongs to
    uintptr_t addr = 0;      // the leaf itself (valid while the registry signature is the one it was walked under)
    int16_t cx[4] = {}, cy[4] = {};
};

struct RegistryScan {
    int resources = 0, composites = 0, leaves = 0;   // visited
    int slots = 0;                                    // composite array slots looked at, null ones included (the bound)
    bool limitHit = false;
    bool fault = false;
    std::vector<LiveLeaf> kept;                       // the leaves the filter kept
};

struct RegistryLimits { int resources = 4096, composites = 65536, leavesPerComposite = 4096, leaves = 1 << 20; };

// Visits the live nodes of a list object; false when a read failed or the limit was reached.
inline bool forEachNode(const MemReader& m, uintptr_t list, int limit, bool& limitHit, const std::function<bool(uint32_t)>& fn) {
    uint32_t node = 0;
    if (!m.get(list, node)) return false;
    int n = 0;
    while (node) {
        if (++n > limit) { limitHit = true; return false; }
        if (!plausible(node)) return false;
        uint8_t dead = 0;
        uint32_t data = 0, next = 0;
        if (!m.get(node + kNodeDead, dead) || !m.get(node + kNodeData, data) || !m.get(node + kNodeNext, next)) return false;
        if (!dead && !fn(data)) return false;
        node = next;
    }
    return true;
}

// One resource's composites and leaves into `s` (the walk's body; `res` is plausible). False when a read failed or a limit
// was reached.
inline bool walkResource(const MemReader& m, uint32_t res, const std::function<bool(const LiveLeaf&)>& keep, RegistryScan& s, const RegistryLimits& lim) {
    ++s.resources;
    char rn[17] = {};
    uint32_t comps = 0;
    uint16_t count = 0;
    if (!m.bytes(res + kResName, rn, 16) || !m.get(res + kResComposites, comps) || !m.get(res + kResCount, count)) return false;
    if (count && !plausible(comps)) return false;
    for (int ci = 0; ci < int(count); ci++) {
        // Every slot counts toward the bound, a null one too (a large count over zeros is a stall).
        if (++s.slots > lim.composites) { s.limitHit = true; return false; }
        uint32_t comp = 0;
        if (!m.get(comps + 4u * uint32_t(ci), comp)) return false;
        if (!comp) continue;
        if (!plausible(comp)) return false;
        if (++s.composites > lim.composites) { s.limitHit = true; return false; }
        int li = 0;
        const bool leavesOk = forEachNode(m, comp + kCompLeaves, lim.leavesPerComposite, s.limitHit, [&](uint32_t leaf) -> bool {
            const int idx = li++;
            if (!leaf) return true;
            if (!plausible(leaf)) return false;
            if (++s.leaves > lim.leaves) { s.limitHit = true; return false; }
            LiveLeaf l;
            std::memcpy(l.res, rn, 16);
            l.comp = ci;
            l.leaf = idx;
            uint32_t rec = 0;
            if (!m.get(leaf + kLeafW, l.r.w) || !m.get(leaf + kLeafH, l.r.h) || !m.get(leaf + kLeafU, l.r.u) || !m.get(leaf + kLeafV, l.r.v) || !m.get(leaf + kLeafFlags, l.flags) ||
                !m.bytes(leaf + kLeafBlend, l.blend, 4) || !m.bytes(leaf + kLeafName, l.name, 16) || !m.get(leaf + kLeafRecord, rec) || !m.bytes(leaf + kLeafCornerX, l.cx, 8) ||
                !m.bytes(leaf + kLeafCornerY, l.cy, 8))
                return false;
            l.record = rec;
            l.resAddr = res;
            l.addr = leaf;
            if (!keep || keep(l)) s.kept.push_back(l);
            return true;
        });
        if (!leavesOk) return false;
    }
    return true;
}

// Walks the registry at `registry` (its address). `keep` decides which leaves are stored (all when empty).
inline bool scanRegistry(const MemReader& m, uintptr_t registry, const std::function<bool(const LiveLeaf&)>& keep, RegistryScan& s, const RegistryLimits& lim = {}) {
    s = RegistryScan{};
    const bool ok = forEachNode(m, registry, lim.resources, s.limitHit, [&](uint32_t res) -> bool {
        if (!res) return true;
        if (!plausible(res)) return false;
        return walkResource(m, res, keep, s, lim);
    });
    if (!ok && !s.limitHit) s.fault = true;
    return ok;
}

// Cache walks per resource; reuse only when address, fingerprint and filter key match.
// Results match scanRegistry without walking unchanged resources.
struct RegistryCache {
    struct Res { uintptr_t addr = 0; uint64_t print = 0; int slots = 0, composites = 0, leaves = 0; std::vector<LiveLeaf> kept; };
    std::vector<uintptr_t> key;
    std::vector<Res> res;
    bool valid = false;
    int walkedLast = 0;   // resources the last call walked leaf by leaf (the others were re-used)
    void clear() { key.clear(); res.clear(); valid = false; }
};
// A resource's fingerprint (its composite array and each composite's leaf list head). False on a failed read, or when its
// composite count passes `maxSlots` (the walk's remaining budget), so a torn count is never read in full.
inline bool resourcePrint(const MemReader& m, uint32_t res, uint64_t& print, int maxSlots = 1 << 30) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } };
    char rn[16] = {};
    uint32_t comps = 0;
    uint16_t count = 0;
    if (!m.bytes(res + kResName, rn, 16) || !m.get(res + kResComposites, comps) || !m.get(res + kResCount, count)) return false;
    mix(rn, 16);
    mix(&comps, 4);
    mix(&count, 2);
    if (count && !plausible(comps)) return false;
    if (int(count) > maxSlots) return false;
    for (int ci = 0; ci < int(count); ci++) {
        uint32_t comp = 0, first = 0, n = 0;
        if (!m.get(comps + 4u * uint32_t(ci), comp)) return false;
        mix(&comp, 4);
        if (!comp) continue;
        if (!plausible(comp) || !m.get(comp + kCompLeaves, first) || !m.get(comp + kCompLeaves + kRegCount, n)) return false;
        mix(&first, 4);
        mix(&n, 4);
    }
    print = h;
    return true;
}
inline bool scanRegistryIncremental(const MemReader& m, uintptr_t registry, const std::function<bool(const LiveLeaf&)>& keep, const std::vector<uintptr_t>& key, RegistryCache& cache,
                                    RegistryScan& s, const RegistryLimits& lim = {}) {
    s = RegistryScan{};
    const bool useCache = cache.valid && cache.key == key;
    std::vector<RegistryCache::Res> next;
    int walked = 0;
    const bool ok = forEachNode(m, registry, lim.resources, s.limitHit, [&](uint32_t res) -> bool {
        if (!res) return true;
        if (!plausible(res)) return false;
        uint64_t print = 0;
        const bool printed = resourcePrint(m, res, print, lim.composites - s.slots);
        RegistryCache::Res* old = nullptr;
        if (useCache && printed)
            for (auto& r : cache.res)
                if (r.addr == res && r.print == print) { old = &r; break; }
        // Unchanged: its counts and kept leaves as the walk found them. Near a limit it is walked (the walk says where it hit).
        if (old && s.slots + old->slots <= lim.composites && s.composites + old->composites <= lim.composites && s.leaves + old->leaves <= lim.leaves) {
            ++s.resources;
            s.slots += old->slots;
            s.composites += old->composites;
            s.leaves += old->leaves;
            s.kept.insert(s.kept.end(), old->kept.begin(), old->kept.end());
            next.push_back(std::move(*old));   // moved: the cache is rebuilt from `next` (or cleared on a failure)
            old->addr = 0;                     // a resource listed twice is walked the second time
            return true;
        }
        const int slots0 = s.slots, comps0 = s.composites, leaves0 = s.leaves;
        const size_t kept0 = s.kept.size();
        ++walked;
        if (!walkResource(m, res, keep, s, lim)) return false;
        RegistryCache::Res r;
        r.addr = res;
        r.print = print;
        r.slots = s.slots - slots0;
        r.composites = s.composites - comps0;
        r.leaves = s.leaves - leaves0;
        r.kept.assign(s.kept.begin() + std::ptrdiff_t(kept0), s.kept.end());
        if (printed) next.push_back(std::move(r));
        return true;
    });
    if (!ok && !s.limitHit) s.fault = true;
    cache.walkedLast = walked;
    if (ok) {
        cache.key = key;
        cache.res = std::move(next);
        cache.valid = true;
    } else cache.clear();
    return ok;
}

// The live resources named `name16` (dead nodes skipped). False when a read failed or the limit was reached.
inline bool resourcesNamed(const MemReader& m, uintptr_t registry, const char* name16, std::vector<uint32_t>& out, bool& limitHit, const RegistryLimits& lim = {}) {
    out.clear();
    limitHit = false;
    return forEachNode(m, registry, lim.resources, limitHit, [&](uint32_t res) -> bool {
        if (!res) return true;
        if (!plausible(res)) return false;
        char rn[16] = {};
        if (!m.bytes(res + kResName, rn, 16)) return false;
        if (std::memcmp(rn, name16, 16) == 0) out.push_back(res);
        return true;
    });
}

// The client's language from the live resources' names (fonttables.h LangVote: the resident frames?? set). Names only; false
// when a read failed or the limit was reached.
inline bool readClientLang(const MemReader& m, uintptr_t registry, LangVote& v, bool& limitHit, const RegistryLimits& lim = {}) {
    v = LangVote{};
    limitHit = false;
    return forEachNode(m, registry, lim.resources, limitHit, [&](uint32_t res) -> bool {
        if (!res) return true;
        if (!plausible(res)) return false;
        char rn[16] = {};
        if (!m.bytes(res + kResName, rn, 16)) return false;
        v.add(rn);
        return true;
    });
}

// Every live leaf of every resource whose +4C is `value`, reading only the list links and +4C. True only when the walk
// completed (every read good, no limit reached). `leaves`: the leaves visited.
inline bool leavesNaming(const MemReader& m, uintptr_t registry, uint32_t value, std::vector<uintptr_t>& out, int& leaves, bool& limitHit, const RegistryLimits& lim = {}) {
    out.clear();
    leaves = 0;
    limitHit = false;
    int slots = 0, comps = 0;
    return forEachNode(m, registry, lim.resources, limitHit, [&](uint32_t res) -> bool {
        if (!res) return true;
        if (!plausible(res)) return false;
        uint32_t arr = 0;
        uint16_t count = 0;
        if (!m.get(res + kResComposites, arr) || !m.get(res + kResCount, count)) return false;
        if (count && !plausible(arr)) return false;
        for (int ci = 0; ci < int(count); ci++) {
            if (++slots > lim.composites) { limitHit = true; return false; }
            uint32_t comp = 0;
            if (!m.get(arr + 4u * uint32_t(ci), comp)) return false;
            if (!comp) continue;
            if (!plausible(comp)) return false;
            if (++comps > lim.composites) { limitHit = true; return false; }
            const bool ok = forEachNode(m, comp + kCompLeaves, lim.leavesPerComposite, limitHit, [&](uint32_t leaf) -> bool {
                if (!leaf) return true;
                if (!plausible(leaf)) return false;
                if (++leaves > lim.leaves) { limitHit = true; return false; }
                uint32_t rec = 0;
                if (!m.get(leaf + kLeafRecord, rec)) return false;
                if (rec == value) out.push_back(leaf);
                return true;
            });
            if (!ok) return false;
        }
        return true;
    });
}

// A cheap registry signature (polled once a second): resource count, XOR of their addresses, composite-count sum and
// the leaf counts of the `watched` resources' composites. A change means a set was loaded, unloaded or edited.
struct RegistrySig {
    uint32_t count = 0, xorRes = 0, comps = 0, leaves = 0;
    bool ok = false;
    bool operator==(const RegistrySig& o) const { return ok == o.ok && count == o.count && xorRes == o.xorRes && comps == o.comps && leaves == o.leaves; }
    bool operator!=(const RegistrySig& o) const { return !(*this == o); }
};
inline RegistrySig registrySignature(const MemReader& m, uintptr_t registry, const std::vector<uintptr_t>& watched, const RegistryLimits& lim = {}) {
    RegistrySig g;
    bool limitHit = false;
    int slots = 0;
    g.ok = forEachNode(m, registry, lim.resources, limitHit, [&](uint32_t res) -> bool {
        if (!res) return true;
        if (!plausible(res)) return false;
        uint16_t count = 0;
        uint32_t comps = 0;
        if (!m.get(res + kResCount, count) || !m.get(res + kResComposites, comps)) return false;
        ++g.count;
        g.xorRes ^= res;
        g.comps += count;
        if (std::find(watched.begin(), watched.end(), uintptr_t(res)) == watched.end()) return true;
        if (count && !plausible(comps)) return false;
        for (int ci = 0; ci < int(count); ci++) {
            if (++slots > lim.composites) { limitHit = true; return false; }
            uint32_t comp = 0, n = 0;
            if (!m.get(comps + 4u * uint32_t(ci), comp)) return false;
            if (!comp) continue;
            if (!plausible(comp) || !m.get(comp + kCompLeaves + kRegCount, n)) return false;
            g.leaves += n;
        }
        return true;
    });
    return g;
}

// Does a leaf sample this record? By its bound record (+4C), else by its (lower-cased) texture name. `alias`: a
// plugin-owned copy of the record (spriteswap.h's nameplate copy), which counts as the record itself: a leaf bound to it
// samples the record's rects (0 = none).
inline bool samples(const LiveLeaf& l, uintptr_t rec, const char* name16, uintptr_t alias = 0) {
    if (l.record) return l.record == rec || (alias && l.record == alias);
    for (int i = 0; i < 16; i++) {
        char a = l.name[i], b = name16[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
// The unique rects of the kept leaves that sample a record (or its copy, `alias`).
inline std::vector<Rect> rectsOf(const RegistryScan& s, uintptr_t rec, const char* name16, uintptr_t alias = 0) {
    std::vector<Rect> out;
    for (const LiveLeaf& l : s.kept) if (samples(l, rec, name16, alias)) out.push_back(l.r);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Rect-set equality misses reordered characters. Verify fontshp/dmgnum rects by composite against 51.DAT.
// Missing or shorter sets are allowed. first describes the first mismatch.
struct CodeMapCheck {
    int leaves = 0, mismatches = 0;
    std::string first;
    bool ok() const { return mismatches == 0; }
};
inline CodeMapCheck checkCodeMap(const RegistryScan& s, uintptr_t rec, const char* name16, uintptr_t alias = 0, TableSet ts = TableSet::English) {
    CodeMapCheck c;
    const RecordTable ff = tableFor(SpriteRec::FontFont, ts);
    const auto ownedRect = [&](const Rect& r) {
        for (size_t i = 0; i < ff.n; i++)
            if (ff.e[i].owned() && ff.e[i].rect() == r) return true;
        return false;
    };
    for (const LiveLeaf& l : s.kept) {
        const bool fontshp = std::memcmp(l.res, kFontshpName, 16) == 0, dmgnum = std::memcmp(l.res, kDmgnumName, 16) == 0;
        if ((!fontshp && !dmgnum) || !samples(l, rec, name16, alias)) continue;
        ++c.leaves;
        const CodeTable t = fontshp ? codesFontshp() : codesDmgnum();
        bool atComp = false, match = false;
        for (size_t i = 0; i < t.n; i++) {
            if (t.e[i].comp != l.comp) continue;
            atComp = true;
            match = match || t.e[i].r == l.r;
        }
        if (match || (!atComp && !ownedRect(l.r))) continue;
        if (!c.mismatches++) {
            char b[200];
            _snprintf_s(b, sizeof b, _TRUNCATE, "%s code %02Xh (composite %d) samples (%d,%d %dx%d), which is not the rect that code has in the tables", fontshp ? "fontshp" : "dmgnum",
                        (fontshp ? 0x20 : 0x30) + l.comp, l.comp, l.r.u, l.r.v, l.r.w, l.r.h);
            c.first = b;
        }
    }
    return c;
}

}  // namespace tf
