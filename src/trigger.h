#pragma once
// Pure mouse-box triggerbot (KMBox / MAKCU / similar). No game memory writes:
// bones + camera are read over DMA, the decision is made here, and the click is
// sent to the little USB box which sits in-line with the gaming PC's mouse.
// The box also reports the physical mouse/keyboard, so the trigger key is the
// one on the *gaming* keyboard (works PC to PC). Device config lives in trigger.ini.
#include "keyboard_bitmap.h"
#include "aim_motion_protocol.h"

#include <cmath>
#include <algorithm>
#include <array>
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
    struct Point { float x = 0, y = 0; };
    std::array<Point, 32> region{};
    unsigned regionSize = 0; // Zero retains point candidates for existing callers.
    float maxTolerancePx = 0;
};

// Prepare the target list once per frame. Rendered radar/overlay entities stay intact.
inline void removeFriends(std::vector<Cand>& candidates) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [](const Cand& candidate) { return candidate.isFriend; }), candidates.end());
}

enum class InputSource { Legacy, MainPc, SecondPc, Box };
struct Binding {
    int key = 0;
    int hid = 0;
    InputSource source = InputSource::SecondPc;
};
inline bool bindableKey(int key) { return key > 0 && key < 255 && key != 27; }

struct Config {
    bool enabled = false;
    bool playersOnly = true;
    bool animals = false;
    int key = 2;  // 1..5 mouse buttons, else key code (2 = right mouse default)
    InputSource keySource = InputSource::Legacy;
    bool keyFromDevice = true;
    bool keyFromLocal = true;  // also accept the key on THIS pc (both PCs work)
    bool keyRemote = true;     // exclusive input from the target Windows session over PCIe
    int keyHid = 0;            // HID usage of the bound key (for the box side), 0 = unknown
    bool useBinary = true;     // MAKCU: use the binary input stream frames (DE AD ... 52)
    int delayMs = 50;    // dwell time before the shot
    int radiusPx = 1;    // requested edge tolerance, capped by each projected body region
    unsigned hitMask = H_Head | H_Neck | H_Spine | H_Pelvis;
    int maxDistM = 150;
    int refireMs = 25;   // minimum interval between click starts
    int clickMs = 8;     // how long the synthetic button stays down
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
    bool showRegions = false;
    Binding secondary;
};

struct TargetMatch {
    const Cand* candidate = nullptr;
    float distanceSq = 0;
};

inline float segmentDistanceSq(Cand::Point p, Cand::Point a, Cand::Point b) {
    const float x = b.x - a.x, y = b.y - a.y;
    const float length = x * x + y * y;
    const float t = length > 0 ? std::clamp(((p.x - a.x) * x + (p.y - a.y) * y) / length, 0.f, 1.f) : 0.f;
    const float dx = p.x - a.x - t * x, dy = p.y - a.y - t * y;
    return dx * dx + dy * dy;
}

inline float regionDistanceSq(const Cand& candidate, float x, float y) {
    if (!candidate.regionSize) {
        const float dx = candidate.x - x, dy = candidate.y - y;
        return dx * dx + dy * dy;
    }
    if (candidate.regionSize < 3 || candidate.regionSize > candidate.region.size())
        return INFINITY;
    float best = INFINITY;
    double area = 0;
    bool positive = false, negative = false;
    const Cand::Point p{x, y};
    for (unsigned i = 0; i < candidate.regionSize; ++i) {
        const auto a = candidate.region[i], b = candidate.region[(i + 1) % candidate.regionSize];
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y))
            return INFINITY;
        const float cross = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        const auto origin = candidate.region[0];
        area += double(a.x - origin.x) * double(b.y - origin.y) - double(a.y - origin.y) * double(b.x - origin.x);
        positive |= cross > 0;
        negative |= cross < 0;
        best = std::min(best, segmentDistanceSq(p, a, b));
    }
    if (!std::isfinite(area) || area == 0) return INFINITY;
    return positive && negative ? best : 0.f;
}

// Geometry and configured filters are independent of the output device.
inline TargetMatch targetAt(const Config& cfg, const std::vector<Cand>& cands, float cx, float cy) {
    TargetMatch result{nullptr, INFINITY};
    if (!std::isfinite(cx) || !std::isfinite(cy)) return result;
    for (const Cand& c : cands) {
        if (c.bone < 0 || c.bone >= 16 || !(cfg.hitMask & (1u << c.bone))) continue;
        if (c.kind != 1 && (cfg.playersOnly || (c.kind != 2 && !(c.kind == 3 && cfg.animals)))) continue;
        if (cfg.maxDistM > 0 && c.dist > float(cfg.maxDistM)) continue;
        if (!std::isfinite(c.dist) || c.dist < 0) continue;
        const float requested = float(std::max(0, cfg.radiusPx));
        const float tolerance = c.regionSize ? std::min(requested, std::max(0.f, c.maxTolerancePx)) : requested;
        const float d2 = regionDistanceSq(c, cx, cy);
        if (std::isfinite(d2) && d2 <= tolerance * tolerance &&
            (!result.candidate || d2 < result.distanceSq ||
             (d2 == result.distanceSq && c.dist < result.candidate->dist))) result = {&c, d2};
    }
    return result;
}

struct Live {
    struct HeadProbe {
        const char* reason = "no sampled player in trigger range";
        float range = 0, gap = 0, ageMs = 0, radiusPx = 0, offsetPx = INFINITY;
        bool isFriend = false;
    } head;
    bool projectionReady = false;
    bool target = false;
    float dist = 0;
    float px = 0;
    int bone = -1;
    bool keyHeld = false;
    uint64_t keyPresses = 0;
    int clicks = 0;
    int heldMs = 0;
    std::string note;
    std::string lastRx;
    std::string clickStatus;
    unsigned regionCount = 0;
    std::array<bool, 2> boundHeld{};
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
void startCapture(int slot = 0);
void stopCapture();
bool cancelCaptureEscape(); // Also consumes the matching window Escape event/repeats.
int captureSlot();
Binding binding(int slot);
void clearBinding(int slot);

void load();
void save();
void touch();  // request a debounced save

std::vector<std::string> listPorts();
void connectDevice();
void disconnectDevice();
void clickOnce();  // one test click through the box

// cands = every visible hitbox of every target (overlay pixels), cx/cy = crosshair
void update(const std::vector<Cand>& cands, float cx, float cy, bool projectionReady);

void setLog(void (*fn)(const char*));
void setRemoteKeys(dma_keyboard::Sample (*fn)());
void setLocalKeyReader(bool (*fn)(int)); // Optional adapter; defaults to GetAsyncKeyState.
void pollInputs();
// Shared read-only remote snapshot, also usable without trigger bindings.
void setRemoteInputNeeded(bool needed);
dma_keyboard::Sample remoteSample();
bool remoteKeysReady();
bool remoteKeysOverLan();
void resetRemoteInput();
const char* keySourceLabel(int slot = 0);
const std::string& ident();
void sendRaw(const std::string& cmd);
// Synchronous, no-queue motion on the existing owner/connection. Never clicks.
// Caller must verify firmware semantics and sensitivity before enabling output.
bool moveRelative(int dx, int dy, uint64_t deadlineUs, bool verifiedImmediate);
bool motionDeviceReady(); // Known MAKCU identity on the existing serial connection.
bool motionSetupAvailable();
bool beginMotionSetup(); // Called only after explicit UI confirmation; never emits a movement.
bool beginMotionRestore(); // Read-only channel verification for a measured profile.
bool calibrationMove(int dx,int dy,uint64_t deadlineUs,bool scoped);
void endMotionSetup(bool measured);
void serviceMotion(); // Nonblocking query on the existing owner; no second reader/writer.
const aim_assist::MotionProtocol& motionFeedback();
bool motionPhysicalBusy();
void completeMotionInterval(uint64_t now,bool isolatedResponse=false);
void pauseMotionOutput(const char* reason);
bool resumeMotionOutput();
int monitorPresetCount();
const char* monitorPresetText(int i);
void monitorPreset(int i);

const char* boneName(int i);

}  // namespace trigger
