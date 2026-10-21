#include "alamo/ala.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

namespace alamo {

namespace {

struct Reader {
    std::span<const uint8_t> d;
    size_t pos = 0;
    bool chunk(uint32_t& id, bool& container, std::span<const uint8_t>& body) {
        if (pos == d.size()) return false;
        if (d.size() - pos < 8) throw AloError("truncated animation chunk header");
        uint32_t size;
        std::memcpy(&id, &d[pos], 4);
        std::memcpy(&size, &d[pos + 4], 4);
        container = size & 0x80000000u;
        size &= 0x7FFFFFFFu;
        if (size > d.size() - pos - 8) throw AloError("animation chunk runs past its parent");
        body = d.subspan(pos + 8, size);
        pos += 8 + size_t(size);
        return true;
    }
    bool mini(uint8_t& id, std::span<const uint8_t>& body) {
        if (pos + 2 > d.size()) return false;
        id = d[pos];
        uint8_t size = d[pos + 1];
        if (pos + 2 + size > d.size()) throw AloError("truncated animation mini-chunk");
        body = d.subspan(pos + 2, size);
        pos += 2 + size;
        return true;
    }
};

template <class T>
T rd(std::span<const uint8_t> s, size_t off = 0) {
    if (off + sizeof(T) > s.size()) throw AloError("animation field past end of chunk");
    T v;
    std::memcpy(&v, s.data() + off, sizeof(T));
    return v;
}

Vec3 rdVec3(std::span<const uint8_t> s) { return {rd<float>(s, 0), rd<float>(s, 4), rd<float>(s, 8)}; }

Vec4 normalizeQ(Vec4 q) {
    float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-8f) return {0, 0, 0, 1};
    return {q[0] / l, q[1] / l, q[2] / l, q[3] / l};
}

Vec4 unpackQ(const int16_t* s) {
    return normalizeQ({s[0] / 32767.f, s[1] / 32767.f, s[2] / 32767.f, s[3] / 32767.f});
}

bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

void readBone(std::span<const uint8_t> body, Animation& a) {
    AnimBone b;
    Reader r{body};
    uint32_t id;
    bool container;
    std::span<const uint8_t> c;
    while (r.chunk(id, container, c)) {
        if (id == 0x1003) {
            Reader m{c};
            uint8_t mid;
            std::span<const uint8_t> mb;
            while (m.mini(mid, mb)) {
                switch (mid) {
                case 0x04: {
                    size_t n = 0;
                    while (n < mb.size() && mb[n]) ++n;
                    b.name.assign(reinterpret_cast<const char*>(mb.data()), n);
                    break;
                }
                case 0x05: b.index = rd<uint32_t>(mb); break;
                case 0x06: b.translationOffset = rdVec3(mb); break;
                case 0x07: b.translationScale = rdVec3(mb); break;
                case 0x0E: b.translationIndex = rd<int16_t>(mb); break;
                case 0x10: b.rotationIndex = rd<int16_t>(mb); break;
                case 0x11: {
                    int16_t q[4];
                    for (int i = 0; i < 4; ++i) q[i] = rd<int16_t>(mb, 2 * i);
                    b.defaultRotation = unpackQ(q);
                    break;
                }
                default: break; // 0x08/0x09 scale, 0x0A, 0x0F: scale tracks are unused by shipped data
                }
            }
        } else if (id == 0x1007) {
            b.visibility.resize(size_t(c.size()) * 8);
            for (size_t i = 0; i < b.visibility.size(); ++i) b.visibility[i] = (c[i / 8] >> (i % 8)) & 1;
        }
    }
    a.bones.push_back(std::move(b));
}

} // namespace

Animation loadAla(std::span<const uint8_t> data) {
    Animation a;
    Reader top{data};
    uint32_t id;
    bool container;
    std::span<const uint8_t> body;
    if (!top.chunk(id, container, body) || id != 0x1000) throw AloError("not an ALA animation (missing 0x1000 chunk)");

    Reader r{body};
    std::span<const uint8_t> c;
    while (r.chunk(id, container, c)) {
        switch (id) {
        case 0x1001: {
            Reader m{c};
            uint8_t mid;
            std::span<const uint8_t> mb;
            while (m.mini(mid, mb)) {
                switch (mid) {
                case 0x01: a.frameCount = rd<uint32_t>(mb); break;
                case 0x02: a.fps = rd<float>(mb); break;
                case 0x0B: a.rotationBlock = rd<uint32_t>(mb); break;
                case 0x0C: a.translationBlock = rd<uint32_t>(mb); break;
                default: break;
                }
            }
            break;
        }
        case 0x1002: readBone(c, a); break;
        case 0x1009:
            a.rotations.resize(c.size() / 2);
            std::memcpy(a.rotations.data(), c.data(), a.rotations.size() * 2);
            break;
        case 0x100A:
            a.translations.resize(c.size() / 2);
            std::memcpy(a.translations.data(), c.data(), a.translations.size() * 2);
            break;
        default: break; // 0x1008: unknown, present in a handful of files
        }
    }

    if (a.frameCount > 100000) throw AloError("implausible frame count");
    if (a.rotations.size() < size_t(a.frameCount) * a.rotationBlock)
        throw AloError("rotation data shorter than frames x block size");
    if (a.translations.size() < size_t(a.frameCount) * a.translationBlock)
        throw AloError("translation data shorter than frames x block size");
    for (auto& b : a.bones) {
        if (b.rotationIndex >= 0 && uint32_t(b.rotationIndex) + 4 > a.rotationBlock) {
            a.warnings.push_back(b.name + ": rotation index outside block");
            b.rotationIndex = -1;
        }
        if (b.translationIndex >= 0 && uint32_t(b.translationIndex) + 3 > a.translationBlock) {
            a.warnings.push_back(b.name + ": translation index outside block");
            b.translationIndex = -1;
        }
    }
    return a;
}

const AnimBone* Animation::find(const std::string& name) const {
    for (const auto& b : bones)
        if (iequal(b.name, name)) return &b;
    return nullptr;
}

Vec3 Animation::translation(const AnimBone& b, uint32_t frame) const {
    if (b.translationIndex < 0 || !frameCount) return b.translationOffset;
    frame = std::min(frame, frameCount - 1);
    const uint16_t* t = &translations[size_t(frame) * translationBlock + size_t(b.translationIndex)];
    return {b.translationOffset[0] + t[0] * b.translationScale[0], b.translationOffset[1] + t[1] * b.translationScale[1],
            b.translationOffset[2] + t[2] * b.translationScale[2]};
}

Vec4 Animation::rotation(const AnimBone& b, uint32_t frame) const {
    if (b.rotationIndex < 0 || !frameCount) return b.defaultRotation;
    frame = std::min(frame, frameCount - 1);
    return unpackQ(&rotations[size_t(frame) * rotationBlock + size_t(b.rotationIndex)]);
}

Mat34 Animation::local(const AnimBone& b, uint32_t frame) const { return quatToMatrix(rotation(b, frame), translation(b, frame)); }

bool Animation::visible(const AnimBone& b, uint32_t frame) const {
    if (b.visibility.empty()) return true;
    return frame < b.visibility.size() ? b.visibility[frame] : b.visibility.back();
}

Mat34 quatToMatrix(const Vec4& q, const Vec3& t) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     t[0],
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     t[1],
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), t[2]};
}

Mat34 inverseRigid(const Mat34& m) {
    // Handles rotation * uniform scale: inverse = R^T / s^2.
    float s2 = m[0] * m[0] + m[4] * m[4] + m[8] * m[8];
    if (s2 < 1e-12f) s2 = 1;
    Mat34 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 4 + j] = m[j * 4 + i] / s2;
    for (int i = 0; i < 3; ++i) r[i * 4 + 3] = -(r[i * 4 + 0] * m[3] + r[i * 4 + 1] * m[7] + r[i * 4 + 2] * m[11]);
    return r;
}

std::vector<Mat34> poseModel(const Model& model, const Animation* anim, uint32_t frame) {
    const size_t n = model.bones.size();
    std::vector<Mat34> local(n), world(n);
    for (size_t i = 0; i < n; ++i) {
        local[i] = model.bones[i].local;
        if (anim) {
            const AnimBone* ab = anim->find(model.bones[i].name);
            if (!ab) {
                for (const auto& b : anim->bones)
                    if (b.index == i && b.name.empty()) { ab = &b; break; }
            }
            if (ab) local[i] = anim->local(*ab, frame);
        }
    }
    std::vector<int> state(n, 0);
    for (size_t i = 0; i < n; ++i) {
        // Bones are stored parent-first in shipped files, but don't rely on it.
        std::vector<size_t> chain;
        for (int32_t b = int32_t(i); b >= 0 && state[b] == 0 && chain.size() <= n; b = model.bones[b].parent) chain.push_back(size_t(b));
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const int32_t p = model.bones[*it].parent;
            world[*it] = (p >= 0 && state[p] == 2) ? multiply(world[p], local[*it]) : local[*it];
            state[*it] = 2;
        }
    }
    return world;
}

} // namespace alamo
