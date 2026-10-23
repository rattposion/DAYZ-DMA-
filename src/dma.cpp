#include "process.h"
#include "clock.h"
#include "telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma warning(push)
#pragma warning(disable : 4200 4201 4324)
#include <vmmdll.h>
#pragma warning(pop)

namespace {

using PfnInitialize = VMM_HANDLE (*)(DWORD, LPCSTR*);
using PfnClose = VOID (*)(VMM_HANDLE);
using PfnMemFree = VOID (*)(PVOID);
using PfnPidGetFromName = BOOL (*)(VMM_HANDLE, LPCSTR, PDWORD);
using PfnMemReadEx = BOOL (*)(VMM_HANDLE, DWORD, ULONG64, PBYTE, DWORD, PDWORD, ULONG64);
using PfnGetModuleFromNameU = BOOL (*)(VMM_HANDLE, DWORD, LPCSTR, PVMMDLL_MAP_MODULEENTRY*, DWORD);
using PfnGetVadU = BOOL (*)(VMM_HANDLE, DWORD, BOOL, PVMMDLL_MAP_VAD*);
using PfnScatterInitialize = VMMDLL_SCATTER_HANDLE (*)(VMM_HANDLE, DWORD, DWORD);
using PfnScatterPrepareEx = BOOL (*)(VMMDLL_SCATTER_HANDLE, QWORD, DWORD, PBYTE, PDWORD);
using PfnScatterExecuteRead = BOOL (*)(VMMDLL_SCATTER_HANDLE);
using PfnScatterCloseHandle = VOID (*)(VMMDLL_SCATTER_HANDLE);
using PfnConfigSet = BOOL (*)(VMM_HANDLE, ULONG64, ULONG64);
using PfnMemReadScatter = DWORD (*)(VMM_HANDLE, DWORD, PPMEM_SCATTER, DWORD, DWORD);
using PfnVirt2Phys = BOOL (*)(VMM_HANDLE, DWORD, ULONG64, PULONG64);
using PfnGetPhysMem = BOOL (*)(VMM_HANDLE, PVMMDLL_MAP_PHYSMEM*);

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = ~uint64_t(0xFFF);
constexpr DWORD kPidPhysical = (DWORD)-1;
constexpr size_t kChunkPages = 48;
constexpr size_t kChunkPagesBg = 16;

enum : int { kPrioBg = 0, kPrioNormal = 1, kPrioUrgent = 2 };
thread_local int tlPrio = kPrioNormal;
thread_local bool tlUrgent = false;

std::string exeDirectory() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p = buf;
    return p.substr(0, p.find_last_of("\\/"));
}

bool fileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

DWORD toPageProtect(DWORD vadProt) {
    switch (vadProt & 7) {
        case 1: return PAGE_READONLY;
        case 2: return PAGE_EXECUTE;
        case 3: return PAGE_EXECUTE_READ;
        case 4: return PAGE_READWRITE;
        case 5: return PAGE_WRITECOPY;
        case 6: return PAGE_EXECUTE_READWRITE;
        case 7: return PAGE_EXECUTE_WRITECOPY;
        default: return PAGE_NOACCESS;
    }
}

thread_local int tlHeld = 0;
struct TimedLock {
    std::recursive_mutex& m;
#ifndef ICKZA_NO_DIAG
    uint64_t waitUs = 0;
#endif
    explicit TimedLock(std::recursive_mutex& mx) : m(mx) {
#ifndef ICKZA_NO_DIAG
        if (!m.try_lock()) {
            const uint64_t t0 = qpcUs();
            m.lock();
            waitUs = qpcUs() - t0;
        }
#else
        m.lock();
#endif
        ++tlHeld;
    }
    ~TimedLock() {
        --tlHeld;
        m.unlock();
    }
};

struct UrgentMark {
    std::atomic<int>& n;
    bool on;
    explicit UrgentMark(std::atomic<int>* c) : n(c[tlPrio]), on(tlPrio > kPrioBg) {
        if (on) n.fetch_add(1);
    }
    void done() {
        if (on) n.fetch_sub(1);
        on = false;
    }
    ~UrgentMark() { done(); }
};

class DmaProcess : public Process {
public:
    DmaProcess(std::string device, std::string dllDir, bool ownRefresh)
        : device_(std::move(device)), dllDir_(std::move(dllDir)), ownRefresh_(ownRefresh), noPaging_(ownRefresh) {}

    void maintain() override {
        TimedLock lk(mx_);
        const ULONGLONG now = GetTickCount64();
        if (physRead_ && now - lastPteFlush_ >= 1000) {
            lastPteFlush_ = now;
            pteCache_.clear();
            tel::count(tel::C_PTE_FLUSH);
        }
        if (!ownRefresh_ || !configSet_ || !vmm_ || now - lastTlb_ < 1000) return;
        lastTlb_ = now;
        pteCache_.clear();
        const uint64_t t0 = qpcUs();
        configSet_(vmm_, VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL, 1);
        tel::time(tel::T_REFRESH, qpcUs() - t0);
        tel::count(tel::C_REFRESHES);
    }

    ~DmaProcess() override {
        if (vmm_ && close_) close_(vmm_);
        if (dll_) FreeLibrary(dll_);
    }

    bool attach(const std::string& exeName, std::string& err) override {
        if (!vmm_ && !init(err)) return false;

        DWORD pid = 0;
        if (!pidFromName_(vmm_, exeName.c_str(), &pid)) {
            const ULONGLONG now = GetTickCount64();
            bool found = false;
            if (ownRefresh_ && configSet_ && now - lastProcRefresh_ >= 2000) {
                lastProcRefresh_ = now;
                configSet_(vmm_, VMMDLL_OPT_REFRESH_FREQ_MEDIUM, 1);
                found = pidFromName_(vmm_, exeName.c_str(), &pid) != FALSE;
            }
            if (!found) {
                err = exeName + " not running";
                return false;
            }
        }
        PVMMDLL_MAP_MODULEENTRY entry = nullptr;
        if (!moduleFromName_(vmm_, pid, exeName.c_str(), &entry, VMMDLL_MODULE_FLAG_NORMAL) || !entry) {
            err = "main module not found yet";
            return false;
        }
        mod_.name = entry->uszText ? entry->uszText : exeName;
        mod_.base = static_cast<uintptr_t>(entry->vaBase);
        mod_.size = entry->cbImageSize;
        memFree_(entry);
        pid_ = pid;
        pteCache_.clear();
        captureAliveProbes();
        return true;
    }

    bool read(uintptr_t addr, void* out, size_t size) const override {
        const uint64_t t0 = qpcUs();
        yieldToHigher();
        UrgentMark um(waiting_);
        TimedLock lk(mx_);
        um.done();
        if (!vmm_ || !pid_) return false;
        bool ok;
        if (physRead_) {
            ReadReq r{addr, out, size, false};
            readPhys(&r, 1);
            ok = r.ok;
        } else {
            ok = readVirt(addr, out, size);
            tel::count(tel::C_SCATTERS);
        }
#ifndef ICKZA_NO_DIAG
        tel::CallInfo ci;
        ci.single = true;
        ci.reqs = 1;
        ci.reqsOk = ok ? 1 : 0;
        ci.bytes = size;
        ci.bytesOk = ok ? size : 0;
        ci.totalUs = qpcUs() - t0;
        ci.lockUs = lk.waitUs;
        tel::call(ci);
#endif
        return ok;
    }

    size_t readMany(std::vector<ReadReq>& reqs) const override {
        const uint64_t t0 = qpcUs();
        if (reqs.empty()) return 0;
        size_t n = 0;
#ifndef ICKZA_NO_DIAG
        uint64_t lockUs = 0;
#endif
        std::vector<uint64_t> pages;
        std::vector<ReadReq*> deferred;
        for (size_t begin = 0; begin < reqs.size();) {
            size_t end = reqs.size();
            if (!tlUrgent) {
                pages.clear();
                for (end = begin; end < reqs.size(); ++end) {
                    const ReadReq& r = reqs[end];
                    const uint64_t first = uint64_t(r.addr) & kPageMask;
                    const uint64_t last = r.size ? (uint64_t(r.addr) + r.size - 1) & kPageMask : first;
                    size_t added = 0;
                    for (uint64_t va = first; va <= last; va += kPageSize)
                        if (std::find(pages.begin(), pages.end(), va) == pages.end()) ++added;
                    if (end > begin && pages.size() + added > (tlPrio == kPrioBg ? kChunkPagesBg : kChunkPages)) break;
                    for (uint64_t va = first; va <= last; va += kPageSize)
                        if (std::find(pages.begin(), pages.end(), va) == pages.end()) pages.push_back(va);
                }
            }
            yieldToHigher();
            UrgentMark um(waiting_);
            TimedLock lk(mx_);
            um.done();
#ifndef ICKZA_NO_DIAG
            lockUs += lk.waitUs;
#endif
            if (!vmm_ || !pid_) return n;
            n += readManyLocked(reqs.data() + begin, end - begin, &deferred);
            begin = end;
        }
        for (size_t begin = 0; begin < deferred.size();) {
            const size_t end = tlUrgent ? deferred.size() : std::min(deferred.size(), begin + (tlPrio == kPrioBg ? kChunkPagesBg : kChunkPages));
            yieldToHigher();
            UrgentMark um(waiting_);
            TimedLock lk(mx_);
            um.done();
#ifndef ICKZA_NO_DIAG
            lockUs += lk.waitUs;
#endif
            if (!vmm_ || !pid_) return n;
            n += readVirtBatchLocked(deferred.data() + begin, end - begin);
            begin = end;
        }
#ifndef ICKZA_NO_DIAG
        tel::CallInfo ci;
        ci.reqs = static_cast<uint32_t>(reqs.size());
        for (const ReadReq& r : reqs) {
            ci.bytes += r.size;
            if (r.ok) {
                ++ci.reqsOk;
                ci.bytesOk += r.size;
            }
        }
        ci.totalUs = qpcUs() - t0;
        ci.lockUs = lockUs;
        tel::call(ci);
#endif
        return n;
    }

    bool readable(uintptr_t addr) const override {
        uint8_t b;
        return read(addr, &b, 1);
    }

    std::vector<Region> regions(uint64_t maxAddr) const override {
        TimedLock lk(mx_);
        std::vector<Region> out;
        if (!vmm_ || !pid_) return out;
        PVMMDLL_MAP_VAD vad = nullptr;
        if (!vadU_(vmm_, pid_, FALSE, &vad) || !vad) return out;
        for (DWORD i = 0; i < vad->cMap; ++i) {
            const VMMDLL_MAP_VADENTRY& e = vad->pMap[i];
            if (e.vaStart >= maxAddr || e.vaEnd < e.vaStart) continue;
            Region r;
            r.base = static_cast<uintptr_t>(e.vaStart);
            r.size = static_cast<size_t>(e.vaEnd - e.vaStart + 1);
            r.protect = toPageProtect(e.Protection);
            if (e.Protection & 0x10) r.protect |= PAGE_GUARD;
            r.type = e.fImage ? MEM_IMAGE : e.fPrivateMemory ? MEM_PRIVATE : MEM_MAPPED;
            if (r.protect & PAGE_GUARD || r.protect == PAGE_NOACCESS) continue;
            out.push_back(r);
        }
        memFree_(vad);
        return out;
    }

    bool alive() const override {
        TimedLock lk(mx_);
        if (!vmm_ || !pid_ || !mod_.base) return false;
        struct PagingOk {
            bool& f;
            explicit PagingOk(bool& b) : f(b) { f = true; }
            ~PagingOk() { f = false; }
        } po(pagingOk_);
        uint16_t mz = 0;
        if (read(mod_.base, &mz, sizeof mz) && mz == 0x5A4D) return true;
        for (const auto& p : probes_) {
            uint8_t b[sizeof p.bytes];
            if (read(p.addr, b, sizeof b) && memcmp(b, p.bytes, sizeof b) == 0) return true;
        }
        return false;
    }

private:
    struct AliveProbe {
        uintptr_t addr;
        uint8_t bytes[16];
    };

    void captureAliveProbes() {
        constexpr int kProbes = 16;
        probes_.clear();
        uint8_t hdr[0x1000];
        if (!read(mod_.base, hdr, sizeof hdr)) return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hdr);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > sizeof hdr) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(hdr + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        const int nsec = nt->FileHeader.NumberOfSections;
        if (reinterpret_cast<const uint8_t*>(sec + nsec) > hdr + sizeof hdr) return;
        std::vector<std::pair<uintptr_t, size_t>> ranges;
        size_t total = 0;
        for (int i = 0; i < nsec; ++i) {
            const DWORD c = sec[i].Characteristics;
            if (c & IMAGE_SCN_MEM_WRITE) continue;
            if (!(c & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_INITIALIZED_DATA))) continue;
            const size_t len = std::min<size_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
            if (len < 0x1000) continue;
            ranges.push_back({mod_.base + sec[i].VirtualAddress, len});
            total += len;
        }
        if (!total) return;
        const size_t step = total / (kProbes * 2);
        for (int k = 0; k < kProbes * 2 && int(probes_.size()) < kProbes; ++k) {
            size_t off = step * size_t(k) + 0x800;
            for (const auto& r : ranges) {
                if (off < r.second) {
                    AliveProbe p;
                    p.addr = r.first + off;
                    if (off + 16 <= r.second && read(p.addr, p.bytes, sizeof p.bytes)) probes_.push_back(p);
                    break;
                }
                off -= r.second;
            }
        }
    }

    DWORD virtFlags() const { return VMMDLL_FLAG_NOCACHE | (noPaging_ && !pagingOk_ ? VMMDLL_FLAG_NOPAGING : 0); }

    bool readVirt(uintptr_t addr, void* out, size_t size) const {
        DWORD got = 0;
        const BOOL ok = readEx_(vmm_, pid_, addr, static_cast<PBYTE>(out), static_cast<DWORD>(size), &got, virtFlags());
        return ok && got == size;
    }

    size_t readManyLocked(ReadReq* reqs, size_t count, std::vector<ReadReq*>* deferVirt = nullptr) const {
        if (physRead_) return readPhys(reqs, count, deferVirt);
        size_t n = 0;
        VMMDLL_SCATTER_HANDLE hs = scatterInit_ ? scatterInit_(vmm_, pid_, VMMDLL_FLAG_NOCACHE) : nullptr;
        if (!hs) {
            for (size_t i = 0; i < count; ++i) n += (reqs[i].ok = readVirt(reqs[i].addr, reqs[i].out, reqs[i].size)) ? 1 : 0;
            return n;
        }
        std::vector<DWORD> got(count, 0);
        for (size_t i = 0; i < count; ++i)
            scatterPrepareEx_(hs, reqs[i].addr, static_cast<DWORD>(reqs[i].size), static_cast<PBYTE>(reqs[i].out), &got[i]);
        const uint64_t t0 = qpcUs();
        scatterExecuteRead_(hs);
        tel::time(tel::T_DEVICE, qpcUs() - t0);
        tel::count(tel::C_SCATTERS);
        scatterClose_(hs);
        for (size_t i = 0; i < count; ++i) n += (reqs[i].ok = got[i] == reqs[i].size) ? 1 : 0;
        return n;
    }

    bool higherWaiting() const {
        for (int p = tlPrio + 1; p <= kPrioUrgent; ++p)
            if (waiting_[p].load(std::memory_order_relaxed) > 0) return true;
        return false;
    }
    void yieldToHigher() const {
        if (tlUrgent || tlHeld) return;
        if (!higherWaiting()) return;
        const uint64_t t0 = qpcUs();
        while (higherWaiting() && qpcUs() - t0 < 2000) std::this_thread::yield();
    }

    uint64_t vaToPa(uint64_t vaPage) const {
        auto it = pteCache_.find(vaPage);
        if (it != pteCache_.end()) return it->second;
        uint64_t pa = 0;
        if (!virt2phys_(vmm_, pid_, vaPage, &pa)) pa = 0;
        pa &= kPageMask;
        pteCache_.emplace(vaPage, pa);
        return pa;
    }

    size_t readPhys(ReadReq* reqs, size_t count, std::vector<ReadReq*>* deferVirt = nullptr) const {
        std::unordered_map<uint64_t, size_t> frameSlot;
        std::vector<uint64_t> frames;
        std::vector<char> needVirt(count, 0);

        const uint64_t tx0 = qpcUs();
        const size_t cached0 = pteCache_.size();
        size_t looks = 0;
        for (size_t i = 0; i < count; ++i) {
            reqs[i].ok = false;
            if (!reqs[i].size || !reqs[i].out) { reqs[i].ok = (reqs[i].size == 0); continue; }
            const uint64_t firstVa = uint64_t(reqs[i].addr) & kPageMask;
            const uint64_t lastVa = (uint64_t(reqs[i].addr) + reqs[i].size - 1) & kPageMask;
            for (uint64_t va = firstVa; va <= lastVa; va += kPageSize) {
                ++looks;
                const uint64_t pa = vaToPa(va);
                if (!pa) { needVirt[i] = 1; break; }
                if (frameSlot.emplace(pa, frames.size()).second) frames.push_back(pa);
            }
        }
        tel::time(tel::T_XLATE, qpcUs() - tx0);
        tel::count(tel::C_PTE_HIT, looks - (pteCache_.size() - cached0));
        tel::count(tel::C_PTE_MISS, pteCache_.size() - cached0);

        if (!frames.empty()) {
            std::vector<uint8_t> buf(frames.size() * kPageSize);
            std::vector<MEM_SCATTER> mems(frames.size());
            std::vector<PMEM_SCATTER> pp(frames.size());
            for (size_t s = 0; s < frames.size(); ++s) {
                MEM_SCATTER& m = mems[s];
                std::memset(&m, 0, sizeof m);
                m.version = MEM_SCATTER_VERSION;
                m.qwA = frames[s];
                m.pb = buf.data() + s * kPageSize;
                m.cb = static_cast<DWORD>(kPageSize);
                pp[s] = &m;
            }
            const uint64_t td0 = qpcUs();
            scatterRead_(vmm_, kPidPhysical, pp.data(), static_cast<DWORD>(pp.size()), VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT);
            tel::time(tel::T_DEVICE, qpcUs() - td0);
            tel::count(tel::C_SCATTERS);
            tel::count(tel::C_PAGES, frames.size());
            uint64_t pagesFailed = 0;
            for (const MEM_SCATTER& m : mems) pagesFailed += m.f ? 0 : 1;
            tel::count(tel::C_PAGES_FAIL, pagesFailed);

            for (size_t i = 0; i < count; ++i) {
                if (needVirt[i] || !reqs[i].size) continue;
                uint8_t* dst = static_cast<uint8_t*>(reqs[i].out);
                uint64_t va = reqs[i].addr;
                size_t left = reqs[i].size;
                bool ok = true;
                while (left) {
                    const uint64_t pa = vaToPa(va & kPageMask);
                    const size_t off = static_cast<size_t>(va & 0xFFF);
                    const size_t n = std::min(left, static_cast<size_t>(kPageSize - off));
                    const auto slot = frameSlot.find(pa);
                    if (!pa || slot == frameSlot.end() || !mems[slot->second].f) { ok = false; break; }
                    std::memcpy(dst, buf.data() + slot->second * kPageSize + off, n);
                    dst += n; va += n; left -= n;
                }
                if (ok) reqs[i].ok = true; else needVirt[i] = 1;
            }
        }

        size_t ok = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!reqs[i].ok && needVirt[i] && reqs[i].size && reqs[i].out) {
                if (deferVirt) {
                    deferVirt->push_back(&reqs[i]);
                    continue;
                }
                reqs[i].ok = readVirt(reqs[i].addr, reqs[i].out, reqs[i].size);
                tel::count(tel::C_VIRT_FB);
                if (!reqs[i].ok) tel::count(tel::C_VIRT_FB_FAIL);
            }
            if (reqs[i].ok) ++ok;
        }
        return ok;
    }

    size_t readVirtBatchLocked(ReadReq* const* rs, size_t count) const {
        size_t n = 0;
        VMMDLL_SCATTER_HANDLE hs = scatterInit_ ? scatterInit_(vmm_, pid_, virtFlags()) : nullptr;
        if (!hs) {
            for (size_t i = 0; i < count; ++i) n += (rs[i]->ok = readVirt(rs[i]->addr, rs[i]->out, rs[i]->size)) ? 1 : 0;
        } else {
            std::vector<DWORD> got(count, 0);
            for (size_t i = 0; i < count; ++i)
                scatterPrepareEx_(hs, rs[i]->addr, static_cast<DWORD>(rs[i]->size), static_cast<PBYTE>(rs[i]->out), &got[i]);
            const uint64_t t0 = qpcUs();
            scatterExecuteRead_(hs);
            tel::time(tel::T_VIRT, qpcUs() - t0);
            scatterClose_(hs);
            for (size_t i = 0; i < count; ++i) n += (rs[i]->ok = got[i] == rs[i]->size) ? 1 : 0;
        }
        tel::count(tel::C_VIRT_FB, count);
        tel::count(tel::C_VIRT_FB_FAIL, count - n);
        return n;
    }

    bool writeMemMap(const std::string& path) {
        const auto getPhysMem = reinterpret_cast<PfnGetPhysMem>(GetProcAddress(dll_, "VMMDLL_Map_GetPhysMem"));
        PVMMDLL_MAP_PHYSMEM map = nullptr;
        if (!getPhysMem || !getPhysMem(vmm_, &map) || !map) return false;
        bool ok = false;
        if (map->dwVersion == VMMDLL_MAP_PHYSMEM_VERSION && map->cMap > 0) {
            FILE* f = nullptr;
            if (!fopen_s(&f, path.c_str(), "w") && f) {
                for (DWORD i = 0; i < map->cMap; ++i)
                    std::fprintf(f, "%016llx - %016llx\n", static_cast<unsigned long long>(map->pMap[i].pa),
                                 static_cast<unsigned long long>(map->pMap[i].pa + map->pMap[i].cb - 1));
                ok = std::fclose(f) == 0;
            }
        }
        memFree_(map);
        return ok;
    }

    bool init(std::string& err) {
        std::vector<std::string> dirs;
        if (!dllDir_.empty()) dirs.push_back(dllDir_);
        dirs.push_back(exeDirectory() + "\\dma");
        dirs.push_back(exeDirectory());
        for (const auto& d : dirs) {
            const std::string path = d + "\\vmm.dll";
            if (fileExists(path)) {
                dll_ = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (dll_) break;
                err = "fatal: can't load " + path + " (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
        }
        if (!dll_) dll_ = LoadLibraryA("vmm.dll");
        if (!dll_) {
            err = "fatal: vmm.dll not found";
            return false;
        }

        auto sym = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(dll_, name)); };
        initialize_ = reinterpret_cast<PfnInitialize>(sym("VMMDLL_Initialize"));
        close_ = reinterpret_cast<PfnClose>(sym("VMMDLL_Close"));
        memFree_ = reinterpret_cast<PfnMemFree>(sym("VMMDLL_MemFree"));
        pidFromName_ = reinterpret_cast<PfnPidGetFromName>(sym("VMMDLL_PidGetFromName"));
        readEx_ = reinterpret_cast<PfnMemReadEx>(sym("VMMDLL_MemReadEx"));
        moduleFromName_ = reinterpret_cast<PfnGetModuleFromNameU>(sym("VMMDLL_Map_GetModuleFromNameU"));
        vadU_ = reinterpret_cast<PfnGetVadU>(sym("VMMDLL_Map_GetVadU"));
        if (!initialize_ || !close_ || !memFree_ || !pidFromName_ || !readEx_ || !moduleFromName_ || !vadU_) {
            err = "fatal: wrong vmm.dll version";
            return false;
        }
        scatterInit_ = reinterpret_cast<PfnScatterInitialize>(sym("VMMDLL_Scatter_Initialize"));
        scatterPrepareEx_ = reinterpret_cast<PfnScatterPrepareEx>(sym("VMMDLL_Scatter_PrepareEx"));
        scatterExecuteRead_ = reinterpret_cast<PfnScatterExecuteRead>(sym("VMMDLL_Scatter_ExecuteRead"));
        scatterClose_ = reinterpret_cast<PfnScatterCloseHandle>(sym("VMMDLL_Scatter_CloseHandle"));
        if (!scatterPrepareEx_ || !scatterExecuteRead_ || !scatterClose_) scatterInit_ = nullptr;
        scatterRead_ = reinterpret_cast<PfnMemReadScatter>(sym("VMMDLL_MemReadScatter"));
        virt2phys_ = reinterpret_cast<PfnVirt2Phys>(sym("VMMDLL_MemVirt2Phys"));
        physRead_ = scatterRead_ && virt2phys_;
        configSet_ = reinterpret_cast<PfnConfigSet>(sym("VMMDLL_ConfigSet"));
        if (!configSet_) ownRefresh_ = false;

        std::string dev = device_.empty() ? "fpga" : device_;
        const std::string mmap = exeDirectory() + "\\mmap.txt";
        const auto open = [&](const std::string& memmap) {
            std::vector<LPCSTR> argv = {"", "-device", dev.c_str(), "-waitinitialize"};
            if (!memmap.empty()) {
                argv.push_back("-memmap");
                argv.push_back(memmap.c_str());
            }
            if (ownRefresh_) argv.push_back("-norefresh");
            return initialize_(DWORD(argv.size()), argv.data());
        };
        bool haveMap = fileExists(mmap);
        if (haveMap) vmm_ = open(mmap);
        if (!vmm_) {
            vmm_ = open("");
            if (vmm_ && haveMap) DeleteFileA(mmap.c_str());
            haveMap = false;
        }
        if (vmm_ && !haveMap && writeMemMap(mmap)) {
            close_(vmm_);
            vmm_ = open(mmap);
            haveMap = vmm_ != nullptr;
            if (!vmm_) {
                vmm_ = open("");
                if (vmm_) DeleteFileA(mmap.c_str());
            }
        }
        lastTlb_ = lastPteFlush_ = GetTickCount64();
        if (!vmm_) {
            err = "fatal: MemProcFS init failed (" + dev + ")";
            return false;
        }
        return true;
    }

    std::string device_, dllDir_;
    bool ownRefresh_ = false;
    bool noPaging_ = false;
    mutable bool pagingOk_ = false;
    ULONGLONG lastProcRefresh_ = 0;
    bool physRead_ = false;
    ULONGLONG lastTlb_ = 0, lastPteFlush_ = 0;
    mutable std::unordered_map<uint64_t, uint64_t> pteCache_;
    std::vector<AliveProbe> probes_;
    mutable std::recursive_mutex mx_;
    mutable std::atomic<int> waiting_[3] = {};
    PfnConfigSet configSet_ = nullptr;
    HMODULE dll_ = nullptr;
    VMM_HANDLE vmm_ = nullptr;
    PfnInitialize initialize_ = nullptr;
    PfnClose close_ = nullptr;
    PfnMemFree memFree_ = nullptr;
    PfnPidGetFromName pidFromName_ = nullptr;
    PfnMemReadEx readEx_ = nullptr;
    PfnGetModuleFromNameU moduleFromName_ = nullptr;
    PfnGetVadU vadU_ = nullptr;
    PfnScatterInitialize scatterInit_ = nullptr;
    PfnScatterPrepareEx scatterPrepareEx_ = nullptr;
    PfnScatterExecuteRead scatterExecuteRead_ = nullptr;
    PfnScatterCloseHandle scatterClose_ = nullptr;
    PfnMemReadScatter scatterRead_ = nullptr;
    PfnVirt2Phys virt2phys_ = nullptr;
};

}

void markUrgentReadThread() {
    tlPrio = kPrioUrgent;
    tlUrgent = true;
}
void markBackgroundReadThread() { tlPrio = kPrioBg; }

std::unique_ptr<Process> makeDmaProcess(const std::string& device, const std::string& dllDir, bool ownRefresh) {
    return std::make_unique<DmaProcess>(device, dllDir, ownRefresh);
}
