#include "alamo/ted.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace alamo {

namespace {

struct Chunk {
    uint32_t id;
    std::span<const uint8_t> body;
};

std::vector<Chunk> children(std::span<const uint8_t> d) {
    std::vector<Chunk> out;
    size_t pos = 0;
    while (pos < d.size()) {
        if (d.size() - pos < 8) throw AloError("map: truncated chunk header");
        uint32_t id, size;
        std::memcpy(&id, &d[pos], 4);
        std::memcpy(&size, &d[pos + 4], 4);
        size &= 0x7FFFFFFFu;
        if (size > d.size() - pos - 8) throw AloError("map: chunk 0x" + std::to_string(id) + " runs past its parent");
        out.push_back({id, d.subspan(pos + 8, size)});
        pos += 8 + size_t(size);
    }
    return out;
}

const Chunk* child(const std::vector<Chunk>& cs, uint32_t id) {
    for (const auto& c : cs)
        if (c.id == id) return &c;
    return nullptr;
}

struct Mini {
    uint8_t id;
    std::span<const uint8_t> body;
};

std::vector<Mini> minis(std::span<const uint8_t> d) {
    std::vector<Mini> out;
    size_t pos = 0;
    while (pos + 2 <= d.size()) {
        uint8_t size = d[pos + 1];
        if (pos + 2 + size > d.size()) throw AloError("map: truncated mini-chunk");
        out.push_back({d[pos], d.subspan(pos + 2, size)});
        pos += 2 + size;
    }
    return out;
}

template <class T>
T rd(std::span<const uint8_t> s, size_t off = 0) {
    if (off + sizeof(T) > s.size()) throw AloError("map: field past end of chunk");
    T v;
    std::memcpy(&v, s.data() + off, sizeof(T));
    return v;
}

Vec3 rdVec3(std::span<const uint8_t> s) { return {rd<float>(s, 0), rd<float>(s, 4), rd<float>(s, 8)}; }

std::string str(std::span<const uint8_t> s) {
    size_t n = 0;
    while (n < s.size() && s[n]) ++n;
    return std::string(reinterpret_cast<const char*>(s.data()), n);
}

std::string utf16ascii(std::span<const uint8_t> s) {
    std::string o;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        uint16_t c = uint16_t(s[i] | s[i + 1] << 8);
        if (!c) break;
        o += c < 128 ? char(c) : '?';
    }
    return o;
}

void readTerrain(std::span<const uint8_t> body, Map& m) {
    auto cs = children(body);
    const Chunk* hdr = child(cs, 0x00);
    if (!hdr) throw AloError("map: terrain has no header");
    auto text = [](std::span<const uint8_t> b) {
        std::string s(reinterpret_cast<const char*>(b.data()), b.size());
        if (const size_t z = s.find('\0'); z != std::string::npos) s.resize(z);
        return s;
    };
    for (const auto& mc : minis(hdr->body)) {
        if (mc.id == 0x00) m.width = rd<uint32_t>(mc.body);
        else if (mc.id == 0x01) m.height = rd<uint32_t>(mc.body);
        else if (mc.id == 0x15 && mc.body.size() == 4) m.water.level = rd<float>(mc.body);
        else if (mc.id == 0x16 && mc.body.size() == 4) m.water.on = rd<uint32_t>(mc.body) != 0;
        else if (mc.id == 0x1A && mc.body.size() == 16)
            for (int k = 0; k < 4; ++k) m.water.color[size_t(k)] = rd<float>(mc.body, size_t(k) * 4);
        else if (mc.id == 0x1D) m.water.bump = text(mc.body);
        else if (mc.id == 0x1E) m.water.texture = text(mc.body);
    }
    if (m.width < 2 || m.height < 2 || m.width > 4096 || m.height > 4096) throw AloError("map: bad terrain size");
    const size_t n = size_t(m.width) * m.height;

    const Chunk* verts = child(cs, 0x0D);
    if (!verts || verts->body.size() != n * 4) throw AloError("map: vertex data doesn't match terrain size");
    m.heights.resize(n);
    m.layerIndex.resize(n);
    for (size_t i = 0; i < n; ++i) {
        m.heights[i] = float(rd<int16_t>(verts->body, i * 4)) * kTedHeightScale;
        m.layerIndex[i] = verts->body[i * 4 + 2];
    }

    if (const Chunk* layers = child(cs, 0x02)) {
        for (const auto& lc : children(layers->body)) {
            if (lc.id != 0x03) continue;
            TerrainLayer l;
            for (const auto& mc : minis(lc.body)) {
                switch (mc.id) {
                case 0x07: l.tiling = rd<float>(mc.body); break;
                case 0x08: l.rotation = rd<float>(mc.body); break;
                case 0x0A: l.tint = rdVec3(mc.body); break;
                case 0x0C: l.diffuse = str(mc.body); break;
                case 0x13: l.bump = str(mc.body); break;
                default: break;
                }
            }
            m.layers.push_back(std::move(l));
        }
    }
    for (uint8_t li : m.layerIndex)
        if (li >= m.layers.size()) {
            m.warnings.push_back("vertex uses texture layer " + std::to_string(li) + " but only " +
                                 std::to_string(m.layers.size()) + " exist");
            break;
        }
}

void readObjects(std::span<const uint8_t> body, Map& m) {
    std::unordered_map<uint32_t, std::vector<uint32_t>> groupsOf;
    for (const auto& c : children(body)) {
        if (c.id == 0x17) {
            auto gs = children(c.body);
            const Chunk* g = child(gs, 0x100);
            if (!g) continue;
            MapGroup grp;
            for (const auto& mc : minis(g->body)) {
                if (mc.id == 0x00) grp.name = str(mc.body);
                else if (mc.id == 0x01) grp.objectIds.push_back(rd<uint32_t>(mc.body));
            }
            const uint32_t gi = uint32_t(m.groups.size());
            for (uint32_t id : grp.objectIds) {
                auto& v = groupsOf[id];
                if (std::find(v.begin(), v.end(), gi) == v.end()) v.push_back(gi);
            }
            m.groups.push_back(std::move(grp));
        } else if (c.id == 0x01) {
            for (const auto& oc : children(c.body)) {
                if (oc.id != 0x44C) continue;
                const auto parts = children(oc.body);
                const Chunk* info = child(parts, 0x459);
                if (!info) continue;
                const auto infoParts = children(info->body);
                const Chunk* props = child(infoParts, 0x4B0);
                if (!props) continue;
                MapObject o;
                bool haveType = false, havePos = false;
                for (const auto& mc : minis(props->body)) {
                    switch (mc.id) {
                    case 0x00: if (mc.body.size() == 4) o.id = rd<uint32_t>(mc.body); break;
                    case 0x01: o.typeCrc = rd<uint32_t>(mc.body); haveType = true; break;
                    case 0x02: o.owner = rd<int32_t>(mc.body); break;
                    case 0x03: o.scale = rd<float>(mc.body); break;
                    case 0x04: o.position = rdVec3(mc.body); havePos = true; break;
                    case 0x05: o.rotation = rdVec3(mc.body); break;
                    default: break;
                    }
                }
                if (!haveType || !havePos) {
                    m.warnings.push_back("object without type or position skipped");
                    continue;
                }
                m.objects.push_back(o);
            }
        }
    }
    for (auto& o : m.objects)
        if (auto it = groupsOf.find(o.id); it != groupsOf.end()) o.groups = it->second;
}

} // namespace

Map loadTed(std::span<const uint8_t> data) {
    Map m;
    auto top = children(data);
    float extentX = 0;
    if (const Chunk* h = child(top, 0x00)) {
        for (const auto& mc : minis(h->body)) {
            if (mc.id == 0x08) m.nameTextId = utf16ascii(mc.body);
            else if (mc.id == 0x0D && mc.body.size() == 4) extentX = rd<float>(mc.body);
        }
    }
    const Chunk* map = child(top, 0x01);
    if (!map) throw AloError("not a TED map (missing 0x0001 chunk)");
    auto cs = children(map->body);
    const Chunk* terrain = child(cs, 0x101);
    if (!terrain) throw AloError("map has no terrain");
    readTerrain(terrain->body, m);
    // Retail maps all use 20 units per vertex; the header extent is width * spacing.
    if (extentX > 0) m.spacing = extentX / float(m.width);

    // Lighting: the first preset of the environment chunk.
    if (const Chunk* env = child(cs, 0x100)) {
        const auto envParts = children(env->body);
        if (const Chunk* presets = child(envParts, 0x04)) {
            const auto ps = children(presets->body);
            if (const Chunk* p = child(ps, 0x06)) {
                MapEnvironment& e = m.environment;
                Vec3 dir{0, 0, 0};
                auto text = [](std::span<const uint8_t> b) {
                    std::string s(reinterpret_cast<const char*>(b.data()), b.size());
                    if (const size_t z = s.find('\0'); z != std::string::npos) s.resize(z);
                    return s;
                };
                for (const auto& mc : minis(p->body)) {
                    const size_t n = mc.body.size();
                    switch (mc.id) {
                    case 0x00: if (n == 12) e.sun = rdVec3(mc.body); break;
                    case 0x01: if (n == 12) e.fill1 = rdVec3(mc.body); break;
                    case 0x02: if (n == 12) e.fill2 = rdVec3(mc.body); break;
                    case 0x04: if (n == 12) e.ambient = rdVec3(mc.body); break;
                    case 0x05: if (n == 4) e.sunIntensity = rd<float>(mc.body); break;
                    case 0x0B: if (n == 4) dir[0] = rd<float>(mc.body); break;
                    case 0x0C: if (n == 4) dir[1] = rd<float>(mc.body); break;
                    case 0x0D: if (n == 4) dir[2] = rd<float>(mc.body); break;
                    case 0x0F: if (n == 12) e.fogColor = rdVec3(mc.body); break;
                    case 0x10: if (n >= 1) e.fog = mc.body[0] != 0; break;
                    case 0x11: if (n == 4) e.fogStart = rd<float>(mc.body); break;
                    case 0x12: if (n == 4) e.fogEnd = rd<float>(mc.body); break;
                    case 0x14: if (n > 4) e.name = text(mc.body); break;
                    case 0x19: e.skyDome = text(mc.body); break;
                    default: break;
                    }
                }
                if (dir[2] < -0.05f) e.sunDirection = dir; // pointing down: a real direction
                e.valid = true;
            }
        }
    }

    for (const auto& c : cs) {
        if (c.id == 0x102) readObjects(c.body, m);
        else if (c.id == 0x103) {
            for (const auto& mc : minis(c.body)) {
                if (mc.id == 0x05) m.playableMin = rdVec3(mc.body);
                else if (mc.id == 0x06) m.playableMax = rdVec3(mc.body);
            }
        } else if (c.id == 0x10A) {
            const size_t n = size_t(m.width) * m.height;
            if (c.body.size() == n * 4) {
                m.vertexFlags.resize(n);
                std::memcpy(m.vertexFlags.data(), c.body.data(), n * 4);
            } else {
                m.warnings.push_back("vertex flag chunk size doesn't match terrain");
            }
        } else if (c.id == 0x10D) {
            for (const auto& mc : minis(c.body))
                if (mc.id == 0x08) m.assetBanks.push_back(str(mc.body));
        }
    }
    return m;
}

float Map::heightAt(float x, float y) const {
    if (heights.empty()) return 0;
    float gx = std::clamp(x / spacing, 0.f, float(width - 1) - 1e-3f);
    float gy = std::clamp(y / spacing, 0.f, float(height - 1) - 1e-3f);
    uint32_t x0 = uint32_t(gx), y0 = uint32_t(gy);
    float fx = gx - float(x0), fy = gy - float(y0);
    float h00 = vertexHeight(x0, y0), h10 = vertexHeight(x0 + 1, y0);
    float h01 = vertexHeight(x0, y0 + 1), h11 = vertexHeight(x0 + 1, y0 + 1);
    return (h00 * (1 - fx) + h10 * fx) * (1 - fy) + (h01 * (1 - fx) + h11 * fx) * fy;
}

int Map::groupIndex(const std::string& name) const {
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i].name == name) return int(i);
    return -1;
}

} // namespace alamo
