#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "crosshair.h"
#include "loot.h"
#include "webmap.h"

struct EspSettings {
    bool dma = true;
    std::string device = "fpga";
    std::string dmaDir;
    bool fullscreen = true;
    std::string displayDevice;
    long displayW = 0, displayH = 0;

    bool skeleton = true, box = false, names = true, info = true, fps = true;
    int predictMs = 12;
    bool uncapped = true;
    int dzRange = 1000;
    bool dzLoot = false;
    int dzLootRange = 150;
#ifndef HIRSCHGANGWARE_NO_DIAG
    bool dzBoneIds = false;
#endif
    bool dzAnimals = false, dzVehicles = true;
    bool dzLootValuables = false;
    bool dzHeld = true;
    bool dzEspHealth = false, dzRadarHealth = false;
#ifndef HIRSCHGANGWARE_NO_DIAG
    std::string dzNameAnchor;
#endif
    bool dzRadar = false;
    int dzRadarRange = 300;
    int dzRadarSize = 200;
    std::string dzMap = "chernarusplus";
    bool dzMapAuto = true;
    bool dzMapTopo = false;
    bool dzMapHigh = false;
    bool dzRadarScreen = false;
    bool dzSideOpen = true;
    bool dzMenu = false;
    int dzRadarZoom = 600;
    bool dzInfected = true;
    bool dzRadarNames = true;
    bool dzRadarEvents = false;
    bool dzRadarCorpses = true;
    int dzRadarLootLabel = 250;
    float dzRadarMarker = 1.0f;
    unsigned dzLootCats = loot::kAll;
    unsigned dzRadarStashLoot = loot::kStashLoot;
    bool dzRadarHeadings = true;
    int dzRadarHeadingLen = 150;
    int dzFilterMode = 0;
    std::string dzFilterList;
    webmap::Options webmap;
    bool crosshair = true;
    int crosshairSize = 5;
    crosshair::Style crosshairStyle = crosshair::Style::Plus;
    COLORREF crosshairColor = crosshair::kDefaultColor;
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
    std::function<bool()> stopping;
};
HWND createPanel(EspSettings& s, PanelHooks hooks);
