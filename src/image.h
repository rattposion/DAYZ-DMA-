#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "process.h"

struct Section {
    std::string name;
    uint32_t rva = 0;
    uint32_t vsize = 0;
    uint32_t flags = 0;
    bool exec() const { return (flags & 0x20000000) != 0; }
};

struct Image {
    uintptr_t base = 0;
    uint64_t preferredBase = 0;
    uint32_t ptrSize = 4;
    uint32_t timestamp = 0;
    std::vector<uint8_t> bytes;
    std::vector<Section> sections;

    bool load(const Process& p, std::string& err);
#ifndef ICKZA_NO_DIAG
    bool loadFile(const std::string& path, std::string& err);
#endif

    bool contains(uint64_t rva, size_t n = 1) const { return rva + n <= bytes.size(); }

    template <class T>
    bool at(uint64_t rva, T& out) const {
        if (!contains(rva, sizeof(T))) return false;
        std::memcpy(&out, &bytes[static_cast<size_t>(rva)], sizeof(T));
        return true;
    }

    const Section* sectionFor(uint64_t rva) const;
};
