#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "image.h"

struct VTableInfo {
    uint32_t rva = 0;
    std::string cls;
    bool exact = false;
    int group = 0;
};

bool findVTables(const Image& img, const std::string& cls, std::vector<VTableInfo>& out, std::string& err);

bool rttiRef(const Image& img, uint64_t at, uint32_t& rva);
std::string rttiTypeName(const Image& img, uint32_t tdRva);
