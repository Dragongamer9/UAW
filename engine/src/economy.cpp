// Resource gathering and income, driven by the same data the original uses:
//   - piles: any object with ResourceBehaviorType (Resource_Units, Resource_For_Faction)
//     placed in the map's Multiplayer group;
//   - collector drones (Novus_Collector): Drone_Capacity, Resources_Per_Grab every
//     Resources_Seconds_Per_Grab, Resource_Gather_Range, Resource_Depot_Type;
//   - depots (Novus_Input_Station): ResourceDepotBehaviorType launches and replaces
//     Resource_Depot_Active_Drones drones, one per Resource_Depot_Seconds_To_Build_Drone;
//   - Reapers (Lua behavior AlienReaperTurret): drain piles in reach at the rate of the
//     Transfer in AlienHarvestResourcesEffect (18/s), searching resource_drone_search_radius;
//   - passive structures: MasariMatterEngine (ResourcesPerTimeBlock / ResourcesTimeSeconds,
//     optional MustBeOfFaction, bank capped at Baseline_Bank_Capacity + each engine's
//     ResourcesStorage, as in MasariMatterEngine.lua) and NeutralRefinery (ResourcesPerSecond).
#include "alamo/sim.hpp"
#include "alamo/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace alamo {

namespace {

constexpr uint32_t kResourceIdBit = 0x80000000u;

float dist2(Vec3 a, Vec3 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

std::string upperStr(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitNames(const std::string* s) {
    std::vector<std::string> out;
    if (!s) return out;
    std::string cur;
    for (char c : *s + ",") {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

float num(const GameObjectType& t, const char* leaf, float fallback) {
    return float(t.number(leaf).value_or(fallback));
}

float scriptNum(const std::unordered_map<std::string, std::string>& d, const char* key, float fallback) {
    auto it = d.find(key);
    if (it == d.end()) return fallback;
    char* end = nullptr;
    const float v = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : v;
}

} // namespace

const Balance::FactionBalance& Balance::of(const std::string& faction) const {
    static const FactionBalance none;
    auto it = factions.find(upperStr(faction));
    return it == factions.end() ? none : it->second;
}

Balance Balance::defaults() {
    Balance b;
    // The Hierarchy only harvests scrap; at the retail rate it strips the map and then
    // starves, so a pile is worth ten times as much to it (same income rate, piles last 10x).
    b.factions["ALIEN"].resourceValue = 10;
    // PROVISIONAL (see docs/TODO.md): rerun the balance games before release.
    // Income multipliers tuned by AI-vs-AI tournaments (simtool tournament, 8 maps, all mods):
    // untouched, Novus won 86/96 games and the Masari 0/96.
    b.factions["NOVUS"].income = 0.4f;
    b.factions["MASARI"].income = 1.3f;
    b.factions["MILITARY"].income = 1.1f;
    b.factions["ALIEN"].income = 4.5f;
    return b;
}

Balance loadBalance(MegVfs& vfs) {
    Balance b = Balance::defaults();
    for (const auto& v : vfs.readAllLayers("Data\\XML\\UAW_Balance.xml")) {
        XmlDocument doc = parseXml(std::string(v.data.begin(), v.data.end()));
        const XmlNode* root = doc.root.child("UAW_Balance");
        if (!root) continue;
        for (const XmlNode* f : root->childrenNamed("Faction")) {
            const std::string* name = f->attr("Name");
            if (!name) continue;
            Balance::FactionBalance& fb = b.factions[upperStr(trim(*name))];
            if (const XmlNode* c = f->child("Income_Multiplier")) fb.income = float(std::atof(c->text.c_str()));
            if (const XmlNode* c = f->child("Resource_Unit_Value")) fb.resourceValue = std::max(0.01f, float(std::atof(c->text.c_str())));
        }
    }
    return b;
}

bool ResourceNode::usableBy(const std::string& faction) const {
    if (factions.empty()) return true;
    for (const auto& f : factions)
        if (iequals(f, faction)) return true;
    return false;
}

std::unordered_map<std::string, std::string> parseScriptData(std::string_view text) {
    // Tokens separated by whitespace; "a = b", "a=b", "a= b" all work. Tables ({}) are skipped.
    std::vector<std::string> tok;
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) tok.push_back(cur); cur.clear(); };
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) flush();
        else if (c == '=') { flush(); tok.push_back("="); }
        else cur += c;
    }
    flush();
    std::unordered_map<std::string, std::string> out;
    for (size_t i = 0; i + 2 < tok.size(); ++i) {
        if (tok[i + 1] != "=" || tok[i] == "=") continue;
        std::string v = tok[i + 2];
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
        if (!v.empty() && v.back() == ',') v.pop_back();
        out[upperStr(tok[i])] = v;
        i += 2;
    }
    return out;
}

const EconomySpec& World::economy(const GameObjectType& t) const {
    auto it = economyCache_.find(&t);
    if (it != economyCache_.end()) return it->second;
    EconomySpec e;
    const auto behaviors = splitNames(t.find("Lua_Land_Behaviors"));
    const std::string* scriptText = t.find("Lua_Script_Data");
    const auto script = parseScriptData(scriptText ? *scriptText : std::string());
    auto hasBehavior = [&](const char* name) {
        for (const auto& b : behaviors)
            if (iequals(b, name)) return true;
        return false;
    };

    // Collector drones.
    const std::string* collector = t.find("Is_Resource_Collector");
    const std::string* depotType = t.find("Resource_Depot_Type");
    if (collector && iequals(trim(*collector), "true") && depotType && !hasBehavior("AlienReaperTurret")) {
        e.collector = true;
        e.depotType = trim(*depotType);
        e.perGrab = num(t, "Resources_Per_Grab", 100);
        e.capacity = num(t, "Drone_Capacity", e.perGrab * 2);
        e.grabTicks = std::max(1, int(std::lround(num(t, "Resources_Seconds_Per_Grab", 1) * kTicksPerSecond)));
        e.gatherRange = num(t, "Resource_Gather_Range", 80);
    }

    // Reapers.
    if (hasBehavior("AlienReaperTurret")) {
        e.reaper = true;
        e.reaperSearch = scriptNum(script, "RESOURCE_DRONE_SEARCH_RADIUS", 200);
        e.reaperReach = scriptNum(script, "RESOURCE_DRONE_HARVEST_DISTANCE", 100);
        e.reaperRate = 18; // AlienHarvestResourcesEffect in the retail data
        if (effects_) {
            // The harvest ability's generator names the effect; its Transfer gives the rate.
            for (const auto& f : t.fields()) {
                const std::string up = upperStr(f.path);
                if (up.find("EFFECT_GENERATOR[") == std::string::npos || up.find("HARVEST") == std::string::npos ||
                    up.find("INSTANT") != std::string::npos || up.size() < 12 || up.substr(up.size() - 12) != ".EFFECT_NAME")
                    continue;
                if (const GameObjectType* fx = effects_->find(trim(f.values.back())))
                    if (auto r = fx->number("Amount_Per_Second"); r && *r != 0) {
                        e.reaperRate = float(std::fabs(*r));
                        break;
                    }
            }
        }
    }

    // Depots.
    if (auto drones = t.number("Resource_Depot_Active_Drones"); drones && *drones > 0) {
        e.drones = int(*drones);
        e.droneBuildTicks = std::max(1, int(std::lround(num(t, "Resource_Depot_Seconds_To_Build_Drone", 3) * kTicksPerSecond)));
        e.collectionRadius = num(t, "Resource_Depot_Collection_Radius", 10000);
        for (const auto& o : db_.objects()) {
            const std::string* d = o.find("Resource_Depot_Type");
            const std::string* c = o.find("Is_Resource_Collector");
            if (d && c && iequals(trim(*d), t.name) && iequals(trim(*c), "true")) {
                e.droneType = &o;
                break;
            }
        }
        if (!e.droneType) e.drones = 0; // e.g. a mod depot whose drones are scripted
    }

    // Passive income.
    if (hasBehavior("MasariMatterEngine")) {
        const float block = scriptNum(script, "MATTERENGINE.RESOURCESPERTIMEBLOCK", 1);
        const float secs = std::max(0.01f, scriptNum(script, "MATTERENGINE.RESOURCESTIMESECONDS", 1));
        e.matterEngine = true;
        e.passivePerSecond = block / secs;
        e.storage = scriptNum(script, "MATTERENGINE.RESOURCESSTORAGE", 100);
        if (auto it2 = script.find("MATTERENGINE.MUSTBEOFFACTION"); it2 != script.end()) e.passiveFaction = it2->second;
    }
    if (hasBehavior("NeutralRefinery")) {
        // Refineries pay whoever owns them, whatever faction.
        const float r = scriptNum(script, "RESOURCESPERSECOND", 0);
        if (r > 0) {
            e.matterEngine = false;
            e.passiveFaction.clear();
            e.passivePerSecond = r;
        }
    }
    // Power network role.
    if (const std::string* role = t.find("Role"); role && (t.find("Radius_Attribute") || iequals(trim(*role), "Self_Powered"))) {
        const std::string r = trim(*role);
        using P = EconomySpec::Power;
        e.power = iequals(r, "Generator") ? P::Generator : iequals(r, "Transmitter") ? P::Transmitter
                : iequals(r, "Receiver") ? P::Receiver : iequals(r, "Self_Powered") ? P::SelfPowered : P::None;
        // The radii are attributes named by Radius_Attribute / Transmit_Radius_Attribute.
        auto attr = [&](const char* which, const char* fallbackName) {
            const std::string* name = t.find(which);
            const std::string n = name ? trim(*name) : fallbackName;
            return float(t.number(n).value_or(0));
        };
        e.powerRadius = attr("Radius_Attribute", "Novus_Power_Powerup_Radius");
        e.transmitRadius = attr("Transmit_Radius_Attribute", "Novus_Power_Transmission_Radius");
    }
    return economyCache_.emplace(&t, e).first->second;
}

void World::loadMapResources() {
    const int mp = map_.groupIndex("Multiplayer");
    for (const MapObject& o : map_.objects) {
        if (mp >= 0 && !o.groups.empty() && std::find(o.groups.begin(), o.groups.end(), uint32_t(mp)) == o.groups.end())
            continue;
        const GameObjectType* t = db_.findByCrc(o.typeCrc);
        if (!t) continue;
        const auto units = t->number("Resource_Units");
        if (!units || *units <= 0) continue;
        ResourceNode n;
        n.id = kResourceIdBit | uint32_t(resources_.size() + 1);
        n.type = t;
        n.position = {o.position[0], o.position[1], map_.heightAt(o.position[0], o.position[1])};
        n.radius = std::clamp(float(t->number("Custom_Hard_XExtent").value_or(t->number("Mouse_Collide_Override_Sphere_Radius").value_or(15))) * o.scale, 5.f, 120.f);
        n.remaining = n.initial = float(*units);
        n.factions = splitNames(t->find("Resource_For_Faction"));
        resources_.push_back(std::move(n));
    }
}

uint32_t World::addResource(const std::string& typeName, Vec3 position) {
    const GameObjectType* t = db_.find(typeName);
    if (!t) return 0;
    ResourceNode n;
    n.id = kResourceIdBit | uint32_t(resources_.size() + 1);
    n.type = t;
    n.position = {position[0], position[1], map_.heightAt(position[0], position[1])};
    n.radius = std::clamp(float(t->number("Custom_Hard_XExtent").value_or(15)), 5.f, 120.f);
    n.remaining = n.initial = float(t->number("Resource_Units").value_or(1000));
    n.factions = splitNames(t->find("Resource_For_Faction"));
    resources_.push_back(std::move(n));
    return resources_.back().id;
}

ResourceNode* World::resourceMut(uint32_t id) {
    if (!(id & kResourceIdBit)) return nullptr;
    const size_t i = (id & ~kResourceIdBit) - 1;
    return i < resources_.size() ? &resources_[i] : nullptr;
}

const ResourceNode* World::resource(uint32_t id) const {
    return const_cast<World*>(this)->resourceMut(id);
}

float World::bankCap(int owner) const {
    if (owner < 0 || owner >= int(players_.size()) || players_[size_t(owner)].baselineBank <= 0) return 0;
    float cap = players_[size_t(owner)].baselineBank;
    for (const Unit& u : units_)
        if (u.owner == owner && !u.underConstruction) {
            const EconomySpec& e = economy(*u.type);
            if (e.matterEngine) cap += e.storage;
        }
    return cap;
}

float World::incomePerSecond(int owner) const {
    if (owner < 0 || owner >= int(players_.size())) return 0;
    float rate = 0;
    for (const Unit& u : units_) {
        if (u.owner != owner || u.underConstruction) continue;
        const EconomySpec& e = economy(*u.type);
        if (e.passivePerSecond > 0 && (e.passiveFaction.empty() || iequals(e.passiveFaction, players_[size_t(owner)].faction)))
            rate += e.passivePerSecond;
    }
    return rate;
}

void World::deposit(int owner, float amount) {
    if (owner < 0 || owner >= int(players_.size()) || amount <= 0) return;
    Player& p = players_[size_t(owner)];
    amount *= balance_.of(p.faction).income;
    const float cap = bankCap(owner);
    if (cap > 0) amount = std::min(amount, std::max(0.f, cap - p.credits));
    p.credits += amount;
    p.earned += amount;
}

std::vector<Vec3> World::planPath(const Unit& u, Vec3 to) const {
    if (u.flying()) return {to}; // aircraft fly straight over everything
    return nav_.findPath(u.position, to);
}

void World::updatePower() {
    // Flood the network from generators: transmitters join when in transmit range of a
    // powered node, then every receiver within a powered node's power radius is on.
    using P = EconomySpec::Power;
    std::vector<Unit*> nodes, receivers;
    for (Unit& u : units_) {
        const P role = economy(*u.type).power;
        if (role == P::None) { u.powered = true; continue; }
        u.powered = false;
        if (u.underConstruction) continue;
        if (role == P::SelfPowered) u.powered = true;
        if (role == P::Generator || role == P::Transmitter) nodes.push_back(&u);
        else receivers.push_back(&u);
    }
    std::vector<Unit*> frontier;
    for (Unit* n : nodes)
        if (economy(*n->type).power == P::Generator) { n->powered = true; frontier.push_back(n); }
    while (!frontier.empty()) {
        Unit* a = frontier.back();
        frontier.pop_back();
        const float reach = economy(*a->type).transmitRadius;
        for (Unit* b : nodes)
            if (!b->powered && b->owner == a->owner && dist2(a->position, b->position) <= std::max(reach, economy(*b->type).transmitRadius)) {
                b->powered = true;
                frontier.push_back(b);
            }
    }
    for (Unit* r : receivers) {
        if (r->powered) continue;
        for (Unit* n : nodes)
            if (n->powered && n->owner == r->owner &&
                dist2(n->position, r->position) <= economy(*n->type).powerRadius + std::max(r->footprintX, r->footprintY)) {
                r->powered = true;
                break;
            }
    }
}

void World::updateEconomy() {
    if (ticks_ % 10 == 0) updatePower();
    // Passive income, capped (Masari) once per tick.
    for (size_t p = 0; p < players_.size(); ++p) {
        const float rate = incomePerSecond(int(p));
        if (rate > 0) deposit(int(p), rate / kTicksPerSecond);
    }
    // Depots and gatherers. Index loops: depots may spawn drones (reallocating units_).
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].underConstruction || !units_[i].alive()) continue;
        const EconomySpec& e = economy(*units_[i].type);
        if (e.drones > 0) updateDepot(units_[i]);
    }
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (u.underConstruction || !u.alive()) continue;
        const EconomySpec& e = economy(*u.type);
        if (e.collector) updateCollector(u, e);
        else if (e.reaper) updateReaper(u, e);
    }
}

void World::updateDepot(Unit& d) {
    const EconomySpec& e = economy(*d.type);
    if (!d.powered) return; // NovusInputStation.lua: drones launch only while powered
    int active = 0;
    for (const Unit& u : units_)
        if (u.depot == d.id && u.alive()) ++active;
    if (active >= e.drones) { d.droneTimer = 0; return; }
    if (++d.droneTimer < e.droneBuildTicks) return;
    d.droneTimer = 0;
    const uint32_t did = d.id;
    const int owner = d.owner;
    const uint32_t id = spawn(e.droneType->name, owner, d.position, 0); // may reallocate units_
    Unit* drone = unit(id);
    Unit* depot = unit(did);
    if (!drone || !depot) return;
    const Vec3 out = exitPoint(*depot, *drone);
    drone->position = {out[0], out[1], map_.heightAt(out[0], out[1])};
    drone->depot = did;
}

void World::updateCollector(Unit& u, const EconomySpec& e) {
    Unit* depot = u.depot ? unit(u.depot) : nullptr;
    if (!depot || depot->owner != u.owner) {
        // Home lost: adopt the nearest depot of the right type.
        depot = nullptr;
        float best = 1e30f;
        for (Unit& d : units_)
            if (d.owner == u.owner && !d.underConstruction && iequals(d.type->name, e.depotType)) {
                const float dd = dist2(d.position, u.position);
                if (dd < best) { best = dd; depot = &d; }
            }
        u.depot = depot ? depot->id : 0;
    }
    if (u.harvest == Unit::Harvest::None && !u.path.empty()) return; // under player orders
    const std::string& faction = players_[size_t(u.owner)].faction;
    const float reach = e.gatherRange;

    switch (u.harvest) {
    case Unit::Harvest::None: {
        if (u.carried >= e.capacity) { u.harvest = Unit::Harvest::Returning; break; }
        const float radius = depot ? economy(*depot->type).collectionRadius : 0;
        const Vec3 home = depot ? depot->position : u.position;
        ResourceNode* best = nullptr;
        float bestD = 1e30f;
        for (ResourceNode& n : resources_) {
            if (n.remaining <= 0 || !n.usableBy(faction)) continue;
            if (depot && dist2(n.position, home) > radius) continue;
            const float d = dist2(n.position, u.position) + 0.5f * dist2(n.position, home);
            if (d < bestD) { bestD = d; best = &n; }
        }
        if (!best) {
            if (u.carried > 0) u.harvest = Unit::Harvest::Returning;
            break;
        }
        u.harvestNode = best->id;
        u.harvest = Unit::Harvest::ToNode;
        u.goal = best->position;
        u.path = planPath(u, best->position);
        break;
    }
    case Unit::Harvest::ToNode: {
        ResourceNode* n = resourceMut(u.harvestNode);
        if (!n || n->remaining <= 0) { u.harvest = Unit::Harvest::None; u.path.clear(); break; }
        if (dist2(u.position, n->position) <= reach + n->radius) {
            u.path.clear();
            u.harvest = Unit::Harvest::Gathering;
            u.harvestTimer = 0;
        } else if (u.path.empty()) {
            u.path = planPath(u, n->position);
        }
        break;
    }
    case Unit::Harvest::Gathering: {
        ResourceNode* n = resourceMut(u.harvestNode);
        if (!n || n->remaining <= 0) {
            u.harvest = u.carried > 0 ? Unit::Harvest::Returning : Unit::Harvest::None;
            break;
        }
        if (++u.harvestTimer < e.grabTicks) break;
        u.harvestTimer = 0;
        const float take = std::min({e.perGrab, n->remaining, e.capacity - u.carried});
        n->remaining -= take;
        u.carried += take;
        if (u.carried >= e.capacity - 0.01f || n->remaining <= 0) u.harvest = Unit::Harvest::Returning;
        break;
    }
    case Unit::Harvest::Returning: {
        if (!depot) { u.path.clear(); break; } // wait for a new depot
        const float dropRange = std::max(depot->footprintX, depot->footprintY) + u.radius + 30;
        if (dist2(u.position, depot->position) <= dropRange) {
            deposit(u.owner, u.carried * balance_.of(players_[size_t(u.owner)].faction).resourceValue);
            u.carried = 0;
            u.path.clear();
            u.harvest = Unit::Harvest::None;
        } else if (u.path.empty()) {
            u.path = planPath(u, depot->position);
        }
        break;
    }
    }
}

void World::updateReaper(Unit& u, const EconomySpec& e) {
    if (u.harvest == Unit::Harvest::None && (!u.path.empty() || u.attackOrder)) return; // under player orders
    const std::string& faction = players_[size_t(u.owner)].faction;
    ResourceNode* n = resourceMut(u.harvestNode);
    if (!n || n->remaining <= 0 || !n->usableBy(faction)) {
        // Look for the nearest pile within the search radius.
        n = nullptr;
        float best = 1e30f;
        for (ResourceNode& r : resources_) {
            if (r.remaining <= 0 || !r.usableBy(faction)) continue;
            const float d = dist2(r.position, u.position) - r.radius;
            if (d <= e.reaperSearch && d < best) { best = d; n = &r; }
        }
        if (!n) { u.harvest = Unit::Harvest::None; u.harvestNode = 0; return; }
        u.harvestNode = n->id;
    }
    if (dist2(u.position, n->position) - n->radius > e.reaperReach) {
        if (u.harvest != Unit::Harvest::ToNode || u.path.empty()) {
            u.harvest = Unit::Harvest::ToNode;
            u.goal = n->position;
            u.path = planPath(u, n->position);
        }
        return;
    }
    u.path.clear();
    u.harvest = Unit::Harvest::Gathering;
    // Face the pile while draining it.
    const float want = std::atan2(n->position[1] - u.position[1], n->position[0] - u.position[0]);
    u.facing += std::clamp(angleDiff(u.facing, want), -u.stoppedTurnRate, u.stoppedTurnRate);
    // Earn at the data rate; the pile shrinks by credits / Resource_Unit_Value.
    const float value = balance_.of(faction).resourceValue;
    const float take = std::min(n->remaining, e.reaperRate / kTicksPerSecond / value);
    n->remaining -= take;
    deposit(u.owner, take * value);
}

} // namespace alamo
