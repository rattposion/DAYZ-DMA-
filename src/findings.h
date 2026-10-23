#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Finding {
    enum Kind {
        Global,
        VTable,
        Field,
    };
    Kind kind = Global;
    std::string name;
    uint64_t value = 0;
    std::string type;
    std::string note;
    std::vector<uint32_t> xrefs;
};

struct EntityRow {
    uint64_t addr = 0;
    std::string cls;
    bool local = false;
    std::string name, health, armor, team, weapon, pos;
};
