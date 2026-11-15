// label_test.cpp - offline check of the trigger-key label logic (no DMA, no GUI).
// Same formula as src/radar.cpp keyLabel(): scan code in bits 16-23, extended bit 24, Pause special case.
#include <windows.h>

#include <cstdio>
#include <cstring>

// VKs whose scan code is extended (E0 prefix); GetKeyNameText needs bit 24 set for these.
static bool vkExtended(int vk) {
    switch (vk) {
        case VK_RCONTROL: case VK_RMENU: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT: case VK_APPS: case VK_LWIN: case VK_RWIN:
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK: case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE:
        case VK_BROWSER_BACK: case VK_BROWSER_FORWARD: case VK_BROWSER_REFRESH: case VK_BROWSER_STOP:
        case VK_BROWSER_SEARCH: case VK_BROWSER_FAVORITES: case VK_BROWSER_HOME:
            return true;
        default:
            return false;
    }
}

static void label(char* out, size_t n, int k) {
    if (k >= 1 && k <= 5) {
        static const char* const m[6] = {"", "Left mouse", "Right mouse", "Middle mouse", "Mouse 4", "Mouse 5"};
        std::snprintf(out, n, "%s", m[k]);
        return;
    }
    if (k <= 0) {
        std::snprintf(out, n, "none");
        return;
    }
    char nm[48] = {};
    UINT sc = MapVirtualKeyA((UINT)k, MAPVK_VK_TO_VSC_EX);
    if (k == VK_PAUSE) sc = 0x45;
    if (sc) {
        LONG lp = LONG(sc & 0xFF) << 16;
        if ((sc >> 8) == 0xE0 || vkExtended(k)) lp |= 1L << 24;
        if (GetKeyNameTextA(lp, nm, (int)sizeof nm) > 0 && nm[0]) {
            std::snprintf(out, n, "%s (0x%02X)", nm, k);
            return;
        }
    }
    std::snprintf(out, n, "code 0x%02X (%d)", k, k);
}

struct Case {
    int vk;
    const char* a;  // akzeptierte Namen (deutsche ODER englische Systemsprache)
    const char* b;
};

int main() {
    const Case cases[] = {
        {0x45, "E", ""},          {0x57, "W", ""},        {0x10, "UMSCHALT", "SHIFT"}, {0x0D, "EINGABE", "ENTER"},
        {0x09, "TAB", ""},        {0x13, "PAUSE", ""},    {0x20, "LEER", "SPACE"},     {0x25, "LINKS", "LEFT"},
        {0x2D, "EINFG", "INSERT"},{0x60, "0", ""},        {1, "Left mouse", ""},       {4, "Mouse 4", ""},
    };
    int bad = 0;
    for (const Case& c : cases) {
        char b[64] = {};
        label(b, sizeof b, c.vk);
        const bool ok = std::strstr(b, c.a) != nullptr || (c.b[0] && std::strstr(b, c.b) != nullptr);
        if (!ok) ++bad;
        std::printf("%-6s vk=0x%02X -> \"%s\"\n", ok ? "OK" : "FEHLER", (unsigned)c.vk, b);
    }
    std::printf("\n%d von %d fehlerhaft\n", bad, (int)(sizeof cases / sizeof cases[0]));
    return bad ? 1 : 0;
}
