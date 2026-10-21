// ALO model files (Alamo Object).
//
// Chunked binary: each chunk is { uint32 id; uint32 size; } where the high bit
// of size means "contains child chunks". Material parameters and connections
// use "mini-chunks" { uint8 id; uint8 size; }.
//
//   0x0200 skeleton      0x0201 bone count, 0x0202 bone { 0x0203 name, 0x0205/0x0206 data }
//   0x0400 mesh          0x0401 name, 0x0402 info, then per material:
//                        0x10100 material { 0x10101 shader, 0x10102..0x10106 params }
//                        0x10000 geometry { 0x10001 counts, 0x10002 vertex format,
//                                           0x10007 vertices (144 bytes each), 0x10004 uint16 indices,
//                                           0x10006 bone map (skinned), 0x1200 collision tree }
//   0x0600 connections   0x0601 counts, 0x0602 mesh->bone, 0x0603 proxy, 0x0604 dazzle (UaW)
//   0x1300 light
//   0x1500 particle system (UaW's particle files share the .ALO extension)
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace alamo {

class AloError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Mat34 = std::array<float, 12>; // row-major 3x4: [r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz]

struct Bone {
    std::string name;
    int32_t parent = -1;
    bool visible = true;
    uint32_t billboard = 0;
    Mat34 local{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

struct Vertex {
    Vec3 position, normal;
    Vec2 uv[4];
    Vec3 tangent, binormal;
    Vec4 color;
    uint32_t boneIndex[4];
    float boneWeight[4];
};
// On disk each vertex is 144 bytes: these fields plus 16 unused bytes after color.

using ShaderParam = std::variant<int32_t, float, Vec3, Vec4, std::string>; // string = texture name

struct Material {
    std::string shader;                         // e.g. MeshBumpColorize.fx
    std::map<std::string, ShaderParam> params;  // e.g. BaseTexture -> "NV_Corruptor.tga"
    const std::string* texture(const std::string& name) const;
};

struct SubMesh {
    Material material;
    std::string vertexFormat;       // e.g. alD3dVertRSkinNU2U3U3
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;  // triangle list
    std::vector<uint32_t> boneMap;  // skinned meshes: local bone slot -> skeleton bone
    bool hasCollisionTree = false;
    bool skinned() const { return vertexFormat.find("Skin") != std::string::npos; }
};

struct Mesh {
    std::string name;
    bool hidden = false;
    bool collision = false;
    Vec3 boundsMin{}, boundsMax{};
    std::vector<SubMesh> subMeshes;
};

struct Connection {
    uint32_t mesh = 0;
    uint32_t bone = 0;
};

struct Proxy {
    std::string name;  // usually a particle effect attached to a bone
    uint32_t bone = 0;
    bool hidden = false;
};

struct Model {
    enum class Kind { Mesh, ParticleSystem } kind = Kind::Mesh;
    std::vector<Bone> bones;
    std::vector<Mesh> meshes;
    std::vector<Connection> connections;
    std::vector<Proxy> proxies;
    size_t lightCount = 0;
    std::vector<uint32_t> lightBones; // bone each light is attached to
    size_t dazzleCount = 0;
    std::vector<std::string> warnings;

    // Bone transform relative to the model root.
    Mat34 boneWorld(uint32_t bone) const;
    // Bone a mesh is attached to (0 = root if not connected).
    uint32_t meshBone(uint32_t mesh) const;
};

Model loadAlo(std::span<const uint8_t> data);

Mat34 multiply(const Mat34& a, const Mat34& b);
Vec3 transformPoint(const Mat34& m, const Vec3& p);
Vec3 transformDir(const Mat34& m, const Vec3& d);

} // namespace alamo
