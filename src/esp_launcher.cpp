#include "esp_launcher.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr UINT kIdentifyMs = 2500;
constexpr char kSection[] = "HirschgangWare";
constexpr char kIdentifyClass[] = "hirschgangware_identify";
std::vector<HWND> gIdentify;

std::string settingsPath(const char* file) {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p = buf;
    return p.substr(0, p.find_last_of("\\/") + 1) + file;
}

std::string iniPath() { return settingsPath("HirschgangWare.ini"); }

bool hasNewSettings() { return GetFileAttributesA(iniPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string readIniPath() {
    return hasNewSettings() ? iniPath() : settingsPath("Ickza.ini");
}

const char* readSection() { return hasNewSettings() ? kSection : "Ickza"; }

LRESULT CALLBACK identifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        HFONT big = CreateFontA(-(rc.bottom * 3 / 4), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HGDIOBJ old = SelectObject(hdc, big);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        char text[16];
        std::snprintf(text, sizeof text, "%d", static_cast<int>(GetWindowLongPtrA(hwnd, GWLP_USERDATA)));
        DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(big);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void closeIdentify() {
    for (HWND w : gIdentify) DestroyWindow(w);
    gIdentify.clear();
}

bool readBool(const char* key, bool def) { return GetPrivateProfileIntA(readSection(), key, def ? 1 : 0, readIniPath().c_str()) != 0; }

std::string readString(const char* key, const std::string& def) {
    char buf[8192];
    GetPrivateProfileStringA(readSection(), key, def.c_str(), buf, sizeof buf, readIniPath().c_str());
    return buf;
}

void writeString(const char* key, const std::string& v) { WritePrivateProfileStringA(kSection, key, v.c_str(), iniPath().c_str()); }
void writeBool(const char* key, bool v) { writeString(key, v ? "1" : "0"); }

}

std::vector<DisplayInfo> listDisplays() {
    std::vector<DisplayInfo> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXA mi{};
            mi.cbSize = sizeof mi;
            GetMonitorInfoA(mon, &mi);
            DisplayInfo d;
            d.rc = mi.rcMonitor;
            d.device = mi.szDevice;
            d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            reinterpret_cast<std::vector<DisplayInfo>*>(lp)->push_back(d);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        return a.rc.left != b.rc.left ? a.rc.left < b.rc.left : a.rc.top < b.rc.top;
    });
    for (size_t i = 0; i < out.size(); ++i) out[i].number = static_cast<int>(i) + 1;
    return out;
}

const DisplayInfo* findDisplay(const std::vector<DisplayInfo>& displays, const EspSettings& s) {
    if (!s.displayDevice.empty())
        for (const auto& d : displays)
            if (_stricmp(d.device.c_str(), s.displayDevice.c_str()) == 0) return &d;
    if (s.displayW > 0 && s.displayH > 0)
        for (const auto& d : displays)
            if (d.width() == s.displayW && d.height() == s.displayH) return &d;
    return nullptr;
}

const DisplayInfo* pickDisplay(const std::vector<DisplayInfo>& displays, const EspSettings& s) {
    if (const DisplayInfo* d = findDisplay(displays, s)) return d;
    for (const auto& d : displays)
        if (!d.primary) return &d;
    return displays.empty() ? nullptr : &displays.front();
}

void identifyDisplays(const std::vector<DisplayInfo>& displays) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSA ic{};
        ic.lpfnWndProc = identifyProc;
        ic.hInstance = GetModuleHandleA(nullptr);
        ic.lpszClassName = kIdentifyClass;
        RegisterClassA(&ic);
        registered = true;
    }
    closeIdentify();
    for (const auto& d : displays) {
        const int side = std::min(d.width(), d.height()) / 3;
        const int x = (d.rc.left + d.rc.right - side) / 2, y = (d.rc.top + d.rc.bottom - side) / 2;
        HWND w = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kIdentifyClass, "", WS_POPUP, x, y, side, side,
                                 nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!w) continue;
        SetWindowLongPtrA(w, GWLP_USERDATA, d.number);
        SetWindowPos(w, HWND_TOPMOST, x, y, side, side, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        gIdentify.push_back(w);
    }
    SetTimer(nullptr, 0, kIdentifyMs, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        closeIdentify();
    });
}

void loadSettings(EspSettings& s) {
    s.dma = readBool("dma", s.dma);
    s.device = readString("device", s.device);
    s.dmaDir = readString("dma_dir", s.dmaDir);
    s.fullscreen = readBool("fullscreen", s.fullscreen);
    s.displayDevice = readString("display_device", s.displayDevice);
    s.displayW = GetPrivateProfileIntA(readSection(), "display_width", s.displayW, readIniPath().c_str());
    s.displayH = GetPrivateProfileIntA(readSection(), "display_height", s.displayH, readIniPath().c_str());
    s.skeleton = readBool("skeleton", s.skeleton);
    s.fps = readBool("fps", s.fps);
    s.uncapped = readBool("uncapped", s.uncapped);
    s.predictMs = GetPrivateProfileIntA(readSection(), "predict_ms", s.predictMs, readIniPath().c_str());
    s.dzRange = GetPrivateProfileIntA(readSection(), "dz_range", s.dzRange, readIniPath().c_str());
    s.dzRange = s.dzRange <= 0 ? 1000 : std::clamp(int(s.dzRange), 25, 1500);
    s.dzLoot = readBool("dz_loot", s.dzLoot);
    s.dzLootRange = GetPrivateProfileIntA(readSection(), "dz_loot_range", s.dzLootRange, readIniPath().c_str());
#ifndef HIRSCHGANGWARE_NO_DIAG
    s.dzBoneIds = readBool("dz_bone_ids", s.dzBoneIds);
#endif
    s.dzAnimals = readBool("dz_animals", s.dzAnimals);
    s.dzVehicles = readBool("dz_vehicles", s.dzVehicles);
    s.dzLootValuables = readBool("dz_loot_valuables", s.dzLootValuables);
    s.dzHeld = readBool("dz_held", s.dzHeld);
#ifndef HIRSCHGANGWARE_NO_DIAG
    s.dzNameAnchor = readString("dz_name_anchor", s.dzNameAnchor);
#endif
    s.dzRadar = readBool("dz_radar", s.dzRadar);
    s.dzRadarRange = GetPrivateProfileIntA(readSection(), "dz_radar_range", s.dzRadarRange, readIniPath().c_str());
    s.dzRadarSize = GetPrivateProfileIntA(readSection(), "dz_radar_size", s.dzRadarSize, readIniPath().c_str());
    s.dzMap = readString("dz_map", s.dzMap);
    s.dzMapAuto = readBool("dz_map_auto", s.dzMapAuto);
    s.dzMapTopo = readBool("dz_map_topo", s.dzMapTopo);
    s.dzMapHigh = readBool("dz_map_high", s.dzMapHigh);
    s.dzRadarScreen = readBool("dz_radar_screen", s.dzRadarScreen);
    s.dzSideOpen = readBool("dz_side_open", s.dzSideOpen);
    s.dzMenu = readBool("dz_menu", s.dzMenu);
    s.dzRadarZoom = GetPrivateProfileIntA(readSection(), "dz_radar_zoom", s.dzRadarZoom, readIniPath().c_str());
    s.dzInfected = readBool("dz_infected", readBool("dz_radar_infected", s.dzInfected));
    s.dzRadarNames = readBool("dz_radar_names", s.dzRadarNames);
    s.dzRadarLootLabel = GetPrivateProfileIntA(readSection(), "dz_radar_loot_label", s.dzRadarLootLabel, readIniPath().c_str());
    s.dzRadarMarker = GetPrivateProfileIntA(readSection(), "dz_radar_marker_x100", int(s.dzRadarMarker * 100 + 0.5f), readIniPath().c_str()) / 100.f;
    s.dzLootCats = unsigned(std::strtoul(readString("dz_loot_cats", std::to_string(s.dzLootCats)).c_str(), nullptr, 10));
    s.dzRadarHeadings = readBool("dz_radar_headings", s.dzRadarHeadings);
    s.dzRadarHeadingLen = GetPrivateProfileIntA(readSection(), "dz_radar_heading_len", s.dzRadarHeadingLen, readIniPath().c_str());
    s.dzFilterMode = GetPrivateProfileIntA(readSection(), "dz_filter_mode", s.dzFilterMode, readIniPath().c_str());
    s.dzFilterList = readString("dz_filter_list", s.dzFilterList);
    s.box = readBool("box", s.box);
    s.names = readBool("names", s.names);
    s.info = readBool("info", s.info);
    s.crosshair = readBool("crosshair", s.crosshair);
    s.webmap.enabled = readBool("webmap_enabled", s.webmap.enabled);
    s.webmap.port = std::clamp(int(GetPrivateProfileIntA(readSection(), "webmap_port", s.webmap.port, readIniPath().c_str())), 1024, 65535);
    s.webmap.hz = std::clamp(int(GetPrivateProfileIntA(readSection(), "webmap_hz", s.webmap.hz, readIniPath().c_str())), 1, 20);
    s.webmap.release = GetPrivateProfileIntA(readSection(), "webmap_release", 0, readIniPath().c_str()) & webmap::AllReleases;
    s.webmap.publicHost = readString("webmap_public_host", "");
    s.crosshairSize = GetPrivateProfileIntA(readSection(), "crosshair_size", s.crosshairSize, readIniPath().c_str());
}

void saveSettings(const EspSettings& s) {
    writeBool("dma", s.dma);
    writeString("device", s.device);
    writeString("dma_dir", s.dmaDir);
    writeBool("fullscreen", s.fullscreen);
    writeString("display_device", s.displayDevice);
    writeString("display_width", std::to_string(s.displayW));
    writeString("display_height", std::to_string(s.displayH));
    writeBool("skeleton", s.skeleton);
    writeBool("fps", s.fps);
    writeBool("uncapped", s.uncapped);
    writeString("predict_ms", std::to_string(s.predictMs));
    writeString("dz_range", std::to_string(s.dzRange));
    writeBool("dz_loot", s.dzLoot);
    writeString("dz_loot_range", std::to_string(s.dzLootRange));
#ifndef HIRSCHGANGWARE_NO_DIAG
    writeBool("dz_bone_ids", s.dzBoneIds);
#endif
    writeBool("dz_animals", s.dzAnimals);
    writeBool("dz_vehicles", s.dzVehicles);
    writeBool("dz_loot_valuables", s.dzLootValuables);
    writeBool("dz_held", s.dzHeld);
#ifndef HIRSCHGANGWARE_NO_DIAG
    writeString("dz_name_anchor", s.dzNameAnchor);
#endif
    writeBool("dz_radar", s.dzRadar);
    writeString("dz_radar_range", std::to_string(s.dzRadarRange));
    writeString("dz_radar_size", std::to_string(s.dzRadarSize));
    writeString("dz_map", s.dzMap);
    writeBool("dz_map_auto", s.dzMapAuto);
    writeBool("dz_map_topo", s.dzMapTopo);
    writeBool("dz_map_high", s.dzMapHigh);
    writeBool("dz_radar_screen", s.dzRadarScreen);
    writeBool("dz_side_open", s.dzSideOpen);
    writeBool("dz_menu", s.dzMenu);
    writeString("dz_radar_zoom", std::to_string(s.dzRadarZoom));
    writeBool("dz_infected", s.dzInfected);
    writeBool("dz_radar_names", s.dzRadarNames);
    writeString("dz_radar_loot_label", std::to_string(s.dzRadarLootLabel));
    writeString("dz_radar_marker_x100", std::to_string(int(s.dzRadarMarker * 100 + 0.5f)));
    writeString("dz_loot_cats", std::to_string(s.dzLootCats));
    writeBool("dz_radar_headings", s.dzRadarHeadings);
    writeString("dz_radar_heading_len", std::to_string(s.dzRadarHeadingLen));
    writeString("dz_filter_mode", std::to_string(s.dzFilterMode));
    writeString("dz_filter_list", s.dzFilterList);
    writeBool("box", s.box);
    writeBool("names", s.names);
    writeBool("info", s.info);
    writeBool("webmap_enabled", s.webmap.enabled);
    writeString("webmap_port", std::to_string(s.webmap.port));
    writeString("webmap_hz", std::to_string(s.webmap.hz));
    writeString("webmap_release", std::to_string(s.webmap.release));
    writeString("webmap_public_host", s.webmap.publicHost);
    writeBool("crosshair", s.crosshair);
    writeString("crosshair_size", std::to_string(s.crosshairSize));
}

