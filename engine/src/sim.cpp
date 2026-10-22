#include "alamo/sim.hpp"

#include <algorithm>
#include <cmath>

namespace alamo {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.f;

MoveClass parseMoveClass(const std::string* s) {
    if (!s) return MoveClass::Other;
    const std::string v = trim(*s);
    if (iequals(v, "Infantry")) return MoveClass::Infantry;
    if (iequals(v, "Hover")) return MoveClass::Hover;
    if (iequals(v, "Walker")) return MoveClass::Walker;
    if (iequals(v, "LargeWalker")) return MoveClass::LargeWalker;
    if (iequals(v, "Wheeled") || iequals(v, "Tracked") || iequals(v, "Vehicle")) return MoveClass::Vehicle;
    if (iequals(v, "Fighter") || iequals(v, "Flying") || iequals(v, "Air")) return MoveClass::Air;
    return MoveClass::Other;
}

float dist2d(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

} // namespace

float angleDiff(float from, float to) {
    float d = std::fmod(to - from, 2 * kPi);
    if (d > kPi) d -= 2 * kPi;
    if (d < -kPi) d += 2 * kPi;
    return d;
}

World::World(const Map& map, const GameDatabase& db, const DamageTable* damage, uint64_t seed, const TechTree* tech)
    : map_(map), db_(db), nav_(map), damage_(damage), tech_(tech), baseNav_(nav_),
      rng_(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}

const char* toString(OrderResult r) {
    switch (r) {
    case OrderResult::Ok: return "ok";
    case OrderResult::NotAllowed: return "not available";
    case OrderResult::NotEnoughCredits: return "not enough credits";
    case OrderResult::PopulationCap: return "population cap reached";
    case OrderResult::BlockedSite: return "site blocked";
    case OrderResult::InvalidUnit: return "invalid unit";
    }
    return "?";
}

BuildInfo buildInfo(const GameObjectType& type, const GameDatabase& db) {
    const GameObjectType* src = &type;
    if (const std::string* uc = type.find("Tactical_Under_Construction_Object_Type"))
        if (const GameObjectType* c = db.find(trim(*uc))) src = c;
    BuildInfo b;
    b.cost = float(src->number("Tactical_Build_Cost_Multiplayer").value_or(type.number("Tactical_Build_Cost_Multiplayer").value_or(0)));
    const double build = src->number("Tactical_Build_Time_Seconds").value_or(type.number("Tactical_Build_Time_Seconds").value_or(10));
    const double pre = src->number("Required_Pre_Build_Time_In_Secs").value_or(0);
    b.ticks = std::max(1, int(std::lround((build + pre) * kTicksPerSecond)));
    const std::string* needs = src->find("Tactical_Build_Requires_Builder");
    const bool requiresBuilder = !needs || iequals(trim(*needs), "true") || iequals(trim(*needs), "yes");
    b.builderTicks = pre > 0 ? int(std::lround(pre * kTicksPerSecond)) : (requiresBuilder ? b.ticks : 0);
    const std::string& cat = type.category;
    b.spawnsUnit = cat.find("Units") != std::string::npos || cat.find("Heroes") != std::string::npos ||
                   cat.find("Walkers") != std::string::npos || cat.find("Military") != std::string::npos;
    return b;
}

int World::addPlayer(const std::string& faction, float credits, int popCap) {
    players_.push_back({int(players_.size()), faction, credits, popCap});
    return players_.back().id;
}

Player* World::player(int id) { return id >= 0 && id < int(players_.size()) ? &players_[size_t(id)] : nullptr; }

int World::popUsed(int owner) const {
    int n = 0;
    for (const Unit& u : units_)
        if (u.owner == owner) {
            n += u.pop;
            for (const GameObjectType* q : u.queue) n += int(q->number("Unit_Pop_Cap").value_or(0));
        }
    return n;
}

bool World::owns(int owner, const std::string& typeName) const {
    for (const Unit& u : units_) {
        if (u.owner != owner || u.underConstruction) continue;
        if (iequals(u.type->name, typeName)) return true;
        for (const GameObjectType* up : u.upgrades)
            if (iequals(up->name, typeName)) return true;
    }
    return false;
}

std::vector<std::string> World::producible(uint32_t id) const {
    const Unit* s = nullptr;
    for (const Unit& u : units_)
        if (u.id == id) s = &u;
    if (!s || !tech_ || s->underConstruction) return {};
    const int owner = s->owner;
    return tech_->producibleBy(s->type->name, [&](const std::string& t) { return owns(owner, t); });
}

std::vector<std::string> World::constructible(uint32_t id) const {
    const Unit* b = nullptr;
    for (const Unit& u : units_)
        if (u.id == id) b = &u;
    if (!b || !tech_ || b->structure) return {}; // structures only appear as prerequisites
    const int owner = b->owner;
    return tech_->constructibleBy(b->type->name, [&](const std::string& t) { return owns(owner, t); });
}

OrderResult World::queueProduction(uint32_t id, const std::string& typeName) {
    Unit* s = unit(id);
    if (!s) return OrderResult::InvalidUnit;
    const GameObjectType* t = db_.find(typeName);
    if (!t) return OrderResult::InvalidUnit;
    bool allowed = false;
    for (const auto& n : producible(id)) allowed = allowed || iequals(n, typeName);
    if (!allowed) return OrderResult::NotAllowed;
    const BuildInfo info = buildInfo(*t, db_);
    if (!info.spawnsUnit) {
        // Upgrades: once per structure (and not while one is queued).
        if (owns(s->owner, t->name)) return OrderResult::NotAllowed;
        for (const GameObjectType* q : s->queue)
            if (q == t) return OrderResult::NotAllowed;
    }
    Player* p = player(s->owner);
    if (p && p->credits < info.cost) return OrderResult::NotEnoughCredits;
    if (p && info.spawnsUnit && popUsed(s->owner) + int(t->number("Unit_Pop_Cap").value_or(0)) > p->popCap)
        return OrderResult::PopulationCap;
    if (p) p->credits -= info.cost;
    if (s->queue.empty()) s->queueTicks = 0;
    s->queue.push_back(t);
    return OrderResult::Ok;
}

void World::cancelProduction(uint32_t id) {
    Unit* s = unit(id);
    if (!s || s->queue.empty()) return;
    if (Player* p = player(s->owner)) p->credits += buildInfo(*s->queue.back(), db_).cost;
    s->queue.pop_back();
    if (s->queue.empty()) s->queueTicks = 0;
}

void World::setRally(uint32_t id, Vec3 p) {
    if (Unit* s = unit(id)) {
        s->rally = p;
        s->hasRally = true;
    }
}

bool World::siteClear(const GameObjectType& type, Vec3 site) const {
    const float fx = float(type.number("Custom_Hard_XExtent").value_or(20));
    const float fy = float(type.number("Custom_Hard_YExtent").value_or(fx));
    for (float y = site[1] - fy; y <= site[1] + fy; y += nav_.spacing() * 0.5f)
        for (float x = site[0] - fx; x <= site[0] + fx; x += nav_.spacing() * 0.5f)
            if (!nav_.passableAt(x, y)) return false;
    for (const Unit& u : units_)
        if (u.structure && std::fabs(u.position[0] - site[0]) < fx + u.footprintX &&
            std::fabs(u.position[1] - site[1]) < fy + u.footprintY)
            return false;
    return true;
}

OrderResult World::commandConstruct(uint32_t builderId, const std::string& typeName, Vec3 site) {
    Unit* b = unit(builderId);
    if (!b) return OrderResult::InvalidUnit;
    const GameObjectType* t = db_.find(typeName);
    if (!t) return OrderResult::InvalidUnit;
    bool allowed = false;
    for (const auto& n : constructible(builderId)) allowed = allowed || iequals(n, typeName);
    if (!allowed) return OrderResult::NotAllowed;
    if (!siteClear(*t, site)) return OrderResult::BlockedSite;
    Player* p = player(b->owner);
    const BuildInfo info = buildInfo(*t, db_);
    if (p && p->credits < info.cost) return OrderResult::NotEnoughCredits;
    if (p) p->credits -= info.cost;

    const int owner = b->owner;
    const uint32_t sid = spawn(typeName, owner, site, 0);
    b = unit(builderId); // spawn may have reallocated
    Unit* s = unit(sid);
    if (!s) return OrderResult::InvalidUnit;
    s->underConstruction = true;
    s->buildProgress = 0;
    s->buildTicks = info.ticks;
    s->builderTicks = info.builderTicks;
    s->health = std::max(1.f, s->maxHealth * 0.05f);
    b->constructing = sid;
    b->attackOrder = 0;
    b->attackMove = false;
    b->goal = site;
    b->path = nav_.findPath(b->position, nav_.nearestPassable({site[0] - s->footprintX - b->radius - 10, site[1], 0}));
    return OrderResult::Ok;
}

void World::rebuildNav() {
    nav_ = baseNav_;
    for (const Unit& u : units_) {
        if (!u.structure) continue;
        for (int y = nav_.cellY(u.position[1] - u.footprintY); y <= nav_.cellY(u.position[1] + u.footprintY); ++y)
            for (int x = nav_.cellX(u.position[0] - u.footprintX); x <= nav_.cellX(u.position[0] + u.footprintX); ++x)
                nav_.setPassable(x, y, false);
    }
    nav_.rebuild();
    navDirty_ = false;
}

Vec3 World::exitPoint(const Unit& s, const Unit& u) {
    // Out the side facing the rally point (or the map centre), clear of the footprint.
    const Vec3 toward = s.hasRally ? s.rally : Vec3{map_.sizeX() / 2, map_.sizeY() / 2, 0};
    float dx = toward[0] - s.position[0], dy = toward[1] - s.position[1];
    const float len = std::max(std::hypot(dx, dy), 1e-3f);
    dx /= len;
    dy /= len;
    const float out = std::max(s.footprintX, s.footprintY) + u.radius + 25;
    return nav_.nearestPassable({s.position[0] + dx * out, s.position[1] + dy * out, 0});
}

void World::updateProduction(Unit& s) {
    if (s.queue.empty() || s.underConstruction) return;
    const GameObjectType* t = s.queue.front();
    const BuildInfo info = buildInfo(*t, db_);
    if (++s.queueTicks < info.ticks) return;
    s.queue.erase(s.queue.begin());
    s.queueTicks = 0;
    if (!info.spawnsUnit) {
        s.upgrades.push_back(t);
        return;
    }
    const uint32_t sid = s.id;
    const int owner = s.owner;
    const float facing = 0;
    const uint32_t id = spawn(t->name, owner, s.position, facing); // may reallocate units_
    Unit* nu = unit(id);
    Unit* st = unit(sid);
    if (!nu || !st) return;
    const Vec3 out = exitPoint(*st, *nu);
    nu->position = {out[0], out[1], map_.heightAt(out[0], out[1])};
    nu->facing = std::atan2(out[1] - st->position[1], out[0] - st->position[0]);
    if (st->hasRally) commandMove({id}, st->rally);
}

void World::updateConstruction(Unit& s) {
    if (!s.underConstruction) return;
    // Progress only while one of the owner's builders assigned to it is close.
    bool working = s.buildProgress * float(s.buildTicks) >= float(s.builderTicks); // past the builder phase
    for (const Unit& b : units_)
        if (b.constructing == s.id && b.owner == s.owner &&
            dist2d(b.position, s.position) <= std::max(s.footprintX, s.footprintY) + b.radius + 40)
            working = true;
    if (!working) return;
    if (s.buildProgress * float(s.buildTicks) + 1 >= float(s.builderTicks))
        for (Unit& b : units_)
            if (b.constructing == s.id) b.constructing = 0; // builder phase over: free the builder
    const float step = 1.f / float(s.buildTicks);
    s.buildProgress = std::min(1.f, s.buildProgress + step);
    s.health = std::min(s.maxHealth, s.health + s.maxHealth * step * 0.95f);
    if (s.buildProgress >= 1) {
        s.underConstruction = false;
        for (Unit& b : units_)
            if (b.constructing == s.id) b.constructing = 0;
    }
}

float World::random01() {
    // xorshift64*: deterministic across platforms, so lockstep replays stay in sync.
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    return float((rng_ * 0x2545F4914F6CDD1Dull) >> 40) / float(1u << 24);
}

float Unit::maxRange() const {
    float r = 0;
    for (const auto& w : weapons) r = std::max(r, w.spec.maxRange);
    return r;
}

uint32_t World::spawn(const std::string& typeName, int owner, Vec3 position, float facing) {
    const GameObjectType* t = db_.find(typeName);
    if (!t) return 0;
    Unit u;
    u.id = nextId_++;
    u.type = t;
    u.owner = owner;
    const Vec3 p = nav_.nearestPassable(position);
    u.position = {p[0], p[1], map_.heightAt(p[0], p[1])};
    u.facing = facing;
    u.moveClass = parseMoveClass(t->find("MovementClass"));
    u.maxSpeed = float(t->number("Max_Speed").value_or(1.0));
    u.turnRate = float(t->number("Max_Rate_Of_Turn").value_or(10.0)) * kDegToRad;
    u.stoppedTurnRate = float(t->number("Stopped_Rate_Of_Turn").value_or(t->number("Max_Rate_Of_Turn").value_or(10.0))) * kDegToRad;
    u.acceleration = float(t->number("OverrideAcceleration").value_or(0.1)) * u.maxSpeed;
    if (u.acceleration <= 0) u.acceleration = 0.1f * u.maxSpeed;
    u.radius = float(t->number("Custom_Hard_XExtent").value_or(t->number("Custom_Soft_Footprint_Radius").value_or(6.0)));
    u.radius = std::clamp(u.radius, 2.f, 150.f);
    u.maxHealth = u.health = float(t->number("Tactical_Health").value_or(100.0));
    if (const std::string* a = t->find("Armor_Type")) u.armorType = trim(*a);
    u.pop = int(t->number("Unit_Pop_Cap").value_or(0));
    const std::string* mc = t->find("MovementClass");
    u.structure = (mc && iequals(trim(*mc), "Building")) || t->category.find("Structure") != std::string::npos;
    if (u.structure) {
        u.footprintX = float(t->number("Custom_Hard_XExtent").value_or(20));
        u.footprintY = float(t->number("Custom_Hard_YExtent").value_or(u.footprintX));
        u.maxSpeed = 0;
        u.radius = std::max(u.footprintX, u.footprintY);
        u.position = {position[0], position[1], map_.heightAt(position[0], position[1])}; // no snapping to open ground
        navDirty_ = true;
    }
    for (const WeaponSpec& w : weaponsFor(*t, db_)) u.weapons.push_back({w, 0, 0, 0});
    units_.push_back(std::move(u));
    return units_.back().id;
}

Unit* World::unit(uint32_t id) {
    for (auto& u : units_)
        if (u.id == id) return &u;
    return nullptr;
}

void World::commandMove(const std::vector<uint32_t>& ids, Vec3 target) {
    std::vector<Unit*> group;
    for (uint32_t id : ids)
        if (Unit* u = unit(id)) group.push_back(u);
    if (group.empty()) return;

    // Square formation centred on the target, spacing from the largest unit,
    // filled in order of distance so nearby units take nearby slots.
    float spacing = 0;
    for (Unit* u : group) spacing = std::max(spacing, u->radius * 2.6f);
    spacing = std::max(spacing, 14.f);
    const int cols = int(std::ceil(std::sqrt(float(group.size()))));
    std::vector<Vec3> slots;
    for (size_t i = 0; i < group.size(); ++i) {
        const int r = int(i) / cols, c = int(i) % cols;
        const float ox = (c - (cols - 1) * 0.5f) * spacing, oy = (r - (cols - 1) * 0.5f) * spacing;
        slots.push_back(nav_.nearestPassable({target[0] + ox, target[1] + oy, 0}));
    }
    std::vector<bool> taken(slots.size(), false);
    std::sort(group.begin(), group.end(), [&](Unit* a, Unit* b) { return dist2d(a->position, target) < dist2d(b->position, target); });
    for (Unit* u : group) {
        size_t best = 0;
        float bestD = 1e30f;
        for (size_t s = 0; s < slots.size(); ++s) {
            if (taken[s]) continue;
            const float d = dist2d(u->position, slots[s]);
            if (d < bestD) { bestD = d; best = s; }
        }
        taken[best] = true;
        u->attackOrder = 0;
        u->attackMove = false;
        u->goal = slots[best];
        u->path = nav_.findPath(u->position, slots[best]);
    }
}

void World::commandStop(const std::vector<uint32_t>& ids) {
    for (uint32_t id : ids)
        if (Unit* u = unit(id)) {
            u->path.clear();
            u->attackOrder = 0;
            u->attackMove = false;
        }
}

void World::commandAttackMove(const std::vector<uint32_t>& ids, Vec3 target) {
    commandMove(ids, target);
    for (uint32_t id : ids)
        if (Unit* u = unit(id); u && !u->weapons.empty()) u->attackMove = true;
}

void World::commandAttack(const std::vector<uint32_t>& ids, uint32_t target) {
    const Unit* t = unit(target);
    if (!t) return;
    for (uint32_t id : ids) {
        Unit* u = unit(id);
        if (!u || u->weapons.empty() || !enemies(u->owner, t->owner)) continue;
        u->attackOrder = target;
        u->attackMove = false;
        u->target = target;
        u->repathTicks = 0;
    }
}

void World::updateTargeting(Unit& u) {
    if (u.weapons.empty()) return;
    const float range = u.maxRange();
    auto edgeDistance = [&](const Unit& t) { return dist2d(u.position, t.position) - t.radius; };

    if (u.attackOrder) {
        const Unit* t = unit(u.attackOrder);
        if (!t) {
            u.attackOrder = 0;
            u.path.clear();
        } else {
            u.target = t->id;
            if (edgeDistance(*t) > range * 0.9f) {
                // Chase: re-plan now and then, or when the target has moved off the goal.
                if (--u.repathTicks <= 0 || dist2d(u.goal, t->position) > range * 0.5f) {
                    u.goal = t->position;
                    u.path = nav_.findPath(u.position, t->position);
                    u.repathTicks = kTicksPerSecond;
                }
            } else {
                u.path.clear();
            }
            return;
        }
    }

    // Keep shooting the current target while it stays (roughly) in range.
    const Unit* current = unit(u.target);
    if (!current || edgeDistance(*current) > range + 20) {
        u.target = 0;
        float best = 1e30f;
        for (const Unit& o : units_) {
            if (!o.alive() || !enemies(u.owner, o.owner)) continue;
            const float d = edgeDistance(o);
            if (d <= range && d < best) {
                best = d;
                u.target = o.id;
            }
        }
    }
    if (u.attackMove) {
        if (u.target) {
            u.path.clear(); // hold and fight
        } else if (u.path.empty() && dist2d(u.position, u.goal) > std::max(u.radius * 2, 20.f)) {
            u.path = nav_.findPath(u.position, u.goal); // area clear: carry on
        } else if (u.path.empty()) {
            u.attackMove = false; // arrived
        }
    }
}

int addSkirmishPlayer(World& world, const GameDatabase& db, const std::string& faction, Vec3 start) {
    const Faction* f = db.faction(faction);
    const int id = world.addPlayer(f ? f->name : faction, f ? f->multiplayerCredits : 10000.f, f ? f->multiplayerPopCap : 90);
    struct Start { const char* faction; const char* structure; const char* builder; };
    static const Start starts[] = {{"Alien", "Alien_Arrival_Site", "Alien_Glyph_Carver"},
                                   {"Novus", "Novus_Remote_Terminal", "Novus_Constructor"},
                                   {"Masari", "Masari_Foundation", "Masari_Architect"},
                                   {"Military", nullptr, "Military_Hummer"}};
    for (const Start& s : starts) {
        if (!iequals(s.faction, faction)) continue;
        const float toCentre = std::atan2(world.map().sizeY() / 2 - start[1], world.map().sizeX() / 2 - start[0]);
        if (s.structure) world.spawn(s.structure, id, start, 0);
        const float out = 110;
        world.spawn(s.builder, id, {start[0] + std::cos(toCentre) * out, start[1] + std::sin(toCentre) * out, 0}, toCentre);
    }
    return id;
}

void World::fireWeapons(Unit& u) {
    if (u.underConstruction) return;
    const Unit* t = u.target ? unit(u.target) : nullptr;
    if (t && !u.moving()) {
        // Face the target while standing (turrets handle aim while moving).
        const float want = std::atan2(t->position[1] - u.position[1], t->position[0] - u.position[0]);
        const float diff = angleDiff(u.facing, want);
        const float rate = std::max(u.stoppedTurnRate, u.turnRate * 0.5f);
        u.facing += std::clamp(diff, -rate, rate);
    }
    for (Unit::WeaponState& w : u.weapons) {
        if (w.cooldown > 0) --w.cooldown;
        const float d = t ? dist2d(u.position, t->position) - t->radius : 1e30f;
        const bool inRange = t && d <= w.spec.maxRange && d + t->radius >= w.spec.minRange;
        if (w.pulsesLeft == 0) {
            if (w.cooldown > 0 || !inRange) continue;
            w.pulsesLeft = w.spec.pulseCount; // start a burst
            w.pulseTimer = 0;
        }
        if (w.pulseTimer > 0) {
            --w.pulseTimer;
            continue;
        }
        if (!inRange) { // target gone mid-burst
            w.pulsesLeft = 0;
            w.cooldown = w.spec.rechargeTicks;
            continue;
        }
        const float muzzle = std::max(u.radius * 0.8f, 6.f);
        const Vec3 from{u.position[0], u.position[1], u.position[2] + muzzle};
        for (int p = 0; p < w.spec.pellets; ++p) {
            const float ang = random01() * 6.2831853f, r = random01() * w.spec.inaccuracy;
            Vec3 aim{t->position[0] + std::cos(ang) * r, t->position[1] + std::sin(ang) * r, 0};
            if (w.spec.pellets > 1 && w.spec.spreadRadians > 0) {
                // Rotate the aim point around the shooter within the spread cone.
                const float off = (random01() - 0.5f) * w.spec.spreadRadians;
                const float ax = aim[0] - from[0], ay = aim[1] - from[1];
                aim[0] = from[0] + ax * std::cos(off) - ay * std::sin(off);
                aim[1] = from[1] + ax * std::sin(off) + ay * std::cos(off);
            }
            aim[2] = map_.heightAt(aim[0], aim[1]) + std::max(t->radius * 0.6f, 4.f);
            Projectile pr;
            pr.owner = u.owner;
            pr.shooter = u.id;
            pr.target = t->id;
            pr.position = from;
            const float dx = aim[0] - from[0], dy = aim[1] - from[1], dz = aim[2] - from[2];
            const float len = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
            const float speed = w.spec.projectileSpeed;
            pr.velocity = {dx / len * speed, dy / len * speed, dz / len * speed};
            pr.distance = std::min(len, w.spec.maxFlight);
            pr.damage = w.spec.damage;
            pr.blastDamage = w.spec.blastDamage;
            pr.blastRange = w.spec.blastRange;
            pr.damageType = w.spec.damageType;
            pr.type = w.spec.projectile;
            projectiles_.push_back(std::move(pr));
        }
        --w.pulsesLeft;
        w.pulseTimer = w.spec.pulseDelayTicks;
        if (w.pulsesLeft == 0) w.cooldown = w.spec.rechargeTicks;
    }
}

void World::applyDamage(Unit& victim, float amount, const std::string& damageType) {
    const float mod = damage_ ? damage_->modifier(damageType, victim.armorType) : 1.f;
    victim.health -= amount * mod;
}

void World::updateProjectiles() {
    std::vector<Projectile> keep;
    keep.reserve(projectiles_.size());
    for (Projectile& p : projectiles_) {
        const float speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
        const float step = std::min(speed, p.distance - p.travelled);
        const float k = speed > 0 ? step / speed : 0;
        p.position = {p.position[0] + p.velocity[0] * k, p.position[1] + p.velocity[1] * k, p.position[2] + p.velocity[2] * k};
        p.travelled += step;
        if (p.travelled + 1e-3f < p.distance) {
            keep.push_back(std::move(p));
            continue;
        }
        // Landed. Direct hit on the intended target if it is still under the impact point,
        // otherwise on any enemy there.
        Unit* hit = nullptr;
        if (Unit* t = unit(p.target); t && t->alive() && dist2d(t->position, p.position) <= t->radius + 3) hit = t;
        if (!hit)
            for (Unit& o : units_)
                if (o.alive() && enemies(p.owner, o.owner) && dist2d(o.position, p.position) <= o.radius) { hit = &o; break; }
        if (hit && p.damage > 0) applyDamage(*hit, p.damage, p.damageType);
        if (p.blastDamage > 0 && p.blastRange > 0)
            for (Unit& o : units_)
                if (o.alive() && enemies(p.owner, o.owner) && dist2d(o.position, p.position) <= p.blastRange + o.radius)
                    applyDamage(o, p.blastDamage, p.damageType);
    }
    projectiles_.swap(keep);
}

void World::removeDead() {
    for (const Unit& u : units_)
        if (!u.alive()) {
            deaths_.push_back({u.id, u.type, u.owner, u.position, ticks_});
            if (u.structure) navDirty_ = true;
        }
    units_.erase(std::remove_if(units_.begin(), units_.end(), [](const Unit& u) { return !u.alive(); }), units_.end());
}

void World::moveUnit(Unit& u) {
    if (u.structure) return;
    if (u.path.empty()) {
        u.speed = std::max(0.f, u.speed - u.acceleration * 2);
        return;
    }
    // Skip ahead when a later waypoint is already in clear view (e.g. after being nudged by others).
    while (u.path.size() > 1 && nav_.lineClear(u.position, u.path[1])) u.path.erase(u.path.begin());

    const Vec3& wp = u.path.front();
    const float dx = wp[0] - u.position[0], dy = wp[1] - u.position[1];
    const float d = std::hypot(dx, dy);
    const bool last = u.path.size() == 1;
    if (d < std::max(u.maxSpeed * 1.5f, last ? 2.f : std::max(u.radius, nav_.spacing() * 0.75f))) {
        u.path.erase(u.path.begin());
        if (u.path.empty()) u.speed = 0;
        return;
    }
    const float want = std::atan2(dy, dx);
    const float diff = angleDiff(u.facing, want);

    // Infantry and hover units move while turning; vehicles and walkers turn
    // most of the way before driving.
    const bool turnInPlace = (u.moveClass == MoveClass::Vehicle || u.moveClass == MoveClass::Walker ||
                              u.moveClass == MoveClass::LargeWalker) && std::fabs(diff) > 50 * kDegToRad;
    const float rate = (turnInPlace || u.speed < 0.05f * u.maxSpeed) ? std::max(u.stoppedTurnRate, u.turnRate * 0.5f) : u.turnRate;
    u.facing += std::clamp(diff, -rate, rate);

    float targetSpeed = turnInPlace ? 0.f : u.maxSpeed * std::max(0.25f, std::cos(std::min(std::fabs(diff), kPi / 2)));
    if (last) targetSpeed = std::min(targetSpeed, std::max(d * 0.25f, 0.3f * u.maxSpeed)); // ease into the goal
    if (u.speed < targetSpeed) u.speed = std::min(targetSpeed, u.speed + u.acceleration);
    else u.speed = std::max(targetSpeed, u.speed - u.acceleration * 2);

    // Infantry/hover steer straight at the waypoint; others go where they face.
    const float heading = (u.moveClass == MoveClass::Infantry || u.moveClass == MoveClass::Hover) ? want : u.facing;
    const float step = std::min(u.speed, d);
    const float mx = std::cos(heading) * step, my = std::sin(heading) * step;
    Vec3 next{u.position[0] + mx, u.position[1] + my, 0};
    if (!nav_.passableAt(next[0], next[1])) {
        // Slide along the wall on whichever axis is free.
        if (nav_.passableAt(u.position[0] + mx, u.position[1])) next = {u.position[0] + mx, u.position[1], 0};
        else if (nav_.passableAt(u.position[0], u.position[1] + my)) next = {u.position[0], u.position[1] + my, 0};
        else next = u.position;
    }
    const bool progressed = std::hypot(next[0] - u.position[0], next[1] - u.position[1]) > 0.1f * u.maxSpeed;
    u.stuckTicks = progressed ? 0 : u.stuckTicks + 1;
    if (u.stuckTicks > kTicksPerSecond / 2) {
        // Wedged (crowding or a bad waypoint): plan again from where we are.
        u.path = nav_.findPath(u.position, u.goal);
        u.stuckTicks = 0;
    }
    u.position = {next[0], next[1], map_.heightAt(next[0], next[1])};
}

void World::separate() {
    // Gentle push apart so units don't stack. O(n^2) is fine at sandbox scale.
    for (size_t i = 0; i < units_.size(); ++i) {
        for (size_t j = i + 1; j < units_.size(); ++j) {
            Unit& a = units_[i];
            Unit& b = units_[j];
            if (a.structure || b.structure) continue; // the nav grid keeps units off footprints
            const float dx = b.position[0] - a.position[0], dy = b.position[1] - a.position[1];
            const float minD = (a.radius + b.radius) * 0.9f;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= minD * minD) continue;
            float d = std::sqrt(d2);
            float nx = 1, ny = 0;
            if (d > 1e-4f) { nx = dx / d; ny = dy / d; }
            else { nx = (a.id < b.id) ? 1.f : -1.f; d = 0; } // deterministic split for exact overlaps
            const float push = (minD - d) * 0.25f;
            // Units standing still yield more than moving ones.
            const float wa = a.moving() ? 0.35f : 0.65f, wb = b.moving() ? 0.35f : 0.65f;
            const float sa = wa / (wa + wb), sb = wb / (wa + wb);
            Vec3 pa{a.position[0] - nx * push * sa, a.position[1] - ny * push * sa, 0};
            Vec3 pb{b.position[0] + nx * push * sb, b.position[1] + ny * push * sb, 0};
            if (nav_.passableAt(pa[0], pa[1])) a.position = {pa[0], pa[1], map_.heightAt(pa[0], pa[1])};
            if (nav_.passableAt(pb[0], pb[1])) b.position = {pb[0], pb[1], map_.heightAt(pb[0], pb[1])};
        }
    }
}

void World::tick() {
    if (navDirty_) rebuildNav();
    for (size_t i = 0; i < units_.size(); ++i) updateConstruction(units_[i]);
    for (size_t i = 0; i < units_.size(); ++i) updateProduction(units_[i]); // may spawn (index loop on purpose)
    for (Unit& u : units_) updateTargeting(u);
    for (Unit& u : units_) moveUnit(u);
    separate();
    for (Unit& u : units_) fireWeapons(u);
    updateProjectiles();
    removeDead();
    ++ticks_;
}

std::vector<uint32_t> World::unitsNear(Vec3 p, float radius) const {
    std::vector<std::pair<float, uint32_t>> hits;
    for (const Unit& u : units_) {
        const float d = dist2d(u.position, p);
        if (d <= radius + u.radius) hits.push_back({d, u.id});
    }
    std::sort(hits.begin(), hits.end());
    std::vector<uint32_t> out;
    for (auto& h : hits) out.push_back(h.second);
    return out;
}

} // namespace alamo
