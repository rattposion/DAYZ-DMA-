#include "rtti.h"

#include <algorithm>

namespace {

bool refToRva(const Image& img, uint32_t v, uint32_t& rva) {
    if (img.ptrSize == 8) {
        rva = v;
    } else {
        if (v < img.base) return false;
        rva = v - static_cast<uint32_t>(img.base);
    }
    return img.contains(rva);
}

}

bool rttiRef(const Image& img, uint64_t at, uint32_t& rva) {
    uint32_t v;
    return img.at(at, v) && refToRva(img, v, rva);
}

std::string rttiTypeName(const Image& img, uint32_t tdRva) {
    size_t p = tdRva + 2 * img.ptrSize;
    if (!img.contains(p, 5)) return {};
    const char* s = reinterpret_cast<const char*>(&img.bytes[p]);
    size_t n = 0;
    while (p + n < img.bytes.size() && n < 128 && s[n]) ++n;
    std::string raw(s, n);
    if (raw.rfind(".?AV", 0) != 0 && raw.rfind(".?AU", 0) != 0) return {};
    raw = raw.substr(4);
    auto at = raw.find("@@");
    return at == std::string::npos ? raw : raw.substr(0, at);
}

bool findVTables(const Image& img, const std::string& cls, std::vector<VTableInfo>& out, std::string& err) {
    out.clear();

    uint32_t targetTd = 0;
    bool haveTd = false;
    for (const char* prefix : {".?AV", ".?AU"}) {
        const std::string mangled = prefix + cls + "@@";
        const auto it = std::search(img.bytes.begin(), img.bytes.end(), mangled.begin(), mangled.end());
        if (it != img.bytes.end()) {
            targetTd = static_cast<uint32_t>(it - img.bytes.begin()) - 2 * img.ptrSize;
            haveTd = true;
            break;
        }
    }
    if (!haveTd) {
        err = "no RTTI for class '" + cls + "' in module";
        return false;
    }

    struct Col {
        uint32_t rva;
        std::string cls;
        bool exact;
    };
    std::vector<Col> cols;
    const uint32_t sigWanted = img.ptrSize == 8 ? 1 : 0;
    for (const auto& sec : img.sections) {
        if (sec.exec()) continue;
        const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
        for (uint64_t loc = std::max<uint64_t>(sec.rva, 12); loc + 12 <= hi; loc += 4) {
            uint32_t sig = 0, offset = 0, cdOffset = 0;
            if (!img.at(loc - 12, sig) || sig != sigWanted) continue;
            if (!img.at(loc - 8, offset) || offset > 0x1000) continue;
            if (!img.at(loc - 4, cdOffset) || cdOffset > 0x1000) continue;
            uint32_t tdRva = 0, chdRva = 0;
            if (!rttiRef(img, loc, tdRva) || !rttiRef(img, loc + 4, chdRva)) continue;
            const std::string name = rttiTypeName(img, tdRva);
            if (name.empty()) continue;

            uint32_t chdSig = 0, nBases = 0;
            if (!img.at(chdRva, chdSig) || chdSig != 0) continue;
            if (!img.at(chdRva + 8ull, nBases) || nBases == 0 || nBases > 64) continue;
            uint32_t arrRva = 0;
            if (!rttiRef(img, chdRva + 12ull, arrRva)) continue;

            bool derives = false;
            for (uint32_t i = 0; i < nBases && !derives; ++i) {
                uint32_t bcdRva = 0, baseTd = 0;
                if (!rttiRef(img, arrRva + 4ull * i, bcdRva)) break;
                if (!rttiRef(img, bcdRva, baseTd)) break;
                derives = baseTd == targetTd;
            }
            if (derives && offset == 0) cols.push_back({static_cast<uint32_t>(loc - 12), name, tdRva == targetTd});
        }
    }
    if (cols.empty()) {
        err = "class '" + cls + "' has RTTI but no polymorphic instances (no vtable)";
        return false;
    }

    for (const auto& col : cols) {
        const uint64_t colVa = static_cast<uint64_t>(img.base) + col.rva;
        for (const auto& sec : img.sections) {
            if (sec.exec()) continue;
            const uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(sec.rva) + sec.vsize, img.bytes.size());
            for (uint64_t at = sec.rva; at + img.ptrSize <= hi; at += img.ptrSize) {
                uint64_t v = 0;
                if (img.ptrSize == 4) {
                    uint32_t v32;
                    img.at(at, v32);
                    v = v32;
                } else {
                    img.at(at, v);
                }
                if (v == colVa) {
                    VTableInfo vt;
                    vt.rva = static_cast<uint32_t>(at + img.ptrSize);
                    vt.cls = col.cls;
                    vt.exact = col.exact;
                    out.push_back(vt);
                }
            }
        }
    }
    if (out.empty()) {
        err = "found RTTI for '" + cls + "' but no vtable references it";
        return false;
    }
    return true;
}
