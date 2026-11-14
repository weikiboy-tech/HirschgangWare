#pragma once
// Pure mouse-box triggerbot (KMBox / MAKCU / similar). No game memory writes:
// bones + camera are read over DMA, the decision is made here, and the click is
// sent to the little USB box which sits in-line with the gaming PC's mouse.
// The box also reports the physical mouse/keyboard, so the trigger key is the
// one on the *gaming* keyboard (works PC to PC). Device config lives in trigger.ini.
#include <cstdint>
#include <string>
#include <vector>

namespace trigger {

// hitbox bits, order matches the DayZ joint enum used by the ESP
enum Hit : unsigned {
    H_Head = 1u << 0, H_Neck = 1u << 1, H_Spine = 1u << 2, H_Pelvis = 1u << 3,
    H_RSh = 1u << 4, H_REl = 1u << 5, H_RHand = 1u << 6,
    H_LSh = 1u << 7, H_LEl = 1u << 8, H_LHand = 1u << 9,
    H_RHip = 1u << 10, H_RKnee = 1u << 11, H_RFoot = 1u << 12,
    H_LHip = 1u << 13, H_LKnee = 1u << 14, H_LFoot = 1u << 15,
    H_All = 0xFFFFu,
};

struct Cand {
    float x = 0, y = 0;
    float dist = 0;
    int bone = 0;
    int kind = 0;  // 1 survivor, 2 infected, 3 animal, 4 vehicle
    bool isFriend = false;
    uint64_t id = 0;
};

struct Config {
    bool enabled = false;
    bool playersOnly = true;
    bool animals = false;
    int key = 2;  // 1..5 mouse buttons, else key code (2 = right mouse default)
    bool keyFromDevice = true;
    bool keyFromLocal = true;  // also accept the key on THIS pc (both PCs work)
    bool keyRemote = true;     // also accept the key typed on the gaming PC (read via DMA)
    int keyHid = 0;            // HID usage of the bound key (for the box side), 0 = unknown
    bool useBinary = true;     // MAKCU: use the binary input stream frames (DE AD ... 52)
    int delayMs = 50;    // dwell time before the shot
    int radiusPx = 15;   // hit radius around the crosshair
    unsigned hitMask = H_Head | H_Neck | H_Spine | H_Pelvis;
    int maxDistM = 150;
    int refireMs = 60;
    int clickMs = 25;    // how long the synthetic button stays down
    bool modeNet = false;
    std::string port;
    int baud = 115200;
    bool autoBaud = true;  // try 115200 and 4 M and keep whichever answers
    std::string host = "192.168.4.1";
    int netPort = 8888;
    bool sendMonitor = true;
    int monitorIdx = 3;
    std::string cmdConnect = "km.version()";
    std::string cmdMonitor = "km.monitor(4,0,0)";
    std::string cmdDown = "km.left(1)";
    std::string cmdUp = "km.left(0)";
    std::string cmdKeyPoll = "km.get_key({vk})";
    bool autoConnect = true;
    bool moduleOpen = false;
    bool showDebug = false;
};

struct Live {
    bool target = false;
    float dist = 0;
    float px = 0;
    int bone = -1;
    bool keyHeld = false;
    int clicks = 0;
    int heldMs = 0;
    std::string note;
    std::string lastRx;
};

Config& cfg();
Live& live();
const std::string& status();
bool connected();
bool capturing();
int baudUsed();  // 0 while not connected, else the baud that worked
void probeInput();  // step through candidate enable-commands, logging all raw replies
bool probing();
bool justBound();  // a bind happened <450ms ago (so a stop-click does not restart it)
void startCapture();
void stopCapture();

void load();
void save();
void touch();  // request a debounced save

std::vector<std::string> listPorts();
void connectDevice();
void disconnectDevice();
void clickOnce();  // one test click through the box

// cands = every visible hitbox of every target (overlay pixels), cx/cy = crosshair
void update(const std::vector<Cand>& cands, float cx, float cy);

void setLog(void (*fn)(const char*));
void setRemoteKeys(bool (*fn)(uint8_t* out256));  // target keyboard state via DMA
bool remoteKeysReady();
void setKeyScan(void (*fn)());
void keyScanNow();
const std::string& ident();
void sendRaw(const std::string& cmd);
int monitorPresetCount();
const char* monitorPresetText(int i);
void monitorPreset(int i);

const char* boneName(int i);

}  // namespace trigger
