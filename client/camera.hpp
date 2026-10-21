// RTS camera: orbits a ground point. Yaw 0 looks north (+Y); pitch is the
// angle above the horizon; distance is from the ground point to the eye.
#pragma once

#include "math.hpp"

#include "alamo/ted.hpp"

#include <algorithm>
#include <cmath>

namespace uaw {

struct RtsCamera {
    float x = 0, y = 0;      // ground point
    float yaw = 0;           // degrees
    float pitch = 52;        // degrees
    float distance = 900;

    static constexpr float kMinDistance = 120, kMaxDistance = 4500, kMinPitch = 20, kMaxPitch = 85;

    // Move relative to the view: forward/right in screen terms, scaled by zoom.
    void pan(float forward, float right, float seconds) {
        const float r = yaw * 3.14159265f / 180.f;
        const float speed = distance * 1.4f * seconds;
        x += (std::sin(r) * forward + std::cos(r) * right) * speed;
        y += (std::cos(r) * forward - std::sin(r) * right) * speed;
    }
    void rotate(float dyaw, float dpitch) {
        yaw = std::fmod(yaw + dyaw + 360.f, 360.f);
        pitch = std::clamp(pitch + dpitch, kMinPitch, kMaxPitch);
    }
    void zoom(float factor) { distance = std::clamp(distance * factor, kMinDistance, kMaxDistance); }
    void clampTo(const alamo::Map& m) {
        x = std::clamp(x, 0.f, m.sizeX());
        y = std::clamp(y, 0.f, m.sizeY());
    }

    alamo::Vec3 target(const alamo::Map& m) const { return {x, y, m.heightAt(x, y)}; }
    alamo::Vec3 eye(const alamo::Map& m) const {
        const float r = yaw * 3.14159265f / 180.f, p = pitch * 3.14159265f / 180.f;
        alamo::Vec3 t = target(m);
        alamo::Vec3 e{t[0] - std::sin(r) * std::cos(p) * distance, t[1] - std::cos(r) * std::cos(p) * distance,
                      t[2] + std::sin(p) * distance};
        // Never end up under the ground.
        e[2] = std::max(e[2], m.heightAt(e[0], e[1]) + 30.f);
        return e;
    }
};

} // namespace uaw
