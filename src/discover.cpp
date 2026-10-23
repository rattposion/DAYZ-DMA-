#include "discover.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

std::vector<ClassInfo> listClasses(const Image& img, const std::string& filter) {
    std::vector<ClassInfo> out;
    std::unordered_set<std::string> seen;
    const uint32_t sigWanted = img.ptrSize == 8 ? 1 : 0;
    for (const auto& sec : img.sections) {
        if (sec.exec()) continue;
        const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
        for (uint64_t loc = std::max<uint64_t>(sec.rva, 12); loc + 12 <= hi; loc += 4) {
            uint32_t sig = 0, offset = 0, cdOffset = 0;
            if (!img.at(loc - 12, sig) || sig != sigWanted) continue;
            if (!img.at(loc - 8, offset) || offset != 0) continue;
            if (!img.at(loc - 4, cdOffset) || cdOffset > 0x1000) continue;
            uint32_t tdRva = 0, chdRva = 0;
            if (!rttiRef(img, loc, tdRva) || !rttiRef(img, loc + 4, chdRva)) continue;
            const std::string name = rttiTypeName(img, tdRva);
            if (name.empty() || (!filter.empty() && name.find(filter) == std::string::npos)) continue;
            uint32_t chdSig = 0, nBases = 0, arrRva = 0;
            if (!img.at(chdRva, chdSig) || chdSig != 0) continue;
            if (!img.at(chdRva + 8ull, nBases) || nBases == 0 || nBases > 64) continue;
            if (!rttiRef(img, chdRva + 12ull, arrRva)) continue;
            if (!seen.insert(name).second) continue;
            ClassInfo ci;
            ci.name = name;
            for (uint32_t i = 1; i < nBases; ++i) {
                uint32_t bcdRva = 0, baseTd = 0;
                if (!rttiRef(img, arrRva + 4ull * i, bcdRva) || !rttiRef(img, bcdRva, baseTd)) break;
                const std::string b = rttiTypeName(img, baseTd);
                if (!b.empty()) ci.bases.push_back(b);
            }
            out.push_back(ci);
        }
    }
    std::sort(out.begin(), out.end(), [](const ClassInfo& a, const ClassInfo& b) { return a.name < b.name; });
    return out;
}

std::string classNameAt(const Process& p, const Image& img, uint64_t addr) {
    uint64_t vt = 0, col = 0;
    if (!p.readPtr(static_cast<uintptr_t>(addr), img.ptrSize, vt)) return {};
    if (vt < img.base + img.ptrSize || vt >= img.base + img.bytes.size()) return {};
    const uint64_t vtRva = vt - img.base;
    if (img.ptrSize == 4) {
        uint32_t c32 = 0;
        if (!img.at(vtRva - 4, c32)) return {};
        col = c32;
    } else if (!img.at(vtRva - 8, col)) {
        return {};
    }
    if (col < img.base || col >= img.base + img.bytes.size()) return {};
    uint32_t tdRva = 0;
    if (!rttiRef(img, col - img.base + 12, tdRva)) return {};
    return rttiTypeName(img, tdRva);
}

std::string classNameFromVtable(const Image& img, uint64_t vt) {
    if (vt < img.base + img.ptrSize || vt >= img.base + img.bytes.size()) return {};
    const uint64_t vtRva = vt - img.base;
    uint64_t col = 0;
    if (img.ptrSize == 4) {
        uint32_t c32 = 0;
        if (!img.at(vtRva - 4, c32)) return {};
        col = c32;
    } else if (!img.at(vtRva - 8, col)) {
        return {};
    }
    if (col < img.base || col >= img.base + img.bytes.size()) return {};
    uint32_t tdRva = 0;
    if (!rttiRef(img, col - img.base + 12, tdRva)) return {};
    return rttiTypeName(img, tdRva);
}

namespace {

struct PtrHit {
    uint64_t addr;
    uint64_t value;
};

bool isWritable(DWORD prot) {
    return (prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

std::vector<PtrHit> scanPointers(const Process& p, uint32_t ptrSize, const std::unordered_set<uint64_t>& targets) {
    std::vector<PtrHit> hits;
    if (targets.empty()) return hits;
    const uint64_t lo = *std::min_element(targets.begin(), targets.end());
    const uint64_t hi = *std::max_element(targets.begin(), targets.end());
    const uint64_t maxAddr = ptrSize == 4 ? 0x100000000ull : 0x7FFFFFFF0000ull;

    constexpr size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk);
    for (const auto& r : p.regions(maxAddr)) {
        if (!isWritable(r.protect)) continue;
        if (r.type != MEM_PRIVATE && r.type != MEM_IMAGE) continue;
        for (size_t off = 0; off < r.size; off += kChunk) {
            const size_t n = std::min(kChunk, r.size - off);
            if (!p.read(r.base + off, buf.data(), n)) {
                std::fill(buf.begin(), buf.begin() + n, uint8_t{0});
                bool any = false;
                for (size_t po = 0; po < n; po += 0x1000)
                    any |= p.read(r.base + off + po, &buf[po], std::min<size_t>(0x1000, n - po));
                if (!any) continue;
            }
            for (size_t i = 0; i + ptrSize <= n; i += ptrSize) {
                uint64_t v;
                if (ptrSize == 4) {
                    uint32_t v32;
                    std::memcpy(&v32, &buf[i], 4);
                    v = v32;
                } else {
                    std::memcpy(&v, &buf[i], 8);
                }
                if (v < lo || v > hi) continue;
                if (targets.count(v)) hits.push_back({r.base + off + i, v});
            }
        }
    }
    return hits;    
}

std::vector<PtrHit> scanPointerRanges(const Process& p, uint32_t ptrSize, const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                                      size_t maxHits) {
    std::vector<PtrHit> hits;
    if (ranges.empty()) return hits;
    const uint64_t lo = ranges.front().first, hi = ranges.back().second;
    const uint64_t maxAddr = ptrSize == 4 ? 0x100000000ull : 0x7FFFFFFF0000ull;
    constexpr size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk);
    for (const auto& r : p.regions(maxAddr)) {
        if (!isWritable(r.protect) || r.type != MEM_PRIVATE) continue;
        for (size_t off = 0; off < r.size; off += kChunk) {
            const size_t n = std::min(kChunk, r.size - off);
            if (!p.read(r.base + off, buf.data(), n)) {
                std::fill(buf.begin(), buf.begin() + n, uint8_t{0});
                bool any = false;
                for (size_t po = 0; po < n; po += 0x1000)
                    any |= p.read(r.base + off + po, &buf[po], std::min<size_t>(0x1000, n - po));
                if (!any) continue;
            }
            for (size_t i = 0; i + ptrSize <= n; i += ptrSize) {
                uint64_t v = 0;
                std::memcpy(&v, &buf[i], ptrSize);
                if (v < lo || v >= hi) continue;
                auto it = std::upper_bound(ranges.begin(), ranges.end(), v,
                                           [](uint64_t x, const std::pair<uint64_t, uint64_t>& rg) { return x < rg.first; });
                if (it == ranges.begin() || v >= std::prev(it)->second) continue;
                hits.push_back({r.base + off + i, v});
                if (hits.size() >= maxHits) return hits;
            }
        }
    }
    return hits;
}

}

std::vector<uint32_t> findCodeXrefs(const Image& img, uint32_t rva, size_t maxHits) {
    std::vector<uint32_t> out;
    const uint64_t va = static_cast<uint64_t>(img.base) + rva;
    for (const auto& sec : img.sections) {
        if (!sec.exec()) continue;
        const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
        for (uint64_t i = sec.rva; i + 4 <= hi && out.size() < maxHits; ++i) {
            uint32_t v;
            std::memcpy(&v, &img.bytes[static_cast<size_t>(i)], 4);
            if (img.ptrSize == 4) {
                if (v == static_cast<uint32_t>(va)) out.push_back(static_cast<uint32_t>(i));
            } else {
                for (uint32_t tail : {0u, 1u, 4u}) {
                    const int64_t target = static_cast<int64_t>(i) + 4 + tail + static_cast<int32_t>(v);
                    if (target == rva) {
                        out.push_back(static_cast<uint32_t>(i));
                        break;
                    }
                }
            }
        }
    }
    return out;
}

namespace {

std::string hexs(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

void scanWorldTables(const Process& p, const Image& img, uint64_t world, std::vector<std::string>& log) {
    constexpr uint32_t kWorldBytes = 0x4000;
    constexpr uint32_t kMaxSlots = 4096;
    constexpr size_t kMaxTables = 48;
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    std::vector<uint8_t> blk(kWorldBytes, 0);
    if (!p.read(static_cast<uintptr_t>(world), blk.data(), blk.size()))
        for (uint32_t o = 0; o < kWorldBytes; o += 0x1000)
            p.read(static_cast<uintptr_t>(world + o), &blk[o], (std::min)(0x1000u, kWorldBytes - o));

    struct Cand { uint32_t off; uint64_t buf; uint32_t count; };
    std::vector<Cand> cands;
    for (uint32_t o = 0; o + 12 <= kWorldBytes; o += 8) {
        uint64_t buf;
        uint32_t count;
        std::memcpy(&buf, &blk[o], 8);
        std::memcpy(&count, &blk[o + 8], 4);
        if (hp(buf) && count != 0 && count <= 20000) cands.push_back({o, buf, count});
    }

    std::vector<uint64_t> probe(cands.size() * 4, 0);
    std::vector<Process::ReadReq> reqs;
    for (size_t i = 0; i < cands.size(); ++i)
        reqs.push_back({static_cast<uintptr_t>(cands[i].buf), &probe[i * 4], (std::min)(cands[i].count, 4u) * 8});
    p.readMany(reqs);
    std::vector<size_t> keep;
    for (size_t i = 0; i < cands.size() && keep.size() < kMaxTables; ++i) {
        const uint32_t n = (std::min)(cands[i].count, 4u);
        uint32_t good = 0;
        for (uint32_t k = 0; k < n; ++k)
            if (hp(probe[i * 4 + k])) ++good;
        if (good > 0 && good * 2 >= n) keep.push_back(i);
    }

    std::vector<std::vector<uint64_t>> arr(keep.size()), vts(keep.size());
    reqs.clear();
    for (size_t j = 0; j < keep.size(); ++j) {
        arr[j].assign((std::min)(cands[keep[j]].count, kMaxSlots), 0);
        reqs.push_back({static_cast<uintptr_t>(cands[keep[j]].buf), arr[j].data(), arr[j].size() * 8});
    }
    p.readMany(reqs);
    reqs.clear();
    for (size_t j = 0; j < keep.size(); ++j) {
        vts[j].assign(arr[j].size(), 0);
        for (size_t k = 0; k < arr[j].size(); ++k)
            if (hp(arr[j][k])) reqs.push_back({static_cast<uintptr_t>(arr[j][k]), &vts[j][k], 8});
    }
    p.readMany(reqs);

    log.push_back("world object table scan (lists at World + off, and the classes they hold):");
    size_t bestJ = SIZE_MAX;
    uint32_t bestCount = 0;
    for (size_t j = 0; j < keep.size(); ++j) {
        std::map<std::string, int> tally;
        size_t named = 0;
        for (size_t k = 0; k < arr[j].size(); ++k) {
            if (!hp(arr[j][k])) continue;
            std::string n = classNameFromVtable(img, vts[j][k]);
            if (!n.empty()) ++named;
            tally[n.empty() ? "<no RTTI>" : n]++;
        }
        if (named == 0) continue;
        std::vector<std::pair<std::string, int>> rows(tally.begin(), tally.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string line = "  +" + hexs(cands[keep[j]].off) + "  count " + std::to_string(cands[keep[j]].count) + ":";
        for (size_t s = 0; s < rows.size() && s < 6; ++s) line += "  " + std::to_string(rows[s].second) + "x " + rows[s].first;
        log.push_back(line);
        if (cands[keep[j]].count > bestCount) { bestCount = cands[keep[j]].count; bestJ = j; }
    }

    if (bestJ != SIZE_MAX) {
        constexpr uint32_t kVisualState = 0x158, kVsPos = 0x2C;
        const auto& slots = arr[bestJ];
        std::vector<size_t> sample;
        std::map<uint64_t, int> noRttiVts;
        for (size_t k = 0; k < slots.size(); ++k) {
            if (!hp(slots[k])) continue;
            if (classNameFromVtable(img, vts[bestJ][k]).empty()) noRttiVts[vts[bestJ][k]]++;
            if (sample.size() < 16) sample.push_back(k);
        }
        std::vector<uint64_t> vsPtr(sample.size(), 0);
        reqs.clear();
        for (size_t s = 0; s < sample.size(); ++s)
            reqs.push_back({static_cast<uintptr_t>(slots[sample[s]] + kVisualState), &vsPtr[s], 8});
        p.readMany(reqs);
        std::vector<float> pos(sample.size() * 3, 0);
        reqs.clear();
        std::vector<size_t> posOf;
        for (size_t s = 0; s < sample.size(); ++s)
            if (hp(vsPtr[s])) { reqs.push_back({static_cast<uintptr_t>(vsPtr[s] + kVsPos), &pos[s * 3], 12}); posOf.push_back(s); }
        p.readMany(reqs);
        log.push_back("  sample of +" + hexs(cands[keep[bestJ]].off) + " (slot + 0x158 -> +0x2C), to confirm loot positions:");
        for (size_t s = 0; s < sample.size(); ++s) {
            std::string nm = classNameFromVtable(img, vts[bestJ][sample[s]]);
            char b[128];
            if (hp(vsPtr[s]))
                std::snprintf(b, sizeof b, "    %-20s VisualState %s  pos %.0f, %.0f, %.0f",
                              (nm.empty() ? "<no RTTI>" : nm).c_str(), hexs(vsPtr[s]).c_str(),
                              pos[s * 3], pos[s * 3 + 1], pos[s * 3 + 2]);
            else
                std::snprintf(b, sizeof b, "    %-20s no VisualState at +0x158 (%s)",
                              (nm.empty() ? "<no RTTI>" : nm).c_str(), hexs(vsPtr[s]).c_str());
            log.push_back(b);
        }
        std::vector<std::pair<uint64_t, int>> vr(noRttiVts.begin(), noRttiVts.end());
        std::sort(vr.begin(), vr.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string vl = "  distinct <no RTTI> vtables in +" + hexs(cands[keep[bestJ]].off) + " (VA xN): ";
        for (size_t s = 0; s < vr.size() && s < 8; ++s) vl += hexs(vr[s].first) + " x" + std::to_string(vr[s].second) + "  ";
        log.push_back(vl + (vr.size() > 8 ? "..." : ""));
    }
}

bool fromWorld(const Process& p, const Image& img, const DiscoverConfig& cfg, const std::string& cls,
               const std::unordered_map<uint64_t, size_t>& vtIndex, Discovery& out, std::string& why) {
    if (img.ptrSize != 8) {
        why = "world chain: 64-bit images only";
        return false;
    }
    auto heapPtr = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };
    auto vtOf = [&](uint64_t obj, size_t& idx) {
        uint64_t vt = 0;
        if (!p.readPtr(static_cast<uintptr_t>(obj), 8, vt)) return false;
        const auto it = vtIndex.find(vt);
        if (it == vtIndex.end()) return false;
        idx = it->second;
        return true;
    };

    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !heapPtr(world)) {
        why = "world chain: [base+" + hexs(cfg.worldRva) + "] = " + hexs(world) + ", not a pointer (other game build, or still loading)";
        return false;
    }
    const std::string worldCls = classNameAt(p, img, world);
    out.log.push_back("world chain: [base+" + hexs(cfg.worldRva) + "] = " + hexs(world) + (worldCls.empty() ? " (no RTTI)" : " (" + worldCls + ")"));

    std::vector<std::pair<uint64_t, size_t>> found;
    std::unordered_set<uint64_t> seen;
    uint64_t local = 0;
    if (cfg.localLink) {
        uint64_t link = 0, inner = 0;
        size_t idx = 0;
        if (p.readPtr(static_cast<uintptr_t>(world + cfg.localLink), 8, link) && heapPtr(link) &&
            p.readPtr(static_cast<uintptr_t>(link + cfg.localDeref), 8, inner) && heapPtr(inner) && vtOf(inner - cfg.localAdj, idx)) {
            local = inner - cfg.localAdj;
            found.push_back({local, idx});
            seen.insert(local);
            out.log.push_back("  LocalPlayer (+" + hexs(cfg.localLink) + "): " + hexs(local) + " (" + out.vtables[idx].cls + ")");
        } else {
            out.log.push_back("  LocalPlayer (+" + hexs(cfg.localLink) + "): no player behind it (dead/respawning, or offset stale)");
        }
    }

    struct TableRes {
        const DiscoverConfig::Table* t;
        int32_t count;
        size_t matched;
    };
    std::vector<TableRes> tables;
    std::map<std::string, int> otherCls;
    for (const auto& t : cfg.tables) {
        uint64_t buf = 0;
        int32_t count = 0;
        const std::string tag = "  " + t.name + " (+" + hexs(t.off) + "): ";
        if (!p.readPtr(static_cast<uintptr_t>(world + t.off), 8, buf) || !p.read(static_cast<uintptr_t>(world + t.off + 8), count)) {
            out.log.push_back(tag + "unreadable");
            continue;
        }
        if (count < 0 || count > 20000 || (count > 0 && !heapPtr(buf))) {
            out.log.push_back(tag + "buf " + hexs(buf) + ", count " + std::to_string(count) + " - not a table");
            continue;
        }
        std::vector<uint64_t> slots(static_cast<size_t>(count), 0);
        if (count && !p.read(static_cast<uintptr_t>(buf), slots.data(), slots.size() * 8)) {
            out.log.push_back(tag + "count " + std::to_string(count) + " but the array at " + hexs(buf) + " is unreadable");
            continue;
        }
        std::vector<uint64_t> vts(slots.size(), 0);
        std::vector<Process::ReadReq> reqs;
        for (size_t i = 0; i < slots.size(); ++i)
            if (heapPtr(slots[i])) reqs.push_back({static_cast<uintptr_t>(slots[i]), &vts[i], 8});
        p.readMany(reqs);
        size_t nonNull = 0, matched = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i]) continue;
            ++nonNull;
            const auto it = vtIndex.find(vts[i]);
            if (it == vtIndex.end()) {
                std::string n = classNameFromVtable(img, vts[i]);
                otherCls[n.empty() ? "<no RTTI>" : n]++;
                continue;
            }
            ++matched;
            if (seen.insert(slots[i]).second) found.push_back({slots[i], it->second});
        }
        out.log.push_back(tag + "count " + std::to_string(count) + ", " + std::to_string(nonNull) + " set, " + std::to_string(matched) +
                          " " + cls + "/derived (the rest are animals, vehicles, ...)");
        tables.push_back({&t, count, matched});
    }
    if (!otherCls.empty()) {
        std::vector<std::pair<std::string, int>> rows(otherCls.begin(), otherCls.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        out.log.push_back("  other entity classes in the tables (not " + cls + "):");
        for (const auto& r : rows) out.log.push_back("    " + std::to_string(r.second) + "x " + r.first);
    }
    scanWorldTables(p, img, world, out.log);
    if (found.empty()) {
        why = "world chain: no " + cls + " objects in the tables or behind LocalPlayer (not spawned yet, or offsets stale)";
        return false;
    }

    std::stable_sort(found.begin() + (local ? 1 : 0), found.end(),
                     [&](const auto& a, const auto& b) { return out.vtables[a.second].group < out.vtables[b.second].group; });
    for (const auto& f : found) {
        if (cfg.maxEntities && out.entities.size() >= cfg.maxEntities) break;
        Entity e;
        e.addr = f.first;
        e.cls = out.vtables[f.second].cls;
        e.local = f.first == local;
        out.entities.push_back(e);
    }

    for (const auto& vt : out.vtables) {
        Finding f;
        f.kind = Finding::VTable;
        f.name = vt.cls + "_vtable";
        f.value = vt.rva;
        f.note = vt.exact ? "exact class" : "derives from " + cls;
        out.findings.push_back(f);
    }
    Finding w;
    w.name = "World";
    w.value = cfg.worldRva;
    w.note = "pointer to the world object" + (worldCls.empty() ? std::string() : " (" + worldCls + ")") + "; " +
             std::to_string(found.size()) + " " + cls + "/derived reached through it";
    w.xrefs = findCodeXrefs(img, cfg.worldRva);
    out.findings.push_back(w);
    for (const auto& t : tables) {
        Finding f;
        f.kind = Finding::Field;
        f.name = "World." + t.t->name;
        f.value = t.t->off;
        f.type = "ptr";
        f.note = "Entity*[], int count at +" + hexs(t.t->off + 8) + "; " + std::to_string(t.count) + " entries, " +
                 std::to_string(t.matched) + " " + cls + "/derived";
        out.findings.push_back(f);
    }
    if (local) {
        Finding f;
        f.kind = Finding::Field;
        f.name = "World.LocalPlayer";
        f.value = cfg.localLink;
        f.type = "ptr";
        f.note = "weak ptr: entity = *(*(World + " + hexs(cfg.localLink) + ") + " + hexs(cfg.localDeref) + ") - " + hexs(cfg.localAdj);
        out.findings.push_back(f);
    }
    return true;
}

}

void findLootList(const Process& p, const Image& img, const DiscoverConfig& cfg, int waitSec,
                  const std::function<void(const std::string&)>& log, const std::function<void(int)>& wait) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] loot finder: 64-bit images only"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !hp(world)) {
        say("[*] loot finder: world pointer at base+" + hexs(cfg.worldRva) + " not resolved (in game?)");
        return;
    }

    constexpr uint32_t kWorldBytes = 0x8000;
    std::vector<uint8_t> blk(kWorldBytes, 0);
    for (uint32_t o = 0; o < kWorldBytes; o += 0x1000)
        p.read(static_cast<uintptr_t>(world + o), &blk[o], (std::min)(0x1000u, kWorldBytes - o));

    struct Cand { uint32_t off; uint64_t buf; int32_t count; };
    std::vector<Cand> cands;
    for (uint32_t o = 0; o + 12 <= kWorldBytes; o += 8) {
        uint64_t buf;
        int32_t count;
        std::memcpy(&buf, &blk[o], 8);
        std::memcpy(&count, &blk[o + 8], 4);
        if (hp(buf) && count > 0 && count <= 200000) cands.push_back({o, buf, count});
    }

    std::vector<uint64_t> probe(cands.size() * 4, 0);
    std::vector<Process::ReadReq> reqs;
    for (size_t i = 0; i < cands.size(); ++i)
        reqs.push_back({static_cast<uintptr_t>(cands[i].buf), &probe[i * 4], (std::min)(cands[i].count, 4) * 8u});
    p.readMany(reqs);
    std::vector<Cand> keep;
    for (size_t i = 0; i < cands.size(); ++i) {
        const int32_t n = (std::min)(cands[i].count, 4);
        int32_t good = 0;
        for (int32_t k = 0; k < n; ++k)
            if (hp(probe[i * 4 + k])) ++good;
        if (good > 0 && good * 2 >= n) keep.push_back(cands[i]);
    }

    say("[*] loot finder: watching " + std::to_string(keep.size()) + " candidate tables in the World object.");
    say("[*] loot finder: DROP A KNOWN NUMBER OF ITEMS NOW (e.g. a stack of 10) - re-reading counts in " +
        std::to_string(waitSec) + " s ...");
    wait(waitSec * 1000);

    std::vector<uint64_t> hdr(keep.size() * 2, 0);
    reqs.clear();
    for (size_t i = 0; i < keep.size(); ++i)
        reqs.push_back({static_cast<uintptr_t>(world + keep[i].off), &hdr[i * 2], 16});
    p.readMany(reqs);

    struct Delta { uint32_t off; int32_t before; int32_t after; };
    std::vector<Delta> deltas;
    for (size_t i = 0; i < keep.size(); ++i) {
        const int32_t after = static_cast<int32_t>(hdr[i * 2 + 1] & 0xFFFFFFFF);
        if (after != keep[i].count) deltas.push_back({keep[i].off, keep[i].count, after});
    }
    std::sort(deltas.begin(), deltas.end(),
              [](const Delta& a, const Delta& b) { return (b.after - b.before) < (a.after - a.before); });

    if (deltas.empty()) {
        say("[*] loot finder: no table changed. Drop items on the ground within view and try again (drop more).");
        return;
    }
    say("[*] loot finder: tables whose count changed (the delta matching how many you dropped = the loot list):");
    for (const auto& d : deltas) {
        char b[96];
        std::snprintf(b, sizeof b, "    World +%s  %d -> %d  (delta %+d)", hexs(d.off).c_str(), d.before, d.after,
                      d.after - d.before);
        say(b);
    }
}

void findItemNames(const Process& p, const Image& img, const DiscoverConfig& cfg, uint32_t listOff, int n,
                   const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] item names: 64-bit images only"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };
    auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7F; };
    auto identLike = [](const std::string& s) {
        if (s.size() < 3 || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
        for (char c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        return true;
    };
    auto findStrings = [&](const uint8_t* b, size_t nb) {
        std::vector<std::pair<uint32_t, std::string>> out;
        for (size_t o = 0; o < nb;) {
            if (printable(b[o]) && (o == 0 || !printable(b[o - 1]))) {
                size_t e = o;
                while (e < nb && printable(b[e])) ++e;
                if (e - o >= 3 && e - o < 48 && e < nb && b[e] == 0) {
                    std::string s(reinterpret_cast<const char*>(b + o), e - o);
                    if (identLike(s)) out.push_back({static_cast<uint32_t>(o), s});
                }
                o = e + 1;
            } else {
                ++o;
            }
            if (out.size() >= 24) break;
        }
        return out;
    };

    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !hp(world)) {
        say("[*] item names: world pointer not resolved (in game?)");
        return;
    }
    uint64_t hdr[2] = {};
    { std::vector<Process::ReadReq> r = {{static_cast<uintptr_t>(world + listOff), hdr, 16}}; p.readMany(r); }
    const uint64_t buf = hdr[0];
    const int32_t cnt = static_cast<int32_t>(hdr[1] & 0xFFFFFFFF);
    if (!hp(buf) || cnt <= 0) { say("[*] item names: item list at World+" + hexs(listOff) + " is empty"); return; }
    const int take = (std::min)(cnt, n > 0 ? n : 12);
    std::vector<uint64_t> slots(take, 0);
    { std::vector<Process::ReadReq> r = {{static_cast<uintptr_t>(buf), slots.data(), size_t(take) * 8}}; p.readMany(r); }

    say("[*] item names: scanning " + std::to_string(take) + " items at World+" + hexs(listOff) +
        " for identifier strings (inline, 1 and 2 pointers deep) ...");
    constexpr size_t kEnt = 0x600, kChild = 0x100;
    constexpr size_t kMaxL1 = 48;
    std::map<std::string, int> tally;
    std::map<std::string, std::set<std::string>> vals;
    std::map<std::string, std::string> example;

    for (int i = 0; i < take; ++i) {
        if (!hp(slots[i])) continue;
        std::vector<uint8_t> eb(kEnt, 0);
        { std::vector<Process::ReadReq> r = {{static_cast<uintptr_t>(slots[i]), eb.data(), kEnt}}; p.readMany(r); }
        std::set<std::string> found;
        std::string line = "  item " + std::to_string(i) + " @ " + hexs(slots[i]) + ": ";
        int shown = 0;
        auto record = [&](const std::string& path, const std::string& s) {
            if (!found.insert(path).second) return;
            tally[path]++;
            vals[path].insert(s);
            if (!example.count(path)) example[path] = s;
            if (shown < 8) { line += path + "=\"" + s + "\"  "; ++shown; }
        };

        for (const auto& fs : findStrings(eb.data(), kEnt)) record("inline+" + hexs(fs.first), fs.second);

        std::vector<std::pair<uint32_t, uint64_t>> l1;
        for (uint32_t o = 0; o + 8 <= kEnt; o += 8) {
            uint64_t v;
            std::memcpy(&v, &eb[o], 8);
            if (hp(v)) l1.push_back({o, v});
        }
        std::vector<std::vector<uint8_t>> cb(l1.size(), std::vector<uint8_t>(kChild, 0));
        std::vector<Process::ReadReq> r1;
        for (size_t k = 0; k < l1.size(); ++k) r1.push_back({static_cast<uintptr_t>(l1[k].second), cb[k].data(), kChild});
        p.readMany(r1);
        for (size_t k = 0; k < l1.size(); ++k) {
            if (!r1[k].ok) continue;
            for (const auto& fs : findStrings(cb[k].data(), kChild)) {
                const std::string path = fs.first == 0 ? "+" + hexs(l1[k].first)
                                                       : "+" + hexs(l1[k].first) + "@" + hexs(fs.first);
                record(path, fs.second);
            }
        }

        std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> l2;
        for (size_t k = 0; k < l1.size() && k < kMaxL1; ++k) {
            if (!r1[k].ok) continue;
            for (uint32_t o = 0; o + 8 <= kChild; o += 8) {
                uint64_t v;
                std::memcpy(&v, &cb[k][o], 8);
                if (hp(v)) l2.push_back({l1[k].first, o, v});
            }
        }
        std::vector<std::vector<uint8_t>> cb2(l2.size(), std::vector<uint8_t>(kChild, 0));
        std::vector<Process::ReadReq> r2;
        for (size_t k = 0; k < l2.size(); ++k) r2.push_back({static_cast<uintptr_t>(std::get<2>(l2[k])), cb2[k].data(), kChild});
        p.readMany(r2);
        for (size_t k = 0; k < l2.size(); ++k) {
            if (!r2[k].ok) continue;
            for (const auto& fs : findStrings(cb2[k].data(), kChild)) {
                std::string path = "+" + hexs(std::get<0>(l2[k])) + "->+" + hexs(std::get<1>(l2[k]));
                if (fs.first) path += "@" + hexs(fs.first);
                record(path, fs.second);
            }
        }
        say(line);
    }

    say("[*] item names: offset paths ranked by DISTINCT values (the item TYPE is the per-item-distinct one):");
    std::vector<std::string> order;
    for (const auto& kv : tally) order.push_back(kv.first);
    auto withUnderscore = [&](const std::string& p) {
        size_t n = 0;
        for (const auto& v : vals[p])
            if (v.find('_') != std::string::npos) ++n;
        return n;
    };
    std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        if (vals[a].size() != vals[b].size()) return vals[a].size() > vals[b].size();
        if (tally[a] != tally[b]) return tally[a] > tally[b];
        return withUnderscore(a) > withUnderscore(b);
    });
    for (size_t j = 0; j < order.size() && j < 30; ++j) {
        const std::string& path = order[j];
        std::string ex;
        size_t k = 0;
        for (const auto& v : vals[path]) {
            if (k++) ex += " ";
            ex += "\"" + v + "\"";
            if (k == 4) { ex += " ..."; break; }
        }
        char b[256];
        std::snprintf(b, sizeof b, "    %-26s  %zu distinct in %d/%d items  e.g. %s",
                      path.c_str(), vals[path].size(), tally[path], take, ex.c_str());
        say(b);
    }
}


void findPlayerNames(const Process& p, const Image& img, const DiscoverConfig& cfg, int n,
                     const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] player names: 64-bit images only"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    std::vector<VTableInfo> vts;
    std::string err;
    if (!findVTables(img, "DayZPlayer", vts, err)) {
        say("[*] player names: no DayZPlayer RTTI (" + err + ")");
        return;
    }
    std::unordered_map<uint64_t, bool> playerVt;
    for (const auto& v : vts)
        playerVt[static_cast<uint64_t>(img.base) + v.rva] = v.cls.find("Infected") == std::string::npos;
    if (playerVt.empty()) { say("[*] player names: no DayZPlayer vtables"); return; }

    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !hp(world)) {
        say("[*] player names: world pointer not resolved (in game?)");
        return;
    }

    std::vector<uint64_t> cands;
    for (const auto& t : cfg.tables) {
        uint64_t hdr[2] = {};
        std::vector<Process::ReadReq> reqs = {{static_cast<uintptr_t>(world + t.off), hdr, 16}};
        p.readMany(reqs);
        const uint64_t buf = hdr[0];
        const int32_t cnt = static_cast<int32_t>(hdr[1] & 0xFFFFFFFF);
        if (!reqs[0].ok || !hp(buf) || cnt <= 0 || cnt > 4096) continue;
        std::vector<uint64_t> more(static_cast<size_t>(cnt), 0);
        reqs = {{static_cast<uintptr_t>(buf), more.data(), more.size() * 8}};
        p.readMany(reqs);
        if (!reqs[0].ok) continue;
        for (uint64_t s : more)
            if (hp(s)) cands.push_back(s);
    }
    std::vector<uint64_t> vt(cands.size(), 0);
    std::vector<Process::ReadReq> reqs;
    for (size_t i = 0; i < cands.size(); ++i) reqs.push_back({static_cast<uintptr_t>(cands[i]), &vt[i], 8});
    p.readMany(reqs);
    std::vector<uint64_t> players;
    std::set<uint64_t> seen;
    for (size_t i = 0; i < cands.size(); ++i) {
        const auto it = playerVt.find(vt[i]);
        if (it == playerVt.end() || !it->second) continue;
        if (seen.insert(cands[i]).second) players.push_back(cands[i]);
    }
    const size_t take = std::min(players.size(), n > 0 ? static_cast<size_t>(n) : size_t(12));
    if (!take) { say("[*] player names: no survivor entities in the world tables yet"); return; }
    say("[*] player names: scanning " + std::to_string(take) + " survivor(s) for name strings (inline, 1-3 pointers deep) ...");

    auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7F; };
    auto nameLike = [](const std::string& s) {
        if (s.size() < 2 || s.size() > 40) return false;
        int letters = 0;
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++letters;
            else if (c == ' ' || c == '\'' || c == '-' || c == '.' || c == '_' || (c >= '0' && c <= '9')) continue;
            else return false;
        }
        return letters >= 2;
    };
    auto findStrings = [&](const uint8_t* b, size_t nb) {
        std::vector<std::pair<uint32_t, std::string>> out;
        for (size_t o = 0; o < nb;) {
            if (printable(b[o]) && (o == 0 || !printable(b[o - 1]))) {
                size_t e = o;
                while (e < nb && printable(b[e])) ++e;
                if (e - o >= 2 && e - o <= 40 && e < nb && b[e] == 0) {
                    std::string s(reinterpret_cast<const char*>(b + o), e - o);
                    if (nameLike(s)) out.push_back({static_cast<uint32_t>(o), s});
                }
                o = e + 1;
            } else {
                ++o;
            }
        }
        return out;
    };

    constexpr size_t kEnt = 0x2000, kChild = 0x100;
    constexpr size_t kMaxL1 = 128, kMaxL2 = 384, kMaxL3 = 384;
    std::map<std::string, int> tally;
    std::map<std::string, std::set<std::string>> vals;
    std::map<std::string, std::string> example;

    for (size_t i = 0; i < take; ++i) {
        std::vector<uint8_t> eb(kEnt, 0);
        { std::vector<Process::ReadReq> r = {{static_cast<uintptr_t>(players[i]), eb.data(), kEnt}}; p.readMany(r); }
        std::set<std::string> found;
        std::string line = "  player " + std::to_string(i) + " @ " + hexs(players[i]) + ": ";
        int shown = 0;
        auto record = [&](const std::string& path, const std::string& s) {
            if (!found.insert(path).second) return;
            tally[path]++;
            vals[path].insert(s);
            if (!example.count(path)) example[path] = s;
            if (shown < 8) { line += path + "=\"" + s + "\" "; ++shown; }
        };

        for (const auto& fs : findStrings(eb.data(), kEnt)) record("inline+" + hexs(fs.first), fs.second);

        std::vector<std::pair<uint32_t, uint64_t>> l1;
        for (uint32_t o = 0; o + 8 <= kEnt; o += 8) {
            uint64_t v;
            std::memcpy(&v, &eb[o], 8);
            if (hp(v)) l1.push_back({o, v});
        }
        const size_t nL1 = std::min(l1.size(), kMaxL1);
        std::vector<std::vector<uint8_t>> cb(nL1, std::vector<uint8_t>(kChild, 0));
        std::vector<Process::ReadReq> r1;
        for (size_t k = 0; k < nL1; ++k) r1.push_back({static_cast<uintptr_t>(l1[k].second), cb[k].data(), kChild});
        p.readMany(r1);
        for (size_t k = 0; k < nL1; ++k) {
            if (!r1[k].ok) continue;
            for (const auto& fs : findStrings(cb[k].data(), kChild)) {
                const std::string path = fs.first == 0 ? "+" + hexs(l1[k].first)
                                                       : "+" + hexs(l1[k].first) + "@" + hexs(fs.first);
                record(path, fs.second);
            }
        }

        std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> l2;
        for (size_t k = 0; k < nL1; ++k) {
            if (!r1[k].ok) continue;
            for (uint32_t o = 0; o + 8 <= kChild; o += 8) {
                if (l2.size() >= kMaxL2) break;
                uint64_t v;
                std::memcpy(&v, &cb[k][o], 8);
                if (hp(v)) l2.push_back({l1[k].first, o, v});
            }
            if (l2.size() >= kMaxL2) break;
        }
        std::vector<std::vector<uint8_t>> cb2(l2.size(), std::vector<uint8_t>(kChild, 0));
        std::vector<Process::ReadReq> r2;
        for (size_t k = 0; k < l2.size(); ++k) r2.push_back({static_cast<uintptr_t>(std::get<2>(l2[k])), cb2[k].data(), kChild});
        p.readMany(r2);
        for (size_t k = 0; k < l2.size(); ++k) {
            if (!r2[k].ok) continue;
            for (const auto& fs : findStrings(cb2[k].data(), kChild)) {
                std::string path = "+" + hexs(std::get<0>(l2[k])) + "->+" + hexs(std::get<1>(l2[k]));
                if (fs.first) path += "@" + hexs(fs.first);
                record(path, fs.second);
            }
        }

        std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>> l3;
        for (size_t k = 0; k < l2.size(); ++k) {
            if (!r2[k].ok) continue;
            for (uint32_t o = 0; o + 8 <= kChild; o += 8) {
                if (l3.size() >= kMaxL3) break;
                uint64_t v;
                std::memcpy(&v, &cb2[k][o], 8);
                if (hp(v)) l3.push_back({std::get<0>(l2[k]), std::get<1>(l2[k]), o, v});
            }
            if (l3.size() >= kMaxL3) break;
        }
        std::vector<std::vector<uint8_t>> cb3(l3.size(), std::vector<uint8_t>(kChild, 0));
        std::vector<Process::ReadReq> r3;
        for (size_t k = 0; k < l3.size(); ++k) r3.push_back({static_cast<uintptr_t>(std::get<3>(l3[k])), cb3[k].data(), kChild});
        p.readMany(r3);
        for (size_t k = 0; k < l3.size(); ++k) {
            if (!r3[k].ok) continue;
            for (const auto& fs : findStrings(cb3[k].data(), kChild)) {
                std::string path = "+" + hexs(std::get<0>(l3[k])) + "->+" + hexs(std::get<1>(l3[k])) + "->+" + hexs(std::get<2>(l3[k]));
                if (fs.first) path += "@" + hexs(fs.first);
                record(path, fs.second);
            }
        }
        say(line);
    }

    say("[*] player names: offset paths ranked by DISTINCT values (look for YOUR OWN name - the local player is entity 0):");
    std::vector<std::string> order;
    for (const auto& kv : tally) order.push_back(kv.first);
    std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        if (vals[a].size() != vals[b].size()) return vals[a].size() > vals[b].size();
        if (tally[a] != tally[b]) return tally[a] > tally[b];
        return example[a] < example[b];
    });
    for (size_t j = 0; j < order.size() && j < 40; ++j) {
        const std::string& pa = order[j];
        std::string ex;
        size_t k = 0;
        for (const auto& v : vals[pa]) {
            if (k++) ex += " ";
            ex += "\"" + v + "\"";
            if (k == 4) { ex += " ..."; break; }
        }
        char b[256];
        std::snprintf(b, sizeof b, "    %-32s  %zu distinct in %zu/%zu players  e.g. %s",
                      pa.c_str(), vals[pa].size(), size_t(tally[pa]), take, ex.c_str());
        say(b);
    }
}

static void traceNameChains(const Process& p, uint64_t world, const std::vector<uint64_t>& ents,
                            const std::unordered_set<uint64_t>& textAddrs,
                            const std::unordered_set<uint64_t>& targets,
                            const std::function<void(const std::string&)>& say) {
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };
    struct Node { uint64_t addr; int parent; uint32_t off; int depth; bool rootWorld; uint64_t root; };
    std::vector<Node> nodes;
    std::unordered_set<uint64_t> visited;
    for (uint64_t e : ents)
        if (hp(e) && visited.insert(e).second) nodes.push_back({e, -1, 0, 0, false, e});
    if (hp(world) && visited.insert(world).second) nodes.push_back({world, -1, 0, 0, true, world});
    if (nodes.empty()) { say("[*] find name: no entities/World to trace from (in game?)."); return; }

    constexpr int kMaxDepth = 6;
    constexpr size_t kBudget = 600000;
    constexpr uint32_t kWin = 0x400;
    constexpr uint32_t kRootWin = 0x2800;
    constexpr size_t kBatch = 256;

    auto report = [&](int idx, uint32_t finalOff, uint64_t v) {
        std::vector<uint32_t> offs;
        int i = idx;
        while (nodes[i].parent != -1) { offs.push_back(nodes[i].off); i = nodes[i].parent; }
        std::string s = nodes[i].rootWorld ? "World" : ("entity " + hexs(nodes[i].root));
        for (auto it = offs.rbegin(); it != offs.rend(); ++it) s += "->+" + hexs(*it);
        s += "@" + hexs(finalOff);
        if (!textAddrs.count(v)) s += "  (RVString header; name text at +0x10)";
        say("    " + s);
    };

    say("[*] find name: tracing a chain from entities/World (BFS, up to " + std::to_string(kMaxDepth) + " hops) ...");
    size_t head = 0;
    int found = 0;
    while (head < nodes.size() && found < 8 && nodes.size() < kBudget) {
        const size_t end = std::min(nodes.size(), head + kBatch);
        std::vector<std::vector<uint8_t>> bufs;
        std::vector<size_t> idxs;
        bufs.reserve(end - head);
        for (size_t i = head; i < end; ++i) {
            bufs.emplace_back(static_cast<size_t>(nodes[i].depth == 0 ? kRootWin : kWin), uint8_t{0});
            idxs.push_back(i);
        }
        std::vector<Process::ReadReq> reqs;
        reqs.reserve(idxs.size());
        for (size_t b = 0; b < idxs.size(); ++b)
            reqs.push_back({static_cast<uintptr_t>(nodes[idxs[b]].addr), bufs[b].data(), bufs[b].size()});
        p.readMany(reqs);
        for (size_t b = 0; b < idxs.size() && found < 8; ++b) {
            if (!reqs[b].ok) continue;
            const size_t i = idxs[b];
            const int di = nodes[i].depth;
            const bool rw = nodes[i].rootWorld;
            const uint64_t rt = nodes[i].root;
            const std::vector<uint8_t>& buf = bufs[b];
            for (uint32_t o = 0; o + 8 <= buf.size(); o += 8) {
                uint64_t v;
                std::memcpy(&v, &buf[o], 8);
                if (targets.count(v)) {
                    report(static_cast<int>(i), o, v);
                    if (++found >= 8) break;
                } else if (hp(v) && di < kMaxDepth && nodes.size() < kBudget && visited.insert(v).second) {
                    nodes.push_back({v, static_cast<int>(i), o, di + 1, rw, rt});
                }
            }
        }
        head = end;
    }
    if (!found)
        say("[*] find name: no chain within " + std::to_string(kMaxDepth) + " hops (" + std::to_string(nodes.size()) +
            " objects walked) reached the string - it likely hangs off a global the walk didn't seed.");
}

void findMyName(const Process& p, const Image& img, const DiscoverConfig& cfg, const std::string& name,
                const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (name.size() < 2) { say("[*] find name: give a name to search for (--findname \"name\")"); return; }
    if (img.ptrSize != 8) { say("[*] find name: 64-bit images only"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    const std::string needle = name + '\0';
    const uint64_t maxAddr = 0x7FFFFFFF0000ull;
    std::vector<uint64_t> found;
    constexpr size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk);
    for (const auto& r : p.regions(maxAddr)) {
        if (!isWritable(r.protect) || r.type != MEM_PRIVATE) continue;
        for (size_t off = 0; off < r.size && found.size() < 64; off += kChunk) {
            const size_t n = std::min(kChunk, r.size - off);
            if (!p.read(r.base + off, buf.data(), n)) {
                std::fill(buf.begin(), buf.begin() + n, uint8_t{0});
                for (size_t po = 0; po < n; po += 0x1000)
                    p.read(r.base + off + po, &buf[po], std::min<size_t>(0x1000, n - po));
            }
            for (size_t at = 0; at + needle.size() <= n;) {
                const auto it = std::search(buf.begin() + at, buf.begin() + n, needle.begin(), needle.end());
                if (it == buf.begin() + n) break;
                at = size_t(it - buf.begin()) + 1;
                found.push_back(r.base + off + size_t(it - buf.begin()));
            }
        }
    }
    if (found.empty()) {
        say("[*] find name: \"" + name + "\" was not found ANYWHERE in the client's writable memory.");
        say("[*] find name: conclusion - that name is not replicated to the client; it cannot be read.");
        return;
    }
    say("[*] find name: \"" + name + "\" found " + std::to_string(found.size()) + "x in memory (first: " + hexs(found[0]) + ").");

    std::unordered_set<uint64_t> targets;
    for (uint64_t a : found) {
        targets.insert(a);
        if (a >= 0x10) targets.insert(a - 0x10);
    }
    const auto hits = scanPointers(p, img.ptrSize, targets);
    if (hits.empty()) {
        say("[*] find name: no pointer in memory targets the string - orphaned data, nothing to follow.");
        return;
    }

    uint64_t world = 0;
    p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world);
    std::vector<uint64_t> ents;
    if (hp(world))
        for (const auto& t : cfg.tables) {
            uint64_t hdr[2] = {};
            std::vector<Process::ReadReq> reqs = {{static_cast<uintptr_t>(world + t.off), hdr, 16}};
            p.readMany(reqs);
            const uint64_t tbuf = hdr[0];
            const int32_t cnt = static_cast<int32_t>(hdr[1] & 0xFFFFFFFF);
            if (!reqs[0].ok || !hp(tbuf) || cnt <= 0 || cnt > 4096) continue;
            std::vector<uint64_t> slots(static_cast<size_t>(cnt), 0);
            reqs = {{static_cast<uintptr_t>(tbuf), slots.data(), slots.size() * 8}};
            p.readMany(reqs);
            if (reqs[0].ok)
                for (uint64_t s : slots)
                    if (hp(s)) ents.push_back(s);
        }

    say("[*] find name: pointers to the string, classified by where the pointer slot lives:");
    int shown = 0;
    bool anchored = false;
    for (const auto& h : hits) {
        if (shown++ >= 48) { say("    ... more"); break; }
        char b[176];
        if (h.addr >= img.base && h.addr < img.base + img.bytes.size()) {
            std::snprintf(b, sizeof b, "    slot %s = %s  MODULE global (RVA %s)",
                          hexs(h.addr).c_str(), hexs(h.value).c_str(), hexs(h.addr - img.base).c_str());
            anchored = true;
        } else if (hp(world) && h.addr >= world && h.addr < world + 0x8000) {
            std::snprintf(b, sizeof b, "    slot %s = %s  World + %s",
                          hexs(h.addr).c_str(), hexs(h.value).c_str(), hexs(h.addr - world).c_str());
            anchored = true;
        } else {
            const auto it = std::find_if(ents.begin(), ents.end(),
                                         [&](uint64_t e) { return h.addr >= e && h.addr < e + 0x2000; });
            if (it != ents.end()) {
                std::snprintf(b, sizeof b, "    slot %s = %s  entity %s + %s",
                              hexs(h.addr).c_str(), hexs(h.value).c_str(), hexs(*it).c_str(), hexs(h.addr - *it).c_str());
                anchored = true;
            } else {
                std::snprintf(b, sizeof b, "    slot %s = %s  heap (reachable through some chain)",
                              hexs(h.addr).c_str(), hexs(h.value).c_str());
            }
        }
        say(b);
    }
    say("[*] find name: MODULE-global and World slots are the interesting ones - a stable chain from");
    say("[*] find name: either can be wired into the ESP. Entity + off means the name IS on the player struct.");

    if (!anchored) {
        const std::unordered_set<uint64_t> textAddrs(found.begin(), found.end());
        traceNameChains(p, world, ents, textAddrs, targets, say);
    }
}

void findScoreboard(const Process& p, const Image& img, const DiscoverConfig& cfg, const std::string& myName,
                    const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] scoreboard: 64-bit images only"); return; }
    if (myName.size() < 2) { say("[*] scoreboard: also pass --findname \"<your name>\" so I can confirm the roster"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !hp(world)) {
        say("[*] scoreboard: world pointer not resolved (in game?)");
        return;
    }

    auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7F; };
    auto nameLike = [](const std::string& s) {
        if (s.size() < 2 || s.size() > 40) return false;
        int letters = 0;
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++letters;
            else if (c == ' ' || c == '\'' || c == '-' || c == '.' || c == '_' || (c >= '0' && c <= '9')) continue;
            else return false;
        }
        return letters >= 2;
    };
    auto findStrings = [&](const uint8_t* b, size_t nb) {
        std::vector<std::pair<uint32_t, std::string>> out;
        for (size_t o = 0; o < nb;) {
            if (printable(b[o]) && (o == 0 || !printable(b[o - 1]))) {
                size_t e = o;
                while (e < nb && printable(b[e])) ++e;
                if (e - o >= 2 && e - o <= 40 && e < nb && b[e] == 0) {
                    std::string s(reinterpret_cast<const char*>(b + o), e - o);
                    if (nameLike(s)) out.push_back({static_cast<uint32_t>(o), s});
                }
                o = e + 1;
            } else {
                ++o;
            }
        }
        return out;
    };

    struct Node { uint64_t addr; int depth; int parent; uint32_t off; };
    std::vector<Node> q;
    std::unordered_set<uint64_t> seen;
    seen.insert(world);
    q.push_back({world, 0, -1, 0});
    constexpr int kMaxDepth = 4;
    constexpr uint32_t kWin = 0x800, kRootWin = 0x2800;
    constexpr size_t kBudget = 80000;
    struct Arr { int node; uint32_t off; uint64_t buf; int count; };
    std::vector<Arr> arrays;
    std::unordered_set<uint64_t> bufSeen;
    size_t head = 0;
    while (head < q.size() && q.size() < kBudget) {
        const size_t end = std::min(q.size(), head + 256);
        std::vector<std::vector<uint8_t>> bufs;
        std::vector<size_t> idx;
        bufs.reserve(end - head);
        for (size_t i = head; i < end; ++i) {
            bufs.emplace_back(static_cast<size_t>(q[i].depth == 0 ? kRootWin : kWin), uint8_t{0});
            idx.push_back(i);
        }
        std::vector<Process::ReadReq> reqs;
        reqs.reserve(idx.size());
        for (size_t b = 0; b < idx.size(); ++b) reqs.push_back({static_cast<uintptr_t>(q[idx[b]].addr), bufs[b].data(), bufs[b].size()});
        p.readMany(reqs);
        for (size_t b = 0; b < idx.size(); ++b) {
            if (!reqs[b].ok) continue;
            const size_t i = idx[b];
            const int d = q[i].depth;
            const std::vector<uint8_t>& buf = bufs[b];
            for (uint32_t o = 0; o + 12 <= buf.size(); o += 8) {
                uint64_t v;
                uint32_t cnt;
                std::memcpy(&v, &buf[o], 8);
                std::memcpy(&cnt, &buf[o + 8], 4);
                if (hp(v) && cnt >= 3 && cnt <= 256 && bufSeen.insert(v).second) arrays.push_back({static_cast<int>(i), o, v, static_cast<int>(cnt)});
                if (hp(v) && d < kMaxDepth && q.size() < kBudget && seen.insert(v).second) q.push_back({v, d + 1, static_cast<int>(i), o});
            }
        }
        head = end;
    }
    auto chainOf = [&](int node, uint32_t finalOff) {
        std::vector<uint32_t> offs;
        int i = node;
        while (q[i].parent != -1) { offs.push_back(q[i].off); i = q[i].parent; }
        std::string s = "World";
        for (auto it = offs.rbegin(); it != offs.rend(); ++it) s += "->+" + hexs(*it);
        return s + "->+" + hexs(finalOff);
    };
    say("[*] scoreboard: walked " + std::to_string(q.size()) + " objects from World, testing " + std::to_string(arrays.size()) + " array candidate(s) ...");

    int rosters = 0, tested = 0;
    for (const auto& a : arrays) {
        if (rosters >= 3 || tested >= 600) break;
        ++tested;
        const int ecap = std::min(a.count, 64);
        std::vector<uint64_t> raw(static_cast<size_t>(ecap), 0);
        { std::vector<Process::ReadReq> r = {{static_cast<uintptr_t>(a.buf), raw.data(), static_cast<size_t>(ecap) * 8}}; p.readMany(r); }
        std::vector<uint64_t> ents;
        for (uint64_t e : raw) if (hp(e)) ents.push_back(e);
        if (ents.size() < 3) continue;

        std::vector<std::vector<uint8_t>> eb(ents.size(), std::vector<uint8_t>(0x400, 0));
        { std::vector<Process::ReadReq> r; for (size_t k = 0; k < ents.size(); ++k) r.push_back({static_cast<uintptr_t>(ents[k]), eb[k].data(), 0x400}); p.readMany(r); }
        std::unordered_map<uint64_t, int> vtTally;
        for (const auto& e : eb) { uint64_t vt; std::memcpy(&vt, e.data(), 8); if (hp(vt)) vtTally[vt]++; }
        uint64_t domVt = 0;
        int domN = 0;
        for (const auto& kv : vtTally) if (kv.second > domN) { domN = kv.second; domVt = kv.first; }
        if (domN < 3) continue;
        std::vector<size_t> keep;
        for (size_t k = 0; k < ents.size(); ++k) { uint64_t vt; std::memcpy(&vt, eb[k].data(), 8); if (vt == domVt) keep.push_back(k); }

        std::map<std::string, std::map<size_t, std::string>> pathVals;
        for (size_t k : keep)
            for (const auto& fs : findStrings(eb[k].data(), 0x400)) pathVals["@" + hexs(fs.first)][k] = fs.second;
        struct SlotRef { size_t k; uint32_t off; uint64_t ptr; };
        std::vector<SlotRef> slots;
        for (size_t k : keep)
            for (uint32_t so = 0; so + 8 <= 0x400 && slots.size() < 4000; so += 8) {
                uint64_t pv;
                std::memcpy(&pv, &eb[k][so], 8);
                if (hp(pv)) slots.push_back({k, so, pv});
            }
        std::vector<uint8_t> sb(slots.size() * 0x40, 0);
        { std::vector<Process::ReadReq> r; for (size_t s = 0; s < slots.size(); ++s) r.push_back({static_cast<uintptr_t>(slots[s].ptr), &sb[s * 0x40], 0x40}); p.readMany(r); }
        for (size_t s = 0; s < slots.size(); ++s)
            for (const auto& fs : findStrings(&sb[s * 0x40], 0x40))
                pathVals["+" + hexs(slots[s].off) + "@" + hexs(fs.first)][slots[s].k] = fs.second;

        for (const auto& pv : pathVals) {
            bool hasMe = false;
            std::set<std::string> distinct;
            for (const auto& kv : pv.second) { distinct.insert(kv.second); if (kv.second == myName) hasMe = true; }
            if (!hasMe || distinct.size() < 2 || pv.second.size() * 2 < keep.size()) continue;
            ++rosters;
            say("[*] scoreboard: ROSTER FOUND - array " + chainOf(a.node, a.off) + "  (buffer " + hexs(a.buf) +
                ", count " + std::to_string(a.count) + ")");
            say("[*] scoreboard:   each entry (same class, vtable " + hexs(domVt) + ") -> name at " + pv.first +
                " (one pointer = RVString, text at +0x10)");
            std::string names;
            size_t shown = 0;
            for (const auto& kv : pv.second) {
                if (shown++) names += ", ";
                names += "\"" + kv.second + "\"";
                if (shown >= 40) { names += ", ..."; break; }
            }
            say("[*] scoreboard:   " + std::to_string(pv.second.size()) + " names: " + names);
            break;
        }
    }
    if (!rosters)
        say("[*] scoreboard: no array under World (depth " + std::to_string(kMaxDepth) +
            ") had an entry equal to \"" + myName + "\" - the roster is deeper or shaped differently.");
}

void findClassNames(const Process& p, const Image& img, const std::string& className, const std::string& myName,
                    const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] class names: 64-bit images only"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    std::vector<VTableInfo> vts;
    std::string err;
    if (!findVTables(img, className, vts, err) || vts.empty()) { say("[*] class names: no RTTI for '" + className + "' (" + err + ")"); return; }
    std::unordered_set<uint64_t> vtVas;
    for (const auto& v : vts) vtVas.insert(static_cast<uint64_t>(img.base) + v.rva);
    std::vector<uint64_t> insts;
    std::unordered_set<uint64_t> instSeen;
    for (const auto& h : scanPointers(p, img.ptrSize, vtVas))
        if (instSeen.insert(h.addr).second) insts.push_back(h.addr);
    if (insts.empty()) { say("[*] class names: no live '" + className + "' instances"); return; }
    const size_t take = std::min(insts.size(), size_t(128));
    say("[*] class names: " + std::to_string(insts.size()) + " live '" + className + "' instance(s); scanning " +
        std::to_string(take) + " for name strings ...");

    auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7F; };
    auto nameLike = [](const std::string& s) {
        if (s.size() < 2 || s.size() > 40) return false;
        int letters = 0;
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++letters;
            else if (c == ' ' || c == '\'' || c == '-' || c == '.' || c == '_' || (c >= '0' && c <= '9')) continue;
            else return false;
        }
        return letters >= 2;
    };
    auto findStrings = [&](const uint8_t* b, size_t nb) {
        std::vector<std::pair<uint32_t, std::string>> out;
        for (size_t o = 0; o < nb;) {
            if (printable(b[o]) && (o == 0 || !printable(b[o - 1]))) {
                size_t e = o;
                while (e < nb && printable(b[e])) ++e;
                if (e - o >= 2 && e - o <= 40 && e < nb && b[e] == 0) {
                    std::string s(reinterpret_cast<const char*>(b + o), e - o);
                    if (nameLike(s)) out.push_back({static_cast<uint32_t>(o), s});
                }
                o = e + 1;
            } else {
                ++o;
            }
        }
        return out;
    };

    constexpr uint32_t kWin = 0x400;
    std::map<std::string, std::set<std::string>> vals;
    std::map<std::string, int> cover;
    std::map<std::string, std::string> example;
    std::set<std::string> myPaths;
    auto record = [&](const std::string& path, const std::string& s, std::set<std::string>& perInst) {
        if (!perInst.insert(path).second) return;
        vals[path].insert(s);
        cover[path]++;
        if (!example.count(path)) example[path] = s;
        if (s == myName) myPaths.insert(path);
    };

    std::vector<std::vector<uint8_t>> eb(take, std::vector<uint8_t>(kWin, 0));
    { std::vector<Process::ReadReq> r; for (size_t k = 0; k < take; ++k) r.push_back({static_cast<uintptr_t>(insts[k]), eb[k].data(), kWin}); p.readMany(r); }
    for (size_t k = 0; k < take; ++k) {
        std::set<std::string> perInst;
        for (const auto& fs : findStrings(eb[k].data(), kWin)) record("@" + hexs(fs.first), fs.second, perInst);
        struct S { uint32_t off; uint64_t ptr; };
        std::vector<S> slots;
        for (uint32_t o = 0; o + 8 <= kWin && slots.size() < 512; o += 8) {
            uint64_t v;
            std::memcpy(&v, &eb[k][o], 8);
            if (hp(v)) slots.push_back({o, v});
        }
        std::vector<uint8_t> sb(slots.size() * 0x40, 0);
        { std::vector<Process::ReadReq> r; for (size_t s = 0; s < slots.size(); ++s) r.push_back({static_cast<uintptr_t>(slots[s].ptr), &sb[s * 0x40], 0x40}); p.readMany(r); }
        for (size_t s = 0; s < slots.size(); ++s)
            for (const auto& fs : findStrings(&sb[s * 0x40], 0x40)) record("+" + hexs(slots[s].off) + "@" + hexs(fs.first), fs.second, perInst);
    }

    std::vector<std::string> order;
    for (const auto& kv : vals) order.push_back(kv.first);
    std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        if (vals[a].size() != vals[b].size()) return vals[a].size() > vals[b].size();
        return cover[a] > cover[b];
    });
    say("[*] class names: offset paths ranked by DISTINCT values (a real name field = one distinct per instance):");
    for (size_t j = 0; j < order.size() && j < 24; ++j) {
        const std::string& pa = order[j];
        std::string ex;
        size_t n = 0;
        for (const auto& v : vals[pa]) { if (n++) ex += " "; ex += "\"" + v + "\""; if (n == 4) { ex += " ..."; break; } }
        char b[320];
        std::snprintf(b, sizeof b, "    %-20s  %2zu distinct / %2d of %zu  e.g. %s%s", pa.c_str(), vals[pa].size(), cover[pa], take,
                      ex.c_str(), myPaths.count(pa) ? "   <<< HAS YOUR NAME" : "");
        say(b);
    }
}

void findNameChain(const Process& p, const Image& img, const DiscoverConfig& cfg,
                   const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] name chain: 64-bit images only"); return; }
    auto hp = [](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; };

    std::vector<VTableInfo> vts;
    std::string err;
    if (!findVTables(img, "DayZPlayer", vts, err)) { say("[*] name chain: no DayZPlayer RTTI (" + err + ")"); return; }
    std::unordered_map<uint64_t, bool> playerVt;
    for (const auto& v : vts) playerVt[static_cast<uint64_t>(img.base) + v.rva] = v.cls.find("Infected") == std::string::npos;
    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !hp(world)) {
        say("[*] name chain: world pointer not resolved (in game?)");
        return;
    }
    std::vector<uint64_t> cands;
    for (const auto& t : cfg.tables) {
        uint64_t hdr[2] = {};
        std::vector<Process::ReadReq> reqs = {{static_cast<uintptr_t>(world + t.off), hdr, 16}};
        p.readMany(reqs);
        const uint64_t buf = hdr[0];
        const int32_t cnt = static_cast<int32_t>(hdr[1] & 0xFFFFFFFF);
        if (!reqs[0].ok || !hp(buf) || cnt <= 0 || cnt > 4096) continue;
        std::vector<uint64_t> more(static_cast<size_t>(cnt), 0);
        reqs = {{static_cast<uintptr_t>(buf), more.data(), more.size() * 8}};
        p.readMany(reqs);
        if (!reqs[0].ok) continue;
        for (uint64_t s : more) if (hp(s)) cands.push_back(s);
    }
    std::vector<uint64_t> vt(cands.size(), 0);
    std::vector<Process::ReadReq> rq;
    for (size_t i = 0; i < cands.size(); ++i) rq.push_back({static_cast<uintptr_t>(cands[i]), &vt[i], 8});
    p.readMany(rq);
    std::vector<uint64_t> players;
    std::set<uint64_t> seen;
    for (size_t i = 0; i < cands.size(); ++i) {
        const auto it = playerVt.find(vt[i]);
        if (it == playerVt.end() || !it->second) continue;
        if (seen.insert(cands[i]).second) players.push_back(cands[i]);
    }
    if (players.empty()) { say("[*] name chain: no survivor entities in the world tables yet"); return; }

    struct Chain { std::string label; std::vector<uint32_t> hops; uint32_t fin; bool header; };
    auto readName = [&](uint64_t root, const Chain& c) -> std::string {
        uint64_t cur = root;
        for (uint32_t h : c.hops) { if (!hp(cur) || !p.readPtr(static_cast<uintptr_t>(cur + h), 8, cur)) return {}; }
        uint64_t slot = 0;
        if (!hp(cur) || !p.readPtr(static_cast<uintptr_t>(cur + c.fin), 8, slot) || !hp(slot)) return {};
        uint8_t b[49] = {};
        if (!p.read(static_cast<uintptr_t>(slot + (c.header ? 0x10 : 0)), b, 48)) return {};
        size_t n = 0;
        int letters = 0;
        while (n < 48 && b[n]) {
            if (b[n] < 0x20 || b[n] >= 0x7F) break;
            if ((b[n] >= 'a' && b[n] <= 'z') || (b[n] >= 'A' && b[n] <= 'Z')) ++letters;
            ++n;
        }
        return (n >= 2 && letters >= 2) ? std::string(reinterpret_cast<char*>(b), n) : std::string();
    };

    const std::vector<Chain> entChains = {
        {"A", {0x2628, 0x3B8, 0x50}, 0x338, true},
        {"B", {0x26A0, 0x3B8, 0x348}, 0x248, false},
    };
    const size_t take = std::min(players.size(), size_t(32));
    say("[*] name chain: entity-rooted chains on " + std::to_string(take) + " survivor(s):");
    uint64_t local = 0, link = 0, inner = 0;
    if (cfg.localLink && p.readPtr(static_cast<uintptr_t>(world + cfg.localLink), 8, link) && hp(link) &&
        p.readPtr(static_cast<uintptr_t>(link + cfg.localDeref), 8, inner) && hp(inner))
        local = inner - cfg.localAdj;
    for (size_t i = 0; i < take; ++i) {
        std::string line = "  entity " + std::to_string(i) + " @ " + hexs(players[i]) + (players[i] == local ? " (you)" : "") + ":";
        for (const auto& c : entChains) {
            const std::string nm = readName(players[i], c);
            line += "  " + c.label + "=" + (nm.empty() ? "-" : ("\"" + nm + "\""));
        }
        say(line);
    }

    const std::vector<Chain> worldChains = {
        {"+0x30->+0x8->+0x3E8@0xC0", {0x30, 0x8, 0x3E8}, 0xC0, true},
        {"+0x30->+0x78->+0x370@0x0", {0x30, 0x78, 0x370}, 0x0, true},
        {"+0x30->+0x8->+0x2C8->+0x308@0x240", {0x30, 0x8, 0x2C8, 0x308}, 0x240, true},
        {"+0x90->+0x100->+0xA8->+0x148@0x3C0", {0x90, 0x100, 0xA8, 0x148}, 0x3C0, true},
        {"+0x370->+0x18->+0x170->+0x188@0x248", {0x370, 0x18, 0x170, 0x188}, 0x248, true},
        {"+0xB60->+0x218@0x338", {0xB60, 0x218}, 0x338, true},
        {"+0xB60->+0x358@0x158", {0xB60, 0x358}, 0x158, false},
    };
    say("[*] name chain: World-rooted candidates (one-off):");
    for (const auto& c : worldChains) {
        const std::string nm = readName(world, c);
        say("    World->" + c.label + " = " + (nm.empty() ? "-" : ("\"" + nm + "\"")));
    }
}

bool discoverEntities(const Process& p, const Image& img, const std::string& clsList, Discovery& out, std::string& status,
                      const DiscoverConfig& cfg) {
    out = Discovery{};
    std::string err;
    std::vector<std::string> classes;
    for (size_t at = 0; at <= clsList.size();) {
        const size_t comma = std::min(clsList.find(',', at), clsList.size());
        if (comma > at) classes.push_back(clsList.substr(at, comma - at));
        at = comma + 1;
    }
    if (classes.empty()) {
        status = "no class given";
        return false;
    }
    const std::string& cls = classes.front();
    if (!findVTables(img, cls, out.vtables, err)) {
        status = err;
        return false;
    }
    for (size_t c = 1; c < classes.size(); ++c) {
        std::vector<VTableInfo> more;
        if (!findVTables(img, classes[c], more, err)) continue;
        for (auto& v : more) {
            bool dup = false;
            for (const auto& have : out.vtables) dup |= have.rva == v.rva;
            if (dup) continue;
            v.exact = false;
            v.group = static_cast<int>(c);
            out.vtables.push_back(v);
        }
    }

    std::unordered_map<uint64_t, size_t> vtIndex;
    std::unordered_set<uint64_t> vtVas;
    for (size_t i = 0; i < out.vtables.size(); ++i) {
        const uint64_t va = static_cast<uint64_t>(img.base) + out.vtables[i].rva;
        vtIndex[va] = i;
        vtVas.insert(va);
    }
    std::string chainWhy;
    if (cfg.worldRva) {
        if (fromWorld(p, img, cfg, cls, vtIndex, out, chainWhy)) {
            status = "ok";
            return true;
        }
        out.log.push_back(chainWhy + " - fell back to the heap scan");
    }

    std::unordered_map<uint64_t, size_t> instances;
    for (const auto& h : scanPointers(p, img.ptrSize, vtVas)) {
        instances[h.addr] = vtIndex[h.value];
    }
    if (instances.empty()) {
        status = (chainWhy.empty() ? "" : chainWhy + "; heap scan: ") + "no live '" + cls + "' objects yet";
        return false;
    }

    std::unordered_set<uint64_t> instSet;
    for (auto& kv : instances) instSet.insert(kv.first);
    const auto refs = scanPointers(p, img.ptrSize, instSet);

    const uint64_t imgLo = img.base, imgHi = img.base + img.bytes.size();
    struct GlobalRef {
        uint32_t rva;
        uint64_t target;
    };
    std::vector<GlobalRef> globalRefs;
    for (const auto& r : refs)
        if (r.addr >= imgLo && r.addr < imgHi) globalRefs.push_back({static_cast<uint32_t>(r.addr - imgLo), r.value});

    struct Cand {
        const GlobalRef* g;
        std::vector<uint32_t> xrefs;
    };
    std::vector<Cand> cands;
    for (const auto& g : globalRefs)
        if (out.vtables[instances[g.target]].exact) cands.push_back({&g, findCodeXrefs(img, g.rva)});
    const GlobalRef* player1 = nullptr;
    std::vector<uint32_t> player1Xrefs;
    if (!cands.empty()) {
        auto best = std::max_element(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.xrefs.size() < b.xrefs.size(); });
        player1 = best->g;
        player1Xrefs = best->xrefs;
    }
    if (!player1 && cfg.requireGlobal) {
        status = "found " + std::to_string(instances.size()) + " object(s) but no global points at one (not in a game yet?)";
        return false;
    }

    struct ChainKey {
        uint32_t g, o1;
        int hops;
        bool operator<(const ChainKey& k) const { return std::tie(g, o1, hops) < std::tie(k.g, k.o1, k.hops); }
    };
    std::map<ChainKey, std::map<uint32_t, uint64_t>> chains;
    if (cfg.chains && !player1) {
        constexpr uint64_t kObj = 0x8000, kLink = 0x100;
        const uint64_t maxUser = img.ptrSize == 4 ? 0x100000000ull : 0x7FFFFFFF0000ull;
        auto heapPtr = [&](uint64_t v) { return v >= 0x10000 && v < maxUser && !(v >= imgLo && v < imgHi) && v % img.ptrSize == 0; };
        std::vector<PtrHit> h1;
        for (const auto& r : refs)
            if (!(r.addr >= imgLo && r.addr < imgHi)) h1.push_back(r);
        auto byAddr = [](const PtrHit& a, const PtrHit& b) { return a.addr < b.addr; };
        std::sort(h1.begin(), h1.end(), byAddr);
        auto inRange = [&](const std::vector<PtrHit>& v, uint64_t lo, uint64_t hi) {
            auto first = std::lower_bound(v.begin(), v.end(), PtrHit{lo, 0}, byAddr);
            auto last = std::lower_bound(first, v.end(), PtrHit{hi, 0}, byAddr);
            return std::make_pair(first, last);
        };

        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (const auto& h : h1) {
            const uint64_t lo = h.addr >= kLink ? h.addr - kLink + 1 : 0;
            if (!ranges.empty() && lo <= ranges.back().second) ranges.back().second = std::max(ranges.back().second, h.addr + 1);
            else ranges.push_back({lo, h.addr + 1});
        }
        std::vector<PtrHit> h2 = scanPointerRanges(p, img.ptrSize, ranges, 4000000);
        std::sort(h2.begin(), h2.end(), byAddr);

        size_t total = 0;
        for (const auto& sec : img.sections) {
            if (sec.exec() || !(sec.flags & 0x80000000)) continue;
            const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
            for (uint64_t at = sec.rva; at + img.ptrSize <= hi && total < 200000; at += img.ptrSize) {
                uint64_t v = 0;
                std::memcpy(&v, &img.bytes[static_cast<size_t>(at)], img.ptrSize);
                if (!heapPtr(v)) continue;
                const uint32_t g = static_cast<uint32_t>(at);
                auto r1 = inRange(h1, v, v + kObj);
                for (auto it = r1.first; it != r1.second; ++it, ++total)
                    chains[{g, static_cast<uint32_t>(it->addr - v), 1}][0] = it->value;
                auto r2 = inRange(h2, v, v + kObj);
                for (auto it = r2.first; it != r2.second; ++it) {
                    auto t = inRange(h1, it->value, it->value + kLink);
                    for (auto jt = t.first; jt != t.second; ++jt, ++total)
                        chains[{g, static_cast<uint32_t>(it->addr - v), 2}][static_cast<uint32_t>(jt->addr - it->value)] = jt->value;
                }
            }
        }
    }

    struct ListCand {
        uint32_t rva;
        uint32_t alen, ulen;
        std::vector<uint64_t> items;
        size_t live = 0;
    };
    ListCand best;
    for (const auto& sec : img.sections) {
        if (sec.exec() || !(sec.flags & 0x80000000)) continue;
        const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
        for (uint64_t at = sec.rva; at + img.ptrSize + 8 <= hi; at += 4) {
            uint64_t buf = 0;
            if (img.ptrSize == 4) {
                uint32_t b32;
                img.at(at, b32);
                buf = b32;
            } else {
                img.at(at, buf);
            }
            uint32_t alen = 0, ulen = 0;
            img.at(at + img.ptrSize, alen);
            img.at(at + img.ptrSize + 4, ulen);
            if (buf < 0x10000 || (buf >= imgLo && buf < imgHi)) continue;
            if (alen == 0 || alen > 4096 || ulen == 0 || ulen > alen) continue;

            std::vector<uint8_t> raw(static_cast<size_t>(ulen) * img.ptrSize);
            if (!p.read(static_cast<uintptr_t>(buf), raw.data(), raw.size())) continue;
            ListCand c;
            c.rva = static_cast<uint32_t>(at);
            c.alen = alen;
            c.ulen = ulen;
            bool ok = true;
            for (uint32_t i = 0; i < ulen && ok; ++i) {
                uint64_t v = 0;
                if (img.ptrSize == 4) {
                    uint32_t v32;
                    std::memcpy(&v32, &raw[i * 4], 4);
                    v = v32;
                } else {
                    std::memcpy(&v, &raw[i * 8], 8);
                }
                if (v == 0) {
                    c.items.push_back(0);
                } else if (instSet.count(v)) {
                    c.items.push_back(v);
                    ++c.live;
                } else {
                    ok = false;
                }
            }
            if (ok && c.live > best.live) best = std::move(c);
        }
    }

    std::unordered_set<uint64_t> seen;
    auto add = [&](uint64_t addr, bool local) {
        if (!seen.insert(addr).second) return;
        Entity e;
        e.addr = addr;
        e.cls = out.vtables[instances[addr]].cls;
        e.local = local;
        out.entities.push_back(e);
    };
    struct ChainPick {
        ChainKey key;
        uint32_t o2;
        uint64_t target;
        size_t xrefs;
    };
    std::vector<ChainPick> localCands;
    std::vector<std::pair<ChainKey, size_t>> tables;
    std::map<uint32_t, size_t> globalXrefs;
    if (!chains.empty()) {
        std::map<uint32_t, size_t> perGlobal;
        for (const auto& c : chains) ++perGlobal[c.first.g];
        std::vector<std::pair<size_t, uint32_t>> order;
        for (const auto& g : perGlobal) order.push_back({g.second, g.first});
        std::sort(order.rbegin(), order.rend());
        for (size_t i = 0; i < order.size() && i < 48; ++i) globalXrefs[order[i].second] = findCodeXrefs(img, order[i].second).size();
        for (const auto& c : chains) {
            if (!globalXrefs.count(c.first.g)) continue;
            std::unordered_set<uint64_t> distinct;
            for (const auto& t : c.second) distinct.insert(t.second);
            if (c.first.hops == 2 && distinct.size() >= 2) {
                tables.push_back({c.first, distinct.size()});
            } else if (distinct.size() == 1) {
                const auto& t = *c.second.begin();
                if (out.vtables[instances[t.second]].group == 0) localCands.push_back({c.first, t.first, t.second, globalXrefs[c.first.g]});
            }
        }
        std::sort(localCands.begin(), localCands.end(), [](const ChainPick& a, const ChainPick& b) {
            if (a.xrefs != b.xrefs) return a.xrefs > b.xrefs;
            if (a.key.hops != b.key.hops) return a.key.hops < b.key.hops;
            return a.key.o1 < b.key.o1;
        });
        std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    }

    if (player1) add(player1->target, true);
    else if (!localCands.empty()) add(localCands.front().target, true);
    for (uint64_t v : best.items)
        if (v) add(v, false);
    if (!cfg.requireGlobal) {
        std::vector<std::pair<int, uint64_t>> all;
        for (const auto& kv : instances) all.push_back({out.vtables[kv.second].group, kv.first});
        std::sort(all.begin(), all.end());
        for (const auto& a : all) {
            if (cfg.maxEntities && out.entities.size() >= cfg.maxEntities) break;
            add(a.second, false);
        }
    }

    for (const auto& vt : out.vtables) {
        Finding f;
        f.kind = Finding::VTable;
        f.name = vt.cls + "_vtable";
        f.value = vt.rva;
        f.note = vt.exact ? "exact class" : "derives from " + cls;
        out.findings.push_back(f);
    }
    auto hx = [](uint64_t v) {
        char b[32];
        std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
        return std::string(b);
    };
    auto objName = [&](uint32_t g) {
        uint64_t v = 0;
        std::memcpy(&v, &img.bytes[g], img.ptrSize);
        const std::string n = classNameAt(p, img, v);
        return n.empty() ? "obj_" + hx(g).substr(2) : n;
    };
    for (size_t i = 0; i < localCands.size() && i < 6; ++i) {
        const ChainPick& c = localCands[i];
        const std::string on = objName(c.key.g);
        Finding f;
        f.name = i == 0 ? "local_player_chain" : "local_player_chain_alt" + std::to_string(i);
        f.value = c.key.g;
        f.note = "[[base+" + hx(c.key.g) + "] (" + on + ") +" + hx(c.key.o1) + "]" +
                 (c.key.hops == 2 ? " +" + hx(c.o2) + "]" : "") + " -> " + out.vtables[instances[c.target]].cls +
                 (i == 0 ? "; most-referenced of " + std::to_string(localCands.size()) + " single-target path(s) - verify" : "");
        f.xrefs = findCodeXrefs(img, c.key.g);
        out.findings.push_back(f);
    }
    std::set<uint32_t> namedGlobals;
    for (size_t i = 0; i < tables.size() && i < 8; ++i) {
        const ChainKey& k = tables[i].first;
        const std::string on = objName(k.g);
        if (namedGlobals.insert(k.g).second) {
            Finding g;
            g.name = on == "obj_" + hx(k.g).substr(2) ? on : "g_" + on;
            g.value = k.g;
            g.note = "pointer to " + on + " (holds entity tables)";
            g.xrefs = findCodeXrefs(img, k.g);
            out.findings.push_back(g);
        }
        uint64_t v = 0;
        std::memcpy(&v, &img.bytes[k.g], img.ptrSize);
        int32_t count = 0;
        const bool haveCount = p.read(static_cast<uintptr_t>(v + k.o1 + img.ptrSize), count) && count >= static_cast<int32_t>(tables[i].second) &&
                               count < 100000;
        Finding f;
        f.kind = Finding::Field;
        f.name = on + ".table_" + hx(k.o1).substr(2);
        f.value = k.o1;
        f.type = "ptr";
        f.note = cls + "*[] (instances of the classes searched), " + std::to_string(tables[i].second) + " seen in the first 32 slots" +
                 (haveCount ? "; int count " + std::to_string(count) + " at +" + hx(k.o1 + img.ptrSize) : "");
        out.findings.push_back(f);
    }

    if (player1) {
        Finding f;
        f.name = "player1";
        f.value = player1->rva;
        f.note = "pointer to local " + cls;
        f.note += "; most-referenced of " + std::to_string(cands.size()) + " global(s) pointing at it";
        f.xrefs = player1Xrefs;
        out.findings.push_back(f);
        for (const auto& c : cands) {
            if (c.g == player1) continue;
            char nm[48];
            std::snprintf(nm, sizeof nm, "player1_alias_%X", c.g->rva);
            Finding a;
            a.name = nm;
            a.value = c.g->rva;
            a.note = "also points at the local player right now (camera/focus alias?)";
            a.xrefs = c.xrefs;
            out.findings.push_back(a);
        }
    }
    if (best.live) {
        Finding f;
        f.name = "players";
        f.value = best.rva;
        f.note = "vector<" + cls + "*> { buf, alen, ulen }; " + std::to_string(best.live) + " live entr" +
                 (best.live == 1 ? "y" : "ies");
        f.xrefs = findCodeXrefs(img, best.rva);
        out.findings.push_back(f);

        Finding n;
        n.name = "players_count";
        n.value = best.rva + img.ptrSize + 4;
        n.note = "ulen of the players vector (int)";
        n.xrefs = findCodeXrefs(img, static_cast<uint32_t>(n.value));
        out.findings.push_back(n);
    }
    status = "ok";
    return true;
}

bool findRenderMatrices(const Image& img, std::vector<Finding>& out, std::string& status) {
    if (img.ptrSize != 4) {
        status = "render matrices: only 32-bit images are supported";
        return false;
    }
    auto inData = [&](uint32_t va) {
        if (va < img.base) return false;
        const Section* s = img.sectionFor(va - img.base);
        return s && !s->exec() && (s->flags & 0x80000000);
    };
    for (const auto& sec : img.sections) {
        if (!sec.exec()) continue;
        const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
        for (uint64_t i = sec.rva; i + 10 <= hi; ++i) {
            const uint8_t* c = &img.bytes[static_cast<size_t>(i)];
            if (c[0] != 0x68 || c[5] != 0x68 || c[6] != 0xA6 || c[7] != 0x0B || c[8] || c[9]) continue;
            uint32_t mv, proj = 0, mvp = 0;
            std::memcpy(&mv, c + 1, 4);
            if (!inData(mv)) continue;
            for (uint64_t k = i + 10; k + 10 <= hi && k < i + 48 && !proj; ++k) {
                const uint8_t* d = &img.bytes[static_cast<size_t>(k)];
                if (d[0] == 0x68 && d[5] == 0x68 && d[6] == 0xA7 && d[7] == 0x0B && !d[8] && !d[9]) {
                    std::memcpy(&proj, d + 1, 4);
                    for (uint64_t m = k + 10; m + 5 <= hi && m < k + 64 && !mvp; ++m)
                        if (img.bytes[static_cast<size_t>(m)] == 0xB9) {
                            uint32_t v;
                            std::memcpy(&v, &img.bytes[static_cast<size_t>(m + 1)], 4);
                            if (inData(v) && v != mv && v != proj) mvp = v;
                        }
                }
            }
            if (!proj || !inData(proj) || !mvp) continue;
            auto add = [&](const char* name, uint32_t va, const char* note) {
                Finding f;
                f.kind = Finding::Global;
                f.name = name;
                f.value = va - img.base;
                f.note = note;
                f.xrefs = findCodeXrefs(img, static_cast<uint32_t>(f.value));
                out.push_back(f);
            };
            add("mvmatrix", mv, "float[16] column-major, camera (modelview) matrix read back each frame");
            add("projmatrix", proj, "float[16] column-major, projection matrix read back each frame (fov, scope, aspect)");
            add("mvpmatrix", mvp, "float[16] column-major, projmatrix * mvmatrix: world -> clip space");
            return true;
        }
    }
    status = "render matrices: no glGetFloatv(GL_MODELVIEW_MATRIX/GL_PROJECTION_MATRIX) readback found";
    return false;
}

namespace {

bool dzHeap(uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; }

std::string dzText(const uint8_t* b, size_t cap) {
    size_t n = 0;
    while (n < cap && b[n] >= 0x20 && b[n] < 0x7F) ++n;
    if (n == cap || b[n] != 0 || n < 2) return {};
    return std::string(reinterpret_cast<const char*>(b), n);
}

uint64_t dzLocalPlayer(const Process& p, uint64_t world) {
    uint64_t link = 0, inner = 0;
    if (!p.read(static_cast<uintptr_t>(world + 0x29B0), link) || !dzHeap(link)) return 0;
    if (!p.read(static_cast<uintptr_t>(link + 0x8), inner) || !dzHeap(inner)) return 0;
    return inner - 0xA8;
}

std::vector<std::string> dzClassNames(const Process& p, const std::vector<uint64_t>& objs) {
    std::vector<uint64_t> cfg(objs.size(), 0), str(objs.size(), 0);
    std::vector<Process::ReadReq> rq;
    for (size_t i = 0; i < objs.size(); ++i) rq.push_back({static_cast<uintptr_t>(objs[i] + 0x130), &cfg[i], 8});
    p.readMany(rq);
    rq.clear();
    for (size_t i = 0; i < objs.size(); ++i) rq.push_back({static_cast<uintptr_t>(dzHeap(cfg[i]) ? cfg[i] + 0x98 : 0), &str[i], 8});
    p.readMany(rq);
    constexpr size_t kN = 64;
    std::vector<uint8_t> txt(objs.size() * kN, 0);
    rq.clear();
    for (size_t i = 0; i < objs.size(); ++i) rq.push_back({static_cast<uintptr_t>(dzHeap(str[i]) ? str[i] + 0x10 : 0), &txt[i * kN], kN});
    p.readMany(rq);
    std::vector<std::string> out(objs.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        if (!rq[i].ok) continue;
        std::string s = dzText(&txt[i * kN], kN);
        bool ident = !s.empty() && std::isalpha(static_cast<unsigned char>(s[0]));
        for (char c : s) ident = ident && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (ident) out[i] = s;
    }
    return out;
}

}

void findHandItem(const Process& p, const Image& img, const DiscoverConfig& cfg, int waitSec,
                  const std::function<void(const std::string&)>& log, const std::function<void(int)>& wait) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] hand item: 64-bit images only"); return; }
    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !dzHeap(world)) {
        say("[*] hand item: world pointer not resolved (in game?)");
        return;
    }
    const uint64_t me = dzLocalPlayer(p, world);
    if (!me) { say("[*] hand item: local player not resolved (spawned in?)"); return; }

    const uint64_t modLo = static_cast<uint64_t>(img.base), modHi = modLo + img.bytes.size();
    auto isVt = [&](uint64_t v) { return v >= modLo && v < modHi && v % 8 == 0; };

    using Path = std::pair<uint32_t, int32_t>;
    constexpr uint32_t kEnt = 0x1800, kObj = 0x400;
    auto snapshot = [&]() {
        std::map<Path, std::pair<uint64_t, std::string>> out;
        std::vector<uint8_t> eb(kEnt, 0);
        for (uint32_t o = 0; o < kEnt; o += 0x400) p.read(static_cast<uintptr_t>(me + o), &eb[o], 0x400);
        std::vector<std::pair<uint32_t, uint64_t>> hop1;
        for (uint32_t a = 0; a + 8 <= kEnt; a += 8) {
            uint64_t v;
            std::memcpy(&v, &eb[a], 8);
            if (dzHeap(v) && v != me) hop1.push_back({a, v});
        }
        std::vector<uint8_t> ob(hop1.size() * kObj, 0);
        std::vector<Process::ReadReq> rq;
        for (size_t i = 0; i < hop1.size(); ++i) rq.push_back({static_cast<uintptr_t>(hop1[i].second), &ob[i * kObj], kObj});
        p.readMany(rq);
        struct Cand {
            Path path;
            uint64_t obj;
        };
        std::vector<Cand> cands;
        for (size_t i = 0; i < hop1.size(); ++i) {
            if (!rq[i].ok) continue;
            uint64_t vt0;
            std::memcpy(&vt0, &ob[i * kObj], 8);
            if (isVt(vt0)) cands.push_back({{hop1[i].first, -1}, hop1[i].second});
            for (uint32_t b = 8; b + 8 <= kObj; b += 8) {
                uint64_t v;
                std::memcpy(&v, &ob[i * kObj + b], 8);
                if (dzHeap(v) && v != me) cands.push_back({{hop1[i].first, int32_t(b)}, v});
            }
        }
        std::vector<uint64_t> vt(cands.size(), 0);
        rq.clear();
        for (size_t i = 0; i < cands.size(); ++i) rq.push_back({static_cast<uintptr_t>(cands[i].obj), &vt[i], 8});
        p.readMany(rq);
        std::vector<Cand> items;
        for (size_t i = 0; i < cands.size(); ++i)
            if (cands[i].path.second < 0 || (rq[i].ok && isVt(vt[i]))) items.push_back(cands[i]);
        std::vector<uint64_t> objs;
        for (const auto& c : items) objs.push_back(c.obj);
        const auto names = dzClassNames(p, objs);
        for (size_t i = 0; i < items.size(); ++i)
            if (!names[i].empty()) out[items[i].path] = {items[i].obj, names[i]};
        return out;
    };
    auto pathStr = [](const Path& pt) {
        return "+" + hexs(pt.first) + (pt.second < 0 ? std::string() : " -> +" + hexs(uint32_t(pt.second)));
    };

    say("[*] hand item: local player " + hexs(me) + ". Hold an item in your hands now (a weapon is best).");
    const auto a = snapshot();
    {
        int shown = 0;
        for (const auto& kv : a) {
            if (shown++ >= 60) break;
            say("    before: entity " + pathStr(kv.first) + "   " + kv.second.second);
        }
    }
    say("[*] hand item: " + std::to_string(a.size()) + " named objects reachable from you. NOW put a DIFFERENT item in your hands"
        " (or holster to empty hands) - re-reading in " + std::to_string(waitSec) + " s ...");
    wait(waitSec * 1000);
    const auto b = snapshot();

    std::vector<std::string> lines;
    std::set<Path> all;
    for (const auto& kv : a) all.insert(kv.first);
    for (const auto& kv : b) all.insert(kv.first);
    for (const Path& pt : all) {
        const auto ia = a.find(pt), ib = b.find(pt);
        const uint64_t oa = ia == a.end() ? 0 : ia->second.first, ob = ib == b.end() ? 0 : ib->second.first;
        if (oa == ob) continue;
        const std::string na = ia == a.end() ? "(empty)" : ia->second.second, nb = ib == b.end() ? "(empty)" : ib->second.second;
        lines.push_back("    entity " + pathStr(pt) + "   " + na + "  ->  " + nb);
    }
    if (lines.empty()) {
        say("[*] hand item: nothing changed. Make sure the second item is really in your hands, and rerun.");
    } else {
        say("[*] hand item: paths whose item changed (the one matching your swap = Ickza dz::kInventory -> kHandItem):");
        for (const auto& l : lines) say(l);
    }
}

static void pairRoster(const Process& p, const Image& img, const DiscoverConfig& cfg, uint64_t world,
                const std::map<uint64_t, std::vector<uint32_t>>& slotsOf, const std::vector<uint64_t>& clients, uint64_t holder,
                uint64_t arr, uint32_t nameSlot, const std::function<void(const std::string&)>& say) {
    uint64_t nc = 0;
    for (uint64_t c : clients)
        if (holder >= c && holder < c + 0x1000) nc = c;
    std::string globals;
    if (const auto it = slotsOf.find(nc); it != slotsOf.end())
        for (uint32_t r : it->second) globals += (globals.empty() ? "" : ", ") + hexs(r);
    int32_t cnt = 0;
    p.read(static_cast<uintptr_t>(holder + 8), cnt);
    say("[*] roster: PLAYER LIST = *(NetworkClient+" + hexs(holder - nc) + "), count at +" + hexs(holder - nc + 8) + " = " +
        std::to_string(cnt) + ", gamertag RVString at identity +" + hexs(nameSlot) + "; NetworkClient* at module RVA " + globals);
    if (cnt <= 0 || cnt > 1024) cnt = 256;

    std::vector<uint64_t> raw(static_cast<size_t>(cnt), 0);
    p.read(static_cast<uintptr_t>(arr), raw.data(), raw.size() * 8);
    std::vector<uint64_t> ids;
    for (uint64_t v : raw) if (dzHeap(v)) ids.push_back(v);
    constexpr uint32_t kId = 0x200, kEnt = 0x3000;
    std::vector<uint8_t> ib(ids.size() * kId, 0);
    std::vector<Process::ReadReq> rq;
    for (size_t i = 0; i < ids.size(); ++i) rq.push_back({static_cast<uintptr_t>(ids[i]), &ib[i * kId], kId});
    p.readMany(rq);
    std::vector<std::string> idName(ids.size());
    {
        std::vector<uint64_t> sp(ids.size(), 0);
        for (size_t i = 0; i < ids.size(); ++i) std::memcpy(&sp[i], &ib[i * kId + nameSlot], 8);
        std::vector<uint8_t> tb(ids.size() * 48, 0);
        rq.clear();
        for (size_t i = 0; i < ids.size(); ++i) rq.push_back({static_cast<uintptr_t>(dzHeap(sp[i]) ? sp[i] + 0x10 : 0), &tb[i * 48], 48});
        p.readMany(rq);
        std::string all;
        for (size_t i = 0; i < ids.size(); ++i) {
            idName[i] = rq[i].ok ? dzText(&tb[i * 48], 48) : std::string();
            if (all.size() < 1500) all += (all.empty() ? "" : ", ") + (idName[i].empty() ? std::string("?") : idName[i]);
        }
        say("[*] roster: " + std::to_string(ids.size()) + " identities: " + all);
    }
    if (ids.empty()) return;

    std::vector<VTableInfo> vts;
    std::string err;
    if (!findVTables(img, "DayZPlayer", vts, err)) { say("[*] roster pairing: no DayZPlayer RTTI"); return; }
    std::unordered_map<uint64_t, bool> playerVt;
    for (const auto& v : vts) playerVt[static_cast<uint64_t>(img.base) + v.rva] = v.cls.find("Infected") == std::string::npos;
    std::vector<uint64_t> cands;
    for (const auto& t : cfg.tables) {
        uint64_t hdr[2] = {};
        if (!p.read(static_cast<uintptr_t>(world + t.off), hdr, 16) || !dzHeap(hdr[0])) continue;
        const int32_t n = static_cast<int32_t>(hdr[1] & 0xFFFFFFFF);
        if (n <= 0 || n > 4096) continue;
        std::vector<uint64_t> more(static_cast<size_t>(n), 0);
        if (!p.read(static_cast<uintptr_t>(hdr[0]), more.data(), more.size() * 8)) continue;
        for (uint64_t s : more) if (dzHeap(s)) cands.push_back(s);
    }
    const uint64_t me = dzLocalPlayer(p, world);
    std::vector<uint64_t> vt(cands.size(), 0);
    rq.clear();
    for (size_t i = 0; i < cands.size(); ++i) rq.push_back({static_cast<uintptr_t>(cands[i]), &vt[i], 8});
    p.readMany(rq);
    std::vector<uint64_t> ents;
    std::set<uint64_t> seen;
    if (dzHeap(me)) { ents.push_back(me); seen.insert(me); }
    for (size_t i = 0; i < cands.size(); ++i) {
        const auto it = playerVt.find(vt[i]);
        if (it != playerVt.end() && it->second && seen.insert(cands[i]).second) ents.push_back(cands[i]);
    }
    say("[*] roster pairing: " + std::to_string(ents.size()) + " survivor entities (" + (dzHeap(me) ? "first = you" : "own entity not found") + ")");
    if (ents.empty()) return;
    std::vector<uint8_t> eb(ents.size() * kEnt, 0);
    rq.clear();
    for (size_t e = 0; e < ents.size(); ++e)
        for (uint32_t o = 0; o < kEnt; o += 0x400) rq.push_back({static_cast<uintptr_t>(ents[e] + o), &eb[e * kEnt + o], 0x400});
    p.readMany(rq);
    auto q64 = [](const std::vector<uint8_t>& b, size_t at) { uint64_t v; std::memcpy(&v, &b[at], 8); return v; };
    auto d32 = [](const std::vector<uint8_t>& b, size_t at) { uint32_t v; std::memcpy(&v, &b[at], 4); return v; };
    auto who = [&](size_t e) { return e == 0 && dzHeap(me) ? std::string("YOU") : "ent" + std::to_string(e); };

    std::unordered_map<uint64_t, size_t> idIx, entIx;
    for (size_t i = 0; i < ids.size(); ++i) idIx[ids[i]] = i;
    for (size_t e = 0; e < ents.size(); ++e) entIx[ents[e]] = e;
    std::map<uint32_t, std::vector<std::string>> idToEnt, entToId;
    for (size_t i = 0; i < ids.size(); ++i)
        for (uint32_t o = 0; o + 8 <= kId; o += 8)
            if (const auto it = entIx.find(q64(ib, i * kId + o)); it != entIx.end())
                idToEnt[o].push_back(who(it->second) + "=" + idName[i]);
    for (size_t e = 0; e < ents.size(); ++e)
        for (uint32_t o = 0; o + 8 <= kEnt; o += 8)
            if (const auto it = idIx.find(q64(eb, e * kEnt + o)); it != idIx.end())
                entToId[o].push_back(who(e) + "=" + idName[it->second]);
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) if (s.size() < 400) s += (s.empty() ? "" : ", ") + x;
        return s;
    };
    for (const auto& kv : idToEnt) say("[*] roster pairing: POINTER identity +" + hexs(kv.first) + " -> entity: " + list(kv.second));
    for (const auto& kv : entToId) say("[*] roster pairing: POINTER entity +" + hexs(kv.first) + " -> identity: " + list(kv.second));

    struct Pair { uint32_t io, eo; std::vector<std::string> hits; };
    std::vector<Pair> pairs;
    for (uint32_t io = 0; io + 4 <= kId; io += 4) {
        size_t ptrs = 0;
        for (size_t i = 0; i < ids.size(); ++i) ptrs += dzHeap(q64(ib, i * kId + (io & ~7u))) && q64(ib, i * kId + (io & ~7u)) >> 32;
        if (ptrs * 2 > ids.size()) continue;
        std::unordered_map<uint32_t, size_t> val;
        for (size_t i = 0; i < ids.size(); ++i) {
            const uint32_t v = d32(ib, i * kId + io);
            if (v >= 2 && v != 0xFFFFFFFF) val[v] = val.count(v) ? SIZE_MAX : i;
        }
        size_t uniq = 0;
        for (const auto& kv : val) uniq += kv.second != SIZE_MAX;
        if (uniq * 10 < ids.size() * 9) continue;
        std::unordered_map<uint32_t, std::vector<std::string>> byEo;
        for (size_t e = 0; e < ents.size(); ++e)
            for (uint32_t eo = 0; eo + 4 <= kEnt; eo += 4) {
                const auto it = val.find(d32(eb, e * kEnt + eo));
                if (it != val.end() && it->second != SIZE_MAX) byEo[eo].push_back(who(e) + "=" + idName[it->second]);
            }
        for (auto& kv : byEo) pairs.push_back({io, kv.first, std::move(kv.second)});
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.hits.size() > b.hits.size(); });
    int shown = 0;
    for (const auto& pr : pairs) {
        if (shown++ >= 12) break;
        say("[*] roster pairing: ID identity +" + hexs(pr.io) + " == entity +" + hexs(pr.eo) + " (" + std::to_string(pr.hits.size()) +
            " survivors): " + list(pr.hits));
    }
    if (idToEnt.empty() && entToId.empty() && pairs.empty())
        say("[*] roster pairing: no pointer or shared id between identities and survivor entities");
    else
        say("[*] roster pairing: ^ the right link names YOU with your own gamertag and the others with real players");
}

void findNetRoster(const Process& p, const Image& img, const DiscoverConfig& cfg,
                   const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    if (img.ptrSize != 8) { say("[*] roster: 64-bit images only"); return; }
    uint64_t world = 0;
    if (!p.readPtr(static_cast<uintptr_t>(img.base + cfg.worldRva), 8, world) || !dzHeap(world)) {
        say("[*] roster: world pointer not resolved (in game?)");
        return;
    }
    uint64_t hub = 0, mid = 0, ident = 0, nameStr = 0;
    std::string myName;
    if (p.read(static_cast<uintptr_t>(world + 0x30), hub) && dzHeap(hub) && p.read(static_cast<uintptr_t>(hub + 0x8), mid) && dzHeap(mid) &&
        p.read(static_cast<uintptr_t>(mid + 0x3E8), ident) && dzHeap(ident) && p.read(static_cast<uintptr_t>(ident + 0xC0), nameStr) &&
        dzHeap(nameStr)) {
        uint8_t tb[64] = {};
        if (p.read(static_cast<uintptr_t>(nameStr + 0x10), tb, sizeof tb)) myName = dzText(tb, sizeof tb);
    }
    if (myName.empty()) {
        say("[*] roster: own identity chain (World+0x30 ...) didn't resolve; searching the NetworkClient without it");
        ident = 0;
    } else {
        say("[*] roster: you are \"" + myName + "\", identity object " + hexs(ident));
    }

    std::vector<VTableInfo> vts;
    std::string err;
    if (!findVTables(img, "NetworkClient", vts, err)) { say("[*] roster: no NetworkClient RTTI (" + err + ")"); return; }
    std::unordered_set<uint64_t> ncVt;
    for (const auto& v : vts) ncVt.insert(static_cast<uint64_t>(img.base) + v.rva);
    std::map<uint64_t, std::vector<uint32_t>> slotsOf;
    for (const auto& sec : img.sections) {
        if (sec.exec() || !(sec.flags & 0x80000000u)) continue;
        std::vector<uint8_t> buf(sec.vsize, 0);
        for (uint32_t o = 0; o < sec.vsize; o += 0x1000)
            p.read(static_cast<uintptr_t>(img.base + sec.rva + o), &buf[o], (std::min)(0x1000u, sec.vsize - o));
        for (uint32_t o = 0; o + 8 <= sec.vsize; o += 8) {
            uint64_t v;
            std::memcpy(&v, &buf[o], 8);
            if (dzHeap(v)) slotsOf[v].push_back(sec.rva + o);
        }
    }
    std::vector<uint64_t> vals;
    for (const auto& kv : slotsOf) vals.push_back(kv.first);
    std::vector<uint64_t> vt(vals.size(), 0);
    std::vector<Process::ReadReq> rq;
    for (size_t i = 0; i < vals.size(); ++i) rq.push_back({static_cast<uintptr_t>(vals[i]), &vt[i], 8});
    p.readMany(rq);
    std::vector<uint64_t> clients;
    for (size_t i = 0; i < vals.size(); ++i)
        if (rq[i].ok && ncVt.count(vt[i])) clients.push_back(vals[i]);
    say("[*] roster: " + std::to_string(vals.size()) + " heap pointers in the module's data, " + std::to_string(clients.size()) +
        " point at a NetworkClient");
    if (clients.empty()) {
        say("[*] roster: no NetworkClient - offline mode has no network client. Join a server and run this again.");
        return;
    }

    if (!ident) {
        constexpr uint32_t kObj = 0x400, kSlots = 0x300, kEnt = 64, kProbe = 4;
        const uint64_t modLo = img.base, modHi = img.base + img.bytes.size();
        auto inMod = [&](uint64_t v) { return v >= modLo && v < modHi; };
        std::vector<std::string> paths;
        std::vector<uint64_t> arrs, holders;
        std::unordered_set<uint64_t> seenArr;
        auto addArr = [&](uint64_t a, const std::string& path, uint64_t holder = 0) {
            if (dzHeap(a) && seenArr.insert(a).second && arrs.size() < 4096) {
                arrs.push_back(a);
                paths.push_back(path);
                holders.push_back(holder);
            }
        };
        for (uint64_t nc : clients) {
            std::vector<uint8_t> nb(kObj, 0);
            p.read(static_cast<uintptr_t>(nc), nb.data(), kObj);
            std::vector<std::pair<uint32_t, uint64_t>> h1;
            for (uint32_t o = 0; o + 8 <= kObj; o += 8) {
                uint64_t v;
                std::memcpy(&v, &nb[o], 8);
                if (!dzHeap(v) || v == nc || inMod(v)) continue;
                h1.push_back({o, v});
                addArr(v, "NetworkClient+" + hexs(o), nc + o);
            }
            std::vector<uint8_t> hb(h1.size() * 0x200, 0);
            rq.clear();
            for (size_t i = 0; i < h1.size(); ++i) rq.push_back({static_cast<uintptr_t>(h1[i].second), &hb[i * 0x200], 0x200});
            p.readMany(rq);
            for (size_t i = 0; i < h1.size(); ++i) {
                if (!rq[i].ok) continue;
                for (uint32_t k = 0; k + 8 <= 0x200; k += 8) {
                    uint64_t v;
                    std::memcpy(&v, &hb[i * 0x200 + k], 8);
                    if (dzHeap(v) && !inMod(v)) addArr(v, "NetworkClient+" + hexs(h1[i].first) + " -> +" + hexs(k));
                }
            }
        }
        std::vector<uint64_t> ab(arrs.size() * kEnt, 0);
        rq.clear();
        for (size_t i = 0; i < arrs.size(); ++i) rq.push_back({static_cast<uintptr_t>(arrs[i]), &ab[i * kEnt], kEnt * 8});
        p.readMany(rq);
        std::vector<uint64_t> head(arrs.size() * kProbe, 0);
        std::vector<Process::ReadReq> vq;
        for (size_t i = 0; i < arrs.size(); ++i)
            for (uint32_t k = 0; k < kProbe; ++k)
                vq.push_back({static_cast<uintptr_t>(dzHeap(ab[i * kEnt + k]) ? ab[i * kEnt + k] : 0), &head[i * kProbe + k], 8});
        p.readMany(vq);
        std::vector<size_t> cand;
        for (size_t i = 0; i < arrs.size(); ++i) {
            if (!rq[i].ok) continue;
            uint64_t cls = 0;
            int same = 0;
            bool bad = false;
            for (uint32_t k = 0; k < kProbe; ++k) {
                const uint64_t e = ab[i * kEnt + k], v = head[i * kProbe + k];
                if (!dzHeap(e)) continue;
                if (!inMod(v)) { bad = true; break; }
                if (!cls) cls = v;
                if (v == cls) ++same; else { bad = true; break; }
            }
            if (!bad && same >= 1) cand.push_back(i);
        }
        say("[*] roster: " + std::to_string(arrs.size()) + " pointer blocks near the NetworkClient, " + std::to_string(cand.size()) +
            " hold objects of one class; probing them for names");
        int hits = 0;
        size_t best = SIZE_MAX, bestDistinct = 1;
        uint32_t bestSlot = 0;
        for (size_t i : cand) {
            std::vector<uint64_t> ents;
            for (uint32_t k = 0; k < kEnt; ++k)
                if (dzHeap(ab[i * kEnt + k]) && !inMod(ab[i * kEnt + k])) ents.push_back(ab[i * kEnt + k]);
            const size_t np = (std::min<size_t>)(ents.size(), kProbe);
            std::vector<uint8_t> eb(np * kSlots, 0);
            rq.clear();
            for (size_t k = 0; k < np; ++k) rq.push_back({static_cast<uintptr_t>(ents[k]), &eb[k * kSlots], kSlots});
            p.readMany(rq);
            std::vector<std::pair<size_t, uint32_t>> who;
            std::vector<uint8_t> tb;
            std::vector<Process::ReadReq> tq;
            for (size_t k = 0; k < np; ++k)
                for (uint32_t o = 0; o + 8 <= kSlots; o += 8) {
                    uint64_t v;
                    std::memcpy(&v, &eb[k * kSlots + o], 8);
                    if (dzHeap(v) && !inMod(v)) who.push_back({k, o});
                }
            tb.assign(who.size() * 48, 0);
            for (size_t w = 0; w < who.size(); ++w) {
                uint64_t v;
                std::memcpy(&v, &eb[who[w].first * kSlots + who[w].second], 8);
                tq.push_back({static_cast<uintptr_t>(v + 0x10), &tb[w * 48], 48});
            }
            p.readMany(tq);
            std::map<uint32_t, int> slotHits;
            for (size_t w = 0; w < who.size(); ++w) {
                const std::string s = tq[w].ok ? dzText(&tb[w * 48], 48) : std::string();
                int letters = 0;
                for (char c : s) letters += std::isalpha(static_cast<unsigned char>(c)) ? 1 : 0;
                if (letters >= 2) ++slotHits[who[w].second];
            }
            for (const auto& sh : slotHits) {
                std::vector<uint64_t> sp(ents.size(), 0);
                rq.clear();
                for (size_t k = 0; k < ents.size(); ++k) rq.push_back({static_cast<uintptr_t>(ents[k] + sh.first), &sp[k], 8});
                p.readMany(rq);
                std::vector<uint8_t> nb2(ents.size() * 48, 0);
                rq.clear();
                for (size_t k = 0; k < ents.size(); ++k)
                    rq.push_back({static_cast<uintptr_t>(dzHeap(sp[k]) ? sp[k] + 0x10 : 0), &nb2[k * 48], 48});
                p.readMany(rq);
                std::string names;
                int n = 0;
                std::set<std::string> distinct;
                for (size_t k = 0; k < ents.size(); ++k) {
                    const std::string nm = rq[k].ok ? dzText(&nb2[k * 48], 48) : std::string();
                    if (nm.empty()) continue;
                    ++n;
                    distinct.insert(nm);
                    if (names.size() < 600) names += (names.empty() ? "" : ", ") + nm;
                }
                if (n == 0) continue;
                ++hits;
                bool gamertags = false;
                for (const auto& d : distinct) gamertags |= !d.empty() && d.back() != '=';
                if (holders[i] && gamertags && distinct.size() > bestDistinct) {
                    bestDistinct = distinct.size();
                    best = i;
                    bestSlot = sh.first;
                }
                say("[*] roster: candidate " + paths[i] + " = " + hexs(arrs[i]) + ", " + std::to_string(ents.size()) + " entries of " +
                    classNameAt(p, img, ents[0]) + ", name slot +" + hexs(sh.first) + ": " + std::to_string(n) + " named: " + names);
            }
        }
        if (!hits) say("[*] roster: no named object array within two hops of the NetworkClient");
        if (best != SIZE_MAX) pairRoster(p, img, cfg, world, slotsOf, clients, holders[best], arrs[best], bestSlot, say);
        return;
    }

    constexpr uint32_t kNc = 0x800, kArr = 0x800, kDeep = 0x200;
    auto nameOf = [&](uint64_t id) {
        uint64_t s = 0;
        uint8_t tb[64] = {};
        if (!p.read(static_cast<uintptr_t>(id + 0xC0), s) || !dzHeap(s) || !p.read(static_cast<uintptr_t>(s + 0x10), tb, sizeof tb))
            return std::string();
        return dzText(tb, sizeof tb);
    };
    auto reportArray = [&](const std::string& path, uint64_t arr, uint32_t idx) {
        std::vector<uint64_t> ents(256, 0);
        p.read(static_cast<uintptr_t>(arr), ents.data(), ents.size() * 8);
        std::string names;
        int n = 0;
        for (uint64_t e : ents) {
            if (!dzHeap(e)) continue;
            const std::string nm = nameOf(e);
            if (nm.empty()) continue;
            ++n;
            if (names.size() < 600) names += (names.empty() ? "" : ", ") + nm;
        }
        say("[*] roster: FOUND your identity at " + path + " [" + std::to_string(idx) + "]; " + std::to_string(n) +
            " named entries: " + names);
    };
    int found = 0;
    for (uint64_t nc : clients) {
        std::string globals;
        for (uint32_t r : slotsOf[nc]) globals += (globals.empty() ? "" : ", ") + hexs(r);
        say("[*] roster: NetworkClient " + hexs(nc) + " (" + classNameAt(p, img, nc) + "), held by module RVA " + globals);
        std::vector<uint8_t> nb(kNc, 0);
        p.read(static_cast<uintptr_t>(nc), nb.data(), kNc);
        std::vector<std::pair<uint32_t, uint64_t>> h1;
        for (uint32_t o = 0; o + 8 <= kNc; o += 8) {
            uint64_t v;
            std::memcpy(&v, &nb[o], 8);
            if (v == ident) {
                reportArray("NetworkClient+" + hexs(o) + " (inline)", nc + o, 0);
                ++found;
            } else if (dzHeap(v) && v != nc) {
                h1.push_back({o, v});
            }
        }
        std::vector<uint8_t> ab(h1.size() * kArr, 0);
        rq.clear();
        for (size_t i = 0; i < h1.size(); ++i) rq.push_back({static_cast<uintptr_t>(h1[i].second), &ab[i * kArr], kArr});
        p.readMany(rq);
        std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> h2;
        for (size_t i = 0; i < h1.size(); ++i) {
            if (!rq[i].ok) continue;
            for (uint32_t k = 0; k + 8 <= kArr; k += 8) {
                uint64_t v;
                std::memcpy(&v, &ab[i * kArr + k], 8);
                if (v == ident) {
                    uint32_t cnt = 0;
                    if (h1[i].first + 12 <= kNc) std::memcpy(&cnt, &nb[h1[i].first + 8], 4);
                    reportArray("NetworkClient+" + hexs(h1[i].first) + " -> array (+" + hexs(h1[i].first + 8) + " = " +
                                    std::to_string(cnt) + ")",
                                h1[i].second, k / 8);
                    ++found;
                } else if (dzHeap(v) && k < kDeep) {
                    h2.push_back({h1[i].first, k, v});
                }
            }
        }
        if (found) continue;
        std::vector<uint8_t> db(h2.size() * kDeep, 0);
        rq.clear();
        for (size_t i = 0; i < h2.size(); ++i) rq.push_back({static_cast<uintptr_t>(std::get<2>(h2[i])), &db[i * kDeep], kDeep});
        p.readMany(rq);
        for (size_t i = 0; i < h2.size(); ++i) {
            if (!rq[i].ok) continue;
            for (uint32_t k = 0; k + 8 <= kDeep; k += 8) {
                uint64_t v;
                std::memcpy(&v, &db[i * kDeep + k], 8);
                if (v != ident) continue;
                reportArray("NetworkClient+" + hexs(std::get<0>(h2[i])) + " -> +" + hexs(std::get<1>(h2[i])) + " -> array",
                            std::get<2>(h2[i]), k / 8);
                ++found;
            }
        }
    }
    if (!found) say("[*] roster: your identity is in no array within two hops of the NetworkClient");

    const uint64_t me = dzLocalPlayer(p, world);
    if (!me) return;
    std::vector<uint8_t> ib(0x200, 0), eb(0x1800, 0);
    p.read(static_cast<uintptr_t>(ident), ib.data(), ib.size());
    for (uint32_t o = 0; o < eb.size(); o += 0x400) p.read(static_cast<uintptr_t>(me + o), &eb[o], 0x400);
    int pairs = 0;
    for (uint32_t io = 0; io + 4 <= ib.size(); io += 4) {
        uint32_t iv;
        std::memcpy(&iv, &ib[io], 4);
        if (iv < 2 || iv > 0xFFFFFF) continue;
        for (uint32_t eo = 0; eo + 4 <= eb.size() && pairs < 24; eo += 4) {
            uint32_t ev;
            std::memcpy(&ev, &eb[eo], 4);
            if (ev != iv) continue;
            char line[128];
            std::snprintf(line, sizeof line, "    identity +0x%X == entity +0x%X  (value %u)", io, eo, iv);
            say(line);
            ++pairs;
        }
    }
    if (!pairs) say("[*] roster: no 32-bit value shared by your identity and your entity (network id not found)");
    else say("[*] roster: ^ shared values; the network id is the pair that also matches for other players");
}

void findWorldGlobal(const Process& p, const Image& img, const DiscoverConfig& cfg,
                     const std::vector<Entity>& entities, const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) { log(s); };
    auto sayf = [&](const char* fmt, auto... args) {
        char b[256];
        std::snprintf(b, sizeof b, fmt, args...);
        say(b);
    };
    if (img.ptrSize != 8) { say("[!] world finder: 64-bit images only"); return; }
    const uint64_t base = img.base, imgHi = img.base + img.bytes.size();
    auto heapPtr = [&](uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0 && !(v >= base && v < imgHi); };

    std::vector<uint64_t> players;
    for (const auto& e : entities)
        if (heapPtr(e.addr)) players.push_back(e.addr);
    if (players.empty()) { say("[!] world finder: no entity objects to anchor on (spawn in first)"); return; }
    const std::unordered_set<uint64_t> playerSet(players.begin(), players.end());
    sayf("[*] world finder: anchoring on %zu discovered entity object(s)", players.size());

    struct Cand { uint32_t rva; uint64_t world; size_t xrefs; };
    std::vector<Cand> cands;
    auto addCand = [&](uint32_t rva, uint64_t world) {
        for (const auto& c : cands) if (c.rva == rva) return;
        cands.push_back({rva, world, findCodeXrefs(img, rva).size()});
    };

    const uint32_t nearOff = cfg.tables.size() > 0 ? cfg.tables[0].off : 0xF70;
    const uint32_t farOff = cfg.tables.size() > 1 ? cfg.tables[1].off : 0x10B8;
    {
        std::unordered_set<uint64_t> entVts;
        for (uint64_t pl : players) {
            uint64_t vt = 0;
            if (p.readPtr(static_cast<uintptr_t>(pl), 8, vt) && vt >= base && vt < imgHi) entVts.insert(vt);
        }
        std::vector<std::pair<uint32_t, uint64_t>> globals;
        for (const auto& sec : img.sections) {
            if (sec.exec() || !(sec.flags & 0x80000000)) continue;
            const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
            for (uint64_t at = sec.rva; at + 8 <= hi; at += 8) {
                uint64_t v = 0;
                std::memcpy(&v, &img.bytes[static_cast<size_t>(at)], 8);
                if (heapPtr(v)) globals.push_back({static_cast<uint32_t>(at), v});
            }
        }
        std::unordered_map<uint64_t, std::vector<uint32_t>> wvToRvas;
        for (const auto& g : globals) wvToRvas[g.second].push_back(g.first);
        std::vector<uint64_t> wvs;
        wvs.reserve(wvToRvas.size());
        for (const auto& kv : wvToRvas) wvs.push_back(kv.first);
        sayf("[*] method 3 (tables): %zu module globals, %zu distinct target objects; checking Near(+0x%X)/Far(+0x%X)",
             globals.size(), wvs.size(), nearOff, farOff);

        auto headerOk = [&](const uint64_t* hdr, uint64_t& buf, uint32_t& count) {
            buf = hdr[0];
            count = static_cast<uint32_t>(hdr[1] & 0xFFFFFFFF);
            return heapPtr(buf) && count >= 1 && count <= 20000;
        };
        struct Surv { uint64_t wv, nearBuf, farBuf; uint32_t nearCnt, farCnt; };
        std::vector<Surv> surv;
        constexpr size_t kBatch = 16384;
        for (size_t s = 0; s < wvs.size(); s += kBatch) {
            const size_t n = std::min(kBatch, wvs.size() - s);
            std::vector<uint64_t> buf(n * 4);
            std::vector<Process::ReadReq> reqs(n * 2);
            for (size_t i = 0; i < n; ++i) {
                reqs[i * 2] = {static_cast<uintptr_t>(wvs[s + i] + nearOff), &buf[i * 4], 16};
                reqs[i * 2 + 1] = {static_cast<uintptr_t>(wvs[s + i] + farOff), &buf[i * 4 + 2], 16};
            }
            p.readMany(reqs);
            for (size_t i = 0; i < n; ++i) {
                uint64_t nb = 0, fb = 0;
                uint32_t nc = 0, fc = 0;
                const bool nk = reqs[i * 2].ok && headerOk(&buf[i * 4], nb, nc);
                const bool fk = reqs[i * 2 + 1].ok && headerOk(&buf[i * 4 + 2], fb, fc);
                if (nk || fk) surv.push_back({wvs[s + i], nk ? nb : 0, fk ? fb : 0, nk ? nc : 0, fk ? fc : 0});
            }
        }
        sayf("[*] method 3: %zu object(s) with a Near/Far-shaped table; confirming entity vtables", surv.size());

        auto tableHoldsEntity = [&](uint64_t bufAddr, uint32_t count) {
            const uint32_t n = std::min<uint32_t>(count, 16);
            if (!n) return false;
            std::vector<uint64_t> slots(n, 0);
            if (!p.read(static_cast<uintptr_t>(bufAddr), slots.data(), n * 8)) return false;
            std::vector<Process::ReadReq> vreq;
            std::vector<uint64_t> vts(n, 0);
            for (uint32_t i = 0; i < n; ++i)
                if (heapPtr(slots[i])) vreq.push_back({static_cast<uintptr_t>(slots[i]), &vts[i], 8});
            if (vreq.empty()) return false;
            p.readMany(vreq);
            for (uint32_t i = 0; i < n; ++i)
                if (entVts.count(vts[i])) return true;
            return false;
        };
        for (const auto& sv : surv) {
            const bool ok = (sv.nearBuf && tableHoldsEntity(sv.nearBuf, sv.nearCnt)) ||
                            (sv.farBuf && tableHoldsEntity(sv.farBuf, sv.farCnt));
            if (!ok) continue;
            const std::string cls = classNameAt(p, img, sv.wv);
            for (uint32_t rva : wvToRvas[sv.wv]) addCand(rva, sv.wv);
            sayf("[+] method 3: World 0x%llX %s - Near count %u, Far count %u (%zu global(s) point here)",
                 (unsigned long long)sv.wv, cls.empty() ? "(no RTTI)" : ("(" + cls + ")").c_str(), sv.nearCnt, sv.farCnt,
                 wvToRvas[sv.wv].size());
        }
    }

    if (cands.empty() && cfg.localLink) {
        std::unordered_set<uint64_t> innerTargets;
        for (uint64_t pl : players) innerTargets.insert(pl + cfg.localAdj);
        std::unordered_set<uint64_t> linkTargets;
        for (const auto& h : scanPointers(p, img.ptrSize, innerTargets))
            if (h.addr > cfg.localDeref) linkTargets.insert(h.addr - cfg.localDeref);
        if (linkTargets.empty()) {
            say("[*] method 1 (link): no weak-ref holder for the player - the link offsets moved; trying method 2");
        } else {
            std::unordered_set<uint64_t> worldSet;
            for (const auto& h : scanPointers(p, img.ptrSize, linkTargets))
                if (h.addr > cfg.localLink) worldSet.insert(h.addr - cfg.localLink);
            for (const auto& h : scanPointers(p, img.ptrSize, worldSet))
                if (h.addr >= base && h.addr < imgHi) {
                    addCand(static_cast<uint32_t>(h.addr - base), h.value);
                    sayf("[*] method 1 (link): base+0x%X -> World 0x%llX", static_cast<uint32_t>(h.addr - base), (unsigned long long)h.value);
                }
        }
    }

    if (cands.empty()) {
        const auto pslots = scanPointers(p, img.ptrSize, playerSet);
        constexpr int kMaxIndex = 512;
        constexpr size_t kMaxBufTargets = 4000000;
        std::unordered_set<uint64_t> bufTargets;
        for (const auto& h : pslots) {
            for (int k = 0; k <= kMaxIndex; ++k) {
                const uint64_t b = h.addr - uint64_t(k) * 8;
                if (b < 0x10000) break;
                bufTargets.insert(b);
            }
            if (bufTargets.size() > kMaxBufTargets) break;
        }
        std::vector<PtrHit> bslots = scanPointers(p, img.ptrSize, bufTargets);
        std::sort(bslots.begin(), bslots.end(), [](const PtrHit& a, const PtrHit& b) { return a.addr < b.addr; });
        for (const auto& sec : img.sections) {
            if (sec.exec() || !(sec.flags & 0x80000000)) continue;
            const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
            for (uint64_t at = sec.rva; at + 8 <= hi; at += 8) {
                uint64_t wv = 0;
                std::memcpy(&wv, &img.bytes[static_cast<size_t>(at)], 8);
                if (!heapPtr(wv)) continue;
                auto it = std::lower_bound(bslots.begin(), bslots.end(), wv,
                                           [](const PtrHit& h, uint64_t v) { return h.addr < v; });
                for (; it != bslots.end() && it->addr < wv + 0x4000; ++it) {
                    const uint32_t off = static_cast<uint32_t>(it->addr - wv);
                    uint64_t buf = 0;
                    int32_t count = 0;
                    if (!p.readPtr(static_cast<uintptr_t>(wv + off), 8, buf) || buf != it->value) continue;
                    if (!p.read(static_cast<uintptr_t>(wv + off + 8), count) || count <= 0 || count > 20000) continue;
                    addCand(static_cast<uint32_t>(at), wv);
                    sayf("[*] method 2 (table): base+0x%X -> World 0x%llX, table +0x%X (buf 0x%llX, count %d)",
                         static_cast<uint32_t>(at), (unsigned long long)wv, off, (unsigned long long)buf, count);
                }
            }
        }
    }

    if (cands.empty()) {
        say("[!] world finder: could not recover the World global from either method.");
        say("    Last resort: stand among several infected/players and run the normal dump (run_dump.bat).");
        return;
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.xrefs > b.xrefs; });
    for (const auto& c : cands)
        sayf("[+] World global candidate: base+0x%X  ->  World 0x%llX  [%zu code refs]", c.rva, (unsigned long long)c.world, c.xrefs);
    sayf("[+] NEW worldRva = 0x%X   <-- set dz::kWorld (src\\esp.cpp) and the dayz profile (src\\profile.cpp) to this", cands.front().rva);

    std::vector<std::string> tlog;
    scanWorldTables(p, img, cands.front().world, tlog);
    for (const auto& l : tlog) say("[*] " + l);
}
