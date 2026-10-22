// Transports (GarrisonableBehaviorType): units ride inside another unit (an APC, a gunship, the
// Novus Flying Fortress) until they are unloaded. While inside they are off the map: not drawn,
// not targetable, not moving or shooting; they follow the transport.
#include "alamo/sim.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace alamo {
namespace {

std::vector<std::string> words(const std::string* s) {
    std::vector<std::string> out;
    if (!s) return out;
    std::string cur;
    for (char c : *s + ",") {
        if (c == ',' || c == '|' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

bool hasAttribute(const GameObjectType& t, const std::string& name) {
    const std::string* v = t.find(name);
    if (!v) return false;
    std::string s = trim(*v);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s == "true" || s == "yes" || s == "1";
}

} // namespace

const GarrisonSpec* World::garrisonOf(const GameObjectType& t) const {
    if (auto it = garrisons_.find(&t); it != garrisons_.end()) return it->second.get();
    std::unique_ptr<GarrisonSpec> spec;
    if (auto cap = t.number("Max_Garrison_Capacity"); cap && *cap > 0) {
        spec = std::make_unique<GarrisonSpec>();
        spec->capacity = int(*cap);
        spec->required = words(t.find("Garrisonable_Attributes"));
        spec->forbidden = words(t.find("Garrisonable_Anti_Attributes"));
        spec->deathDamage = float(t.number("Damage_Percent_On_Destruction").value_or(1.0));
    }
    const GarrisonSpec* out = spec.get();
    garrisons_[&t] = std::move(spec);
    return out;
}

bool World::canBoard(const Unit& rider, const Unit& transport) const {
    const GarrisonSpec* g = garrisonOf(*transport.type);
    if (!g || rider.id == transport.id || rider.structure || rider.inside || !rider.alive() || !transport.alive()) return false;
    if (rider.owner != transport.owner || transport.underConstruction || rider.flying()) return false;
    if (garrisonOf(*rider.type)) return false; // no transports inside transports
    if (int(transport.passengers.size()) >= g->capacity) return false;
    if (!g->required.empty()) {
        bool ok = false;
        for (const std::string& a : g->required) ok = ok || hasAttribute(*rider.type, a);
        if (!ok) return false;
    }
    for (const std::string& a : g->forbidden)
        if (hasAttribute(*rider.type, a)) return false;
    return true;
}

OrderResult World::commandBoard(const std::vector<uint32_t>& ids, uint32_t transportId) {
    Unit* t = unit(transportId);
    if (!t) return OrderResult::InvalidUnit;
    if (!garrisonOf(*t->type)) return OrderResult::NotAllowed;
    int sent = 0;
    for (uint32_t id : ids) {
        Unit* r = unit(id);
        if (!r || !canBoard(*r, *t)) continue;
        r->boarding = t->id;
        r->attackOrder = r->target = 0;
        r->attackMove = false;
        r->harvest = Unit::Harvest::None;
        r->goal = t->position;
        r->path = planPath(*r, t->position);
        ++sent;
    }
    return sent ? OrderResult::Ok : OrderResult::NotAllowed;
}

OrderResult World::unload(uint32_t transportId) {
    Unit* t = unit(transportId);
    if (!t) return OrderResult::InvalidUnit;
    if (t->passengers.empty()) return OrderResult::NotAllowed;
    // Out in a ring around it, on passable ground under it if it flies.
    const std::vector<uint32_t> riders = t->passengers;
    t->passengers.clear();
    const float ring = t->radius + 12;
    for (size_t k = 0; k < riders.size(); ++k) {
        Unit* r = unit(riders[k]);
        if (!r) continue;
        const float a = t->facing + 3.14159265f + 6.2831853f * float(k) / float(std::max<size_t>(riders.size(), 1));
        const float rr = ring + 10 * float(k / 12);
        Vec3 p = nav_.nearestPassable({t->position[0] + std::cos(a) * rr, t->position[1] + std::sin(a) * rr, 0});
        p[2] = map_.heightAt(p[0], p[1]);
        r->inside = 0;
        r->position = p;
        r->path.clear();
        r->speed = 0;
    }
    return OrderResult::Ok;
}

void World::updateGarrisons() {
    for (Unit& u : units_) {
        if (u.inside) {
            const Unit* t = unit(u.inside);
            if (t) u.position = t->position;
            continue;
        }
        if (!u.boarding) continue;
        Unit* t = unit(u.boarding);
        if (!t || !canBoard(u, *t)) {
            u.boarding = 0;
            continue;
        }
        const float d = std::hypot(t->position[0] - u.position[0], t->position[1] - u.position[1]);
        if (d <= t->radius + u.radius + 25) {
            u.boarding = 0;
            u.inside = t->id;
            u.path.clear();
            u.speed = 0;
            u.target = 0;
            t->passengers.push_back(u.id);
        } else if (u.path.empty() && --u.repathTicks <= 0) {
            // Follow a moving transport (re-planned once a second at most).
            u.path = planPath(u, t->position);
            u.repathTicks = kTicksPerSecond;
        }
    }
}

} // namespace alamo
