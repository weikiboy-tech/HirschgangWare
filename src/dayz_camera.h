#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace dayz_camera {

struct Vec3 { float x, y, z; };
struct Cam {
    Vec3 right, up, fwd, pos;
    float px, py; // Tangents of the horizontal and vertical half-angles.
};

// Layout used by the existing DayZ World camera reader. A different layout
// requires evidence for both pose and projection, not a search for unit vectors.
constexpr size_t kBytes = 0xE8;
constexpr size_t kBasis = 0x08, kTanX = 0xD0, kTanY = 0xE0;
constexpr uint64_t kProjectionMaxAgeUs = 35000;

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline bool validLens(const Cam& c) {
    return c.px > 0 && std::isnormal(c.px) && c.py > 0 && std::isnormal(c.py);
}

inline bool decodePoseAndLens(const uint8_t* bytes, size_t size, Cam& c) {
    if (size < kBytes) return false;
    static_assert(sizeof(Vec3) == 12);
    std::memcpy(&c.right, bytes + kBasis, sizeof c.right);
    std::memcpy(&c.up, bytes + kBasis + 12, sizeof c.up);
    std::memcpy(&c.fwd, bytes + kBasis + 24, sizeof c.fwd);
    std::memcpy(&c.pos, bytes + kBasis + 36, sizeof c.pos);
    std::memcpy(&c.px, bytes + kTanX, sizeof c.px);
    std::memcpy(&c.py, bytes + kTanY, sizeof c.py);
    if (!finite(c.pos)) return false;
    for (const Vec3 a : {c.right, c.up, c.fwd}) {
        if (!finite(a) || std::fabs(dot(a, a) - 1) > 0.02f) return false;
    }
    return std::fabs(dot(c.right, c.up)) <= 0.02f &&
           std::fabs(dot(c.up, c.fwd)) <= 0.02f &&
           std::fabs(dot(c.fwd, c.right)) <= 0.02f;
}

inline bool sameLens(const Cam& a, const Cam& b) {
    return a.px == b.px && a.py == b.py;
}
inline bool samePose(const Cam& a, const Cam& b) {
    const auto same = [](Vec3 u, Vec3 v) { return u.x == v.x && u.y == v.y && u.z == v.z; };
    return same(a.right, b.right) && same(a.up, b.up) && same(a.fwd, b.fwd) && same(a.pos, b.pos);
}

inline bool fresh(uint64_t stamp, uint64_t now, uint64_t maxAge) {
    return stamp && now >= stamp && now - stamp < maxAge;
}

// Drawing can retain the previous projection across a brief DMA stall.
// Input and targeting still use kProjectionMaxAgeUs.
constexpr uint64_t kVisualHoldMaxAgeUs = 120000;
struct VisualProjection {
    uint64_t world=0, source=0, at=0;
    float lensX=0, lensY=0;
    long left=0, top=0, right=0, bottom=0;

    bool canHold(const VisualProjection& current,uint64_t now) const {
        return world && source && world==current.world && source==current.source &&
            lensX==current.lensX && lensY==current.lensY &&
            left==current.left && top==current.top && right==current.right && bottom==current.bottom &&
            current.at>=at && current.at<=now && fresh(at,now,kVisualHoldMaxAgeUs);
    }
};

inline bool matchesSource(uint64_t snapshotWorld, uint64_t cameraWorld, uint64_t readAddress, uint64_t activeCamera) {
    return snapshotWorld && snapshotWorld == cameraWorld && readAddress && readAddress == activeCamera;
}

inline bool canPredict(const Cam& previous, const Cam& current, bool sameSource, double dt) {
    return sameSource && dt > 0.0005 && dt < 0.1 &&
           validLens(previous) && validLens(current) && sameLens(previous, current);
}

// This is the supported symmetric, full-frame camera projection. The video
// viewport is independent of the lens. Off-screen points remain projectable.
inline bool toScreen(const Cam& c, const Vec3& p, float w, float h, float& sx, float& sy) {
    if (!validLens(c) || !(w > 0 && h > 0)) return false;
    const Vec3 d{p.x - c.pos.x, p.y - c.pos.y, p.z - c.pos.z};
    const float z = dot(d, c.fwd);
    if (!std::isfinite(z) || z < 0.1f) return false;
    sx = w * 0.5f * (1 + dot(d, c.right) / z / c.px);
    sy = h * 0.5f * (1 - dot(d, c.up) / z / c.py);
    // Downstream drawing converts to Windows integer coordinates. Reject
    // unrepresentable results while preserving ordinary off-screen geometry.
    return std::fabs(sx) < 1e8f && std::fabs(sy) < 1e8f;
}

} // namespace dayz_camera
