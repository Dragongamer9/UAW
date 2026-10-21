// ALA animation files (Forces of Corruption / Universe at War layout).
//
//   0x1000 animation
//     0x1001 header (mini-chunks): 1 frame count, 2 fps, 3 bone count,
//            0x0B rotation block size (int16 components per frame),
//            0x0C translation block size (uint16 components per frame), 0x0D scale block size
//     0x1002 bone, repeated
//       0x1003 info (mini-chunks): 4 name, 5 bone index, 6 translation offset, 7 translation scale,
//              8 scale offset, 9 scale scale, 0x0E translation index, 0x0F scale index,
//              0x10 rotation index, 0x11 default rotation (int16 x,y,z,w / 32767)
//       0x1007 visibility bitfield, one bit per frame (optional)
//     0x1009 rotation data:    frames x rotationBlock int16 (quaternions x,y,z,w / 32767)
//     0x100A translation data: frames x translationBlock uint16 (offset + value * scale)
//
// A bone's pose for a frame is T(translation) * R(rotation), relative to its parent,
// replacing the model's rest transform for that bone.
#pragma once

#include "alamo/alo.hpp"

#include <span>
#include <string>
#include <vector>

namespace alamo {

struct AnimBone {
    std::string name;
    uint32_t index = 0;                   // bone index in the model's skeleton
    Vec3 translationOffset{}, translationScale{};
    int32_t translationIndex = -1;        // first component in the per-frame translation block, -1 = constant
    int32_t rotationIndex = -1;           // first component in the per-frame rotation block, -1 = constant
    Vec4 defaultRotation{0, 0, 0, 1};     // x, y, z, w
    std::vector<bool> visibility;         // per frame; empty = always visible
};

struct Animation {
    uint32_t frameCount = 0;
    float fps = 30.f;
    uint32_t rotationBlock = 0, translationBlock = 0;
    std::vector<AnimBone> bones;
    std::vector<int16_t> rotations;       // frameCount * rotationBlock
    std::vector<uint16_t> translations;   // frameCount * translationBlock
    std::vector<std::string> warnings;

    float duration() const { return fps > 0 ? float(frameCount) / fps : 0.f; }
    const AnimBone* find(const std::string& boneName) const;

    Vec3 translation(const AnimBone& b, uint32_t frame) const;
    Vec4 rotation(const AnimBone& b, uint32_t frame) const;
    Mat34 local(const AnimBone& b, uint32_t frame) const;
    bool visible(const AnimBone& b, uint32_t frame) const;
};

Animation loadAla(std::span<const uint8_t> data);

// World transforms for every bone of `model` at `frame`. Bones the animation
// doesn't mention keep their rest transform.
std::vector<Mat34> poseModel(const Model& model, const Animation* anim, uint32_t frame);

Mat34 quatToMatrix(const Vec4& q, const Vec3& t);
Mat34 inverseRigid(const Mat34& m); // inverse of a rotation+translation (+uniform scale) matrix

} // namespace alamo
