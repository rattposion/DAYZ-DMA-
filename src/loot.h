#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace loot {

enum Cat : int {
    Other,
    Gun,
    Ammo,
    Attachment,
    Melee,
    Explosive,
    Medical,
    Food,
    Drink,
    Tool,
    Clothing,
    Bag,
    VehiclePart,
    Base,
    Rare,
    Count
};

constexpr unsigned kAll = (1u << Count) - 1;

struct Info {
    const char* name;
    const char* id;
    COLORREF color;
};
const Info& info(int cat);

inline bool shown(unsigned mask, int cat) { return cat >= 0 && cat < Count && (mask >> cat & 1u) != 0; }

bool valuable(int cat);

int classify(int engineCat, const std::string& name);

size_t loadOverrides(const std::string& path);

enum FilterMode : int { FilterOff, FilterBlack, FilterWhite };

struct FilterEntry {
    std::string pat;
    bool on = true;
    bool operator==(const FilterEntry& o) const { return on == o.on && pat == o.pat; }
};

struct Filter {
    int mode = FilterOff;
    std::vector<FilterEntry> list;
    bool operator==(const Filter& o) const { return mode == o.mode && list == o.list; }
    bool operator!=(const Filter& o) const { return !(*this == o); }
};

bool globMatch(const std::string& pat, const std::string& name);

void setFilter(const Filter& f);
Filter filter();
bool listed(const std::string& name);

bool visible(unsigned mask, int cat, const std::string& name);

std::string serializeList(const std::vector<FilterEntry>& list);
std::vector<FilterEntry> parseList(const std::string& s);

}
