// Tactical superweapons (see SuperweaponSpec in sim.hpp).
#include "alamo/sim.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace alamo {
namespace {

int ticksOf(float seconds) { return int(std::lround(seconds * kTicksPerSecond)); }

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// SWOwnerList[Find_Object_Type("Novus_Superweapon_EMP")]="NOVUS_SUPERWEAPON_EMP_WEAPON"
std::string weaponNameFor(const std::string& script, const std::string& enabler) {
    const std::string u = upper(script), want = upper(enabler);
    size_t pos = 0;
    while ((pos = u.find("FIND_OBJECT_TYPE(\"", pos)) != std::string::npos) {
        pos += 18;
        const size_t end = u.find('"', pos);
        if (end == std::string::npos) break;
        const std::string name = u.substr(pos, end - pos);
        const size_t q1 = u.find('"', u.find('=', end));
        const size_t q2 = q1 == std::string::npos ? q1 : u.find('"', q1 + 1);
        if (name == want && q2 != std::string::npos) return script.substr(q1 + 1, q2 - q1 - 1);
        pos = end;
    }
    return {};
}

} // namespace

const SuperweaponSpec* World::superweaponOf(const GameObjectType& enabler) const {
    if (auto it = superweapons_.find(&enabler); it != superweapons_.end()) return it->second.get();
    std::unique_ptr<SuperweaponSpec> spec;
    const std::string* behaviors = enabler.find("Lua_Land_Behaviors");
    const std::string* script = enabler.find("Lua_Script_Data");
    if (behaviors && script && upper(*behaviors).find("SUPERWEAPONENABLERBEHAVIOR") != std::string::npos) {
        const std::string name = weaponNameFor(*script, enabler.name);
        if (const GameObjectType* w = name.empty() ? nullptr : db_.find(name)) {
            spec = std::make_unique<SuperweaponSpec>();
            SuperweaponSpec& s = *spec;
            s.weapon = w;
            s.cooldown = float(w->number("TSW_Cooldown_Countdown_Seconds").value_or(180));
            s.warmUp = float(w->number("TSW_Warm_Up_Seconds").value_or(s.cooldown));
            s.radius = float(w->number("TSW_Area_Effect_Radius").value_or(150));
            if (const std::string* p = w->find("Bombardment_Projectile"))
                for (const std::string& n : words(*p))
                    if (const GameObjectType* pt = db_.find(n)) s.projectiles.push_back(pt);
            if (!s.projectiles.empty()) {
                s.salvos = std::max(1, int(w->number("Bombardment_Number_Of_Salvos").value_or(1)));
                s.shotsPerSalvo = std::max(1, int(w->number("Bombardment_Shots_Per_Salvo").value_or(1)));
                s.shotDelay = float(w->number("Bombardment_Shot_Delay").value_or(0.5));
                s.salvoDelay = float(w->number("Bombardment_Salvo_Delay").value_or(1));
                // Shots land across the weapon's area (Bombardment_Radius is the spawn ring in the sky).
                s.spread = s.shotsPerSalvo * s.salvos > 1 ? s.radius : 0;
            }
            if (const std::string* sp = w->find("Spawn_Object_On_Death"); sp && !trim(*sp).empty()) {
                s.spawnOnEnd = db_.find(trim(*sp));
                s.lifetime = 3; // the gravity ball drops, then collapses into the singularity
            }
            if (auto life = w->number("Tornado_Lifetime_Seconds")) s.lifetime = float(*life);
        }
    }
    const SuperweaponSpec* out = spec.get();
    superweapons_[&enabler] = std::move(spec);
    return out;
}

float World::superweaponCountdown(uint32_t id) const {
    const Unit* u = nullptr;
    for (const Unit& x : units_)
        if (x.id == id) u = &x;
    if (!u || u->underConstruction || !superweaponOf(*u->type) || u->superweaponReadyTick < 0) return -1;
    return std::max(0.f, float(u->superweaponReadyTick - int64_t(ticks_)) / kTicksPerSecond);
}

uint32_t World::createEffectObject(const GameObjectType& ot, int owner, Vec3 position, int lifeTicks) {
    Unit o;
    o.id = nextId_++;
    o.type = &ot;
    o.owner = o.originalOwner = owner;
    o.position = position;
    o.maxSpeed = 0;
    o.maxHealth = o.health = std::max(1.f, float(ot.number("Tactical_Health").value_or(1)));
    o.radius = 1;
    o.lifeTicks = lifeTicks;
    initAbilities(o);
    const uint32_t id = o.id;
    pendingObjects_.push_back(std::move(o));
    return id;
}

OrderResult World::fireSuperweapon(uint32_t id, Vec3 target) {
    RecordScope rec_(*this, Command{0, Command::Kind::Superweapon, {id}, 0, -1, 0, target});
    if (rec_.captured) return OrderResult::Ok; // sent over the network; it runs when its turn comes
    Unit* u = unit(id);
    if (!u) return OrderResult::InvalidUnit;
    const SuperweaponSpec* s = superweaponOf(*u->type);
    if (!s) return OrderResult::NotAllowed;
    if (superweaponCountdown(id) != 0) return OrderResult::NotAllowed; // building, warming up or recharging
    const int owner = u->owner;
    u->superweaponReadyTick = int64_t(ticks_) + ticksOf(s->cooldown);
    target[2] = map_.heightAt(target[0], target[1]);
    createEffectObject(*s->weapon, owner, target, ticksOf(s->lifetime));
    if (!s->projectiles.empty()) {
        // Shots fall in salvos; each lands about a second after it is released.
        int shot = 0;
        for (int salvo = 0; salvo < s->salvos; ++salvo)
            for (int k = 0; k < s->shotsPerSalvo; ++k, ++shot) {
                const float t = float(salvo) * (float(s->shotsPerSalvo) * s->shotDelay + s->salvoDelay) + float(k) * s->shotDelay + 1;
                Vec3 p = target;
                if (s->spread > 0) {
                    const float a = random01() * 6.2831853f, r = std::sqrt(random01()) * s->spread;
                    p = {target[0] + std::cos(a) * r, target[1] + std::sin(a) * r, 0};
                    p[2] = map_.heightAt(p[0], p[1]);
                }
                superweaponEvents_.push_back({int64_t(ticks_) + ticksOf(t), owner, s->projectiles[size_t(shot) % s->projectiles.size()], true, p});
            }
    }
    if (s->spawnOnEnd) superweaponEvents_.push_back({int64_t(ticks_) + ticksOf(s->lifetime), owner, s->spawnOnEnd, false, target});
    return OrderResult::Ok;
}

void World::updateSuperweapons() {
    // Arm enablers as they are finished: the first shot after the warm-up.
    for (Unit& u : units_)
        if (u.structure && !u.underConstruction && u.superweaponReadyTick < 0)
            if (const SuperweaponSpec* s = superweaponOf(*u.type)) u.superweaponReadyTick = int64_t(ticks_) + ticksOf(s->warmUp);
    // Moving superweapons (the Masari magma channel, a walking firestorm): drift toward the
    // nearest enemy unit within reach of where they are.
    for (Unit& o : effectObjects_) {
        const float speed = float(o.type->number("Max_Speed").value_or(0));
        if (speed <= 0 || !o.type->number("TSW_Area_Effect_Radius")) continue;
        const float reach = float(*o.type->number("TSW_Area_Effect_Radius")) * 2;
        const Unit* best = nullptr;
        float bestD = reach;
        for (const Unit& u : units_) {
            if (!u.alive() || !enemies(o.owner, u.owner)) continue;
            const float d = std::hypot(u.position[0] - o.position[0], u.position[1] - o.position[1]);
            if (d < bestD) { bestD = d; best = &u; }
        }
        if (!best || bestD < 1) continue;
        const float k = std::min(speed, bestD) / bestD;
        o.position[0] += (best->position[0] - o.position[0]) * k;
        o.position[1] += (best->position[1] - o.position[1]) * k;
        o.position[2] = map_.heightAt(o.position[0], o.position[1]);
    }
    if (superweaponEvents_.empty()) return;
    std::vector<SuperweaponEvent> later;
    for (const SuperweaponEvent& e : superweaponEvents_) {
        if (e.tick > int64_t(ticks_)) {
            later.push_back(e);
            continue;
        }
        if (e.projectile) {
            // Dropped straight down onto the point; damage as the projectile defines it.
            Projectile p;
            p.owner = e.owner;
            p.type = e.type;
            const float drop = 60;
            p.position = {e.point[0], e.point[1], e.point[2] + drop};
            p.velocity = {0, 0, -std::max(drop, float(e.type->number("Max_Speed").value_or(drop)))};
            p.distance = drop;
            p.damage = float(e.type->number("Projectile_Damage").value_or(0));
            p.blastDamage = float(e.type->number("Projectile_Blast_Area_Damage").value_or(0));
            p.blastRange = float(e.type->number("Projectile_Blast_Area_Range").value_or(0));
            if (const std::string* dt = e.type->find("Damage_Type")) p.damageType = trim(*dt);
            projectiles_.push_back(std::move(p));
        } else {
            const float life = float(e.type->number("Tornado_Lifetime_Seconds").value_or(10));
            createEffectObject(*e.type, e.owner, e.point, ticksOf(life));
        }
    }
    superweaponEvents_.swap(later);
}

} // namespace alamo
