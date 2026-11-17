#pragma once

#include "dayz_camera.h"
#include "trigger.h"

namespace trigger_geometry {

using dayz_camera::Vec3;
using Point = trigger::Cand::Point;
constexpr uint64_t kPoseMaxAgeUs = 75000;
struct Viewport { float left, top, right, bottom; };

inline Vec3 midpoint(Vec3 a, Vec3 b) {
    return {(a.x + b.x) * .5f, (a.y + b.y) * .5f, (a.z + b.z) * .5f};
}

inline float length(Vec3 a, Vec3 b) {
    const Vec3 d{a.x - b.x, a.y - b.y, a.z - b.z};
    return std::sqrt(dayz_camera::dot(d, d));
}

inline float turn(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline void capsule(const dayz_camera::Cam& camera, const Viewport& viewport,
                    Vec3 a, Vec3 b, float radius, trigger::Cand candidate,
                    std::vector<trigger::Cand>& output) {
    if (!(radius > 0) || !std::isfinite(radius)) return;
    std::array<Point, 32> points{};
    float minimumRadius = INFINITY;
    for (unsigned end = 0; end < 2; ++end) {
        const Vec3 center = end ? b : a;
        const Vec3 delta{center.x - camera.pos.x, center.y - camera.pos.y, center.z - camera.pos.z};
        const float depth = dayz_camera::dot(delta, camera.fwd);
        if (!(depth > radius * 2.f)) return;
        float x, y;
        const float width = float(viewport.right - viewport.left), height = float(viewport.bottom - viewport.top);
        if (!dayz_camera::toScreen(camera, center, width, height, x, y)) return;
        const float rx = width * .5f * radius / (depth * camera.px);
        const float ry = height * .5f * radius / (depth * camera.py);
        if (!std::isfinite(rx) || !std::isfinite(ry)) return;
        minimumRadius = std::min(minimumRadius, std::min(rx, ry));
        for (unsigned i = 0; i < 16; ++i) {
            const float angle = float(i) * 6.28318530718f / 16.f;
            points[end * 16 + i] = {viewport.left + x + rx * std::cos(angle),
                                    viewport.top + y + ry * std::sin(angle)};
        }
    }
    std::sort(points.begin(), points.end(), [](Point p, Point q) {
        return p.x < q.x || (p.x == q.x && p.y < q.y);
    });
    std::array<Point, 64> hull{};
    unsigned count = 0;
    for (const auto p : points) {
        while (count >= 2 && turn(hull[count - 2], hull[count - 1], p) <= 0) --count;
        hull[count++] = p;
    }
    const unsigned lower = count + 1;
    for (int i = int(points.size()) - 2; i >= 0; --i) {
        const auto p = points[size_t(i)];
        while (count >= lower && turn(hull[count - 2], hull[count - 1], p) <= 0) --count;
        hull[count++] = p;
    }
    if (count <= 3 || --count > candidate.region.size()) return;
    candidate.regionSize = count;
    candidate.x = candidate.y = 0;
    for (unsigned i = 0; i < count; ++i) {
        candidate.region[i] = hull[i];
        candidate.x += hull[i].x / float(count);
        candidate.y += hull[i].y / float(count);
    }
    // Subpixel targets must not become a fixed, several-pixel hit circle.
    candidate.maxTolerancePx = std::min(1.f, minimumRadius * .1f);
    output.push_back(candidate);
}

inline void appendBody(const dayz_camera::Cam& camera, const Viewport& viewport,
                       const std::array<Vec3, 16>& joints, uint64_t poseAt, uint64_t now,
                       const trigger::Cand& identity, std::vector<trigger::Cand>& output) {
    if (!dayz_camera::fresh(poseAt, now, kPoseMaxAgeUs) || !dayz_camera::validLens(camera) ||
        (identity.kind != 1 && identity.kind != 2)) return;
    const auto valid = [&](int joint) { return dayz_camera::finite(joints[size_t(joint)]); };
    const auto add = [&](int joint, Vec3 a, Vec3 b, float radius) {
        if (!dayz_camera::finite(a) || !dayz_camera::finite(b) || length(a, b) > 1.f) return;
        auto candidate = identity;
        candidate.bone = joint;
        capsule(camera, viewport, a, b, radius, candidate, output);
    };
    // Conservative anatomical envelopes derived from the measured skeleton.
    // They approximate body volume. They are not engine collision hitboxes.
    if (valid(0) && valid(1)) {
        const float head = length(joints[0], joints[1]);
        if (head >= .04f && head <= .5f)
            add(0, joints[0], joints[0], std::min(.12f, head * .5f));
    }
    if (!valid(4) || !valid(7)) return;
    const float shoulders = length(joints[4], joints[7]);
    if (!(shoulders >= .12f && shoulders <= 1.f)) return;
    if (valid(1)) add(1, joints[1], midpoint(joints[4], joints[7]), shoulders * .12f);
    if (valid(1) && valid(2)) add(2, joints[1], joints[2], shoulders * .30f);
    if (valid(2) && valid(3)) add(3, joints[2], joints[3], shoulders * .30f);
    constexpr int chains[][3] = {{4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}};
    for (const auto& chain : chains) {
        const float radius = shoulders * (chain[0] >= 10 ? .18f : .12f);
        for (int i = 0; i < 3; ++i) {
            const int joint = chain[i];
            if (!valid(joint)) continue;
            const Vec3 center = joints[size_t(joint)];
            const Vec3 a = i && valid(chain[i - 1]) ? midpoint(joints[size_t(chain[i - 1])], center) : center;
            const Vec3 b = i < 2 && valid(chain[i + 1]) ? midpoint(center, joints[size_t(chain[i + 1])]) : center;
            add(joint, a, b, radius);
        }
    }
}

} // namespace trigger_geometry
