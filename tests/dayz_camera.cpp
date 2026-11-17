#include "../src/dayz_camera.h"
#include "../src/trigger.h"

#include <winsock2.h>
#include <windows.h>
#include "../src/crosshair.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

using namespace dayz_camera;

int main() {
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
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    const auto pixel = pixels[y * width + x];
                    assert(pixel == pixels[y * width + width - 1 - x]);
                    assert(pixel == pixels[(height - 1 - y) * width + x]);
                    hasForeground |= pixel == crosshair::kDefaultColor;
                }
                const auto centerPixel = pixels[(height / 2) * width + width / 2];
                assert(hasForeground);
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
    output.cmdDown.clear();
    trigger::clickOnce();
    assert(live.clicks == 1 && !live.clickStatus.empty());
    trigger::disconnectDevice();
    closesocket(receiver);
    WSACleanup();
    std::puts("DayZ camera, reticle geometry and mouse-box click dispatch passed");
}
