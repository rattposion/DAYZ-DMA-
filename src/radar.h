#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "loot.h"

namespace radar {

constexpr int kMaxRange = 900;

enum class Icon { Survivor, Infected, Animal, Vehicle, Loot };

struct Dot {
    float x = 0, z = 0;
    COLORREF color = RGB(255, 255, 255);
    Icon icon = Icon::Survivor;
    int cat = loot::Other;
    float fwdX = 0, fwdZ = 0;
    std::string label;
};

struct Place {
    float x = 0, z = 0;
    int tier = 3;
    COLORREF color = RGB(255, 255, 255);
    std::string name;
};

struct Online {
    std::string name;
    float dist = -1;
    bool me = false;
};

struct Frame {
    bool ok = false;
    float meX = 0, meZ = 0;
    float fwdX = 0, fwdZ = 0;
    std::string meName;
    std::string mapBase;
    float mapSize = 15360;
    std::vector<Dot> dots;
    std::vector<Place> places;
    std::vector<Online> online;
};

struct Config {
    bool loot = true;
    unsigned lootMask = ::loot::kAll;
    ::loot::Filter filter;
    bool animals = true;
    bool vehicles = true;
    bool infected = true;
    bool names = true;
    bool headings = true;
    int headingLen = 150;
    int lootLabelRange = 250;
    float markerScale = 1.0f;
    bool mapAuto = true;
    std::string map;
    std::string mapDetected;
    bool mapTopo = false;
    bool mapHigh = false;
    struct MapItem {
        std::string id, label;
        bool installed = false;
        bool official = false;
    };
    std::vector<MapItem> mapList;
    struct MapDl {
        bool busy = false;
        std::string label, phase, error;
        int done = 0, total = 0;
        float mbps = 0;
    } dl;
    std::string downloadReq;
    bool cancelReq = false;
    struct Esp {
        bool skeleton = true, box = false, names = true, distance = true;
        bool fps = true, uncapped = true;
        bool held = true;
        bool valuables = false;
        bool minimap = false;
        int range = 400;
        bool crosshair = true;
        int crosshairSize = 10;
    } esp;
    bool showSetup = false;
    bool changed = false;
};

bool sync(bool& want, int& zoomMetres, Config& cfg, const Frame& frame, const RECT* target);

void destroy();

bool visible();

void reloadMaps();

#ifndef ICKZA_NO_DIAG
void setLog(void (*fn)(const char*));
#endif

}
