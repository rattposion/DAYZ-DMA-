#include "process.h"

#include <tlhelp32.h>

#include <cstring>

bool Process::readPtr(uintptr_t addr, uint32_t ptrSize, uint64_t& out) const {
    out = 0;
    if (ptrSize == 4) {
        uint32_t v = 0;
        if (!read(addr, v)) return false;
        out = v;
        return true;
    }
    return read(addr, out);
}

size_t Process::readMany(std::vector<ReadReq>& reqs) const {
    size_t n = 0;
    for (auto& r : reqs) n += (r.ok = read(r.addr, r.out, r.size)) ? 1 : 0;
    return n;
}

namespace {

class LocalProcess : public Process {
public:
    ~LocalProcess() override {
        if (h_) CloseHandle(h_);
    }

    bool attach(const std::string& exeName, std::string& err) override {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            err = "CreateToolhelp32Snapshot(process) failed";
            return false;
        }
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
            if (_stricmp(pe.szExeFile, exeName.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        }
        CloseHandle(snap);
        if (!pid) {
            err = "process '" + exeName + "' is not running";
            return false;
        }

        if (h_) {
            CloseHandle(h_);
            h_ = nullptr;
        }
        h_ = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h_) {
            err = "OpenProcess failed (error " + std::to_string(GetLastError()) +
                  "); try running as the same user / elevated";
            return false;
        }
        pid_ = pid;

        HANDLE msnap = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 10; ++i) {
            msnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (msnap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
            Sleep(50);
        }
        if (msnap == INVALID_HANDLE_VALUE) {
            err = "CreateToolhelp32Snapshot(module) failed (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        MODULEENTRY32 me{};
        me.dwSize = sizeof(me);
        bool found = false;
        for (BOOL ok = Module32First(msnap, &me); ok; ok = Module32Next(msnap, &me)) {
            if (_stricmp(me.szModule, exeName.c_str()) == 0) {
                mod_.name = me.szModule;
                mod_.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                mod_.size = me.modBaseSize;
                found = true;
                break;
            }
        }
        CloseHandle(msnap);
        if (!found) {
            err = "main module not found in process";
            return false;
        }
        return true;
    }

    bool read(uintptr_t addr, void* out, size_t size) const override {
        SIZE_T got = 0;
        return ReadProcessMemory(h_, reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
    }

    bool readable(uintptr_t addr) const override {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        return !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    }

    std::vector<Region> regions(uint64_t maxAddr) const override {
        std::vector<Region> out;
        uint64_t addr = 0;
        while (addr < maxAddr) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQueryEx(h_, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi))) break;
            const uint64_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                Region r;
                r.base = static_cast<uintptr_t>(base);
                r.size = mbi.RegionSize;
                r.protect = mbi.Protect;
                r.type = mbi.Type;
                out.push_back(r);
            }
            const uint64_t next = base + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
        return out;
    }

    bool alive() const override {
        DWORD code = 0;
        return h_ && GetExitCodeProcess(h_, &code) && code == STILL_ACTIVE;
    }

private:
    HANDLE h_ = nullptr;
};

}

std::unique_ptr<Process> makeLocalProcess() { return std::make_unique<LocalProcess>(); }
