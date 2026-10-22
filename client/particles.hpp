// Particle effects in the viewer: the game's particle systems (alamo/particles.hpp) simulated on
// the CPU and drawn as camera-facing quads. Purely visual; the simulation never reads them.
#pragma once

#include "renderer.hpp"
#include "units.hpp"

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"
#include "alamo/particles.hpp"
#include "alamo/sim.hpp"

#include <memory>
#include <random>
#include <unordered_map>

namespace uaw {

class ParticleEffects {
public:
    ParticleEffects(alamo::MegVfs& vfs, const alamo::GameDatabase& db) : vfs_(vfs), db_(db) {}

    // One-shot effect: a Particle object type (Particles.xml, e.g. "Novus_Large_Explosion_Vent")
    // or a particle .ALO by file name. Returns false if there is no such particle system.
    bool spawn(const std::string& name, alamo::Vec3 at, float scale = 1);
    // An effect that follows something (a projectile's trail): call every frame with the same key
    // while it exists; once it stops being refreshed it stops emitting and fades out.
    void follow(uint64_t key, const std::string& name, alamo::Vec3 at, float scale = 1);

    // Spawns impact and death effects for new world events and trails for shots in flight.
    void watch(const alamo::World& w, const UnitControl& control);
    void update(float dt);
    void draw(Renderer& r, const View& view);

    size_t particleCount() const { return particles_.size(); }
    size_t systemCount() const { return instances_.size(); }
    // The particle system an object type or .ALO name resolves to (cached), or nullptr.
    const alamo::ParticleSystemDef* system(const std::string& name, float* scale = nullptr, float* life = nullptr);

private:
    struct Resolved {
        const alamo::ParticleSystemDef* def = nullptr;
        float scale = 1, life = -1; // Scale_Factor, Particle_Lifetime_Frames in seconds (-1 = none)
    };
    struct Instance {
        const alamo::ParticleSystemDef* def;
        alamo::Vec3 position;
        float scale = 1, age = 0, maxAge = -1;
        uint64_t key = 0;      // followed thing, 0 = one-shot
        bool refreshed = true; // followed: seen this frame
        bool emitting = true;
        std::vector<float> emitted; // particles emitted so far, per emitter
    };
    struct Particle {
        const alamo::ParticleEmitterDef* e;
        alamo::Vec3 p, v;
        float age, life, scale;
        alamo::Vec4 uv;
    };
    alamo::MegVfs& vfs_;
    const alamo::GameDatabase& db_;
    std::unordered_map<std::string, Resolved> resolved_;
    std::vector<std::unique_ptr<alamo::ParticleSystemDef>> defs_;
    std::vector<Instance> instances_;
    std::vector<Particle> particles_;
    std::unordered_map<uint64_t, size_t> followed_; // key -> instance index (rebuilt each update)
    std::mt19937 rng_{12345};
    uint64_t lastImpactTick_ = 0, lastDeathTick_ = 0;
    size_t deathsSeen_ = 0;
    bool primed_ = false;
    float rand01() { return std::uniform_real_distribution<float>(0, 1)(rng_); }
    alamo::Vec3 sample(const alamo::ParticleEmitterDef::Shape& s);
    void emit(Instance& in, size_t emitter, int count);
};

} // namespace uaw
