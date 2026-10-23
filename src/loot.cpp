
#include "loot.h"

#include <cctype>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace loot {
namespace {

const Info kInfo[Count] = {
    {"Other", "other", RGB(140, 146, 156)},
    {"Guns", "gun", RGB(255, 70, 70)},
    {"Ammo", "ammo", RGB(255, 165, 30)},
    {"Attachments", "attach", RGB(125, 145, 255)},
    {"Melee", "melee", RGB(255, 125, 95)},
    {"Explosives", "explosive", RGB(255, 70, 210)},
    {"Medical", "medical", RGB(255, 155, 185)},
    {"Food", "food", RGB(160, 225, 70)},
    {"Drink", "drink", RGB(70, 200, 255)},
    {"Tools", "tool", RGB(175, 190, 205)},
    {"Clothing", "clothing", RGB(215, 190, 150)},
    {"Bags", "bag", RGB(195, 140, 90)},
    {"Vehicle parts", "vehicle", RGB(175, 115, 255)},
    {"Base building", "base", RGB(70, 215, 175)},
    {"Rare", "rare", RGB(255, 215, 0)},
};

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Rule {
    int cat;
    const char* pat;
};

bool matches(const std::string& n, const char* pat) {
    if (pat[0] == '=') return n == pat + 1;
    if (pat[0] == '^') return n.rfind(pat + 1, 0) == 0;
    return n.find(pat) != std::string::npos;
}

const Rule kRules[] = {
    {Explosive, "grenade"}, {Explosive, "flashbang"}, {Explosive, "explosive"}, {Explosive, "claymore"},
    {Explosive, "landmine"}, {Explosive, "detonator"}, {Explosive, "=ied"},
    {Attachment, "optic"}, {Attachment, "suppressor"}, {Attachment, "silencer"}, {Attachment, "compensator"},
    {Attachment, "bttstck"}, {Attachment, "buttstock"}, {Attachment, "hndgrd"}, {Attachment, "handguard"},
    {Attachment, "bayonet"}, {Attachment, "universallight"}, {Attachment, "tlrlight"}, {Attachment, "railatt"},
    {Attachment, "carryhandle"}, {Attachment, "bipod"},
    {VehiclePart, "carbattery"}, {VehiclePart, "truckbattery"}, {VehiclePart, "sparkplug"}, {VehiclePart, "glowplug"},
    {VehiclePart, "radiator"}, {VehiclePart, "headlight"}, {VehiclePart, "wheel"}, {VehiclePart, "doors_"},
    {VehiclePart, "_door_"}, {VehiclePart, "hood"}, {VehiclePart, "trunk"}, {VehiclePart, "canistergasoline"},
    {VehiclePart, "tirerepair"}, {VehiclePart, "enginebelt"},
    {Medical, "bandage"}, {Medical, "=rag"}, {Medical, "morphine"}, {Medical, "epinephrine"}, {Medical, "saline"},
    {Medical, "bloodbag"}, {Medical, "bloodtest"}, {Medical, "tetracycline"}, {Medical, "painkiller"},
    {Medical, "charcoal"}, {Medical, "vitamin"}, {Medical, "disinfect"}, {Medical, "iodine"}, {Medical, "purification"},
    {Medical, "splint"}, {Medical, "firstaid"}, {Medical, "defibrillator"}, {Medical, "startkitiv"},
    {Medical, "antichem"}, {Medical, "antipox"}, {Medical, "thermometer"},
    {Drink, "sodacan"}, {Drink, "waterbottle"}, {Drink, "canteen"}, {Drink, "glassbottle"}, {Drink, "filteringbottle"},
    {Drink, "waterpouch"}, {Drink, "vodka"},
    {Melee, "knife"}, {Melee, "machete"}, {Melee, "sword"}, {Melee, "baseballbat"}, {Melee, "cleaver"},
    {Melee, "sickle"}, {Melee, "scythe"}, {Melee, "pitchfork"}, {Melee, "spear"}, {Melee, "brassknuckles"},
    {Melee, "baton"}, {Melee, "cattleprod"}, {Melee, "=pipe"}, {Melee, "mace"}, {Melee, "=broom"},
    {Melee, "hockeystick"}, {Melee, "=crowbar"}, {Melee, "sledgehammer"},
    {Tool, "hatchet"}, {Tool, "axe"}, {Tool, "shovel"}, {Tool, "pliers"}, {Tool, "wrench"}, {Tool, "screwdriver"},
    {Tool, "hammer"}, {Tool, "lockpick"}, {Tool, "hacksaw"}, {Tool, "handsaw"}, {Tool, "matchbox"},
    {Tool, "flashlight"}, {Tool, "headtorch"}, {Tool, "canopener"}, {Tool, "compass"}, {Tool, "chernarusmap"},
    {Tool, "=rope"}, {Tool, "ducttape"}, {Tool, "sewingkit"}, {Tool, "cleaningkit"}, {Tool, "whetstone"},
    {Tool, "battery9v"}, {Tool, "binoculars"}, {Tool, "rangefinder"}, {Tool, "handdrill"}, {Tool, "gasstove"},
    {Tool, "cookingstand"}, {Tool, "fryingpan"}, {Tool, "=pot"}, {Tool, "fishingrod"}, {Tool, "hook"},
    {Tool, "chemlight"}, {Tool, "roadflare"}, {Tool, "personalradio"}, {Tool, "megaphone"}, {Tool, "fireplace"},
    {Tool, "lighter"}, {Tool, "blowtorch"}, {Tool, "=pen"}, {Tool, "=paper"}, {Tool, "snaretrap"}, {Tool, "beartrap"},
    {Base, "fencekit"}, {Base, "watchtowerkit"}, {Base, "shelterkit"}, {Base, "territoryflag"}, {Base, "^flag_"},
    {Base, "codelock"}, {Base, "combinationlock"}, {Base, "barrel"}, {Base, "seachest"}, {Base, "woodencrate"},
    {Base, "tent"}, {Base, "nail"}, {Base, "plank"}, {Base, "woodenlog"}, {Base, "metalplate"}, {Base, "metalwire"},
    {Base, "barbedwire"}, {Base, "camonet"},
    {Food, "beanscan"}, {Food, "peachescan"}, {Food, "spaghettican"}, {Food, "sardinescan"}, {Food, "tunacan"},
    {Food, "dogfoodcan"}, {Food, "catfoodcan"}, {Food, "porkcan"}, {Food, "lunchmeat"}, {Food, "foodcan"},
    {Food, "baconcan"}, {Food, "crabcan"}, {Food, "=rice"}, {Food, "powderedmilk"}, {Food, "cereal"},
    {Food, "crackers"}, {Food, "chips"}, {Food, "honey"}, {Food, "marmalade"}, {Food, "pajka"}, {Food, "=pate"},
    {Food, "brisket"}, {Food, "zagorky"}, {Food, "snack"}, {Food, "=apple"}, {Food, "=pear"}, {Food, "=plum"},
    {Food, "=banana"}, {Food, "=orange"}, {Food, "=kiwi"}, {Food, "=tomato"}, {Food, "pepper"}, {Food, "zucchini"},
    {Food, "pumpkin"}, {Food, "=potato"}, {Food, "mushroom"}, {Food, "meat"}, {Food, "=lard"}, {Food, "=carp"},
    {Food, "mackerel"}, {Food, "bitterling"}, {Food, "walleye"}, {Food, "steelhead"}, {Food, "fillet"},
    {Food, "berry"}, {Food, "cannabis"}, {Food, "=worm"},
};

std::vector<std::pair<int, std::string>> gOverride;

std::mutex gFilterMx;
Filter gFilter;
std::unordered_map<std::string, bool> gListed;

}

const Info& info(int cat) { return kInfo[cat >= 0 && cat < Count ? cat : Other]; }

bool valuable(int cat) {
    switch (cat) {
        case Gun:
        case Ammo:
        case Attachment:
        case Explosive:
        case Bag:
        case Rare: return true;
        default: return false;
    }
}

int classify(int engineCat, const std::string& name) {
    const std::string n = lower(name);
    for (const auto& [cat, pat] : gOverride)
        if (matches(n, pat.c_str())) return cat;
    if (engineCat == Gun) return Gun;
    if (engineCat == Ammo || n.rfind("ammo_", 0) == 0 || n.rfind("mag_", 0) == 0) return Ammo;
    if (engineCat == Clothing)
        return (n.find("bag") != std::string::npos || n.find("backpack") != std::string::npos || n.find("sack") != std::string::npos)
                   ? Bag
                   : Clothing;
    for (const Rule& r : kRules)
        if (matches(n, r.pat)) return r.cat;
    return Other;
}

size_t loadOverrides(const std::string& path) {
    gOverride.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") || !f) return 0;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        s = lower(s.substr(0, s.find('#')));
        char id[32] = {}, pat[128] = {};
        if (sscanf_s(s.c_str(), "%31s %127s", id, unsigned(sizeof id), pat, unsigned(sizeof pat)) != 2) continue;
        int cat = -1;
        for (int c = 0; c < Count; ++c)
            if (std::string(kInfo[c].id) == id) cat = c;
        if (cat < 0) continue;
        std::string p = pat;
        const bool head = p.front() == '*', tail = p.size() > 1 && p.back() == '*';
        if (head) p.erase(0, 1);
        if (tail) p.pop_back();
        if (p.empty()) continue;
        gOverride.push_back({cat, head ? p : tail ? "^" + p : "=" + p});
    }
    std::fclose(f);
    return gOverride.size();
}

bool globMatch(const std::string& pat, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        const char c = char(std::tolower(static_cast<unsigned char>(name[n])));
        if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = n;
        } else if (p < pat.size() && pat[p] == c) {
            ++p;
            ++n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

void setFilter(const Filter& f) {
    Filter clean;
    clean.mode = f.mode >= FilterOff && f.mode <= FilterWhite ? f.mode : FilterOff;
    for (const FilterEntry& e : f.list) {
        const std::string pat = lower(e.pat);
        if (!pat.empty()) clean.list.push_back({pat, e.on});
    }
    std::lock_guard<std::mutex> lk(gFilterMx);
    if (clean == gFilter) return;
    gFilter = std::move(clean);
    gListed.clear();
}

Filter filter() {
    std::lock_guard<std::mutex> lk(gFilterMx);
    return gFilter;
}

bool listed(const std::string& name) {
    std::lock_guard<std::mutex> lk(gFilterMx);
    const auto it = gListed.find(name);
    if (it != gListed.end()) return it->second;
    bool hit = false;
    for (const FilterEntry& e : gFilter.list)
        if (e.on && globMatch(e.pat, name)) {
            hit = true;
            break;
        }
    if (gListed.size() > 20000) gListed.clear();
    gListed.emplace(name, hit);
    return hit;
}

bool visible(unsigned mask, int cat, const std::string& name) {
    int mode;
    {
        std::lock_guard<std::mutex> lk(gFilterMx);
        mode = gFilter.mode;
    }
    if (mode == FilterWhite) return listed(name);
    if (!shown(mask, cat)) return false;
    return mode != FilterBlack || !listed(name);
}

std::string serializeList(const std::vector<FilterEntry>& list) {
    std::string s;
    for (const FilterEntry& e : list) {
        if (e.pat.empty()) continue;
        if (!s.empty()) s += '|';
        if (!e.on) s += '!';
        s += e.pat;
    }
    return s;
}

std::vector<FilterEntry> parseList(const std::string& s) {
    std::vector<FilterEntry> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find('|', a);
        if (b == std::string::npos) b = s.size();
        std::string t = s.substr(a, b - a);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        size_t i = 0;
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        t.erase(0, i);
        FilterEntry e;
        if (!t.empty() && t[0] == '!') {
            e.on = false;
            t.erase(0, 1);
        }
        e.pat = lower(t);
        if (!e.pat.empty()) out.push_back(std::move(e));
        a = b + 1;
    }
    return out;
}

}
