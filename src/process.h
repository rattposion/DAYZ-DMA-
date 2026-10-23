#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ModuleInfo {
    std::string name;
    uintptr_t base = 0;
    size_t size = 0;
};

class Process {
public:
    struct Region {
        uintptr_t base = 0;
        size_t size = 0;
        DWORD protect = 0;
        DWORD type = 0;
    };

    virtual ~Process() = default;

    virtual bool attach(const std::string& exeName, std::string& err) = 0;
    virtual bool read(uintptr_t addr, void* out, size_t size) const = 0;
    virtual bool readable(uintptr_t addr) const = 0;
    virtual std::vector<Region> regions(uint64_t maxAddr) const = 0;
    virtual bool alive() const = 0;

    template <class T>
    bool read(uintptr_t addr, T& out) const { return read(addr, &out, sizeof(T)); }

    struct ReadReq {
        uintptr_t addr = 0;
        void* out = nullptr;
        size_t size = 0;
        bool ok = false;
    };
    virtual size_t readMany(std::vector<ReadReq>& reqs) const;

    virtual void maintain() {}

    bool readPtr(uintptr_t addr, uint32_t ptrSize, uint64_t& out) const;

    DWORD pid() const { return pid_; }
    const ModuleInfo& module() const { return mod_; }

protected:
    DWORD pid_ = 0;
    ModuleInfo mod_;
};

std::unique_ptr<Process> makeLocalProcess();

void markUrgentReadThread();
void markBackgroundReadThread();

std::unique_ptr<Process> makeDmaProcess(const std::string& device, const std::string& dllDir, bool ownRefresh = false);
