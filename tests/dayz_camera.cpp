#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/dayz_camera.h"
#include "../src/overlay_motion.h"
#include "../src/trigger.h"
#include "../src/trigger_geometry.h"

#include <winsock2.h>
#include <windows.h>
#include "../src/crosshair.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

using namespace dayz_camera;

void testBodyRegions() {
    const Cam camera{{1,0,0},{0,1,0},{0,0,1},{0,0,0},1.f,.5625f};
    const std::array<Vec3,16> body{{
        {0,1.7f,10},{0,1.5f,10},{0,1.2f,10},{0,.9f,10},
        {-.22f,1.43f,10},{-.36f,1.15f,10},{-.4f,.92f,10},
        {.22f,1.43f,10},{.36f,1.15f,10},{.4f,.92f,10},
        {-.12f,.85f,10},{-.12f,.45f,10},{-.12f,0,10},
        {.12f,.85f,10},{.12f,.45f,10},{.12f,0,10}
    }};
    trigger::Cand identity;
    identity.id=7; identity.kind=1; identity.dist=10;
    trigger::Config config;
    config.hitMask=trigger::H_All;
    config.radiusPx=4;
    for (const auto lens : {1.f,.2f,.04f}) {
        for (const auto size : {std::pair<int,int>{1920,1080},{2560,1440},{1440,1080}}) {
            Cam view=camera; view.px=lens; view.py=lens*float(size.second)/float(size.first);
            // Keep the synthetic body inside a narrow scope's visible frame.
            view.pos.y=.9f;
            std::vector<trigger::Cand> regions;
            const trigger_geometry::Viewport viewport{7,9,float(size.first+7),float(size.second+9)};
            trigger_geometry::appendBody(view,viewport,body,1000000,1001000,identity,regions);
            assert(regions.size()==16);
            auto friendRegions = regions;
            for (auto& region : friendRegions) region.isFriend = true;
            trigger::removeFriends(friendRegions);
            assert(friendRegions.empty() && regions.size() == 16);
            const auto matches = [&](Vec3 world) {
                float x,y;
                assert(toScreen(view,world,float(size.first),float(size.second),x,y));
                return trigger::targetAt(config,regions,x+7,y+9).candidate;
            };
            assert(matches({0,1.35f,10})); // Gap between neck and spine is now covered.
            assert(matches({0,1.05f,10})); // Gap between spine and pelvis.
            assert(matches({0,1.7f,10}));  // Head center.
            assert(matches({-.29f,1.29f,10})); // Upper-arm interior.
            assert(matches({-.12f,.25f,10})); // Lower-leg interior.
            assert(!matches({.6f,1.2f,10}));  // Outside the torso.
            assert(!matches({0,.1f,10}));    // Gap between the legs stays empty.
            config.radiusPx=60;
            assert(!matches({.6f,1.2f,10})); // High legacy radii cannot inflate regions.
            config.radiusPx=0;
            assert(matches({0,1.35f,10}));
            config.hitMask=trigger::H_Spine | trigger::H_Pelvis;
            for (const float height : {1.35f, 1.05f}) {
                assert(matches({.145f,height,10}));
                assert(matches({-.145f,height,10}));
                assert(!matches({.17f,height,10}));
                assert(!matches({-.17f,height,10}));
            }
            config.hitMask=trigger::H_Head;
            assert(!matches({0,1.35f,10}));
            assert(matches({0,1.7f,10}));
            assert(!matches({.11f,1.7f,10})); // Head radius is unchanged.
            config.hitMask=trigger::H_All;
            config.radiusPx=4;
        }
    }
    std::vector<trigger::Cand> regions;
    const trigger_geometry::Viewport viewport{0,0,1920,1080};
    trigger_geometry::appendBody(camera,viewport,body,1000000,1075000,identity,regions);
    assert(regions.empty()); // Exact freshness boundary.
    trigger_geometry::appendBody(camera,viewport,body,1000000,999999,identity,regions);
    assert(regions.empty());
    auto missing=body;
    for (auto& joint:missing) joint={NAN,NAN,NAN};
    trigger_geometry::appendBody(camera,viewport,missing,1000000,1001000,identity,regions);
    assert(regions.empty());
    auto behind=body;
    for(auto& joint:behind) joint.z=-10;
    trigger_geometry::appendBody(camera,viewport,behind,1000000,1001000,identity,regions);
    assert(regions.empty());
    auto moved=body;
    for(auto& joint:moved) joint.x+=.7f;
    trigger_geometry::appendBody(camera,viewport,moved,1000000,1001000,identity,regions);
    float x,y;
    assert(toScreen(camera,{.7f,1.35f,10},1920,1080,x,y));
    assert(trigger::targetAt(config,regions,x,y).candidate);
    assert(toScreen(camera,{0,1.35f,10},1920,1080,x,y));
    assert(!trigger::targetAt(config,regions,x,y).candidate);
    auto invalid=regions.front();
    invalid.region[0].x=NAN;
    assert(!trigger::targetAt(config,{invalid},invalid.x,invalid.y).candidate);
    invalid.regionSize=3;
    invalid.region[0]=invalid.region[1]=invalid.region[2]={0,0};
    assert(!trigger::targetAt(config,{invalid},0,0).candidate);
    config.maxDistM=5;
    assert(!trigger::targetAt(config,regions,regions[0].x,regions[0].y).candidate);
    config.maxDistM=120;
    auto lying=body;
    for(auto& joint:lying) {
        const float oldX=joint.x;
        joint.x=joint.y;
        joint.y=-oldX;
    }
    regions.clear();
    trigger_geometry::appendBody(camera,viewport,lying,1000000,1001000,identity,regions);
    assert(toScreen(camera,{1.35f,0,10},1920,1080,x,y));
    assert(trigger::targetAt(config,regions,x,y).candidate);
    assert(toScreen(camera,{1.35f,.6f,10},1920,1080,x,y));
    assert(!trigger::targetAt(config,regions,x,y).candidate);
}

namespace {
dma_keyboard::Sample inputSample;
std::array<bool, 256> localInput{};
dma_keyboard::Sample remoteInput() { return inputSample; }
bool localInputDown(int key) { return key > 0 && key < 256 && localInput[size_t(key)]; }
void remoteKey(int vk, bool down) {
    const uint8_t bit = uint8_t(1u << (2 * (unsigned(vk) % 4)));
    auto& byte = inputSample.bits[size_t(vk) / 4];
    if (down) byte |= bit; else byte &= uint8_t(~bit);
}
// GetTickCount64 can have a ~16 ms tick in the isolated test process.
// Give the production 8 ms sample gate a complete tick between snapshots.
void inputStep() { Sleep(25); trigger::pollInputs(); }
int inputVk(int key) {
    constexpr int buttons[]{0, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    return key > 0 && key <= 5 ? buttons[key] : key;
}
}

void testIndependentBindings() {
    trigger::cfg() = {};
    auto& cfg = trigger::cfg();
    cfg.autoConnect = false;
    cfg.key = 'E'; cfg.keySource = trigger::InputSource::MainPc;
    cfg.secondary = {5, 0, trigger::InputSource::SecondPc};
    trigger::setRemoteKeys(remoteInput);
    trigger::setLocalKeyReader(localInputDown);
    inputSample = {}; inputSample.pid = 42;
    inputStep();
    localInput['E'] = true; inputStep();
    assert(!trigger::live().keyHeld); // Same key on the wrong PC is isolated.
    localInput['E'] = false;
    remoteKey('E', true); inputStep();
    assert(trigger::live().keyHeld && trigger::live().boundHeld[0]);
    localInput[5] = true; inputStep();
    assert(trigger::live().boundHeld[0] && trigger::live().boundHeld[1]);
    remoteKey('E', false); inputStep();
    assert(trigger::live().keyHeld && !trigger::live().boundHeld[0]);
    localInput[5] = false; inputStep();
    assert(!trigger::live().keyHeld);
    localInput[5] = true; inputStep();
    assert(trigger::live().keyHeld && trigger::live().boundHeld[1]);
    localInput.fill(false); inputStep();
    // Identical keys on separate PCs remain independent, with OR semantics.
    cfg.key = 'E'; cfg.keySource = trigger::InputSource::MainPc;
    cfg.secondary = {'E', 0, trigger::InputSource::SecondPc};
    remoteKey('E', true); localInput['E'] = true; inputStep();
    assert(trigger::live().boundHeld[0] && trigger::live().boundHeld[1]);
    localInput['E'] = false; inputStep();
    assert(trigger::live().keyHeld && trigger::live().boundHeld[0] && !trigger::live().boundHeld[1]);
    localInput['E'] = true; inputSample.pid = 0; inputStep();
    assert(trigger::live().keyHeld && !trigger::live().boundHeld[0] && trigger::live().boundHeld[1]);
    localInput.fill(false); inputSample.pid = 42; inputSample.bits = {}; inputStep();
    // Either slot can use either PC, including every supported mouse button.
    for (int slot = 0; slot < 2; ++slot) {
        for (const auto source : {trigger::InputSource::MainPc, trigger::InputSource::SecondPc}) {
            for (const int key : std::array<int, 6>{'W', 1, 2, 3, 4, 5}) {
                cfg.key = 0; cfg.secondary = {};
                if (slot) cfg.secondary = {key, 0, source};
                else { cfg.key = key; cfg.keySource = source; }
                inputStep();
                if (source == trigger::InputSource::MainPc) remoteKey(inputVk(key), true);
                else localInput[size_t(key)] = true;
                inputStep();
                if (!trigger::live().keyHeld || !trigger::live().boundHeld[size_t(slot)])
                    std::fprintf(stderr,"binding input failed: slot=%d source=%d key=%d remote=%d\n",slot,int(source),key,int(trigger::remoteKeysReady()));
                assert(trigger::live().keyHeld && trigger::live().boundHeld[size_t(slot)]);
                inputSample.bits = {}; localInput.fill(false); inputStep();
                assert(!trigger::live().keyHeld);
            }
        }
    }
    // A held key on a newly attached session is not a new press.
    cfg.key = 'E'; cfg.keySource = trigger::InputSource::MainPc; cfg.secondary = {};
    inputSample.pid = 43; remoteKey('E', true); inputStep();
    assert(!trigger::live().keyHeld);
    remoteKey('E', false); inputStep(); remoteKey('E', true); inputStep();
    assert(trigger::live().keyHeld);
    inputSample.pid = 0; inputStep();
    assert(!trigger::live().keyHeld);
    inputSample.pid = 44; inputSample.bits = {}; inputStep();
    cfg.secondary = {'Q', 0, trigger::InputSource::SecondPc};
    trigger::startCapture(1); inputStep(); remoteKey('W', true); inputStep();
    assert(!trigger::capturing() && cfg.secondary.key == 'W' && cfg.key == 'E');
    assert(cfg.secondary.source == trigger::InputSource::MainPc && !trigger::live().keyHeld);
    inputStep(); assert(!trigger::live().keyHeld); // Must release a newly bound key first.
    remoteKey('W', false); inputStep(); remoteKey('W', true); inputStep();
    assert(trigger::live().keyHeld); remoteKey('W', false); inputStep();
    // Esc cancels even before the capture is armed, and keeps both old bindings.
    for (int slot = 0; slot < 2; ++slot) {
        trigger::startCapture(slot); localInput[VK_ESCAPE] = true; inputStep();
        assert(!trigger::capturing() && cfg.key == 'E' && cfg.secondary.key == 'W');
        assert(trigger::cancelCaptureEscape()); // Window keydown/repeat is consumed after polling cancellation.
        localInput.fill(false); inputStep();
        trigger::startCapture(slot); inputStep(); remoteKey(VK_ESCAPE, true); inputStep();
        assert(!trigger::capturing() && cfg.key == 'E' && cfg.secondary.key == 'W');
        remoteKey(VK_ESCAPE, false); inputStep();
    }
    trigger::clearBinding(1); trigger::startCapture(1);
    localInput[VK_ESCAPE] = true; inputStep();
    assert(!trigger::capturing() && cfg.secondary.key == 0 && cfg.key == 'E');
    localInput.fill(false); inputStep();
    // Local keyboard and both mice are recognized by capture, too.
    for (const int key : std::array<int, 5>{'F', 2, 3, 4, 5}) {
        trigger::startCapture(1); inputStep(); localInput[size_t(key)] = true; inputStep();
        assert(cfg.secondary.key == key && cfg.secondary.source == trigger::InputSource::SecondPc);
        localInput.fill(false); inputStep();
        trigger::startCapture(1); inputStep(); remoteKey(inputVk(key), true); inputStep();
        assert(cfg.secondary.key == key && cfg.secondary.source == trigger::InputSource::MainPc);
        inputSample.bits = {}; inputStep();
    }
    cfg.key = 'E'; cfg.keySource = trigger::InputSource::MainPc;
    cfg.secondary = {'W', 0, trigger::InputSource::SecondPc};
    trigger::save(); cfg = {}; trigger::load();
    assert(cfg.key == 'E' && cfg.keySource == trigger::InputSource::MainPc);
    assert(cfg.secondary.key == 'W' && cfg.secondary.source == trigger::InputSource::SecondPc);
    cfg.key = VK_ESCAPE; cfg.secondary.key = VK_ESCAPE; trigger::save(); cfg = {}; trigger::load();
    assert(cfg.key == 0 && cfg.secondary.key == 0 && !trigger::bindableKey(VK_ESCAPE));
    inputStep(); trigger::setRemoteKeys(nullptr); trigger::setLocalKeyReader(nullptr);
    cfg = {}; trigger::live() = {};
}

int main() {
    testIndependentBindings();
    testBodyRegions();
    const uint64_t sample = 1000000;
    assert(overlay_motion::leadSeconds(0, sample) == 0.f);
    assert(overlay_motion::leadSeconds(sample, sample - 1) == 0.f);
    assert(std::fabs(overlay_motion::leadSeconds(sample, sample + 10000) - .01f) < 1e-6f);
    for (uint64_t age = 1; age < 100000; ++age) {
        const float before = overlay_motion::leadSeconds(sample, sample + age - 1);
        const float after = overlay_motion::leadSeconds(sample, sample + age);
        assert(after >= 0 && after <= .05f);
        assert(std::fabs(after - before) < .000005f);
    }
    assert(overlay_motion::leadSeconds(sample, sample + 80000) == 0.f);
    assert(overlay_motion::leadSeconds(sample, sample + 300000) == 0.f);
    // Capture the actual overlay fill calls and check their pixel symmetry
    // around the same center used by target detection.
    for (int width : {64, 65}) for (int height : {64, 65}) {
        const RECT viewport{7, 9, 7 + width, 9 + height};
        assert(crosshair::center(viewport.left, viewport.right) == 7 + width / 2.f);
        assert(crosshair::center(viewport.top, viewport.bottom) == 9 + height / 2.f);
        for (auto shape : {crosshair::Style::Plus, crosshair::Style::Ring, crosshair::Style::Dot}) {
            std::array<COLORREF, 65 * 65> pixels{};
            for (int size : {crosshair::kMinSize, 5, crosshair::kMaxSize}) {
                pixels.fill(0);
                crosshair::draw(viewport, shape, size, crosshair::kDefaultColor,
                                [&](const RECT& r, COLORREF color) {
                    assert(r.left >= viewport.left && r.top >= viewport.top);
                    assert(r.right <= viewport.right && r.bottom <= viewport.bottom);
                    for (LONG y = r.top; y < r.bottom; ++y)
                        for (LONG x = r.left; x < r.right; ++x)
                            pixels[(y - viewport.top) * width + x - viewport.left] = color;
                });
                bool hasForeground = false;
                int ringPixels = 0;
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    const auto pixel = pixels[y * width + x];
                    if (shape == crosshair::Style::Ring && pixel) {
                        assert(pixel == crosshair::kDefaultColor);
                        ++ringPixels;
                    }
                    assert(pixel == pixels[y * width + width - 1 - x]);
                    assert(pixel == pixels[(height - 1 - y) * width + x]);
                    hasForeground |= pixel == crosshair::kDefaultColor;
                }
                const auto centerPixel = pixels[(height / 2) * width + width / 2];
                assert(hasForeground);
                if (shape == crosshair::Style::Ring) assert(ringPixels <= 8 * size + 8);
                assert(centerPixel == (shape == crosshair::Style::Ring ? 0 : crosshair::kDefaultColor));
            }
        }
    }

    std::array<uint8_t, kBytes> bytes{};
    const Vec3 pose[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {10, 20, 30}};
    std::memcpy(bytes.data() + kBasis, pose, sizeof pose);
    const auto decode = [&](float px, float py) {
        std::memcpy(bytes.data() + kTanX, &px, sizeof px);
        std::memcpy(bytes.data() + kTanY, &py, sizeof py);
        Cam c{};
        assert(decodePoseAndLens(bytes.data(), bytes.size(), c));
        return c;
    };
    const auto point = [](const Cam& c, Vec3 p, float w, float h, float x, float y) {
        float sx = 0, sy = 0;
        assert(toScreen(c, p, w, h, sx, sy));
        assert(std::fabs(x - sx) < 0.001f && std::fabs(y - sy) < 0.001f);
    };
    const Cam normal = decode(1, 0.5625f);
    const Cam ads = decode(0.5f, 0.28125f);
    const Cam low = decode(0.1f, 0.05625f);
    const Cam high = decode(0.04f, 0.0225f); // Both under the previous 0.05 cutoff.
    const Vec3 fixed{12, 21.125f, 130};
    point(normal, fixed, 1920, 1080, 979.2f, 529.2f);
    point(ads, fixed, 1920, 1080, 998.4f, 518.4f);
    point(low, fixed, 1920, 1080, 1152.f, 432.f);
    point(high, fixed, 1920, 1080, 1440.f, 270.f);
    point(high, fixed, 2560, 1440, 1920.f, 360.f);
    point(high, fixed, 1440, 1080, 1080.f, 270.f);
    point(low, fixed, 1920, 1080, 1152.f, 432.f);
    point(normal, fixed, 1920, 1080, 979.2f, 529.2f);
    point(normal, {110, -36.25f, 130}, 1920, 1080, 1920.f, 1080.f);
    point(high, {10, 20, 130}, 1279, 719, 639.5f, 359.5f);
    Cam yaw = high;
    yaw.right = {0, 0, -1};
    yaw.fwd = {1, 0, 0};
    point(yaw, {110, 20, 28}, 1920, 1080, 1440.f, 540.f);

    float x, y;
    assert(!toScreen(high, {10, 20, 29}, 1920, 1080, x, y));
    assert(!toScreen(high, fixed, 0, 1080, x, y));
    for (float bad : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        const Cam invalid = decode(1.f, bad);
        assert(!validLens(invalid));
        assert(!toScreen(invalid, fixed, 1920, 1080, x, y));
        assert(!canPredict(normal, invalid, true, 0.01));
    }
    Cam bad{};
    assert(!decodePoseAndLens(bytes.data(), 1, bad));
    bytes.fill(0);
    std::memcpy(bytes.data() + kBasis + 4, pose, sizeof pose); // Shifted layout must fail.
    assert(!decodePoseAndLens(bytes.data(), bytes.size(), bad));
    assert(matchesSource(1, 1, 2, 2));
    assert(!matchesSource(1, 2, 2, 2));
    assert(!matchesSource(1, 1, 2, 3));
    assert(!matchesSource(1, 1, 2, 0));
    assert(fresh(1000, 35999, kProjectionMaxAgeUs));
    assert(!fresh(1000, 36000, kProjectionMaxAgeUs));
    assert(!fresh(1000, 999, kProjectionMaxAgeUs));
    assert(canPredict(high, yaw, true, 0.01));
    assert(!canPredict(low, high, true, 0.01));
    assert(!canPredict(high, yaw, false, 0.01));

    trigger::Config cfg;
    cfg.radiusPx = 15;
    cfg.maxDistM = 120;
    const std::vector<trigger::Cand> targets{
        {639.5f, 359.5f, 121, 0, 1, false, 1}, // Farther than configured range.
        {639.5f, 359.5f, 50, 0, 2, false, 2},  // Infected, players-only on.
        {639.5f, 359.5f, 50, 4, 1, false, 3},  // Masked joint.
        {639.5f, 359.5f, 50, -1, 1, false, 6}, // Invalid joint.
        {654.5f, 359.5f, 50, 1, 1, false, 4},  // Exactly on the radius.
        {655.5f, 359.5f, 50, 0, 1, false, 5},
    };
    assert(trigger::targetAt(cfg, targets, 639.5f, 359.5f).candidate->id == 4);
    assert(!trigger::targetAt(cfg, targets, 600.f, 359.5f).candidate);
    cfg.playersOnly = false;
    assert(trigger::targetAt(cfg, targets, 639.5f, 359.5f).candidate->id == 2);
    trigger::cfg() = cfg;
    trigger::cfg().autoConnect = false;
    assert(!trigger::connected() && !trigger::cfg().enabled);
    trigger::update(targets, 639.5f, 359.5f, true);
    assert(trigger::live().projectionReady && trigger::live().target && trigger::live().bone == 0);
    trigger::update(targets, 600.f, 359.5f, true);
    assert(trigger::live().projectionReady && !trigger::live().target && trigger::live().bone == -1);
    trigger::update(targets, 639.5f, 359.5f, false);
    assert(!trigger::live().projectionReady && !trigger::live().target && !trigger::live().clicks);

    // Exercise production click dispatch against a local UDP receiver.
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    const SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(receiver != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    int addressSize = sizeof address;
    assert(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    DWORD timeout = 1000;
    assert(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout) == 0);
    auto& output = trigger::cfg();
    output.modeNet = true;
    output.host = "127.0.0.1";
    output.netPort = ntohs(address.sin_port);
    output.sendMonitor = false;
    output.cmdConnect.clear();
    output.delayMs = 0;
    output.clickMs = 2;
    trigger::connectDevice();
    assert(trigger::connected());

    auto& live = trigger::live();
    live.keyHeld = true;
    trigger::update(targets, 639.5f, 359.5f, true); // Output disabled.
    output.enabled = true;
    live.keyHeld = false;
    trigger::update(targets, 639.5f, 359.5f, true); // Target alone.
    live.keyHeld = true;
    trigger::update(targets, 600.f, 359.5f, true);  // Key alone.
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(receiver, &readSet);
    timeval immediate{};
    assert(select(0, &readSet, nullptr, nullptr, &immediate) == 0 && live.clicks == 0);

    const auto receive = [&](const std::string& expected) {
        char packet[64];
        const int size = recv(receiver, packet, sizeof packet, 0);
        if (size <= 0) std::fprintf(stderr, "Expected %s, recv error %d, output status: %s\n",
                                    expected.c_str(), WSAGetLastError(), live.clickStatus.c_str());
        assert(size > 0);
        assert(std::string(packet, static_cast<size_t>(size)) == expected);
    };
    trigger::update(targets, 639.5f, 359.5f, true);
    receive(output.cmdDown);
    assert(live.clicks == 0);
    const ULONGLONG pressedAt = GetTickCount64();
    while (GetTickCount64() - pressedAt < ULONGLONG(output.clickMs)) Sleep(1);
    live.keyHeld = false;
    trigger::update({}, 0, 0, false);
    receive(output.cmdUp);
    assert(live.clicks == 1);
    output.refireMs = 25;
    output.clickMs = 8;
    const auto repeatStart = GetTickCount64();
    live.keyHeld = true;
    int completed = live.clicks;
    // Exercise repeated downs/ups through the real output state machine.
    while (live.clicks < 4 && GetTickCount64() - repeatStart < 1000) {
        trigger::update(targets, 639.5f, 359.5f, true);
        if (live.clicks > completed) {
            receive(output.cmdDown);
            receive(output.cmdUp);
            completed = live.clicks;
        }
        Sleep(1);
    }
    assert(live.clicks == 4);
    assert(GetTickCount64() - repeatStart >= 50);
    live.keyHeld = false;
    for (int i = 0; i < 40; ++i) {
        trigger::update(targets, 639.5f, 359.5f, true);
        Sleep(1);
    }
    assert(live.clicks == 4);
    live.keyHeld = true;
    for (int i = 0; i < 40; ++i) {
        trigger::update({}, 639.5f, 359.5f, true);
        Sleep(1);
    }
    assert(live.clicks == 4);
    trigger::Cand area;
    area.id=99; area.kind=1; area.dist=20; area.bone=2;
    area.x=639.5f; area.y=340;
    area.regionSize=4;
    area.region[0]={637.5f,335}; area.region[1]={641.5f,335};
    area.region[2]={641.5f,385}; area.region[3]={637.5f,385};
    area.maxTolerancePx=.2f;
    const auto areaStart=GetTickCount64();
    while(live.clicks<5 && GetTickCount64()-areaStart<1000) {
        trigger::update({area},639.5f,359.5f,true);
        Sleep(1);
    }
    assert(live.regionCount==1 && live.clicks==5);
    receive(output.cmdDown); receive(output.cmdUp);
    for(int i=0;i<40;++i) {
        trigger::update({area},645.f,359.5f,true);
        Sleep(1);
    }
    assert(!live.target && live.clicks==5);
    area.isFriend = true;
    std::vector<trigger::Cand> friendOnly{area};
    trigger::removeFriends(friendOnly);
    assert(friendOnly.empty());
    for (int i = 0; i < 40; ++i) {
        trigger::update(friendOnly,639.5f,359.5f,true);
        Sleep(1);
    }
    assert(live.keyHeld && !live.target && live.clicks == 5);
    FD_ZERO(&readSet); FD_SET(receiver, &readSet);
    assert(select(0,&readSet,nullptr,nullptr,&immediate) == 0);
    area.isFriend = false;
    std::vector<trigger::Cand> enemies{area};
    trigger::removeFriends(enemies);
    assert(enemies.size() == 1);
    trigger::update(enemies,639.5f,359.5f,true);
    receive(output.cmdDown);
    // Marking the target as a friend removes it from the following frame.
    enemies[0].isFriend = true;
    trigger::removeFriends(enemies);
    trigger::update(enemies,639.5f,359.5f,true);
    receive(output.cmdUp);
    assert(!live.target && live.clicks == 6);
    output.cmdDown.clear();
    trigger::clickOnce();
    assert(live.clicks == 6 && !live.clickStatus.empty());
    trigger::disconnectDevice();
    closesocket(receiver);
    WSACleanup();
    std::puts("DayZ camera, reticle geometry and mouse-box click dispatch passed");
}
