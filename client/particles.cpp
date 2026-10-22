#include "particles.hpp"

#include "alamo/assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace uaw {

using alamo::Vec3;
using alamo::Vec4;

namespace {

constexpr size_t kMaxParticles = 30000;

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

float interp(const std::vector<std::pair<float, float>>& keys, float t, float fallback) {
    if (keys.empty()) return fallback;
    if (t <= keys.front().first) return keys.front().second;
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].first) {
            const float span = std::max(keys[i].first - keys[i - 1].first, 1e-5f);
            const float k = (t - keys[i - 1].first) / span;
            return keys[i - 1].second + (keys[i].second - keys[i - 1].second) * k;
        }
    return keys.back().second;
}

Vec4 interp(const std::vector<std::pair<float, Vec4>>& keys, float t) {
    if (keys.empty()) return {1, 1, 1, 1};
    if (t <= keys.front().first) return keys.front().second;
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].first) {
            const float span = std::max(keys[i].first - keys[i - 1].first, 1e-5f);
            const float k = (t - keys[i - 1].first) / span;
            Vec4 out;
            for (int c = 0; c < 4; ++c) out[c] = keys[i - 1].second[c] + (keys[i].second[c] - keys[i - 1].second[c]) * k;
            return out;
        }
    return keys.back().second;
}

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 norm(const Vec3& a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0, 0, 1};
}

} // namespace

const alamo::ParticleSystemDef* ParticleEffects::system(const std::string& name, float* scale, float* life) {
    const std::string key = upper(alamo::trim(name));
    auto it = resolved_.find(key);
    if (it == resolved_.end()) {
        Resolved r;
        std::string model = alamo::trim(name);
        if (const alamo::GameObjectType* t = db_.find(model)) {
            const std::string* m = t->find("Land_Model_Name");
            model = m ? alamo::trim(*m) : "";
            r.scale = float(t->number("Scale_Factor").value_or(1.0));
            if (auto f = t->number("Particle_Lifetime_Frames")) r.life = float(*f) / 30.f;
        }
        if (!model.empty()) {
            const std::string path = alamo::modelPath(model.find('.') == std::string::npos ? model + ".alo" : model);
            try {
                if (vfs_.exists(path)) {
                    const auto bytes = vfs_.read(path);
                    if (alamo::isParticleSystem(bytes)) {
                        defs_.push_back(std::make_unique<alamo::ParticleSystemDef>(alamo::parseParticleSystem(bytes)));
                        r.def = defs_.back().get();
                    }
                }
            } catch (const std::exception&) {
            }
        }
        it = resolved_.emplace(key, r).first;
    }
    if (scale) *scale = it->second.scale;
    if (life) *life = it->second.life;
    return it->second.def;
}

bool ParticleEffects::spawn(const std::string& name, Vec3 at, float scale) {
    float s = 1, life = -1;
    const alamo::ParticleSystemDef* def = name.empty() ? nullptr : system(name, &s, &life);
    static const bool log = std::getenv("UAW_PARTICLE_LOG") != nullptr;
    if (log) std::fprintf(stderr, "particles: %s -> %s\n", name.c_str(), def ? def->name.c_str() : "(none)");
    if (!def || def->emitters.empty()) return false;
    Instance in{def, at, s * scale};
    const float len = def->length();
    in.maxAge = life > 0 ? life : (len < 0 ? 2.f : len);
    in.emitted.assign(def->emitters.size(), 0.f);
    instances_.push_back(std::move(in));
    return true;
}

void ParticleEffects::follow(uint64_t key, const std::string& name, Vec3 at, float scale) {
    if (auto it = followed_.find(key); it != followed_.end() && it->second < instances_.size() && instances_[it->second].key == key) {
        Instance& in = instances_[it->second];
        in.position = at;
        in.refreshed = true;
        return;
    }
    float s = 1;
    const alamo::ParticleSystemDef* def = system(name, &s, nullptr);
    static const bool log = std::getenv("UAW_PARTICLE_LOG") != nullptr;
    if (log && def) std::fprintf(stderr, "particles: follow %s -> %s\n", name.c_str(), def->name.c_str());
    if (!def || def->emitters.empty()) return;
    Instance in{def, at, s * scale};
    in.key = key;
    in.maxAge = -1;
    in.emitted.assign(def->emitters.size(), 0.f);
    followed_[key] = instances_.size();
    instances_.push_back(std::move(in));
}

Vec3 ParticleEffects::sample(const alamo::ParticleEmitterDef::Shape& s) {
    switch (s.type) {
    case 0: return s.vector;
    case 4:
        return {s.boxMin[0] + (s.boxMax[0] - s.boxMin[0]) * rand01(), s.boxMin[1] + (s.boxMax[1] - s.boxMin[1]) * rand01(),
                s.boxMin[2] + (s.boxMax[2] - s.boxMin[2]) * rand01()};
    case 5: {
        const float a = rand01() * 6.2831853f, r = s.radius * std::sqrt(rand01());
        return {std::cos(a) * r, std::sin(a) * r, s.zMin + (s.zMax - s.zMin) * rand01()};
    }
    default: {
        // Uniform in a sphere.
        const float z = rand01() * 2 - 1, a = rand01() * 6.2831853f, rr = std::sqrt(std::max(0.f, 1 - z * z));
        const float r = s.radius * std::cbrt(rand01());
        return {std::cos(a) * rr * r, std::sin(a) * rr * r, z * r};
    }
    }
}

void ParticleEffects::emit(Instance& in, size_t ei, int count) {
    const alamo::ParticleEmitterDef& e = in.def->emitters[ei];
    for (int k = 0; k < count && particles_.size() < kMaxParticles; ++k) {
        Particle p;
        p.e = &e;
        const Vec3 off = sample(e.position), vel = sample(e.velocity);
        p.p = {in.position[0] + off[0] * in.scale, in.position[1] + off[1] * in.scale, in.position[2] + off[2] * in.scale};
        p.v = {vel[0] * in.scale, vel[1] * in.scale, vel[2] * in.scale};
        p.age = 0;
        p.life = std::max(0.02f, e.lifetime * (1 + e.lifetimeVariance / 100.f * (rand01() * 2 - 1)));
        p.scale = in.scale;
        p.uv = e.uvChoices.empty() ? e.uv : e.uvChoices[size_t(rand01() * float(e.uvChoices.size())) % e.uvChoices.size()];
        particles_.push_back(p);
    }
}

void ParticleEffects::watch(const alamo::World& w, const UnitControl& control) {
    const int me = control.player();
    auto hidden = [&](const Vec3& p) { return control.fog && w.fogOfWar() && me >= 0 && !w.visibleTo(me, p); };
    if (!primed_) { // don't replay what happened before the first frame
        primed_ = true;
        lastImpactTick_ = w.tickCount();
        deathsSeen_ = w.deaths().size();
    }
    for (const alamo::ImpactEvent& im : w.impacts()) {
        if (im.tick < lastImpactTick_ || !im.type || hidden(im.position)) continue;
        const std::string* a = im.type->find(im.hitUnit ? "Projectile_Object_Detonation_Particle" : "Projectile_Ground_Detonation_Particle");
        const std::string* b = im.type->find(im.hitUnit ? "Projectile_Ground_Detonation_Particle" : "Projectile_Object_Detonation_Particle");
        if (!(a && spawn(alamo::trim(*a), im.position)) && b) spawn(alamo::trim(*b), im.position);
    }
    lastImpactTick_ = w.tickCount() + 1;
    const auto& deaths = w.deaths();
    for (size_t i = deathsSeen_; i < deaths.size(); ++i) {
        const alamo::DeathEvent& d = deaths[i];
        if (!d.type || d.sold || hidden(d.position)) continue;
        if (const std::string* ex = d.type->find("Death_Explosions")) {
            std::string first = alamo::trim(*ex);
            if (const size_t c = first.find_first_of(", "); c != std::string::npos) first.resize(c);
            spawn(first, d.position);
        }
    }
    deathsSeen_ = deaths.size();
    for (const alamo::Projectile& p : w.projectiles()) {
        if (!p.type || !p.id || hidden(p.position)) continue;
        if (const std::string* m = p.type->find("Land_Model_Name"))
            follow(uint64_t(p.id), alamo::trim(*m), p.position, float(p.type->number("Scale_Factor").value_or(1.0)));
    }
}

void ParticleEffects::update(float dt) {
    // Systems: emit, then retire the finished ones.
    for (Instance& in : instances_) {
        in.age += dt;
        if (in.key && !in.refreshed) in.emitting = false; // what it followed is gone
        if (in.key && in.emitting) in.refreshed = false;
        if (in.maxAge >= 0 && in.age > in.maxAge) in.emitting = false;
        if (!in.emitting) continue;
        for (size_t ei = 0; ei < in.def->emitters.size(); ++ei) {
            const alamo::ParticleEmitterDef& e = in.def->emitters[ei];
            if (e.gpu || e.distortion) continue;
            float t = in.age - e.delay;
            if (t < 0) continue;
            if (e.duration > 0) t = std::min(t, e.duration);
            const float bursts = std::floor(t / e.interval) + 1;
            const float due = bursts * e.burst;
            const int n = int(due - in.emitted[ei]);
            if (n > 0) {
                emit(in, ei, std::min(n, 500));
                in.emitted[ei] += float(n);
            }
        }
    }
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [](const Instance& in) { return !in.emitting && in.age > (in.maxAge > 0 ? in.maxAge : 0) + 10; }),
                     instances_.end());
    followed_.clear();
    for (size_t i = 0; i < instances_.size(); ++i)
        if (instances_[i].key && instances_[i].emitting) followed_[instances_[i].key] = i;
    // Particles.
    for (Particle& p : particles_) {
        p.age += dt;
        const Vec3& a = p.e->acceleration;
        p.v = {p.v[0] + a[0] * p.scale * dt, p.v[1] + a[1] * p.scale * dt, p.v[2] + a[2] * p.scale * dt};
        p.p = {p.p[0] + p.v[0] * dt, p.p[1] + p.v[1] * dt, p.p[2] + p.v[2] * dt};
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const Particle& p) { return p.age >= p.life; }),
                     particles_.end());
}

void ParticleEffects::draw(Renderer& r, const View& view) {
    if (particles_.empty()) return;
    const Vec3 f = norm(sub(view.target, view.eye));
    const Vec3 right = norm(cross(f, {0, 0, 1}));
    const Vec3 up = cross(right, f);
    // Batches per texture and blend mode.
    struct Batch { std::vector<GpuVertex> v; };
    std::unordered_map<std::string, Batch> batches;
    std::vector<std::pair<float, const Particle*>> order;
    order.reserve(particles_.size());
    for (const Particle& p : particles_) order.push_back({dot(sub(p.p, view.eye), f), &p});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); // far first
    for (const auto& [depth, pp] : order) {
        const Particle& p = *pp;
        if (depth < 1) continue;
        const alamo::ParticleEmitterDef& e = *p.e;
        const float t = std::clamp(p.age / p.life, 0.f, 1.f);
        const float size = interp(e.size, t, 4.f) * p.scale;
        if (size <= 0.01f) continue;
        Vec4 c = interp(e.color, t);
        if (c[3] <= 0.004f) continue;
        Vec4 uv = p.uv;
        if (!e.uvAnim.empty()) {
            uv = e.uvAnim.front().second;
            for (const auto& [kt, kuv] : e.uvAnim)
                if (kt <= t) uv = kuv;
        }
        Vec3 ax, ay;
        const float h = size / 2;
        if (e.stretched && dot(p.v, p.v) > 1e-4f) {
            Vec3 along = sub(p.v, {f[0] * dot(p.v, f), f[1] * dot(p.v, f), f[2] * dot(p.v, f)});
            const float speed = std::sqrt(dot(p.v, p.v));
            along = norm(along);
            const Vec3 side = norm(cross(f, along));
            const float len = h + speed * e.stretch;
            ax = {along[0] * len, along[1] * len, along[2] * len};
            ay = {side[0] * h, side[1] * h, side[2] * h};
        } else if (e.flat) {
            ax = {h, 0, 0};
            ay = {0, h, 0};
        } else {
            ax = {right[0] * h, right[1] * h, right[2] * h};
            ay = {up[0] * h, up[1] * h, up[2] * h};
        }
        // Billboards are pulled towards the camera by half their size so that puffs on the ground
        // aren't cut in half by it (flat rings are lifted a little instead).
        const Vec3 c0 = e.flat ? Vec3{p.p[0], p.p[1], p.p[2] + 1.5f} : Vec3{p.p[0] - f[0] * h, p.p[1] - f[1] * h, p.p[2] - f[2] * h};
        auto vert = [&](float sx, float sy, float u, float v) {
            GpuVertex g{};
            g.px = c0[0] + ax[0] * sx + ay[0] * sy;
            g.py = c0[1] + ax[1] * sx + ay[1] * sy;
            g.pz = c0[2] + ax[2] * sx + ay[2] * sy;
            g.nz = 1;
            g.u = u;
            g.v = v;
            g.r = c[0];
            g.g = c[1];
            g.b = c[2];
            g.a = c[3];
            return g;
        };
        const float u0 = uv[0], v0 = uv[1], u1 = uv[0] + uv[2], v1 = uv[1] + uv[3];
        Batch& b = batches[(e.additive ? "+" : "-") + e.texture];
        const GpuVertex q0 = vert(-1, 1, u0, v0), q1 = vert(1, 1, u1, v0), q2 = vert(1, -1, u1, v1), q3 = vert(-1, -1, u0, v1);
        b.v.insert(b.v.end(), {q0, q1, q2, q0, q2, q3});
    }
    // Alpha-blended smoke first, additive glow over it.
    for (int pass = 0; pass < 2; ++pass)
        for (auto& [k, b] : batches) {
            const bool additive = k[0] == '+';
            if (additive != (pass == 1)) continue;
            const GLuint tex = r.textureNamed(k.substr(1));
            if (tex) r.drawParticles(b.v, tex, additive); // without its texture a particle is just a square
        }
}

} // namespace uaw
