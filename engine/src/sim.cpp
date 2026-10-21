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

World::World(const Map& map, const GameDatabase& db) : map_(map), db_(db), nav_(map) {}

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
        u->goal = slots[best];
        u->path = nav_.findPath(u->position, slots[best]);
    }
}

void World::commandStop(const std::vector<uint32_t>& ids) {
    for (uint32_t id : ids)
        if (Unit* u = unit(id)) u->path.clear();
}

void World::moveUnit(Unit& u) {
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
    for (Unit& u : units_) moveUnit(u);
    separate();
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
