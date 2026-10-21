#include "alamo/gltf.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace alamo {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

// Alamo is Z-up, glTF is Y-up (right-handed both): (x, y, z) -> (x, z, -y).
Vec3 toYUp(const Vec3& v) { return {v[0], v[2], -v[1]}; }

Vec3 normalize(Vec3 v) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 1e-8f) return {0, 1, 0};
    return {v[0] / l, v[1] / l, v[2] / l};
}

// Placement. Rigid meshes are stored relative to the bone their mesh is attached
// to. Skinned meshes (vertex formats containing "Skin") are stored in model space
// in bind pose (placing them per bone without the inverse bind scatters the parts,
// checked on Orlok and the Novus Reflex), so a posed vertex uses
// posed(bone) * inverse(rest(bone)), picking the bone from the sub-mesh's bone map.
struct Placement {
    std::vector<Mat34> skin; // per bone-map slot
    Mat34 rigid;
    const Mat34& forVertex(const SubMesh& sm, const Vertex& v) const {
        static const Mat34 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (!sm.skinned()) return rigid;
        uint32_t slot = v.boneIndex[0];
        return slot < skin.size() ? skin[slot] : identity;
    }
};

struct Builder {
    std::vector<uint8_t> bin;
    std::ostringstream bufferViews, accessors;
    int viewCount = 0, accessorCount = 0;

    void align() { while (bin.size() % 4) bin.push_back(0); }

    int view(const void* data, size_t bytes, int target) {
        align();
        size_t off = bin.size();
        bin.insert(bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
        if (viewCount) bufferViews << ",";
        bufferViews << "{\"buffer\":0,\"byteOffset\":" << off << ",\"byteLength\":" << bytes;
        if (target) bufferViews << ",\"target\":" << target;
        bufferViews << "}";
        return viewCount++;
    }

    int accessor(int view, int componentType, size_t count, const char* type, const float* mn = nullptr,
                 const float* mx = nullptr) {
        if (accessorCount) accessors << ",";
        accessors << "{\"bufferView\":" << view << ",\"componentType\":" << componentType << ",\"count\":" << count
                  << ",\"type\":\"" << type << "\"";
        if (mn && mx)
            accessors << ",\"min\":[" << mn[0] << "," << mn[1] << "," << mn[2] << "],\"max\":[" << mx[0] << "," << mx[1]
                      << "," << mx[2] << "]";
        accessors << "}";
        return accessorCount++;
    }
};

} // namespace

std::vector<uint8_t> exportGlb(const Model& model, const TextureLoader& textures, const GltfOptions& opt,
                               GltfStats* statsOut) {
    GltfStats stats;
    Builder b;
    std::ostringstream meshes, nodes, materials, images, gtextures;
    int meshCount = 0, materialCount = 0, imageCount = 0;
    std::vector<int> nodeIds;
    std::map<std::string, int> textureIndex; // texture name -> glTF texture index (-1 = missing)

    auto textureFor = [&](const std::string& name) -> int {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        auto it = textureIndex.find(key);
        if (it != textureIndex.end()) return it->second;
        int idx = -1;
        if (auto img = textures(name)) {
            auto png = encodePng(*img);
            int v = b.view(png.data(), png.size(), 0);
            if (imageCount) { images << ","; gtextures << ","; }
            images << "{\"name\":\"" << jsonEscape(name) << "\",\"bufferView\":" << v << ",\"mimeType\":\"image/png\"}";
            gtextures << "{\"source\":" << imageCount << ",\"sampler\":0}";
            idx = imageCount++;
            ++stats.textures;
        } else {
            stats.missingTextures.push_back(name);
        }
        textureIndex[key] = idx;
        return idx;
    };

    const std::vector<Mat34> posed = poseModel(model, opt.animation, opt.frame);
    std::vector<Mat34> rest(model.bones.size());
    for (uint32_t i = 0; i < rest.size(); ++i) rest[i] = model.boneWorld(i);

    for (uint32_t mi = 0; mi < model.meshes.size(); ++mi) {
        const Mesh& mesh = model.meshes[mi];
        if (mesh.hidden && !opt.includeHidden) continue;
        const uint32_t bone = model.meshBone(mi);
        if (opt.animation && bone < model.bones.size()) {
            const AnimBone* ab = opt.animation->find(model.bones[bone].name);
            if (ab && !opt.animation->visible(*ab, opt.frame) && !opt.includeHidden) continue;
        }
        Placement place;
        place.rigid = bone < posed.size() ? posed[bone] : Mat34{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        std::ostringstream prims;
        int primCount = 0;

        for (const SubMesh& sm : mesh.subMeshes) {
            if (sm.vertices.empty() || sm.indices.empty()) continue;
            std::vector<float> pos, nrm, uv;
            pos.reserve(sm.vertices.size() * 3);
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            place.skin.clear();
            for (uint32_t b : sm.boneMap)
                place.skin.push_back(b < posed.size() ? multiply(posed[b], inverseRigid(rest[b]))
                                                      : Mat34{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
            for (const Vertex& v : sm.vertices) {
                const Mat34& m = place.forVertex(sm, v);
                Vec3 p = toYUp(transformPoint(m, v.position));
                Vec3 n = toYUp(normalize(transformDir(m, v.normal)));
                for (int k = 0; k < 3; ++k) {
                    pos.push_back(p[k]);
                    nrm.push_back(n[k]);
                    mn[k] = std::min(mn[k], p[k]);
                    mx[k] = std::max(mx[k], p[k]);
                }
                uv.push_back(v.uv[0][0]);
                uv.push_back(v.uv[0][1]);
            }
            int aPos = b.accessor(b.view(pos.data(), pos.size() * 4, 34962), 5126, sm.vertices.size(), "VEC3", mn, mx);
            int aNrm = b.accessor(b.view(nrm.data(), nrm.size() * 4, 34962), 5126, sm.vertices.size(), "VEC3");
            int aUv = b.accessor(b.view(uv.data(), uv.size() * 4, 34962), 5126, sm.vertices.size(), "VEC2");
            int aIdx = b.accessor(b.view(sm.indices.data(), sm.indices.size() * 2, 34963), 5123, sm.indices.size(), "SCALAR");

            // Material: base colour texture + tint, alpha-blend for additive/alpha shaders.
            const Material& m = sm.material;
            const std::string* tex = m.texture("BaseTexture");
            int texIdx = tex && !tex->empty() ? textureFor(*tex) : -1;
            Vec4 tint{1, 1, 1, 1};
            if (auto it = m.params.find("Diffuse"); it != m.params.end())
                if (auto* v4 = std::get_if<Vec4>(&it->second)) tint = {(*v4)[0], (*v4)[1], (*v4)[2], 1};
            if (auto it = m.params.find("DiffuseColor"); it != m.params.end())
                if (auto* v3 = std::get_if<Vec3>(&it->second)) tint = {(*v3)[0], (*v3)[1], (*v3)[2], 1};
            const bool blended = m.shader.find("Additive") != std::string::npos ||
                                 m.shader.find("Alpha") != std::string::npos ||
                                 m.shader.find("Glow") != std::string::npos;
            if (materialCount) materials << ",";
            materials << "{\"name\":\"" << jsonEscape(m.shader) << "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":["
                      << tint[0] << "," << tint[1] << "," << tint[2] << "," << tint[3] << "]";
            if (texIdx >= 0) materials << ",\"baseColorTexture\":{\"index\":" << texIdx << "}";
            materials << ",\"metallicFactor\":0,\"roughnessFactor\":0.8}";
            if (blended) materials << ",\"alphaMode\":\"BLEND\"";
            materials << ",\"doubleSided\":true}";

            if (primCount) prims << ",";
            prims << "{\"attributes\":{\"POSITION\":" << aPos << ",\"NORMAL\":" << aNrm << ",\"TEXCOORD_0\":" << aUv
                  << "},\"indices\":" << aIdx << ",\"material\":" << materialCount << "}";
            ++materialCount;
            ++primCount;
            ++stats.primitives;
            stats.triangles += sm.indices.size() / 3;
        }
        if (!primCount) continue;
        if (meshCount) { meshes << ","; nodes << ","; }
        meshes << "{\"name\":\"" << jsonEscape(mesh.name) << "\",\"primitives\":[" << prims.str() << "]}";
        nodes << "{\"name\":\"" << jsonEscape(mesh.name) << "\",\"mesh\":" << meshCount << "}";
        nodeIds.push_back(meshCount);
        ++meshCount;
        ++stats.meshes;
    }

    b.align();
    std::ostringstream js;
    js << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"uaw-native alotool\"},\"scene\":0,\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < nodeIds.size(); ++i) js << (i ? "," : "") << nodeIds[i];
    js << "]}],\"nodes\":[" << nodes.str() << "],\"meshes\":[" << meshes.str() << "]";
    if (materialCount) js << ",\"materials\":[" << materials.str() << "]";
    if (imageCount)
        js << ",\"images\":[" << images.str() << "],\"textures\":[" << gtextures.str()
           << "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}]";
    js << ",\"accessors\":[" << b.accessors.str() << "],\"bufferViews\":[" << b.bufferViews.str()
       << "],\"buffers\":[{\"byteLength\":" << b.bin.size() << "}]}";
    std::string json = js.str();
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> out;
    auto le32 = [&](uint32_t v) { for (int s = 0; s < 32; s += 8) out.push_back(uint8_t(v >> s)); };
    le32(0x46546C67); // "glTF"
    le32(2);
    le32(uint32_t(12 + 8 + json.size() + 8 + b.bin.size()));
    le32(uint32_t(json.size()));
    le32(0x4E4F534A); // JSON
    out.insert(out.end(), json.begin(), json.end());
    le32(uint32_t(b.bin.size()));
    le32(0x004E4942); // BIN
    out.insert(out.end(), b.bin.begin(), b.bin.end());
    if (statsOut) *statsOut = std::move(stats);
    return out;
}

} // namespace alamo
