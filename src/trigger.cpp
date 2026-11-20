#include "trigger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace trigger {

namespace {

Config gCfg;
Live gLive;
std::string gStatus = "no device";
HANDLE gSerial = INVALID_HANDLE_VALUE;
SOCKET gSock = INVALID_SOCKET;
sockaddr_in gAddr{};
bool gWsa = false;
bool gCapture = false;
std::array<bool, 2> gWaitForBoundRelease{};
int gCaptureSlot = 0;
bool gCaptureArmed = false;
ULONGLONG gLastBindAt = 0;
ULONGLONG gLastEscapeCancelAt = 0;
void (*gLog)(const char*) = nullptr;
std::string gIdent;
bool gAwaitIdent = false;
std::string gLastLoggedRx;
ULONGLONG gLastRxLog = 0;
// MAKCU binary frame receive state
int gRxState = 0;
uint8_t gFrameCmd = 0;
uint16_t gFrameLen = 0, gFrameGot = 0;
uint8_t gFrameBuf[96] = {};
int gKeyHid = 0;  // HID usage of the last key event from the box
std::array<bool, 256> gBoxHidDown{};
ULONGLONG gLastStreamOn = 0;
bool gStreamLost = false;

void setStatusFwd(const std::string& s);

void tlog(const char* fmt, ...) {
    if (!gLog) return;
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    gLog(b);
}
bool gDirty = false;
ULONGLONG gDirtyAt = 0, gLastRetry = 0;
std::string gRxLine;
bool gBtn[8] = {};
bool gKeyHeld = false;
int gKeyCode = 0;
bool gHaveArmed = false;
uint64_t gArmedId = 0;
ULONGLONG gArmedAt = 0, gDownAt = 0, gLastShot = 0;
ULONGLONG gOutputReleasedAt = 0;

std::string exeDir() {
    char b[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, b, MAX_PATH);
    std::string p(b, n ? n : 0);
    const size_t s = p.find_last_of("\\/");
    p.resize(s == std::string::npos ? 0 : s + 1);
    return p;
}
std::string iniPath() { return exeDir() + "trigger.ini"; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
int toInt(const std::string& s, int dflt) { return s.empty() ? dflt : std::atoi(s.c_str()); }
bool toBool(const std::string& s, bool dflt) {
    if (s.empty()) return dflt;
    const std::string v = lower(trim(s));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool writeLine(const std::string& s) {
    if (gSerial == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const std::string out = s + "\r\n";
    return WriteFile(gSerial, out.data(), DWORD(out.size()), &n, nullptr) && n == out.size();
}

std::string subst(std::string s, const std::string& key, const std::string& val) {
    for (size_t p = s.find(key); p != std::string::npos; p = s.find(key, p + val.size())) s.replace(p, key.size(), val);
    return s;
}

bool deviceSend(const std::string& cmd, int vk = 0) {
    if (cmd.empty()) return false;
    std::string s = cmd;
    if (vk) {
        char b[16];
        std::snprintf(b, sizeof b, "%d", vk);
        s = subst(s, "{vk}", b);
    }
    if (!gCfg.modeNet) return writeLine(s);
    if (gSock == INVALID_SOCKET) return false;
    return sendto(gSock, s.data(), int(s.size()), 0, reinterpret_cast<sockaddr*>(&gAddr), sizeof gAddr) == int(s.size());
}

void setStatus(const std::string& s) {
    gStatus = s;
    tlog("[trg] status: %s", s.c_str());
}

void bindKey(int key, int hid, InputSource source);

bool boxInputEnabled(int slot = 0) {
    const auto b = binding(slot);
    return bindableKey(b.key) && (b.source == InputSource::Box ||
           (b.source == InputSource::Legacy && !gCfg.keyRemote && gCfg.keyFromDevice));
}

void captureBox(int key, int hid) {
    if (gCapture && key == VK_ESCAPE) { cancelCaptureEscape(); return; }
    if (gCapture && gCaptureArmed && gCfg.keyFromDevice && !remoteKeysReady() &&
        ((key >= 1 && key <= 5) || dma_keyboard::validKey(unsigned(key))))
        bindKey(key, hid, InputSource::Box);
}

// ---- input parsing (tolerant; raw lines are shown in the module window) ----
void parseToken(const std::string& rawName, const std::string& rawVal) {
    const std::string name = lower(trim(rawName));
    const std::string val = lower(trim(rawVal));
    if (name.empty()) return;
    const bool on = !(val.empty() || val == "0" || val == "up" || val == "release" || val == "off" || val == "false");

    int btn = 0;
    if (name.find("side1") != std::string::npos || name.find("x1") != std::string::npos || name.find("mouse4") != std::string::npos) btn = 4;
    else if (name.find("side2") != std::string::npos || name.find("x2") != std::string::npos || name.find("mouse5") != std::string::npos) btn = 5;
    else if (name.find("middle") != std::string::npos || name.find("mid") != std::string::npos) btn = 3;
    else if (name.find("left") != std::string::npos) btn = 1;
    else if (name.find("right") != std::string::npos) btn = 2;
    if (btn) {
        gBtn[btn] = on;
        if (on) captureBox(btn, 0);
        return;
    }
    if (name.find("key") != std::string::npos || name.find("vk") != std::string::npos) {
        std::string cs = val;
        if (cs.empty()) {
            const size_t po = name.find('('), pc = name.find(')');
            if (po != std::string::npos && pc > po) cs = name.substr(po + 1, pc - po - 1);
        }
        const int code = cs.empty() ? 0 : (int)std::strtol(cs.c_str(), nullptr, 0);
        if (code > 0) {
            if (!val.empty()) gKeyHeld = on;
            gKeyCode = code;
            if (on) captureBox(code, 0);
        }
    }
}

void parseLine(const std::string& line) {
    const std::string l = trim(line);
    if (l.empty()) return;
    size_t eq = l.find('=');
    const size_t colon = l.find(':');
    if (eq == std::string::npos || (colon != std::string::npos && colon < eq)) eq = colon;
    if (eq == std::string::npos) {
        bool digits = !l.empty();
        for (const char c : l)
            if (!std::isdigit((unsigned char)c)) digits = false;
        if (digits) {
            gKeyHeld = l != "0";
            gKeyCode = 0;
            return;
        }
        parseToken(l, std::string());
    } else {
        parseToken(l.substr(0, eq), l.substr(eq + 1));
    }
}

// ---- MAKCU binary protocol (frames: DE AD len:u16LE cmd payload) ----
struct KvPair {
    int hid, vk;
};
const KvPair kHidVk[] = {
    {0x04, 0x41}, {0x05, 0x42}, {0x06, 0x43}, {0x07, 0x44}, {0x08, 0x45}, {0x09, 0x46}, {0x0A, 0x47}, {0x0B, 0x48},
    {0x0C, 0x49}, {0x0D, 0x4A}, {0x0E, 0x4B}, {0x0F, 0x4C}, {0x10, 0x4D}, {0x11, 0x4E}, {0x12, 0x4F}, {0x13, 0x50},
    {0x14, 0x51}, {0x15, 0x52}, {0x16, 0x53}, {0x17, 0x54}, {0x18, 0x55}, {0x19, 0x56}, {0x1A, 0x57}, {0x1B, 0x58},
    {0x1C, 0x59}, {0x1D, 0x5A},
    {0x1E, 0x31}, {0x1F, 0x32}, {0x20, 0x33}, {0x21, 0x34}, {0x22, 0x35}, {0x23, 0x36}, {0x24, 0x37}, {0x25, 0x38},
    {0x26, 0x39}, {0x27, 0x30},
    {0x28, 0x0D}, {0x29, 0x1B}, {0x2A, 0x08}, {0x2B, 0x09}, {0x2C, 0x20}, {0x2D, 0xBD}, {0x2E, 0xBB}, {0x2F, 0xDB},
    {0x30, 0xDD}, {0x31, 0xDC}, {0x32, 0xDC}, {0x33, 0xBA}, {0x34, 0xDE}, {0x35, 0x60}, {0x36, 0xBC}, {0x37, 0xBE},
    {0x38, 0xBF}, {0x39, 0x14},
    {0x3A, 0x70}, {0x3B, 0x71}, {0x3C, 0x72}, {0x3D, 0x73}, {0x3E, 0x74}, {0x3F, 0x75}, {0x40, 0x76}, {0x41, 0x77},
    {0x42, 0x78}, {0x43, 0x79}, {0x44, 0x7A}, {0x45, 0x7B},
    {0x46, 0x7C}, {0x47, 0x7D}, {0x48, 0x7E}, {0x49, 0x7F}, {0x4A, 0x24}, {0x4B, 0x21}, {0x4C, 0x2D}, {0x4D, 0x23},
    {0x4E, 0x22}, {0x4F, 0x27}, {0x50, 0x25}, {0x51, 0x28}, {0x52, 0x26}, {0x53, 0x2E},
    {0xE0, 0x11}, {0xE1, 0x10}, {0xE2, 0x12}, {0xE3, 0x5B}, {0xE4, 0x5C}, {0xE5, 0x5B}, {0xE6, 0x5C}, {0xE7, 0x5B}};

int hidToVk(int hid) {
    for (const KvPair& p : kHidVk)
        if (p.hid == hid) return p.vk;
    return hid;
}
int vkToHid(int vk) {
    for (const KvPair& p : kHidVk)
        if (p.vk == vk) return p.hid;
    return 0;
}

void bindKey(int vk, int hid, InputSource source) {
    if (vk == VK_ESCAPE) { cancelCaptureEscape(); return; }
    if (!bindableKey(vk)) return;
    if (gCaptureSlot == 1) gCfg.secondary = {vk, hid, source};
    else { gCfg.key = vk; gCfg.keyHid = hid; gCfg.keySource = source; }
    gWaitForBoundRelease[size_t(gCaptureSlot)] = true;
    gCapture = false;
    gLive.note = "bound";
    gLastBindAt = GetTickCount64();
    gDirty = true;
    gDirtyAt = GetTickCount64();
}

void sendBytes(const uint8_t* b, int n) {
    if (gSerial == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(gSerial, b, (DWORD)n, &w, nullptr);
}

// INPUT_CHANGE (0x53) = [kind, id, state]; kind 1 mouse buttons, 2 keyboard HID usage
void handleFrame(uint8_t cmd, const uint8_t* pl, uint16_t len) {
    if (cmd != 0x53) tlog("[trg] frame cmd=0x%02X len=%u", cmd, unsigned(len));
    if (cmd != 0x53 || len != 3) return;
    const uint8_t kind = pl[0], id = pl[1], st = pl[2];
    if (id == 0xFF && st == 0xFF) {
        gStreamLost = true;
        gBoxHidDown.fill(false);
        std::fill(std::begin(gBtn), std::end(gBtn), false);
        gKeyHeld = false;
        tlog("[trg] input stream lost (kind %u)", kind);
        return;
    }
    gStreamLost = false;
    if (kind == 1) {
        if (id > 4) return;
        const int b = id + 1;  // 0..4 -> codes 1..5 (left..mouse5)
        gBtn[b] = st == 1;
        if (st == 1) captureBox(b, 0);
        tlog("[trg] box button %d %s", b, st ? "down" : "up");
    } else if (kind == 2) {
        gBoxHidDown[id] = st == 1;
        gKeyHid = id;
        gKeyHeld = st == 1;
        if (st == 1) captureBox(hidToVk(id), id);
        tlog("[trg] box key hid 0x%02X %s", id, st ? "down" : "up");
    }
}


uint8_t gLastMask = 0xFF;
uint8_t gTail[4] = {};

// MAKCU pushes "km." + one control byte = physical button bitmask (bit0 L,1 R,2 M,3 M4,4 M5)
void maskReport(uint8_t m) {
    static const int b[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; ++i) gBtn[b[i]] = (m >> i) & 1;
    if (m != gLastMask) {
        gLastMask = m;
        tlog("[trg] box buttons mask 0x%02X", m);
    }
    if (gCapture && gCaptureArmed) {
        for (int i = 0; i < 5; ++i)
            if ((m >> i) & 1) {
                captureBox(b[i], 0);
                break;
            }
    }
}

void textLineDone() {
    if (gRxLine.empty()) return;
    const std::string line = gRxLine;
    gRxLine.clear();
    gLive.lastRx = line;
    const ULONGLONG now = GetTickCount64();
    if (line != gLastLoggedRx || now - gLastRxLog > 1500) {
        gLastLoggedRx = line;
        gLastRxLog = now;
        tlog("[trg] rx: %s", line.c_str());
    }
    if (gAwaitIdent) {
        gIdent = line;
        gAwaitIdent = false;
        tlog("[trg] box ident: %s", gIdent.c_str());
    }
    parseLine(line);
}

// byte stream: text lines AND MAKCU binary frames (DE AD lenLE16 cmd payload)
void feedBytes(const char* p, int n) {
    if (gCfg.showDebug && n > 0) {
        char hx[200];
        size_t at = 0;
        for (int i = 0; i < n && i < 40 && at + 4 < sizeof hx; ++i)
            at += size_t(std::snprintf(hx + at, sizeof hx - at, "%02X ", (unsigned char)p[i]));
        if (at) hx[at - 1] = 0;
        tlog("[trg] rxraw: %s", hx);
    }
    for (int i = 0; i < n; ++i) {
        const uint8_t c = uint8_t(p[i]);
        gTail[3] = gTail[2];
        gTail[2] = gTail[1];
        gTail[1] = gTail[0];
        gTail[0] = c;
        if (gTail[3] == 'k' && gTail[2] == 'm' && gTail[1] == '.' && c < 0x20) {
            maskReport(c);
            gRxLine.clear();
            continue;
        }
        switch (gRxState) {
            case 0:
                if (c == 0xDE) gRxState = 1;
                else if (c == '\r' || c == '\n') textLineDone();
                else if (gRxLine.size() < 240) gRxLine.push_back(char(c));
                break;
            case 1:
                if (c == 0xAD) {
                    gRxState = 2;
                } else {
                    gRxState = 0;
                    if (gRxLine.size() < 240) gRxLine.push_back(char(0xDE));
                    if (c == '\r' || c == '\n') textLineDone();
                    else if (gRxLine.size() < 240) gRxLine.push_back(char(c));
                }
                break;
            case 2:
                gFrameLen = c;
                gRxState = 3;
                break;
            case 3:
                gFrameLen |= uint16_t(c) << 8;
                if (gFrameLen > 80) gFrameLen = 80;
                gFrameGot = 0;
                gRxState = 4;
                break;
            case 4:
                gFrameCmd = c;
                if (gFrameLen == 0) {
                    handleFrame(gFrameCmd, gFrameBuf, 0);
                    gRxState = 0;
                } else {
                    gRxState = 5;
                }
                break;
            case 5:
                gFrameBuf[gFrameGot++] = c;
                if (gFrameGot >= gFrameLen) {
                    handleFrame(gFrameCmd, gFrameBuf, gFrameLen);
                    gRxState = 0;
                }
                break;
            default:
                gRxState = 0;
                break;
        }
    }
}

void devicePoll();

// ---- probe: step through candidate "give me your physical input" commands ----
struct ProbeStep {
    const char* label;
    const char* text;
    uint8_t bin[8];
    int len;
};
const ProbeStep kProbe[] = {
    {"text km.get_key(8)  [E hid]", "km.get_key(8)", {}, 0},
    {"text km.get_key(0x08)", "km.get_key(0x08)", {}, 0},
    {"text km.get_key(69) [E vk]", "km.get_key(69)", {}, 0},
    {"text km.key(8)", "km.key(8)", {}, 0},
    {"text km.keys()", "km.keys()", {}, 0},
    {"text km.monitor(0,1,0)", "km.monitor(0,1,0)", {}, 0},
    {"text km.monitor(1,1,0)", "km.monitor(1,1,0)", {}, 0},
    {"text km.monitor(2,1,0)", "km.monitor(2,1,0)", {}, 0},
    {"text km.monitor(3,1,0)", "km.monitor(3,1,0)", {}, 0},
    {"text km.monitor(4,1,0)", "km.monitor(4,1,0)", {}, 0},
    {"text km.monitor(0,2,0)", "km.monitor(0,2,0)", {}, 0},
    {"text km.monitor(1,2,0)", "km.monitor(1,2,0)", {}, 0},
    {"text km.monitor(2,2,0)", "km.monitor(2,2,0)", {}, 0},
    {"text km.monitor(0,3,0)", "km.monitor(0,3,0)", {}, 0},
    {"text km.monitor(0,0,1)", "km.monitor(0,0,1)", {}, 0},
    {"text km.debug(1)", "km.debug(1)", {}, 0},
    {"bin INPUT_STREAM kbd", nullptr, {0xDE, 0xAD, 0x01, 0x00, 0x52, 0x02}, 6},
    {"text km.version()", "km.version()", {}, 0},
};
int gProbeIdx = -1;
ULONGLONG gProbeAt = 0;

void probeStep() {
    const int n = int(sizeof kProbe / sizeof kProbe[0]);
    if (gProbeIdx < 0) return;
    if (!connected()) return;
    const ULONGLONG now = GetTickCount64();
    if (gProbeAt && now - gProbeAt < 600) return;
    gProbeAt = now;
    if (gProbeIdx >= n) {
        gProbeIdx = -1;
        tlog("[trg] probe done");
        return;
    }
    const ProbeStep& s = kProbe[gProbeIdx];
    tlog("[trg] probe %d/%d: %s", gProbeIdx + 1, n, s.label);
    if (s.text) {
        const std::string t = s.text;
        deviceSend(t);
    } else {
        sendBytes(s.bin, s.len);
    }
    devicePoll();
    ++gProbeIdx;
}


void devicePoll();

void sendStreams() {
    const uint8_t nl[2] = {'\r', '\n'};
    const uint8_t mouse[6] = {0xDE, 0xAD, 0x01, 0x00, 0x52, 0x01};
    const uint8_t kbd[6] = {0xDE, 0xAD, 0x01, 0x00, 0x52, 0x02};
    sendBytes(nl, 2);
    sendBytes(mouse, 6);
    devicePoll();
    sendBytes(kbd, 6);
    devicePoll();
    gLastStreamOn = GetTickCount64();
    gStreamLost = false;
    tlog("[trg] binary input streams enabled (mouse+keyboard)");
}

void devicePoll() {
    if (gCfg.modeNet ? gSock == INVALID_SOCKET : gSerial == INVALID_HANDLE_VALUE) return;
    for (int iter = 0; iter < 24; ++iter) {
        char buf[256];
        if (gCfg.modeNet) {
            int from = sizeof gAddr;
            const int n = recvfrom(gSock, buf, (int)sizeof buf - 1, 0, nullptr, &from);
            if (n <= 0) return;
            buf[n] = 0;
            feedBytes(buf, n);
        } else {
            DWORD got = 0;
            if (!ReadFile(gSerial, buf, (DWORD)sizeof buf - 1, &got, nullptr) || got == 0) return;
            buf[got] = 0;
            feedBytes(buf, (int)got);
        }
    }
}

std::vector<std::string> portsImpl() {
    std::vector<std::string> out;
    for (int i = 1; i <= 64; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "COM%d", i);
        char target[256];
        if (QueryDosDeviceA(name, target, sizeof target)) out.push_back(name);
    }
    return out;
}

void closeDevice() {
    gBoxHidDown.fill(false);
    std::fill(std::begin(gBtn), std::end(gBtn), false);
    gKeyHeld = false;
    if (gSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(gSerial);
        gSerial = INVALID_HANDLE_VALUE;
    }
    if (gSock != INVALID_SOCKET) {
        closesocket(gSock);
        gSock = INVALID_SOCKET;
    }
}

DWORD gBaudUsed = 0;

// opens the port at one baud and checks whether the box answers (MAKCU runs 115200 or 4 Mbaud)
bool tryOpenSerial(DWORD baud) {
    const std::string path = "\\\\.\\" + gCfg.port;
    gSerial = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (gSerial == INVALID_HANDLE_VALUE) {
        setStatus("open " + gCfg.port + " failed");
        return false;
    }
    SetupComm(gSerial, 4096, 4096);
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(gSerial, &dcb)) {
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(gSerial, &dcb);
    }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 1;
    to.ReadTotalTimeoutConstant = 1;
    to.WriteTotalTimeoutConstant = 3;
    SetCommTimeouts(gSerial, &to);
    PurgeComm(gSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    gRxLine.clear();
    gRxState = 0;
    gIdent.clear();
    if (!gCfg.cmdConnect.empty()) {
        gAwaitIdent = true;
        deviceSend(gCfg.cmdConnect);
    }
    for (int i = 0; i < 25 && gIdent.empty(); ++i) {
        Sleep(10);
        devicePoll();
    }
    if (gIdent.empty()) {
        tlog("[trg] baud %lu: no answer", (unsigned long)baud);
        CloseHandle(gSerial);
        gSerial = INVALID_HANDLE_VALUE;
        return false;
    }
    gBaudUsed = baud;
    setStatus("connected " + gCfg.port + " @ " + std::to_string(baud));
    tlog("[trg] baud %lu works (ident: %s)", (unsigned long)baud, gIdent.c_str());
    return true;
}

void openDevice() {
    closeDevice();
    if (gCfg.modeNet) {
        if (!gWsa) {
            WSADATA w{};
            if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { setStatus("winsock failed"); return; }
            gWsa = true;
        }
        gSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (gSock == INVALID_SOCKET) { setStatus("socket failed"); return; }
        u_long nb = 1;
        ioctlsocket(gSock, FIONBIO, &nb);
        gAddr = sockaddr_in{};
        gAddr.sin_family = AF_INET;
        gAddr.sin_port = htons((u_short)gCfg.netPort);
        gAddr.sin_addr.s_addr = inet_addr(gCfg.host.c_str());
        setStatus("connected (udp " + gCfg.host + ")");
    } else {
        if (gCfg.port.empty()) {
            const std::vector<std::string> ps = portsImpl();
            std::string all;
            for (const std::string& p : ps) all += (all.empty() ? "" : ", ") + p;
            tlog("[trg] COM ports present: %s", all.empty() ? "(none)" : all.c_str());
            if (ps.empty()) { setStatus("no COM port found"); return; }
            gCfg.port = ps.front();
        }
        const DWORD other = gCfg.baud == 4000000 ? 115200u : 4000000u;
        const DWORD list[2] = {gCfg.autoBaud ? 115200u : DWORD(gCfg.baud), gCfg.autoBaud ? 4000000u : other};
        bool ok = false;
        for (int i = 0; i < 2 && !ok; ++i) ok = tryOpenSerial(list[i]);
        if (!ok) {
            setStatus("no answer on " + gCfg.port + " - other baud / USB3 port / box reset");
            return;
        }
    }
    if (gCfg.modeNet && !gCfg.cmdConnect.empty()) {
        gAwaitIdent = true;
        deviceSend(gCfg.cmdConnect);
        Sleep(30);
        devicePoll();
    }
    if (gCfg.useBinary && !gCfg.modeNet) sendStreams();
    if (gCfg.sendMonitor && !gCfg.cmdMonitor.empty()) deviceSend(gCfg.cmdMonitor);
    devicePoll();
    gLive.note = "connected";
}

}  // namespace

// ---- config ----
void load() {
    bool sawCmdMonitor = false;
    std::ifstream f(iniPath());
    std::string line;
    while (std::getline(f, line)) {
        const std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == '[') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = lower(trim(l.substr(0, eq)));
        const std::string v = trim(l.substr(eq + 1));
        if (k == "enabled") gCfg.enabled = toBool(v, gCfg.enabled);
        else if (k == "playersonly") gCfg.playersOnly = toBool(v, gCfg.playersOnly);
        else if (k == "animals") gCfg.animals = toBool(v, gCfg.animals);
        else if (k == "key") gCfg.key = toInt(v, gCfg.key);
        else if (k == "keysource") {
            if (v == "main") gCfg.keySource = InputSource::MainPc;
            else if (v == "second") gCfg.keySource = InputSource::SecondPc;
            else if (v == "box") gCfg.keySource = InputSource::Box;
        }
        else if (k == "keyfromdevice") gCfg.keyFromDevice = toBool(v, gCfg.keyFromDevice);
        else if (k == "keyfromlocal") gCfg.keyFromLocal = toBool(v, gCfg.keyFromLocal);
        else if (k == "keyremote") gCfg.keyRemote = toBool(v, gCfg.keyRemote);
        else if (k == "keyhid") gCfg.keyHid = toInt(v, gCfg.keyHid);
        else if (k == "secondkey") gCfg.secondary.key = toInt(v, 0);
        else if (k == "secondkeyhid") gCfg.secondary.hid = toInt(v, 0);
        else if (k == "secondkeysource") {
            if (v == "main") gCfg.secondary.source = InputSource::MainPc;
            else if (v == "box") gCfg.secondary.source = InputSource::Box;
            else gCfg.secondary.source = InputSource::SecondPc;
        }
        else if (k == "usebinary") gCfg.useBinary = toBool(v, gCfg.useBinary);
        else if (k == "delayms") gCfg.delayMs = toInt(v, gCfg.delayMs);
        else if (k == "radiuspx") gCfg.radiusPx = toInt(v, gCfg.radiusPx);
        else if (k == "hitmask") gCfg.hitMask = (unsigned)toInt(v, (int)gCfg.hitMask);
        else if (k == "maxdistm") gCfg.maxDistM = toInt(v, gCfg.maxDistM);
        else if (k == "refirems") gCfg.refireMs = toInt(v, gCfg.refireMs);
        else if (k == "clickms") gCfg.clickMs = toInt(v, gCfg.clickMs);
        else if (k == "modenet") gCfg.modeNet = toBool(v, gCfg.modeNet);
        else if (k == "port") gCfg.port = v;
        else if (k == "baud") gCfg.baud = toInt(v, gCfg.baud);
        else if (k == "autobaud") gCfg.autoBaud = toBool(v, gCfg.autoBaud);
        else if (k == "host") gCfg.host = v;
        else if (k == "netport") gCfg.netPort = toInt(v, gCfg.netPort);
        else if (k == "sendmonitor") gCfg.sendMonitor = toBool(v, gCfg.sendMonitor);
        else if (k == "cmdconnect") gCfg.cmdConnect = v;
        else if (k == "cmdmonitor") { gCfg.cmdMonitor = v; sawCmdMonitor = true; }
        else if (k == "monitoridx") gCfg.monitorIdx = toInt(v, gCfg.monitorIdx);
        else if (k == "cmddown") gCfg.cmdDown = v;
        else if (k == "cmdup") gCfg.cmdUp = v;
        else if (k == "cmdkeypoll") gCfg.cmdKeyPoll = v;
        else if (k == "autoconnect") gCfg.autoConnect = toBool(v, gCfg.autoConnect);
        else if (k == "moduleopen") gCfg.moduleOpen = toBool(v, gCfg.moduleOpen);
        else if (k == "showdebug") gCfg.showDebug = toBool(v, gCfg.showDebug);
        else if (k == "showregions") gCfg.showRegions = toBool(v, gCfg.showRegions);
    }
    if (gCfg.delayMs < 0) gCfg.delayMs = 0;
    if (!bindableKey(gCfg.key)) { gCfg.key = 0; gCfg.keyHid = 0; }
    if (!bindableKey(gCfg.secondary.key)) gCfg.secondary = {};
    gCfg.radiusPx = std::clamp(gCfg.radiusPx, 0, 4);
    if (!sawCmdMonitor) gCfg.cmdMonitor = monitorPresetText(gCfg.monitorIdx);
    if (GetFileAttributesA(iniPath().c_str()) == INVALID_FILE_ATTRIBUTES) save();
}

void save() {
    std::ofstream f(iniPath(), std::ios::trunc);
    if (!f) return;
    f << "# HirschgangWare triggerbot - pure mouse box (KMBox / MAKCU / similar). Copy this file PC to PC.\n";
    f << "[Triggerbot]\n";
    f << "enabled=" << (gCfg.enabled ? 1 : 0) << "\n";
    f << "playersOnly=" << (gCfg.playersOnly ? 1 : 0) << "\n";
    f << "animals=" << (gCfg.animals ? 1 : 0) << "\n";
    f << "key=" << gCfg.key << "\n";
    switch (gCfg.keySource) {
        case InputSource::MainPc: f << "keySource=main\n"; break;
        case InputSource::SecondPc: f << "keySource=second\n"; break;
        case InputSource::Box: f << "keySource=box\n"; break;
        case InputSource::Legacy: break;
    }
    f << "keyFromDevice=" << (gCfg.keyFromDevice ? 1 : 0) << "\n";
    f << "keyFromLocal=" << (gCfg.keyFromLocal ? 1 : 0) << "\n";
    f << "keyRemote=" << (gCfg.keyRemote ? 1 : 0) << "\n";
    f << "keyHid=" << gCfg.keyHid << "\n";
    f << "secondKey=" << gCfg.secondary.key << "\n";
    f << "secondKeyHid=" << gCfg.secondary.hid << "\n";
    f << "secondKeySource=" << (gCfg.secondary.source == InputSource::MainPc ? "main" :
        gCfg.secondary.source == InputSource::Box ? "box" : "second") << "\n";
    f << "useBinary=" << (gCfg.useBinary ? 1 : 0) << "\n";
    f << "delayMs=" << gCfg.delayMs << "\n";
    f << "radiusPx=" << gCfg.radiusPx << "\n";
    f << "hitMask=" << gCfg.hitMask << "\n";
    f << "maxDistM=" << gCfg.maxDistM << "\n";
    f << "refireMs=" << gCfg.refireMs << "\n";
    f << "clickMs=" << gCfg.clickMs << "\n";
    f << "modeNet=" << (gCfg.modeNet ? 1 : 0) << "\n";
    f << "port=" << gCfg.port << "\n";
    f << "baud=" << gCfg.baud << "\n";
    f << "autoBaud=" << (gCfg.autoBaud ? 1 : 0) << "\n";
    f << "host=" << gCfg.host << "\n";
    f << "netPort=" << gCfg.netPort << "\n";
    f << "sendMonitor=" << (gCfg.sendMonitor ? 1 : 0) << "\n";
    f << "monitorIdx=" << gCfg.monitorIdx << "\n";
    f << "cmdConnect=" << gCfg.cmdConnect << "\n";
    f << "cmdMonitor=" << gCfg.cmdMonitor << "\n";
    f << "cmdDown=" << gCfg.cmdDown << "\n";
    f << "cmdUp=" << gCfg.cmdUp << "\n";
    f << "cmdKeyPoll=" << gCfg.cmdKeyPoll << "\n";
    f << "autoConnect=" << (gCfg.autoConnect ? 1 : 0) << "\n";
    f << "moduleOpen=" << (gCfg.moduleOpen ? 1 : 0) << "\n";
    f << "showDebug=" << (gCfg.showDebug ? 1 : 0) << "\n";
    f << "showRegions=" << (gCfg.showRegions ? 1 : 0) << "\n";
}

int keyToVk(int key) {
    constexpr int buttons[]{0, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    if (key >= 1 && key <= 5) return buttons[key];
    return dma_keyboard::validKey(unsigned(key)) ? key : 0;
}

bool (*gLocalKeyReader)(int) = nullptr;
bool localKeyDown(int key) {
    if (gLocalKeyReader) return gLocalKeyReader(key);
    const int vk = keyToVk(key);
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool deviceKeyDown(int key, int hid, bool primary = false) {
    if (key >= 1 && key <= 5) return gBtn[key];
    if (gCfg.useBinary) return hid > 0 && hid < 256 && gBoxHidDown[size_t(hid)];
    return gKeyHeld && ((primary && gKeyCode == 0) || gKeyCode == key);
}

void pollKey() {
    if (boxInputEnabled() && !gCfg.cmdKeyPoll.empty()) deviceSend(gCfg.cmdKeyPoll, gCfg.key);
}

Config& cfg() { return gCfg; }
Binding binding(int slot) { return slot == 1 ? gCfg.secondary : Binding{gCfg.key, gCfg.keyHid, gCfg.keySource}; }
int captureSlot() { return gCapture ? gCaptureSlot : -1; }
void clearBinding(int slot) {
    if (slot != 0 && slot != 1) return;
    if (slot == 1) gCfg.secondary = {};
    else { gCfg.key = 0; gCfg.keyHid = 0; }
    gLive.boundHeld[size_t(slot)] = false;
    if (gCapture && gCaptureSlot == slot) stopCapture();
    touch();
}
Live& live() { return gLive; }
const std::string& status() { return gStatus; }
bool connected() { return gSerial != INVALID_HANDLE_VALUE || gSock != INVALID_SOCKET; }
bool capturing() { return gCapture; }
void setLog(void (*fn)(const char*)) { gLog = fn; }
const std::string& ident() { return gIdent; }
void sendRaw(const std::string& cmd) { if (!cmd.empty()) deviceSend(cmd); }
int monitorPresetCount() { return 7; }
const char* monitorPresetText(int i) {
    static const char* const t[7] = {"km.monitor(1,0,0)", "km.monitor(2,0,0)", "km.monitor(3,0,0)", "km.monitor(4,0,0)",
                                     "km.monitor(5,0,0)", "km.monitor(0,0,0)", "off (poll only)"};
    return (i >= 0 && i < 7) ? t[i] : "?";
}
void monitorPreset(int i) {
    if (i < 0 || i > 6) return;
    if (i == 6) {
        gCfg.cmdMonitor.clear();
        gCfg.sendMonitor = false;
        touch();
        return;
    }
    gCfg.cmdMonitor = monitorPresetText(i);
    gCfg.sendMonitor = true;
    gCfg.monitorIdx = i;
    gCfg.useBinary = false;
    touch();
    deviceSend(gCfg.cmdMonitor);
    tlog("[trg] sent monitor: %s", gCfg.cmdMonitor.c_str());
}
bool justBound() { return gLastBindAt != 0 && GetTickCount64() - gLastBindAt < 450; }
void startCapture(int slot) {
    if (slot != 0 && slot != 1) return;
    gCaptureSlot = slot;
    gCapture = true;
    gCaptureArmed = false;
    gLive.note = "press a key or mouse button on either PC";
}
void stopCapture() { gCapture = false; gWaitForBoundRelease.fill(true); gLive.note.clear(); }
bool cancelCaptureEscape() {
    const auto now = GetTickCount64();
    if (!gCapture && (!gLastEscapeCancelAt || now - gLastEscapeCancelAt >= 450)) return false;
    gLastEscapeCancelAt = now;
    stopCapture();
    return true;
}
void touch() { gDirty = true; gDirtyAt = GetTickCount64(); }
std::vector<std::string> listPorts() { return portsImpl(); }
void connectDevice() { openDevice(); }
void probeInput() {
    if (!connected()) {
        gLive.note = "connect the box first";
        return;
    }
    gProbeIdx = 0;
    gProbeAt = 0;
    tlog("[trg] probe start - press E on the MAIN pc repeatedly for ~12s");
}
bool probing() { return gProbeIdx >= 0; }
int baudUsed() { return int(gBaudUsed); }

namespace {

dma_keyboard::Sample (*gRemoteKeys)() = nullptr;
dma_keyboard::Sample gRemote;
dma_keyboard::Bitmap gRemoteInitiallyDown;
ULONGLONG gRemoteAt = 0;
bool gRemoteInputNeeded = false;

bool usesRemote(int slot) {
    const auto b = binding(slot);
    return bindableKey(b.key) && (b.source == InputSource::MainPc ||
           (b.source == InputSource::Legacy && gCfg.keyRemote));
}

bool remoteDown(int key) {
    const int vk = keyToVk(key);
    return gRemote.pid && vk && dma_keyboard::down(gRemote.bits, unsigned(vk));
}

bool remoteInitiallyDown(int key) {
    const int vk = keyToVk(key);
    return vk && dma_keyboard::down(gRemoteInitiallyDown, unsigned(vk));
}

void sampleRemote(ULONGLONG now) {
    if (!gRemoteKeys || (!gRemoteInputNeeded && !gCapture && !usesRemote(0) && !usesRemote(1))) {
        gRemote = {};
        gRemoteAt = 0;
        return;
    }
    if (gRemoteAt && now - gRemoteAt < 8) return;
    gRemoteAt = now;
    const auto current = gRemoteKeys();
    if (current.pid != gRemote.pid) {
        gRemoteInitiallyDown = current.bits;
        tlog("[trg] PCIe keyboard %s", current.pid ? "connected" : "disconnected");
    } else {
        for (size_t i = 0; i < current.bits.size(); ++i)
            gRemoteInitiallyDown[i] &= current.bits[i];
    }
    gRemote = current;
}

template<class IsDown>
int firstBinding(IsDown down) {
    for (int key = 1; key < 255; ++key) {
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || !keyToVk(key)) continue;
        if (down(key)) return key;
    }
    return 0;
}

bool captureInputs() {
    if (!gCapture) return false;
    if (localKeyDown(VK_ESCAPE) || remoteDown(VK_ESCAPE)) {
        cancelCaptureEscape();
        return true;
    }
    const int local = firstBinding(localKeyDown);
    if (!gCaptureArmed) {
        const int remote = firstBinding(remoteDown);
        const bool boxHeld = connected() && gCfg.keyFromDevice && !gRemote.pid &&
            (gKeyHeld || std::any_of(std::begin(gBtn) + 1, std::end(gBtn), [](bool held) { return held; }));
        gCaptureArmed = !local && !remote && !boxHeld;
    } else if (local) {
        bindKey(local, vkToHid(local), InputSource::SecondPc);
    } else {
        const int remote = firstBinding([](int key) {
            return remoteDown(key) && !remoteInitiallyDown(key);
        });
        if (remote) bindKey(remote, vkToHid(remote), InputSource::MainPc);
    }
    return true;
}

bool remoteMouseSettling(ULONGLONG now, int slot) {
    // Only the left-button binding can be fed back by our synthetic left click.
    return usesRemote(slot) && binding(slot).key == 1 &&
           (gDownAt || (gOutputReleasedAt && now - gOutputReleasedAt < 40));
}

bool remoteMouseSettling(ULONGLONG now) {
    bool any = false;
    for (int slot = 0; slot < 2; ++slot) {
        if (!gLive.boundHeld[size_t(slot)]) continue;
        if (!remoteMouseSettling(now, slot)) return false;
        any = true;
    }
    return any;
}

bool boundDown(int slot) {
    const auto b = binding(slot);
    if (!bindableKey(b.key)) return false;
    if (usesRemote(slot)) return remoteDown(b.key) && !remoteInitiallyDown(b.key);
    if (b.source == InputSource::SecondPc) return localKeyDown(b.key);
    return (b.source == InputSource::Legacy && gCfg.keyFromLocal && localKeyDown(b.key)) ||
           (connected() && boxInputEnabled(slot) && deviceKeyDown(b.key, b.hid, slot == 0));
}

}  // namespace

void setRemoteKeys(dma_keyboard::Sample (*fn)()) { gRemoteKeys = fn; }
void setRemoteInputNeeded(bool needed) { gRemoteInputNeeded = needed; }
dma_keyboard::Sample remoteSample() { return gRemote; }
void setLocalKeyReader(bool (*fn)(int)) { gLocalKeyReader = fn; }

void pollInputs() {
    const ULONGLONG now = GetTickCount64();
    sampleRemote(now);
    if (captureInputs()) {
        gLive.keyHeld = false;
        gLive.boundHeld.fill(false);
        return;
    }
    for (int slot = 0; slot < 2; ++slot) {
        const bool rawHeld = boundDown(slot);
        const bool held = remoteMouseSettling(now, slot) ? gRemote.pid && gLive.boundHeld[size_t(slot)] : rawHeld;
        if (gWaitForBoundRelease[size_t(slot)] && !rawHeld) gWaitForBoundRelease[size_t(slot)] = false;
        gLive.boundHeld[size_t(slot)] = held && !gWaitForBoundRelease[size_t(slot)];
    }
    const bool activated = gLive.boundHeld[0] || gLive.boundHeld[1];
    if (activated && !gLive.keyHeld) {
        ++gLive.keyPresses;
        const int slot = gLive.boundHeld[0] ? 0 : 1;
        tlog("[trg] %s bind=%d key=%d total=%llu", keySourceLabel(slot), slot + 1, binding(slot).key,
             static_cast<unsigned long long>(gLive.keyPresses));
    }
    gLive.keyHeld = activated;
}

bool remoteKeysReady() { return gRemote.pid != 0; }

const char* keySourceLabel(int slot) {
    if (usesRemote(slot)) return "Main PC";
    if (binding(slot).source == InputSource::SecondPc) return "Second PC";
    if (binding(slot).source == InputSource::Box) return "Box";
    return "Local / box";
}

namespace {
void beginClick(ULONGLONG now) {
    if (gDownAt) return;
    if (gCfg.cmdUp.empty() || !deviceSend(gCfg.cmdDown)) {
        gLive.clickStatus = "Click down could not be sent. Check the mouse box and click commands.";
        return;
    }
    gLive.clickStatus.clear();
    gDownAt = now;
}

bool finishClick(ULONGLONG now) {
    if (!deviceSend(gCfg.cmdUp)) {
        gLive.clickStatus = "Click release could not be sent. Check the mouse box.";
        return false;
    }
    gDownAt = 0;
    gOutputReleasedAt = now;
    gLive.clickStatus.clear();
    ++gLive.clicks;
    return true;
}
}  // namespace

void clickOnce() {
    if (connected()) beginClick(GetTickCount64());
}
void disconnectDevice() {
    if (gDownAt && connected()) finishClick(GetTickCount64());
    closeDevice();
    setStatus("disconnected");
    gLive.note.clear();
    if (gDownAt) gDownAt = 0;
}

const char* boneName(int i) {
    static const char* const n[16] = {"Head", "Neck",  "Spine", "Pelvis", "R.Shoulder", "R.Elbow", "R.Hand", "L.Shoulder",
                                      "L.Elbow", "L.Hand", "R.Hip", "R.Knee", "R.Foot", "L.Hip",     "L.Knee", "L.Foot"};
    return (i >= 0 && i < 16) ? n[i] : "?";
}

void update(const std::vector<Cand>& cands, float cx, float cy, bool projectionReady) {
    const ULONGLONG now = GetTickCount64();
    probeStep();
    if (gDirty && now - gDirtyAt > 600) {
        gDirty = false;
        save();
    }
    const auto match = projectionReady ? targetAt(gCfg, cands, cx, cy) : TargetMatch{};
    const Cand* best = match.candidate;
    gLive.projectionReady = projectionReady;
    gLive.target = best != nullptr;
    gLive.bone = best ? best->bone : -1;
    gLive.px = best ? std::sqrt(match.distanceSq) : 0.f;
    gLive.dist = best ? best->dist : 0.f;
    gLive.heldMs = 0;
    gLive.regionCount = 0;
    for (const auto& candidate : cands)
        if (candidate.regionSize >= 3 && candidate.bone >= 0 && candidate.bone < 16 &&
            (gCfg.hitMask & (1u << candidate.bone))) ++gLive.regionCount;
    if (!connected()) {
        gHaveArmed = false;
        gArmedId = 0;
        if (gCfg.autoConnect && now - gLastRetry > 1500) {
            gLastRetry = now;
            openDevice();
        }
        return;
    }
    static ULONGLONG lastPoll = 0, lastKeyPoll = 0;
    if (now - lastPoll >= 3) {
        lastPoll = now;
        devicePoll();
    }
    if (now - lastKeyPoll >= 50) {
        lastKeyPoll = now;
        pollKey();
    }
    if (!probing() && gCfg.useBinary && !gCfg.modeNet && (gStreamLost || now - gLastStreamOn > 10000)) sendStreams();

    const bool active = gCfg.enabled && best && gLive.keyHeld;
    if (gDownAt && (!active || now - gDownAt >= (ULONGLONG)std::max(2, gCfg.clickMs))) {
        if (!finishClick(now)) gDownAt = now;  // Retry after the configured button interval.
    }

    if (!active) {
        gHaveArmed = false;
        gArmedId = 0;
        return;
    }
    if (!gHaveArmed || gArmedId != best->id) {
        gHaveArmed = true;
        gArmedId = best->id;
        gArmedAt = now;
    }
    gLive.heldMs = int(now - gArmedAt);
    if (gDownAt || remoteMouseSettling(now)) return;
    // Ensure the device receives a distinct button-up interval between clicks.
    if (gOutputReleasedAt && now - gOutputReleasedAt < 2) return;
    if (now - gArmedAt < (ULONGLONG)gCfg.delayMs) return;
    if (gLastShot && now - gLastShot < (ULONGLONG)std::max(1, gCfg.refireMs)) return;
    gLastShot = now;
    beginClick(now);
}

}  // namespace trigger
