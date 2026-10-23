#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct FieldConfig {
    enum Angles { Degrees, Radians };
    enum Health { IntPair, Float };
    enum Name { InlineChars, StringPtr };

    Angles angles = Degrees;
    Health health = IntPair;
    float healthMax = 100;
    Name name = InlineChars;
    bool team = true;
    bool rotation = false;

    int followDepth = 0;
    size_t childSize = 0x100;
    size_t maxChildren = 256;
    size_t maxEntities = 16;
};

struct DiscoverConfig {
    bool requireGlobal = true;
    bool chains = false;
    size_t maxEntities = 0;

    struct Table {
        std::string name;
        uint32_t off = 0;
    };
    uint32_t worldRva = 0;
    std::vector<Table> tables;
    uint32_t localLink = 0;
    uint32_t localDeref = 0;
    uint32_t localAdj = 0;
};

struct GameProfile {
    std::string id;
    std::string title;
    std::string process;
    std::string cls;
    std::string weaponCls;
    std::string outSub;
    size_t objSize = 0x600;
    std::string hint;
    std::string empty;
    FieldConfig fields;
    DiscoverConfig disc;
};

const std::vector<GameProfile>& gameProfiles();
const GameProfile* findGame(const std::string& id);
