#include "alamo/alo.hpp"

#include <cstdio>
#include <cstring>

namespace alamo {

namespace {

struct Chunk {
    uint32_t id;
    bool container;
    std::span<const uint8_t> body;
};

// Iterates the chunks laid out back to back inside `data`.
class ChunkIter {
public:
    explicit ChunkIter(std::span<const uint8_t> data) : data_(data) {}
    bool next(Chunk& out) {
        if (pos_ == data_.size()) return false;
        if (data_.size() - pos_ < 8) throw AloError("truncated chunk header");
        uint32_t id, size;
        std::memcpy(&id, &data_[pos_], 4);
        std::memcpy(&size, &data_[pos_ + 4], 4);
        out.container = size & 0x80000000u;
        size &= 0x7FFFFFFFu;
        if (size > data_.size() - pos_ - 8) throw AloError("chunk 0x" + hex(id) + " runs past its parent");
        out.id = id;
        out.body = data_.subspan(pos_ + 8, size);
        pos_ += 8 + size_t(size);
        return true;
    }
    static std::string hex(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "%X", v);
        return b;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

// Mini-chunks: { uint8 id; uint8 size; data }.
class MiniIter {
public:
    explicit MiniIter(std::span<const uint8_t> data) : data_(data) {}
    bool next(uint8_t& id, std::span<const uint8_t>& body) {
        if (pos_ + 2 > data_.size()) return false;
        id = data_[pos_];
        uint8_t size = data_[pos_ + 1];
        if (pos_ + 2 + size > data_.size()) throw AloError("truncated mini-chunk");
        body = data_.subspan(pos_ + 2, size);
        pos_ += 2 + size;
        return true;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

template <class T>
T rd(std::span<const uint8_t> s, size_t off = 0) {
    if (off + sizeof(T) > s.size()) throw AloError("field past end of chunk");
    T v;
    std::memcpy(&v, s.data() + off, sizeof(T));
    return v;
}

std::string cstr(std::span<const uint8_t> s) {
    size_t n = 0;
    while (n < s.size() && s[n]) ++n;
    return std::string(reinterpret_cast<const char*>(s.data()), n);
}

void readBone(std::span<const uint8_t> body, Model& m) {
    Bone b;
    ChunkIter it(body);
    Chunk c;
    while (it.next(c)) {
        if (c.id == 0x203) b.name = cstr(c.body);
        else if (c.id == 0x205 || c.id == 0x206) {
            size_t off = 0;
            b.parent = rd<int32_t>(c.body, off); off += 4;
            b.visible = rd<uint32_t>(c.body, off) != 0; off += 4;
            if (c.id == 0x206) { b.billboard = rd<uint32_t>(c.body, off); off += 4; }
            for (int i = 0; i < 12; ++i, off += 4) b.local[i] = rd<float>(c.body, off);
        }
    }
    m.bones.push_back(std::move(b));
}

void readMaterial(std::span<const uint8_t> body, Material& mat) {
    ChunkIter it(body);
    Chunk c;
    while (it.next(c)) {
        if (c.id == 0x10101) { mat.shader = cstr(c.body); continue; }
        if (c.id < 0x10102 || c.id > 0x10106) continue;
        std::string name;
        std::span<const uint8_t> value;
        MiniIter mi(c.body);
        uint8_t id;
        std::span<const uint8_t> mb;
        while (mi.next(id, mb)) {
            if (id == 1) name = cstr(mb);
            else if (id == 2) value = mb;
        }
        switch (c.id) {
        case 0x10102: mat.params[name] = rd<int32_t>(value); break;
        case 0x10103: mat.params[name] = rd<float>(value); break;
        case 0x10104: mat.params[name] = Vec3{rd<float>(value, 0), rd<float>(value, 4), rd<float>(value, 8)}; break;
        case 0x10105: mat.params[name] = cstr(value); break;
        case 0x10106:
            mat.params[name] = Vec4{rd<float>(value, 0), rd<float>(value, 4), rd<float>(value, 8), rd<float>(value, 12)};
            break;
        }
    }
}

constexpr size_t kVertexStride = 144;

void readGeometry(std::span<const uint8_t> body, SubMesh& sm, Model& m, const std::string& meshName) {
    ChunkIter it(body);
    Chunk c;
    uint32_t nVerts = 0, nFaces = 0;
    while (it.next(c)) {
        switch (c.id) {
        case 0x10001:
            nVerts = rd<uint32_t>(c.body, 0);
            nFaces = rd<uint32_t>(c.body, 4);
            break;
        case 0x10002: sm.vertexFormat = cstr(c.body); break;
        case 0x10007: {
            if (c.body.size() % kVertexStride) throw AloError(meshName + ": vertex buffer is not a multiple of 144 bytes");
            const size_t n = c.body.size() / kVertexStride;
            sm.vertices.resize(n);
            for (size_t i = 0; i < n; ++i) {
                auto v = c.body.subspan(i * kVertexStride, kVertexStride);
                Vertex& o = sm.vertices[i];
                std::memcpy(o.position.data(), &v[0], 12);
                std::memcpy(o.normal.data(), &v[12], 12);
                std::memcpy(o.uv, &v[24], 32);
                std::memcpy(o.tangent.data(), &v[56], 12);
                std::memcpy(o.binormal.data(), &v[68], 12);
                std::memcpy(o.color.data(), &v[80], 16);
                // v[96..111]: unused/unknown
                std::memcpy(o.boneIndex, &v[112], 16);
                std::memcpy(o.boneWeight, &v[128], 16);
            }
            break;
        }
        case 0x10004:
            sm.indices.resize(c.body.size() / 2);
            std::memcpy(sm.indices.data(), c.body.data(), sm.indices.size() * 2);
            break;
        case 0x10006:
            sm.boneMap.resize(c.body.size() / 4);
            std::memcpy(sm.boneMap.data(), c.body.data(), sm.boneMap.size() * 4);
            break;
        case 0x1200: sm.hasCollisionTree = true; break;
        }
    }
    if (nVerts && sm.vertices.size() != nVerts)
        m.warnings.push_back(meshName + ": header says " + std::to_string(nVerts) + " vertices, buffer has " +
                             std::to_string(sm.vertices.size()));
    if (nFaces && sm.indices.size() != size_t(nFaces) * 3)
        m.warnings.push_back(meshName + ": header says " + std::to_string(nFaces) + " faces, index buffer has " +
                             std::to_string(sm.indices.size() / 3));
    for (uint16_t ix : sm.indices)
        if (ix >= sm.vertices.size()) throw AloError(meshName + ": index out of range");
}

void readMesh(std::span<const uint8_t> body, Model& m) {
    Mesh mesh;
    ChunkIter it(body);
    Chunk c;
    Material pending;
    bool havePending = false;
    while (it.next(c)) {
        switch (c.id) {
        case 0x401: mesh.name = cstr(c.body); break;
        case 0x402:
            for (int i = 0; i < 3; ++i) mesh.boundsMin[i] = rd<float>(c.body, 4 + 4 * i);
            for (int i = 0; i < 3; ++i) mesh.boundsMax[i] = rd<float>(c.body, 16 + 4 * i);
            mesh.hidden = rd<uint32_t>(c.body, 32) != 0;
            mesh.collision = rd<uint32_t>(c.body, 36) != 0;
            break;
        case 0x10100:
            pending = Material{};
            readMaterial(c.body, pending);
            havePending = true;
            break;
        case 0x10000: {
            SubMesh sm;
            if (havePending) sm.material = std::move(pending);
            havePending = false;
            readGeometry(c.body, sm, m, mesh.name);
            mesh.subMeshes.push_back(std::move(sm));
            break;
        }
        }
    }
    m.meshes.push_back(std::move(mesh));
}

// objects[i] = mesh index for the i-th mesh/light chunk, or -1 for a light.
void readConnections(std::span<const uint8_t> body, Model& m, const std::vector<int>& objects) {
    ChunkIter it(body);
    Chunk c;
    while (it.next(c)) {
        MiniIter mi(c.body);
        uint8_t id;
        std::span<const uint8_t> mb;
        if (c.id == 0x602) {
            // Object indices count meshes and lights together, in file order.
            uint32_t object = 0, bone = 0;
            while (mi.next(id, mb)) {
                if (id == 2) object = rd<uint32_t>(mb);
                else if (id == 3) bone = rd<uint32_t>(mb);
            }
            if (object >= objects.size()) {
                m.warnings.push_back("connection references missing object " + std::to_string(object));
                continue;
            }
            if (objects[object] >= 0) m.connections.push_back({uint32_t(objects[object]), bone});
            else m.lightBones.push_back(bone);
        } else if (c.id == 0x603) {
            Proxy p;
            while (mi.next(id, mb)) {
                if (id == 5) p.name = cstr(mb);
                else if (id == 6) p.bone = rd<uint32_t>(mb);
                else if (id == 7) p.hidden = rd<uint32_t>(mb) != 0;
            }
            m.proxies.push_back(std::move(p));
        } else if (c.id == 0x604) {
            ++m.dazzleCount;
        }
    }
}

} // namespace

const std::string* Material::texture(const std::string& name) const {
    auto it = params.find(name);
    if (it == params.end()) return nullptr;
    return std::get_if<std::string>(&it->second);
}

Model loadAlo(std::span<const uint8_t> data) {
    Model m;
    ChunkIter it(data);
    Chunk c;
    std::vector<int> objects;
    while (it.next(c)) {
        switch (c.id) {
        case 0x200: {
            ChunkIter bi(c.body);
            Chunk b;
            while (bi.next(b))
                if (b.id == 0x202) readBone(b.body, m);
            break;
        }
        case 0x400:
            objects.push_back(int(m.meshes.size()));
            readMesh(c.body, m);
            break;
        case 0x600: readConnections(c.body, m, objects); break;
        case 0x1300:
            objects.push_back(-1);
            ++m.lightCount;
            break;
        case 0x1500: m.kind = Model::Kind::ParticleSystem; break;
        default: m.warnings.push_back("unknown top-level chunk 0x" + ChunkIter::hex(c.id)); break;
        }
    }
    for (auto& b : m.bones)
        if (b.parent >= int32_t(m.bones.size())) {
            m.warnings.push_back("bone " + b.name + " has invalid parent");
            b.parent = -1;
        }
    for (auto& con : m.connections)
        if (con.bone >= m.bones.size()) {
            m.warnings.push_back("connection references missing bone " + std::to_string(con.bone));
            con.bone = 0;
        }
    return m;
}

Mat34 multiply(const Mat34& a, const Mat34& b) {
    Mat34 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = j == 3 ? a[i * 4 + 3] : 0.f;
            for (int k = 0; k < 3; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    }
    return r;
}

Vec3 transformPoint(const Mat34& m, const Vec3& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

Vec3 transformDir(const Mat34& m, const Vec3& d) {
    return {m[0] * d[0] + m[1] * d[1] + m[2] * d[2], m[4] * d[0] + m[5] * d[1] + m[6] * d[2],
            m[8] * d[0] + m[9] * d[1] + m[10] * d[2]};
}

Mat34 Model::boneWorld(uint32_t bone) const {
    Mat34 r{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    int guard = 0;
    for (int32_t b = int32_t(bone); b >= 0 && b < int32_t(bones.size()) && guard < 256; b = bones[b].parent, ++guard)
        r = multiply(bones[b].local, r);
    return r;
}

uint32_t Model::meshBone(uint32_t mesh) const {
    for (const auto& c : connections)
        if (c.mesh == mesh) return c.bone;
    return 0;
}

} // namespace alamo
