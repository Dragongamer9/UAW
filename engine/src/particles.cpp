// Particle system parser (see particles.hpp). Property meanings were worked out by comparing the
// game's 1,245 particle files: emission rates, lifetimes and size/colour tables line up with
// what the effects look like in the game (explosion flashes, rising smoke, flying dirt).
#include "alamo/particles.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace alamo {
namespace {

struct Chunk {
    uint32_t id;
    bool container;
    std::span<const uint8_t> body;
};

std::vector<Chunk> chunks(std::span<const uint8_t> d) {
    std::vector<Chunk> out;
    size_t off = 0;
    while (off + 8 <= d.size()) {
        uint32_t id, size;
        std::memcpy(&id, d.data() + off, 4);
        std::memcpy(&size, d.data() + off + 4, 4);
        const bool container = (size & 0x80000000u) != 0;
        size &= 0x7fffffffu;
        if (off + 8 + size > d.size()) break;
        out.push_back({id, container, d.subspan(off + 8, size)});
        off += 8 + size;
    }
    return out;
}

uint32_t u32(std::span<const uint8_t> b) {
    uint32_t v = 0;
    if (b.size() >= 4) std::memcpy(&v, b.data(), 4);
    return v;
}

std::vector<float> floats(std::span<const uint8_t> b) {
    std::vector<float> v(b.size() / 4);
    if (!v.empty()) std::memcpy(v.data(), b.data(), v.size() * 4);
    return v;
}

// One component's properties: id -> value bytes (containers: the float array inside).
struct Props {
    std::map<uint32_t, std::span<const uint8_t>> values;
    std::map<uint32_t, bool> isTable;
    bool has(uint32_t id) const { return values.count(id) != 0; }
    float f(uint32_t id, float fallback = 0) const {
        auto it = values.find(id);
        if (it == values.end()) return fallback;
        if (it->second.size() == 1) return float(it->second[0]);
        if (it->second.size() < 4) return fallback;
        float v;
        std::memcpy(&v, it->second.data(), 4);
        return v;
    }
    uint32_t i(uint32_t id) const {
        auto it = values.find(id);
        if (it == values.end()) return 0;
        return it->second.size() == 1 ? it->second[0] : u32(it->second);
    }
    bool b(uint32_t id) const { return i(id) != 0; }
    std::vector<float> v(uint32_t id) const {
        auto it = values.find(id);
        return it == values.end() ? std::vector<float>{} : floats(it->second);
    }
    std::string s(uint32_t id) const {
        auto it = values.find(id);
        if (it == values.end()) return {};
        std::string r(reinterpret_cast<const char*>(it->second.data()), it->second.size());
        if (const size_t z = r.find('\0'); z != std::string::npos) r.resize(z);
        return r;
    }
};

Props readProps(std::span<const uint8_t> list) {
    Props p;
    uint32_t key = 0;
    for (const Chunk& c : chunks(list)) {
        if (c.id == 0 && !c.container) {
            key = u32(c.body);
        } else if (c.id == 1) {
            if (c.container) {
                for (const Chunk& t : chunks(c.body))
                    if (t.id == 1) p.values[key] = t.body;
                p.isTable[key] = true;
            } else {
                p.values[key] = c.body;
            }
        }
    }
    return p;
}

// A position/velocity distribution (26 floats; the first is the shape type as an integer).
ParticleEmitterDef::Shape shapeOf(const std::vector<float>& t) {
    ParticleEmitterDef::Shape s;
    if (t.empty()) return s;
    int type;
    std::memcpy(&type, &t[0], 4);
    s.type = type;
    auto at = [&](size_t i) { return i < t.size() ? t[i] : 0.f; };
    s.vector = {at(1), at(2), at(3)};
    s.radius = at(10);
    s.boxMin = {at(11), at(12), at(13)};
    s.boxMax = {at(14), at(15), at(16)};
    // Type 0: the vector itself (a fixed offset or velocity).
    if (type == 5) { // cylinder: radius, height range
        s.radius = at(21);
        s.zMin = at(22);
        s.zMax = at(23);
    }
    return s;
}

// Tables: a count, then rows of `width` floats (the first is the time).
template <class F> void table(const std::vector<float>& v, size_t width, F&& row) {
    if (v.empty()) return;
    uint32_t n;
    std::memcpy(&n, &v[0], 4);
    for (size_t k = 0; k < n && 1 + (k + 1) * width <= v.size(); ++k) row(&v[1 + k * width]);
}

} // namespace

bool isParticleSystem(std::span<const uint8_t> data) { return data.size() >= 8 && u32(data) == 0x1500; }

float ParticleSystemDef::length() const {
    float end = 0;
    for (const ParticleEmitterDef& e : emitters) {
        if (e.gpu) continue;
        if (e.duration <= 0) return -1;
        end = std::max(end, e.delay + e.duration + e.lifetime * (1 + e.lifetimeVariance / 100));
    }
    return end;
}

ParticleSystemDef parseParticleSystem(std::span<const uint8_t> data) {
    if (!isParticleSystem(data)) throw std::runtime_error("not a particle system");
    ParticleSystemDef sys;
    const auto root = chunks(data);
    if (root.empty()) return sys;
    for (const Chunk& c : chunks(root[0].body)) {
        if (c.id == 0 && !c.container) {
            sys.name = std::string(reinterpret_cast<const char*>(c.body.data()), strnlen(reinterpret_cast<const char*>(c.body.data()), c.body.size()));
            continue;
        }
        if (c.id != 0x1520) continue;
        for (const Chunk& ec : chunks(c.body)) {
            if (ec.id != 0x1540) continue;
            ParticleEmitterDef e;
            for (const Chunk& block : chunks(ec.body)) {
                if (!block.container) {
                    if (block.id == 0)
                        e.name = std::string(reinterpret_cast<const char*>(block.body.data()),
                                             strnlen(reinterpret_cast<const char*>(block.body.data()), block.body.size()));
                    continue;
                }
                uint32_t type = 0;
                Props p;
                for (const Chunk& part : chunks(block.body)) {
                    if (part.id == 0 && !part.container) type = u32(part.body);
                    else if (part.id == 1)
                        for (const Chunk& list : chunks(part.body))
                            if (list.id == 0 && list.container) p = readProps(list.body);
                }
                switch (block.id) {
                case 1: { // spawner
                    e.spawnerType = type;
                    for (const auto& [k, v] : p.values) e.raw[k] = p.isTable.count(k) || v.size() >= 4 ? floats(v) : std::vector<float>{float(v.empty() ? 0 : v[0])};
                    if (type == 0x3c) { // DX10 variant: same idea, its own ids
                        e.gpu = true;
                        e.burst = p.f(0x1770, 1);
                        e.interval = std::max(1e-3f, p.f(0x1771, 1));
                        e.position = shapeOf(p.v(0x1773));
                        e.velocity = shapeOf(p.v(0x1775));
                        break;
                    }
                    e.burst = std::max(0.f, p.f(0x64, 1));
                    if (!p.isTable.count(0x65)) e.interval = std::max(1e-3f, p.f(0x65, 1));
                    e.delay = p.f(0x6, 0); // seconds after the system starts
                    e.duration = p.isTable.count(0x68) ? 0 : p.f(0x68, 0);
                    if (p.isTable.count(0x69)) e.position = shapeOf(p.v(0x69));
                    if (p.isTable.count(0x6b)) e.velocity = shapeOf(p.v(0x6b));
                    break;
                }
                case 3: // lifetime
                    e.lifetime = std::max(0.01f, p.f(0, 1));
                    e.lifetimeVariance = p.f(1, 0);
                    break;
                case 4: { // renderer
                    e.rendererType = type;
                    e.texture = p.has(0x64) ? p.s(0x64) : p.s(0x1900);
                    e.shader = p.has(0x65) && p.values.at(0x65).size() > 4 ? p.s(0x65) : p.has(0x66) && p.values.at(0x66).size() > 4 ? p.s(0x66) : p.s(0x1901);
                    std::string sh = e.shader;
                    std::transform(sh.begin(), sh.end(), sh.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    e.additive = sh.find("additive") != std::string::npos || sh.find("alpha") == std::string::npos;
                    e.distortion = sh.find("heat") != std::string::npos || sh.find("distort") != std::string::npos ||
                                   sh.find("refract") != std::string::npos;
                    if (type == 0x1d) {
                        if (p.f(0xc8, 0) > 0.5f) {
                            e.stretched = true;
                            e.stretch = p.f(0xca, 0.1f);
                        } else {
                            e.flat = true;
                        }
                    }
                    if (type == 0x3d) { // DX10 renderer: its own rectangle
                        const auto r = p.v(0x1906);
                        if (r.size() == 4) e.uv = {r[0], r[1], r[2], r[3]};
                    }
                    break;
                }
                case 5: // modifiers
                    switch (type) {
                    case 0x0a: {
                        const auto a = p.v(0xc8);
                        if (a.size() == 3) e.acceleration = {a[0], a[1], a[2]};
                        break;
                    }
                    case 0x0e: {
                        const auto r = p.v(0xc8);
                        if (r.size() == 4) e.uv = {r[0], r[1], r[2], r[3]};
                        break;
                    }
                    case 0x0f:
                        table(p.v(0x12c), 2, [&](const float* r) { e.size.push_back({r[0], r[1]}); });
                        break;
                    case 0x11:
                        table(p.v(0x12c), 5, [&](const float* r) { e.color.push_back({r[0], Vec4{r[1], r[2], r[3], r[4]}}); });
                        break;
                    case 0x12:
                        table(p.v(0x12c), 5, [&](const float* r) { e.uvAnim.push_back({r[0], Vec4{r[1], r[2], r[3], r[4]}}); });
                        break;
                    case 0x21: {
                        const auto v = p.v(0xc8);
                        if (!v.empty()) {
                            uint32_t n;
                            std::memcpy(&n, &v[0], 4);
                            for (size_t k = 0; k < n && 1 + (k + 1) * 4 <= v.size(); ++k)
                                e.uvChoices.push_back({v[1 + k * 4], v[2 + k * 4], v[3 + k * 4], v[4 + k * 4]});
                        }
                        break;
                    }
                    default: break;
                    }
                    break;
                default: break;
                }
            }
            sys.emitters.push_back(std::move(e));
        }
    }
    return sys;
}

} // namespace alamo
