// Particle systems: the .ALO files whose root chunk is 0x1500 (Universe at War's particle format).
//
// Layout (all chunks are the usual Alamo id + size, high bit = container):
//   0x1500 { 0x0000 name, 0x0001 u8, 0x0002 u8, 0x1520 { 0x1540 emitter ... } }
//   0x1540 emitter { 0x0000 name, then blocks 0x0001 spawner, 0x0002 (?), 0x0003 lifetime,
//                    0x0004 renderer, 0x0005 modifier (any number) }
//   each block { 0x0000 u32 component type, 0x0001 { 0x0000 property list } }
//   property list: pairs of 0x0000 u32 property id, 0x0001 value (bool byte, float, vector,
//                  string, or a container holding a 0x0001 float array for tables)
// The meaning of the properties is inferred from the data (see particles.cpp); unknown ones
// are kept in `raw` for tools.
#pragma once

#include "alamo/alo.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace alamo {

struct ParticleEmitterDef {
    std::string name;
    uint32_t spawnerType = 0, rendererType = 0;
    // Emission: `burst` particles every `interval` seconds, for `duration` seconds (0 = for as
    // long as the system lives), starting after `delay`.
    float burst = 1, interval = 1, duration = 0, delay = 0;
    float lifetime = 1, lifetimeVariance = 0; // seconds; variance in percent
    // Start position / velocity distributions (units, units per second).
    struct Shape {
        int type = 0;            // 0 fixed vector, 2 sphere, 4 box, 5 cylinder (others: sphere)
        Vec3 vector{};           // type 0
        float radius = 0;        // sphere / cylinder radius
        Vec3 boxMin{}, boxMax{}; // box
        float zMin = 0, zMax = 0; // cylinder height range
    } position, velocity;
    Vec3 acceleration{};         // modifier 0x0a (units per second squared)
    // Size and colour over the particle's life (time 0..1).
    std::vector<std::pair<float, float>> size;  // modifier 0x0f
    std::vector<std::pair<float, Vec4>> color;  // modifier 0x11
    // Texture: atlas rectangle (u, v, width, height), or several to pick from at random.
    Vec4 uv{0, 0, 1, 1};
    std::vector<Vec4> uvChoices;                 // modifier 0x21
    std::vector<std::pair<float, Vec4>> uvAnim;  // modifier 0x12: flipbook over life
    std::string texture, shader;
    bool additive = true;        // shader name contains "Additive"
    bool distortion = false;     // heat haze shaders (screen distortion): not drawn
    bool stretched = false;      // renderer 0x1d mode 1: stretched along the velocity
    bool flat = false;           // renderer 0x1d mode 0: lying flat (shock rings on the ground)
    float stretch = 0;           // its factor
    bool gpu = false;            // spawner 0x3c: the DX10 duplicate of another emitter
    std::map<uint32_t, std::vector<float>> raw; // spawner properties as numbers (for tools)
};

struct ParticleSystemDef {
    std::string name;
    std::vector<ParticleEmitterDef> emitters;
    // Seconds until every emitter has stopped and its last particle is gone (or -1 if it
    // emits for ever).
    float length() const;
};

// Parses a particle .ALO; throws on a file that is not a particle system.
ParticleSystemDef parseParticleSystem(std::span<const uint8_t> data);
bool isParticleSystem(std::span<const uint8_t> data);

} // namespace alamo
