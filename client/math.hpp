// Small 4x4 matrix helpers for the client (column-major, as OpenGL expects).
// World space is Alamo's: Z up, X east, Y north.
#pragma once

#include "alamo/alo.hpp"

#include <array>
#include <cmath>

namespace uaw {

struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    const float* data() const { return m.data(); }

    friend Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.at(i, k) * b.at(k, j);
                r.at(i, j) = s;
            }
        return r;
    }

    static Mat4 fromAlamo(const alamo::Mat34& a) {
        Mat4 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) r.at(i, j) = a[i * 4 + j];
        return r;
    }
    static Mat4 translate(float x, float y, float z) {
        Mat4 r;
        r.at(0, 3) = x;
        r.at(1, 3) = y;
        r.at(2, 3) = z;
        return r;
    }
    static Mat4 scale(float s) {
        Mat4 r;
        r.at(0, 0) = r.at(1, 1) = r.at(2, 2) = s;
        return r;
    }
    static Mat4 scale3(float x, float y, float z) {
        Mat4 r;
        r.at(0, 0) = x;
        r.at(1, 1) = y;
        r.at(2, 2) = z;
        return r;
    }
    static Mat4 rotateZ(float radians) {
        Mat4 r;
        const float c = std::cos(radians), s = std::sin(radians);
        r.at(0, 0) = c;
        r.at(0, 1) = -s;
        r.at(1, 0) = s;
        r.at(1, 1) = c;
        return r;
    }
    static Mat4 perspective(float fovyRadians, float aspect, float zNear, float zFar) {
        Mat4 r;
        const float f = 1.f / std::tan(fovyRadians / 2);
        r.m = {};
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
        r.at(2, 3) = 2 * zFar * zNear / (zNear - zFar);
        r.at(3, 2) = -1;
        return r;
    }
    static Mat4 lookAt(const alamo::Vec3& eye, const alamo::Vec3& target, const alamo::Vec3& up) {
        alamo::Vec3 f = normalize(sub(target, eye));
        alamo::Vec3 s = normalize(cross(f, up));
        alamo::Vec3 u = cross(s, f);
        Mat4 r;
        for (int i = 0; i < 3; ++i) {
            r.at(0, i) = s[i];
            r.at(1, i) = u[i];
            r.at(2, i) = -f[i];
        }
        r.at(0, 3) = -dot(s, eye);
        r.at(1, 3) = -dot(u, eye);
        r.at(2, 3) = dot(f, eye);
        return r;
    }

    static alamo::Vec3 sub(const alamo::Vec3& a, const alamo::Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    static float dot(const alamo::Vec3& a, const alamo::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static alamo::Vec3 cross(const alamo::Vec3& a, const alamo::Vec3& b) {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }
    static alamo::Vec3 normalize(const alamo::Vec3& v) {
        const float l = std::sqrt(dot(v, v));
        return l > 1e-8f ? alamo::Vec3{v[0] / l, v[1] / l, v[2] / l} : alamo::Vec3{0, 0, 1};
    }
};

} // namespace uaw
