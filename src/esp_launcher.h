#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct EspSettings {
    bool dma = false;
    std::string device = "fpga";
    std::string dmaDir;
    bool fullscreen = true;
    std::string displayDevice;
    long displayW = 0, displayH = 0;

    bool skeleton = true, box = false, names = true, info = true, fps = true;
    int predictMs = 12;
    bool uncapped = true;
    int dzRange = 400;
    bool dzLoot = false;
    int dzLootRange = 150;
#ifndef ICKZA_NO_DIAG
    bool dzBoneIds = false;
#endif
    bool dzAnimals = false, dzVehicles = true;
    bool dzLootValuables = false;
    bool dzHeld = true;
#ifndef ICKZA_NO_DIAG
    std::string dzNameAnchor;
#endif
    bool dzRadar = false;
    int dzRadarRange = 300;
    int dzRadarSize = 360;
    std::string dzMap = "chernarusplus";
    bool dzMapAuto = true;
    bool dzMapTopo = false;
    bool dzMapHigh = false;
    bool dzRadarScreen = false;
    int dzRadarZoom = 600;
    bool dzInfected = true;
    bool dzRadarNames = true;
    int dzRadarLootLabel = 250;
    float dzRadarMarker = 1.0f;
    unsigned dzLootCats = 0x7FFF;
    bool dzRadarHeadings = true;
    int dzRadarHeadingLen = 150;
    int dzFilterMode = 0;
    std::string dzFilterList;
    bool crosshair = true;
    int crosshairSize = 10;
};

struct DisplayInfo {
    int number = 0;
    RECT rc{};
    std::string device;
    bool primary = false;
    long width() const { return rc.right - rc.left; }
    long height() const { return rc.bottom - rc.top; }
};

std::vector<DisplayInfo> listDisplays();
const DisplayInfo* findDisplay(const std::vector<DisplayInfo>& displays, const EspSettings& s);
const DisplayInfo* pickDisplay(const std::vector<DisplayInfo>& displays, const EspSettings& s);
void identifyDisplays(const std::vector<DisplayInfo>& displays);

void loadSettings(EspSettings& s);
void saveSettings(const EspSettings& s);

struct PanelInfo {
    unsigned long pid = 0;
    std::string server;
    double mbps = 0;
};

struct PanelHooks {
    std::function<void()> start, stop;
    std::function<bool()> running;
    std::function<PanelInfo()> info;
};
HWND createPanel(EspSettings& s, PanelHooks hooks);
