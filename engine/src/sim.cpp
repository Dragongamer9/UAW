#include "alamo/sim.hpp"

#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
      rng_(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {
    loadMapResources();
}

const char* toString(OrderResult r) {
    switch (r) {
    case OrderResult::Ok: return "ok";
    case OrderResult::NotAllowed: return "not available";
    case OrderResult::NotEnoughCredits: return "not enough credits";
    case OrderResult::PopulationCap: return "population cap reached";
    case OrderResult::BlockedSite: return "site blocked";
    case OrderResult::InvalidUnit: return "invalid unit";
    case OrderResult::BuildLimit: return "build limit reached";
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

int World::countOwned(int owner, const GameObjectType& type) const {
    int n = 0;
    for (const Unit& u : units_) {
        if (u.owner != owner) continue;
        if (u.type == &type) ++n;
        for (const GameObjectType* q : u.queue)
            if (q == &type) ++n;
    }
    return n;
}

namespace {
bool atBuildLimit(const World& w, int owner, const GameObjectType& t) {
    const auto limit = t.number("Build_Limit_Current_Per_Player");
    return limit && *limit > 0 && w.countOwned(owner, t) >= int(*limit);
}
} // namespace

int World::addPlayer(const std::string& faction, float credits, int popCap) {
    Player p;
    p.id = int(players_.size());
    p.faction = faction;
    p.credits = credits;
    p.popCap = popCap;
    if (const Faction* f = db_.faction(faction)) p.baselineBank = f->baselineBankCapacity;
    players_.push_back(std::move(p));
    if (const ResearchTree* t = researchTree(players_.back().id)) players_.back().locks = ResearchLocks::initial(*t);
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

bool World::isBaseBuilder(const GameObjectType& t) const {
    const std::string* f = t.find("Is_Tactical_Base_Builder");
    return f && (iequals(trim(*f), "yes") || iequals(trim(*f), "true"));
}

bool World::anyBaseBuilder() const {
    if (anyBaseBuilder_ < 0) {
        anyBaseBuilder_ = 0;
        for (const auto& o : db_.objects())
            if (isBaseBuilder(o)) { anyBaseBuilder_ = 1; break; }
    }
    return anyBaseBuilder_ == 1;
}

std::vector<const GameObjectType*> squadMembers(const GameObjectType& t, const GameDatabase& db) {
    std::vector<const GameObjectType*> out;
    const std::string* company = t.find("Company_Units");
    if (!company) return out;
    std::string cur;
    for (char c : *company + ",") {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (const GameObjectType* m = cur.empty() ? nullptr : db.find(cur); m && !m->find("Company_Units")) out.push_back(m);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

bool isStructureType(const GameObjectType& t) {
    const std::string* mc = t.find("MovementClass");
    return (mc && iequals(trim(*mc), "Building")) || t.category.find("Structure") != std::string::npos;
}

const std::vector<const GameObjectType*>& World::socketsOf(const GameObjectType& type) const {
    auto it = sockets_.find(&type);
    if (it != sockets_.end()) return it->second;
    // Walk the hardpoint tree (walker -> crown/legs -> sockets); a socket is a hardpoint that
    // some upgrade names as its Tactical_Builder.
    std::vector<const GameObjectType*> out;
    std::vector<std::pair<const GameObjectType*, int>> todo{{&type, 0}};
    while (!todo.empty()) {
        auto [t, depth] = todo.back();
        todo.pop_back();
        const std::string* hps = t->find("Attached_Hard_Points");
        if (!hps || depth > 4) continue;
        std::vector<std::string> items;
        std::string cur;
        for (char c : *hps + ",") {
            if (c == ',' || c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                if (!cur.empty()) items.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        for (size_t i = 0; i < items.size(); i += 2) {
            const GameObjectType* hp = db_.find(items[i]);
            if (!hp) continue;
            // Socket instances are often variants of the socket the tech tree names
            // (Alien_Walker_Habitat_BACK_HP00 of ..._Body_Socket): count the named one.
            for (const GameObjectType* v = hp; v && tech_; v = v->variantOf.empty() ? nullptr : db_.find(v->variantOf))
                if (!tech_->producibleBy(v->name, [](const std::string&) { return true; }).empty()) {
                    out.push_back(v);
                    break;
                }
            todo.push_back({hp, depth + 1});
        }
    }
    return sockets_.emplace(&type, std::move(out)).first->second;
}

const GameObjectType* World::socketFor(const Unit& u, const GameObjectType& upgrade) const {
    // A free socket of `u` that can take the upgrade.
    if (!tech_) return nullptr;
    for (const GameObjectType* sock : socketsOf(*u.type)) {
        bool fits = false;
        for (const BuildOption& o : tech_->options(upgrade.name))
            for (const std::string& b : o.builders) fits = fits || iequals(b, sock->name);
        if (!fits) continue;
        const auto total = std::count(socketsOf(*u.type).begin(), socketsOf(*u.type).end(), sock);
        const auto used = std::count(u.socketsUsed.begin(), u.socketsUsed.end(), sock);
        if (used < total) return sock;
    }
    return nullptr;
}

bool World::allowedByOptions(const GameObjectType& t) const {
    if (superweaponsOn_) return true;
    if (superweaponOf(t)) return false;
    if (const std::string* into = t.find("Tactical_Buildable_Constructed")) // a construction site of one
        if (const GameObjectType* done = db_.find(trim(*into)); done && superweaponOf(*done)) return false;
    return true;
}

std::vector<std::string> World::producible(uint32_t id) const {
    const Unit* s = nullptr;
    for (const Unit& u : units_)
        if (u.id == id) s = &u;
    if (!s || !tech_ || s->underConstruction) return {};
    const int owner = s->owner;
    auto out = tech_->producibleBy(s->type->name, [&](const std::string& t) { return owns(owner, t); });
    // Hardpoints bought into the unit's own sockets (walker legs, crowns, bodies).
    for (const GameObjectType* sock : socketsOf(*s->type))
        for (const std::string& n : tech_->producibleBy(sock->name, [&](const std::string& t) { return owns(owner, t); })) {
            const GameObjectType* t = db_.find(n);
            if (!t || std::find(out.begin(), out.end(), n) != out.end()) continue;
            if (socketFor(*s, *t)) out.push_back(n); // one per free socket (each leg takes its own)
        }
    if (!s->structure) {
        // A mobile builder listed as Tactical_Builder of a structure constructs it on site
        // (mods such as Human Resurgence write it that way); that's constructible(), not production.
        std::erase_if(out, [&](const std::string& n) {
            const GameObjectType* t = db_.find(n);
            return t && isStructureType(*t);
        });
    }
    std::erase_if(out, [&](const std::string& n) { // still to be researched, or ruled out by the game's options
        const GameObjectType* t = db_.find(n);
        return t && (typeLocked(owner, *t) || !allowedByOptions(*t));
    });
    return out;
}

namespace {
// "Alien, Alien_ZM06_KamalRex" lists Alien.
bool listsFaction(const std::string& affiliation, const std::string& faction) {
    size_t start = 0;
    while (start <= affiliation.size()) {
        size_t end = affiliation.find(',', start);
        if (end == std::string::npos) end = affiliation.size();
        if (iequals(trim(std::string_view(affiliation).substr(start, end - start)), faction)) return true;
        start = end + 1;
    }
    return false;
}
} // namespace

std::vector<std::string> World::constructible(uint32_t id) const {
    const Unit* b = nullptr;
    for (const Unit& u : units_)
        if (u.id == id) b = &u;
    if (!b || !tech_ || b->structure) return {}; // structures only appear as prerequisites
    // Only real base builders construct (Is_Tactical_Base_Builder). The flag tells them apart
    // from units that are merely required, e.g. the Reaper listed for Hierarchy walkers.
    if (!isBaseBuilder(*b->type) && anyBaseBuilder()) return {};
    const int owner = b->owner;
    const auto has = [&](const std::string& t) { return owns(owner, t); };
    auto out = tech_->constructibleBy(b->type->name, has);
    for (const std::string& n : tech_->producibleBy(b->type->name, has)) {
        const GameObjectType* t = db_.find(n);
        if (t && isStructureType(*t) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    // Builder-less objects (Hierarchy walkers: Tactical_Builder null, only a core required; the
    // Remix missile silo) are placed by the faction's base builders.
    if (!fieldBuiltReady_) {
        fieldBuiltReady_ = true;
        for (const std::string& obj : tech_->objects()) {
            const GameObjectType* t = db_.find(obj);
            if (!t) continue;
            for (const BuildOption& o : tech_->options(obj)) {
                if (!o.builders.empty()) continue;
                bool namesBuilder = false;
                for (const std::string& r : o.required)
                    if (const GameObjectType* rt = db_.find(r); rt && isBaseBuilder(*rt)) namesBuilder = true;
                if (!namesBuilder) fieldBuilt_.push_back({t, o.required});
            }
        }
    }
    if (const Player* p = const_cast<World*>(this)->player(owner)) {
        for (const auto& [t, required] : fieldBuilt_) {
            const std::string* aff = t->find("Affiliation");
            if (!aff || !listsFaction(*aff, p->faction)) continue;
            if (std::find(out.begin(), out.end(), t->name) != out.end()) continue;
            bool ok = true;
            for (const std::string& r : required) ok = ok && has(r);
            if (ok) out.push_back(t->name);
        }
    }
    std::erase_if(out, [&](const std::string& n) {
        const GameObjectType* t = db_.find(n);
        return t && (typeLocked(owner, *t) || !allowedByOptions(*t));
    });
    return out;
}

OrderResult World::queueProduction(uint32_t id, const std::string& typeName) {
    RecordScope rec_(*this, Command{0, Command::Kind::Queue, {id}, 0, -1, 0, {}, typeName});
    if (rec_.captured) return OrderResult::Ok; // sent over the network; it runs when its turn comes
    if (const Unit* q = unit(id); q && q->sellTicks >= 0) return OrderResult::NotAllowed; // being sold
    Unit* s = unit(id);
    if (!s) return OrderResult::InvalidUnit;
    const GameObjectType* t = db_.find(typeName);
    if (!t) return OrderResult::InvalidUnit;
    bool allowed = false;
    for (const auto& n : producible(id)) allowed = allowed || iequals(n, typeName);
    if (!allowed) return OrderResult::NotAllowed;
    const BuildInfo info = buildInfo(*t, db_);
    const GameObjectType* socket = info.spawnsUnit ? nullptr : socketFor(*s, *t);
    if (!info.spawnsUnit && !socket) {
        // Upgrades: once per player (and not while one is queued). Socket hardpoints go per unit
        // (producible() already checked there is a free socket).
        if (owns(s->owner, t->name)) return OrderResult::NotAllowed;
        for (const GameObjectType* q : s->queue)
            if (q == t) return OrderResult::NotAllowed;
    }
    Player* p = player(s->owner);
    if (atBuildLimit(*this, s->owner, *t)) return OrderResult::BuildLimit;
    if (p && p->credits < info.cost) return OrderResult::NotEnoughCredits;
    if (p && info.spawnsUnit && popUsed(s->owner) + int(t->number("Unit_Pop_Cap").value_or(0)) > p->popCap)
        return OrderResult::PopulationCap;
    if (p) p->credits -= info.cost;
    if (s->queue.empty()) s->queueTicks = 0;
    s->queue.push_back(t);
    if (socket) s->socketsUsed.push_back(socket);
    return OrderResult::Ok;
}

void World::cancelProduction(uint32_t id) {
    RecordScope rec_(*this, Command{0, Command::Kind::CancelQueue, {id}});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    Unit* s = unit(id);
    if (!s || s->queue.empty()) return;
    if (Player* p = player(s->owner)) p->credits += buildInfo(*s->queue.back(), db_).cost;
    s->queue.pop_back();
    if (s->queue.empty()) s->queueTicks = 0;
}

void World::setRally(uint32_t id, Vec3 p) {
    RecordScope rec_(*this, Command{0, Command::Kind::Rally, {id}, 0, -1, 0, p});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
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
    for (const Unit& u : units_) {
        if (u.structure && std::fabs(u.position[0] - site[0]) < fx + u.footprintX &&
            std::fabs(u.position[1] - site[1]) < fy + u.footprintY)
            return false;
        // Don't bury units being built (walkers, Reapers) or big ones standing there.
        if (!u.structure && !u.flying() && (u.underConstruction || u.radius >= 15) &&
            std::fabs(u.position[0] - site[0]) < fx + u.radius && std::fabs(u.position[1] - site[1]) < fy + u.radius)
            return false;
    }
    return true;
}

OrderResult World::commandConstruct(uint32_t builderId, const std::string& typeName, Vec3 site) {
    RecordScope rec_(*this, Command{0, Command::Kind::Construct, {builderId}, 0, -1, 0, site, typeName});
    if (rec_.captured) return OrderResult::Ok; // sent over the network; it runs when its turn comes
    Unit* b = unit(builderId);
    if (!b) return OrderResult::InvalidUnit;
    const GameObjectType* t = db_.find(typeName);
    if (!t) return OrderResult::InvalidUnit;
    bool allowed = false;
    for (const auto& n : constructible(builderId)) allowed = allowed || iequals(n, typeName);
    if (!allowed) return OrderResult::NotAllowed;
    if (!siteClear(*t, site)) return OrderResult::BlockedSite;
    if (atBuildLimit(*this, b->owner, *t)) return OrderResult::BuildLimit;
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
    const float approach = std::max(s->footprintX, s->radius) + b->radius + 10;
    b->path = planPath(*b, nav_.nearestPassable({site[0] - approach, site[1], 0}));
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

const BuildInfo& World::cachedBuildInfo(const GameObjectType& t) const {
    auto it = buildCache_.find(&t);
    if (it == buildCache_.end()) it = buildCache_.emplace(&t, buildInfo(t, db_)).first;
    return it->second;
}

void World::updateProduction(Unit& s) {
    if (s.queue.empty() || s.underConstruction || !s.powered) return; // unpowered Novus structures idle
    const GameObjectType* t = s.queue.front();
    const BuildInfo info = cachedBuildInfo(*t);
    if (++s.queueTicks < info.ticks) return;
    s.queue.erase(s.queue.begin());
    s.queueTicks = 0;
    if (!info.spawnsUnit) {
        s.upgrades.push_back(t);
        // A hardpoint brings its weapons (walker turrets, structure turrets).
        if (std::find(socketsOf(*s.type).begin(), socketsOf(*s.type).end(), t) == socketsOf(*s.type).end())
            for (const WeaponSpec& w : weaponsFor(*t, db_)) s.weapons.push_back({w, 0, 0, 0});
        return;
    }
    const uint32_t sid = s.id;
    const int owner = s.owner;
    const float facing = 0;
    spawn(t->name, owner, s.position, facing); // may reallocate units_
    const std::vector<uint32_t> ids = lastSpawned_; // a squad brings several
    if (Player* p = player(owner)) p->unitsBuilt += int(ids.size());
    Unit* st = unit(sid);
    if (!st || ids.empty()) return;
    for (size_t i = 0; i < ids.size(); ++i) {
        Unit* nu = unit(ids[i]);
        if (!nu) continue;
        Vec3 out = exitPoint(*st, *nu);
        if (i > 0) out = nav_.nearestPassable({out[0] + float(i % 3) * 9 - 9, out[1] + float(i / 3) * 9, 0});
        nu->position = {out[0], out[1], map_.heightAt(out[0], out[1])};
        nu->facing = std::atan2(out[1] - st->position[1], out[0] - st->position[0]);
    }
    if (st->hasRally) commandMove(ids, st->rally);
}

void World::updateConstruction(Unit& s) {
    if (!s.underConstruction) return;
    // Builders that stopped short (path blocked, pushed aside) head for the site again.
    if (ticks_ % 15 == 0)
        for (Unit& b : units_)
            if (b.constructing == s.id && b.path.empty() &&
                dist2d(b.position, s.position) > std::max({s.footprintX, s.footprintY, s.radius}) + b.radius + 40) {
                const float approach = std::max({s.footprintX, s.footprintY, s.radius}) + b.radius + 10;
                const float a = std::atan2(b.position[1] - s.position[1], b.position[0] - s.position[0]);
                b.path = planPath(b, nav_.nearestPassable({s.position[0] + std::cos(a) * approach, s.position[1] + std::sin(a) * approach, 0}));
                if (b.path.empty()) b.constructing = 0; // unreachable: give up so the AI/player can reassign
            }
    // Progress only while one of the owner's builders assigned to it is close.
    bool working = s.buildProgress * float(s.buildTicks) >= float(s.builderTicks); // past the builder phase
    for (const Unit& b : units_)
        if (b.constructing == s.id && b.owner == s.owner &&
            dist2d(b.position, s.position) <= std::max({s.footprintX, s.footprintY, s.radius}) + b.radius + 40)
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
        if (Player* p = player(s.owner)) ++p->structuresBuilt;
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
    for (const auto& w : weapons) r = std::max(r, weaponRange(w));
    return r;
}

Unit World::makeUnit(const GameObjectType* t, int owner, Vec3 position, float facing) {
    Unit u;
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
    u.structure = isStructureType(*t);
    if (u.structure) {
        u.footprintX = float(t->number("Custom_Hard_XExtent").value_or(20));
        u.footprintY = float(t->number("Custom_Hard_YExtent").value_or(u.footprintX));
        u.maxSpeed = 0;
        u.radius = std::max(u.footprintX, u.footprintY);
        u.position = {position[0], position[1], map_.heightAt(position[0], position[1])}; // no snapping to open ground
    }
    for (const WeaponSpec& w : weaponsFor(*t, db_)) u.weapons.push_back({w, 0, 0, 0});
    if (const std::string* hps = t->find("Attached_Hard_Points")) {
        // "HP_Type, bone" pairs.
        std::vector<std::string> items;
        std::string cur;
        for (char c : *hps + ",") {
            if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (!cur.empty()) items.push_back(cur); cur.clear(); }
            else cur += c;
        }
        for (size_t i = 0; i < items.size(); i += 2) {
            const GameObjectType* ht = db_.find(items[i]);
            if (!ht) continue;
            const float hpHealth = float(ht->number("Tactical_Health").value_or(0));
            if (hpHealth <= 0) continue;
            Unit::HardPoint hp;
            hp.type = ht;
            hp.health = hp.maxHealth = hpHealth;
            if (const std::string* a = ht->find("Armor_Type")) hp.armorType = trim(*a);
            const std::string* cat = ht->find("CategoryMask");
            hp.leg = cat && cat->find("LegSocketHP") != std::string::npos;
            const std::string* dp = ht->find("HP_Single_Destruction_Destroys_Parent");
            hp.destroysParent = dp && (iequals(trim(*dp), "yes") || iequals(trim(*dp), "true"));
            u.hardpoints.push_back(std::move(hp));
        }
        if (u.maxHealth >= 99999 && !u.hardpoints.empty()) {
            u.shieldedByHardpoints = true;
            float total = 0;
            bool anyCore = false;
            for (const auto& hp : u.hardpoints) anyCore = anyCore || hp.destroysParent;
            for (const auto& hp : u.hardpoints)
                if (hp.leg || hp.destroysParent || !anyCore) total += hp.maxHealth;
            u.maxHealth = u.health = std::max(total, 1.f);
        }
    }
    initDefenses(u);
    initAbilities(u);
    return u;
}

uint32_t World::spawn(const std::string& typeName, int owner, Vec3 position, float facing) {
    const GameObjectType* t = db_.find(typeName);
    if (!t) return 0;
    lastSpawned_.clear();
    // Squads (Company_Units, e.g. Military_Team_Marines = 5 Marines) arrive as their members.
    if (const auto members = squadMembers(*t, db_); !members.empty()) {
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < members.size(); ++i) {
            const float a = float(i) * 2.3999632f, r = i == 0 ? 0.f : 10.f + 4.f * std::sqrt(float(i));
            const uint32_t id = spawn(members[i]->name, owner, {position[0] + std::cos(a) * r, position[1] + std::sin(a) * r, 0}, facing);
            if (id) ids.push_back(id);
        }
        lastSpawned_ = ids;
        return ids.empty() ? 0 : ids.front();
    }
    // Masari units that only fight in an elemental mode (Peacebringer, Sentry: weapons on
    // their _Fire/_Ice variants) start in the first mode listed (Type_Switch_Types).
    if (!isStructureType(*t) && weaponsFor(*t, db_).empty())
        if (const std::string* modes = t->find("Type_Switch_Types")) {
            std::string first = trim(modes->substr(0, modes->find(',')));
            if (const GameObjectType* v = db_.find(first); v && !weaponsFor(*v, db_).empty()) t = v;
        }
    Unit u = makeUnit(t, owner, position, facing);
    u.id = nextId_++;
    if (u.structure) navDirty_ = true;
    units_.push_back(std::move(u));
    lastSpawned_ = {units_.back().id};
    return units_.back().id;
}

Unit* World::unit(uint32_t id) {
    for (auto& u : units_)
        if (u.id == id) return &u;
    return nullptr;
}

void World::commandMove(const std::vector<uint32_t>& ids, Vec3 target, bool queued) {
    RecordScope rec_(*this, Command{0, Command::Kind::Move, ids, 0, -1, queued ? 1 : 0, target});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    std::vector<Unit*> group;
    for (uint32_t id : ids)
        if (Unit* u = unit(id); u && !u->inside) {
            u->boarding = 0;
            group.push_back(u);
        }
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
    // Big groups: units starting close together share one long path to the target (then a short one
    // to their slot) instead of each searching the whole map.
    const bool share = group.size() > 4;
    std::map<std::pair<int, int>, std::vector<Vec3>> shared;
    const std::vector<Vec3>* noPath = nullptr;
    for (Unit* u : group) {
        size_t best = 0;
        float bestD = 1e30f;
        for (size_t s = 0; s < slots.size(); ++s) {
            if (taken[s]) continue;
            const float d = dist2d(u->position, slots[s]);
            if (d < bestD) { bestD = d; best = s; }
        }
        taken[best] = true;
        if (queued && (u->moving() || !u->queuedMoves.empty())) { // after what it is already doing
            u->queuedMoves.push_back(slots[best]);
            continue;
        }
        u->queuedMoves.clear();
        u->attackOrder = 0;
        u->attackMove = false;
        u->patrol = false;
        u->guarding = 0;
        u->goal = slots[best];
        u->harvest = Unit::Harvest::None;
        if (u->flowTicks > 0) { u->flowGoal = slots[best]; continue; } // flowing: carries on there afterwards
        if (planFlow(*u, slots[best])) continue; // Novus: along the power network
        if (!share || u->flying()) {
            u->path = planPath(*u, slots[best]);
            continue;
        }
        const std::pair<int, int> key{int(std::floor(u->position[0] / 64)), int(std::floor(u->position[1] / 64))};
        auto it = shared.find(key);
        if (it == shared.end()) it = shared.emplace(key, nav_.findPath(u->position, target)).first;
        if (it->second.empty()) { // no way there from here
            u->path.clear();
            (void)noPath;
            continue;
        }
        u->path = it->second;
        if (dist2d(target, slots[best]) > 1) {
            std::vector<Vec3> tail = nav_.findPath(u->path.back(), slots[best]);
            if (!tail.empty()) u->path.insert(u->path.end(), tail.begin(), tail.end());
        }
    }
}

void World::commandStop(const std::vector<uint32_t>& ids) {
    RecordScope rec_(*this, Command{0, Command::Kind::Stop, ids});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    for (uint32_t id : ids)
        if (Unit* u = unit(id)) {
            u->path.clear();
            u->queuedMoves.clear();
            u->attackOrder = 0;
            u->attackMove = false;
            u->patrol = false;
            u->guarding = 0;
            u->harvest = Unit::Harvest::None;
        }
}

void World::commandGuard(const std::vector<uint32_t>& ids, uint32_t friendId) {
    RecordScope rec_(*this, Command{0, Command::Kind::Guard, ids, friendId});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    const Unit* f = unit(friendId);
    if (!f) return;
    std::vector<uint32_t> mobile;
    for (uint32_t id : ids)
        if (const Unit* u = unit(id); u && id != friendId && !u->structure && !u->inside && !enemies(u->owner, f->owner)) mobile.push_back(id);
    if (mobile.empty()) return;
    commandMove(mobile, f->position); // nested: not recorded on its own
    for (uint32_t id : mobile)
        if (Unit* u = unit(id)) u->guarding = friendId;
}

void World::commandPatrol(const std::vector<uint32_t>& ids, Vec3 target) {
    RecordScope rec_(*this, Command{0, Command::Kind::Patrol, ids, 0, -1, 0, target});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    std::vector<Vec3> from;
    for (uint32_t id : ids) from.push_back(unit(id) ? unit(id)->position : Vec3{});
    commandAttackMove(ids, target); // nested: not recorded on its own
    for (size_t i = 0; i < ids.size(); ++i)
        if (Unit* u = unit(ids[i]); u && u->attackMove && !u->structure) {
            u->patrol = true;
            u->patrolFrom = from[i];
            u->patrolTo = u->goal; // its own place in the formation
        }
}

void World::commandAttackMove(const std::vector<uint32_t>& ids, Vec3 target) {
    RecordScope rec_(*this, Command{0, Command::Kind::AttackMove, ids, 0, -1, 0, target});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    commandMove(ids, target);
    for (uint32_t id : ids)
        if (Unit* u = unit(id); u && !u->weapons.empty()) u->attackMove = true;
}

void World::commandAttack(const std::vector<uint32_t>& ids, uint32_t target) {
    RecordScope rec_(*this, Command{0, Command::Kind::Attack, ids, target});
    if (rec_.captured) return; // sent over the network; it runs when its turn comes
    const Unit* t = unit(target);
    if (!t) return;
    for (uint32_t id : ids) {
        Unit* u = unit(id);
        if (!u || u->inside || u->weapons.empty() || !enemies(u->owner, t->owner)) continue;
        u->boarding = 0;
        u->queuedMoves.clear();
        u->attackOrder = target;
        u->attackMove = false;
        u->patrol = false;
        u->guarding = 0;
        u->target = target;
        u->repathTicks = 0;
    }
}

int World::targetPriority(const Unit& u, const Unit& t) const {
    if (!priorities_) return 1000;
    auto sit = prioritySetOf_.find(u.type);
    if (sit == prioritySetOf_.end()) {
        const std::string* name = u.type->find("Targeting_Priority_Set");
        sit = prioritySetOf_.emplace(u.type, name ? priorities_->find(*name) : nullptr).first;
    }
    const TargetPrioritySet* set = sit->second;
    if (!set) return 1000;
    const auto key = std::make_pair(static_cast<const void*>(set), static_cast<const void*>(t.type));
    if (auto it = priorityCache_.find(key); it != priorityCache_.end()) return it->second;
    int p = 1000;
    std::string name = t.type->name;
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    if ((!set->categoryRestrictions.empty() && categoriesAny(*t.type, set->categoryRestrictions)) ||
        std::find(set->typeExclusions.begin(), set->typeExclusions.end(), name) != set->typeExclusions.end()) {
        p = -1;
    } else {
        bool byType = false;
        for (const auto& [what, pri] : set->priorities) // the type itself is the most specific
            if (what == name) { p = pri; byType = true; }
        if (!byType) {
            for (const auto& [what, pri] : set->priorities)
                if (pri < p && categoriesAny(*t.type, what)) p = pri;
            if (p == 1000 && !set->categoryExclusions.empty() && categoriesAny(*t.type, set->categoryExclusions)) p = -1;
        }
    }
    return priorityCache_[key] = p;
}

void World::updateTargeting(Unit& u) {
    if (u.weapons.empty()) return;
    if (u.inside) { u.target = 0; return; } // riders don't fight
    const float range = u.maxRange();
    auto edgeDistance = [&](const Unit& t) { return dist2d(u.position, t.position) - t.radius; };

    if (u.attackOrder) {
        const Unit* t = unit(u.attackOrder);
        if (!t || !canHitAny(u, *t)) {
            u.attackOrder = 0;
            u.path.clear();
        } else {
            u.target = t->id;
            if (edgeDistance(*t) > range * 0.9f) {
                // Chase: re-plan when the target has moved well off the goal (checked twice a second),
                // or when the path has run out.
                if (--u.repathTicks <= 0 && (u.path.empty() || dist2d(u.goal, t->position) > std::max(range * 0.5f, 40.f))) {
                    u.goal = t->position;
                    u.path = planPath(u, t->position);
                    u.repathTicks = kTicksPerSecond / 2;
                }
            } else {
                u.path.clear();
            }
            return;
        }
    }

    // Keep shooting the current target while it stays (roughly) in range; every half second look for
    // a better one (Targeting_Priority_Set: preferred kinds first, then the current target, then one
    // nearly dead, then the nearest).
    const Unit* current = unit(u.target);
    const bool lost = !current || edgeDistance(*current) > range + 20 || (current->cloaked && enemies(u.owner, current->owner)) ||
                      !canHitAny(u, *current);
    if (lost || (ticks_ + u.id) % 15 == 0) {
        const uint32_t was = lost ? 0 : u.target;
        u.target = 0;
        double best = 1e300;
        for (const Unit& o : units_) {
            if (!o.alive() || !enemies(u.owner, o.owner) || o.cloaked || o.inside) continue;
            const float d = edgeDistance(o);
            if (d > range || !canHitAny(u, o)) continue;
            const int pri = targetPriority(u, o);
            if (pri < 0) continue; // never picked on its own
            const double score = double(pri) * 1e5 + (o.id == was ? 0 : 5e4) + (o.health < 0.2f * o.maxHealth ? 0 : 2e4) + double(d);
            if (score < best) {
                if (fog_ && !visibleTo(u.owner, o.position)) {
                    // Big targets: seen when the side facing the attacker is.
                    const float dx = u.position[0] - o.position[0], dy = u.position[1] - o.position[1];
                    const float len = std::max(std::hypot(dx, dy), 1e-3f), k = std::min(o.radius, len) / len;
                    if (!visibleTo(u.owner, {o.position[0] + dx * k, o.position[1] + dy * k, 0})) continue;
                }
                best = score;
                u.target = o.id;
            }
        }
    }
    if (u.attackMove) {
        if (u.target) {
            u.path.clear(); // hold and fight
        } else if (u.path.empty() && dist2d(u.position, u.goal) > std::max(u.radius * 2, 20.f)) {
            u.path = planPath(u, u.goal); // area clear: carry on
            if (u.path.empty()) u.attackMove = false; // can't get there: stop trying (every tick)
        } else if (u.path.empty() && u.patrol) {
            std::swap(u.patrolFrom, u.patrolTo); // patrol: arrived, now back the other way
            u.goal = u.patrolTo;
            u.path = planPath(u, u.goal);
            if (u.path.empty()) u.attackMove = u.patrol = false;
        } else if (u.path.empty()) {
            u.attackMove = false; // arrived
        }
    }
}

StartingForces defaultStartingForces() {
    return {{"ALIEN", {"Alien_Arrival_Site", "Alien_Glyph_Carver"}},
            {"NOVUS", {"Novus_Remote_Terminal", "Novus_Constructor"}},
            {"MASARI", {"Masari_Foundation", "Masari_Architect"}},
            {"MILITARY", {"Military_Hummer"}}};
}

StartingForces parseStartingForces(std::string_view lua) {
    StartingForces out;
    size_t i = lua.find("StartingForces");
    if (i == std::string_view::npos) return out;
    i = lua.find('{', i);
    if (i == std::string_view::npos) return out;
    // Walk the nested table: depth 1 holds ["FACTION"] = { "TYPE", ... } entries.
    int depth = 0;
    std::string key, current;
    auto readString = [&](size_t& p) {
        const char q = lua[p++];
        std::string s;
        while (p < lua.size() && lua[p] != q) s += lua[p++];
        ++p;
        return s;
    };
    for (size_t p = i; p < lua.size();) {
        const char c = lua[p];
        if (c == '-' && p + 1 < lua.size() && lua[p + 1] == '-') { // comment to end of line
            while (p < lua.size() && lua[p] != '\n') ++p;
            continue;
        }
        if (c == '{') {
            ++depth;
            if (depth == 2) current = key;
            ++p;
        } else if (c == '}') {
            if (--depth == 0) break;
            ++p;
        } else if (c == '"' || c == '\'') {
            std::string s = readString(p);
            if (depth == 1) key = s;
            else if (depth == 2 && !current.empty()) {
                for (char& ch : current) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                out[current].push_back(s);
            }
        } else {
            ++p;
        }
    }
    return out;
}

StartingForces loadStartingForces(MegVfs& vfs) {
    StartingForces forces = defaultStartingForces();
    const char* path = "Data\\Scripts\\Story\\DefaultLandScript.lua";
    try {
        // Every layer's script, bottom to top: each faction's list from the topmost
        // layer that has one (so all mods' factions are available together).
        for (const auto& v : vfs.readAllLayers(path)) {
            const auto& bytes = v.data;
            if (!bytes.empty() && bytes[0] == 0x1B) continue; // compiled Lua
            for (auto& [faction, list] : parseStartingForces(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())))
                forces[faction] = list;
        }
    } catch (const std::exception&) {
    }
    return forces;
}

int addSkirmishPlayer(World& world, const GameDatabase& db, const std::string& faction, Vec3 start,
                      const StartingForces* forces) {
    const Faction* f = db.faction(faction);
    const int id = world.addPlayer(f ? f->name : faction, f ? f->multiplayerCredits : 10000.f, f ? f->multiplayerPopCap : 90);
    static const StartingForces defaults = defaultStartingForces();
    const StartingForces& table = forces ? *forces : defaults;
    std::string upper = faction;
    for (char& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    const auto it = table.find(upper);
    if (it == table.end()) return id;
    const float toCentre = std::atan2(world.map().sizeY() / 2 - start[1], world.map().sizeX() / 2 - start[0]);
    float clear = 60; // distance from the start marker that is outside the command structure
    int extra = 0;
    for (size_t k = 0; k < it->second.size(); ++k) {
        const GameObjectType* t = db.find(it->second[k]);
        if (!t) continue;
        if (k == 0) { // the script spawns the first entry exactly on the marker
            const uint32_t hq = world.spawn(t->name, id, start, 0);
            if (const Unit* u = world.unit(hq))
                clear = std::max(clear, std::hypot(u->footprintX, u->footprintY) + 10);
            continue;
        }
        // Everything else fans out toward the map centre, in front of the base.
        const float a = toCentre + 0.35f * float((extra + 1) / 2) * (extra % 2 ? 1.f : -1.f);
        const float out = clear + 40 + 20 * float(extra / 2);
        const Vec3 p = world.nav().nearestPassable({start[0] + std::cos(a) * out, start[1] + std::sin(a) * out, 0});
        world.spawn(t->name, id, p, toCentre);
        ++extra;
    }
    return id;
}

void World::fireWeapons(Unit& u) {
    if (u.underConstruction || u.leaping || u.inside || u.sellTicks >= 0) return;
    const Unit* t = u.target ? unit(u.target) : nullptr;
    if (t && !u.moving()) {
        // Face the target while standing (turrets handle aim while moving).
        const float want = std::atan2(t->position[1] - u.position[1], t->position[0] - u.position[0]);
        const float diff = angleDiff(u.facing, want);
        const float rate = std::max(u.stoppedTurnRate, u.turnRate * 0.5f);
        u.facing += std::clamp(diff, -rate, rate);
    }
    // Deploying units (the Dragoon) set up while standing and pack up to move; only set up, they fire.
    if (u.deployNeed > 0) {
        if (u.moving()) u.deployed = 0;
        else if (u.deployed < u.deployNeed) ++u.deployed;
    }
    const bool packed = u.deployNeed > 0 && u.deployed < u.deployNeed;
    for (Unit::WeaponState& w : u.weapons) {
        if (w.cooldown > 0) --w.cooldown;
        if (u.disarmed || packed) { w.pulsesLeft = 0; continue; }
        if (u.fooHeal && w.spec.fooFighters > 0) {
            if (w.cooldown == 0 && fooHealing(u, w)) w.cooldown = w.spec.rechargeTicks;
            continue;
        }
        const float d = t ? dist2d(u.position, t->position) - t->radius : 1e30f;
        const bool inRange = t && d <= u.weaponRange(w) && d + t->radius >= w.spec.minRange && canHit(w, *t);
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
            w.cooldown = int(std::lround(float(w.spec.rechargeTicks) / u.fireRate));
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
            pr.damage = w.spec.damage * u.damageCast;
            pr.blastDamage = w.spec.blastDamage * u.damageCast;
            pr.blastRange = w.spec.blastRange;
            pr.damageType = w.spec.damageType;
            pr.type = w.spec.projectile;
            projectiles_.push_back(std::move(pr));
        }
        --w.pulsesLeft;
        u.firedThisTick = true;
        w.pulseTimer = w.spec.pulseDelayTicks;
        if (w.pulsesLeft == 0) w.cooldown = int(std::lround(float(w.spec.rechargeTicks) / u.fireRate));
    }
}

bool World::fooHealing(Unit& u, Unit::WeaponState& w) {
    // The most hurt friend in range that the fighters may work on (not the categories they
    // won't touch, e.g. Organic: they repair machines).
    Unit* best = nullptr;
    float worst = 0.999f;
    const float range = u.weaponRange(w);
    for (Unit& o : units_) {
        if (!o.alive() || o.id == u.id || o.inside || o.underConstruction || enemies(u.owner, o.owner) || o.owner != u.owner) continue;
        const float f = o.health / std::max(1.f, o.maxHealth);
        if (f >= worst || dist2d(u.position, o.position) - o.radius > range) continue;
        if (!canHit(w, o)) continue;
        worst = f;
        best = &o;
    }
    if (!best) return false;
    best->health = std::min(best->maxHealth, best->health + w.spec.fooHeal * float(w.spec.fooFighters));
    u.fooTarget = best->id;
    return true;
}

void World::applyDamage(Unit& victim, float amount, const std::string& damageType) {
    amount *= victim.damageTaken;
    victim.damagedThisTick = true;
    if (amount <= 0) return;
    // A shield takes the hit first, with its own armor type.
    if (victim.shield > 0) {
        const float smod = damage_ && !victim.shieldArmor.empty() ? damage_->modifier(damageType, victim.shieldArmor) : 1.f;
        const float hit = amount * std::max(0.f, smod);
        if (hit <= victim.shield) { victim.shield -= hit; return; }
        amount *= (hit - victim.shield) / hit; // what is left after the shield breaks
        victim.shield = 0;
    }
    float extra = 0; // ModifyArmor effects
    for (const auto& [type, add] : victim.armorMods)
        if (iequals(type, damageType)) extra += add;
    if (!victim.shieldedByHardpoints) {
        const float mod = (damage_ ? damage_->modifier(damageType, victim.armorType) : 1.f) + extra;
        float dealt = amount * std::max(0.f, mod);
        if (victim.dma > 0) { // Masari dark-matter armor soaks it up
            const float soak = std::min(victim.dma, dealt);
            victim.dma -= soak;
            dealt -= soak;
        }
        victim.health -= dealt;
        return;
    }
    // Legs first, then the core; other hardpoints (weapon crowns) only fall with the body.
    Unit::HardPoint* hp = nullptr;
    bool anyCore = false;
    for (auto& h : victim.hardpoints) anyCore = anyCore || h.destroysParent;
    for (auto& h : victim.hardpoints)
        if (h.alive() && h.leg && (!hp || h.health < hp->health)) hp = &h;
    if (!hp)
        for (auto& h : victim.hardpoints)
            if (h.alive() && (h.destroysParent || !anyCore)) { hp = &h; break; }
    if (!hp) { victim.health = 0; return; }
    const float mod = std::max(0.f, (damage_ ? damage_->modifier(damageType, hp->armorType.empty() ? victim.armorType : hp->armorType) : 1.f) + extra);
    const float dealt = std::min(hp->health, amount * mod);
    hp->health -= dealt;
    victim.health -= dealt;
    if (!hp->alive()) {
        // Its weapons go with it.
        const GameObjectType* gone = hp->type;
        victim.weapons.erase(std::remove_if(victim.weapons.begin(), victim.weapons.end(),
                                            [&](const Unit::WeaponState& w) { return w.spec.source == gone; }),
                             victim.weapons.end());
        if (hp->destroysParent) victim.health = 0;
    }
    if (victim.health <= 0.01f) victim.health = 0;
}

void World::updateProjectiles() {
    std::vector<Projectile> keep;
    keep.reserve(projectiles_.size());
    if (impacts_.size() > 4096) impacts_.erase(impacts_.begin(), impacts_.begin() + 2048);
    for (Projectile& p : projectiles_) {
        if (!p.id) p.id = nextProjectileId_++;
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
        if (Unit* t = unit(p.target); t && t->alive() && !t->inside && dist2d(t->position, p.position) <= t->radius + 3) hit = t;
        if (!hit)
            for (Unit& o : units_)
                if (o.alive() && !o.inside && enemies(p.owner, o.owner) && dist2d(o.position, p.position) <= o.radius) { hit = &o; break; }
        // A redirecting shield sends the shot back at whoever fired it.
        if (hit && p.shooter != 0 && hit->id != p.shooter)
            if (const float chance = redirectChance(*hit); chance > 0 && random01() < chance) {
                if (const Unit* src = unit(p.shooter); src && src->alive() && !src->inside) {
                    Projectile back = p;
                    back.owner = hit->owner;
                    back.shooter = hit->id;
                    back.target = src->id;
                    back.id = 0;
                    const float speed = std::max(1.f, std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]));
                    const float dx = src->position[0] - p.position[0], dy = src->position[1] - p.position[1], dz = src->position[2] + 4 - p.position[2];
                    const float len = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
                    back.velocity = {dx / len * speed, dy / len * speed, dz / len * speed};
                    back.distance = len;
                    back.travelled = 0;
                    keep.push_back(std::move(back));
                }
                impacts_.push_back({p.type, p.position, true, ticks_});
                continue;
            }
        if (hit && p.damage > 0) applyDamage(*hit, p.damage, p.damageType);
        impacts_.push_back({p.type, p.position, hit != nullptr, ticks_});
        projectileImpact(p, hit);
        if (p.blastDamage > 0 && p.blastRange > 0)
            for (Unit& o : units_)
                if (o.alive() && !o.inside && enemies(p.owner, o.owner) && dist2d(o.position, p.position) <= p.blastRange + o.radius)
                    applyDamage(o, p.blastDamage, p.damageType);
    }
    projectiles_.swap(keep);
}

void World::removeDead() {
    // A transport that dies drops its riders, who take Damage_Percent_On_Destruction of their health.
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].alive() || units_[i].passengers.empty()) continue;
        const float share = garrisonOf(*units_[i].type) ? garrisonOf(*units_[i].type)->deathDamage : 1.f;
        const std::vector<uint32_t> riders = units_[i].passengers;
        units_[i].health = 1; // briefly, so unload() finds it
        unload(units_[i].id);
        units_[i].health = 0;
        for (uint32_t id : riders)
            if (Unit* r = unit(id)) {
                r->health -= r->maxHealth * share;
                if (r->health <= 0.01f) r->health = 0;
            }
    }
    for (Unit& u : units_)
        if (!u.alive() && u.inside)
            if (Unit* t = unit(u.inside)) t->passengers.erase(std::remove(t->passengers.begin(), t->passengers.end(), u.id), t->passengers.end());
    for (const Unit& u : units_)
        if (!u.alive()) {
            deaths_.push_back({u.id, u.type, u.owner, u.position, ticks_, u.sold, u.facing});
            if (Player* p = u.sold ? nullptr : player(u.owner)) ++(u.structure ? p->structuresLost : p->unitsLost);
            if (u.structure) navDirty_ = true;
            if (!u.sold) deathSpawn(u);
        }
    units_.erase(std::remove_if(units_.begin(), units_.end(), [](const Unit& u) { return !u.alive(); }), units_.end());
}

void World::moveUnit(Unit& u) {
    if (u.structure || u.leaping || u.inside) return; // leaps move in updateLeap, riders with their transport
    if (u.path.empty()) {
        u.speed = std::max(0.f, u.speed - u.acceleration * 2);
        return;
    }
    // Skip ahead when a later waypoint is already in clear view (e.g. after being nudged by others).
    while (!u.flying() && u.path.size() > 1 && nav_.lineClear(u.position, u.path[1])) u.path.erase(u.path.begin());

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
    // Units caught on blocked ground (a footprint raised around them) may always step off it.
    if (!u.flying() && !nav_.passableAt(next[0], next[1]) && nav_.passableAt(u.position[0], u.position[1])) {
        // Slide along the wall on whichever axis is free.
        if (nav_.passableAt(u.position[0] + mx, u.position[1])) next = {u.position[0] + mx, u.position[1], 0};
        else if (nav_.passableAt(u.position[0], u.position[1] + my)) next = {u.position[0], u.position[1] + my, 0};
        else next = u.position;
    }
    const bool progressed = std::hypot(next[0] - u.position[0], next[1] - u.position[1]) > 0.1f * u.maxSpeed;
    u.stuckTicks = progressed ? 0 : u.stuckTicks + 1;
    if (u.stuckTicks > kTicksPerSecond / 2) {
        // Wedged (crowding or a bad waypoint): plan again from where we are.
        u.path = planPath(u, u.goal);
        u.stuckTicks = 0;
    }
    u.position = {next[0], next[1], map_.heightAt(next[0], next[1])};
}

void World::separate() {
    // Gentle push apart so units don't stack. Ordinary units are bucketed on a grid of the
    // largest one's diameter, so only neighbouring cells are compared; the few big ones
    // (walkers, capital ships) would make the cells huge, so they are checked against everyone.
    constexpr float kBig = 32;
    float cell = 16;
    std::vector<size_t> big;
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (u.structure) continue;
        if (u.radius > kBig) big.push_back(i);
        else cell = std::max(cell, u.radius * 2);
    }
    auto pushApart = [&](size_t i, size_t j) {
        Unit& a = units_[i];
        Unit& b = units_[j];
        if (a.structure || b.structure) return; // the nav grid keeps units off footprints
        if (a.inside || b.inside) return;
        if (a.leaping || b.leaping) return;
        if (a.flying() != b.flying()) return;   // aircraft pass over ground units
        const float dx = b.position[0] - a.position[0], dy = b.position[1] - a.position[1];
        const float minD = (a.radius + b.radius) * 0.9f;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= minD * minD) return;
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
        if (a.flying() || nav_.passableAt(pa[0], pa[1])) a.position = {pa[0], pa[1], map_.heightAt(pa[0], pa[1])};
        if (b.flying() || nav_.passableAt(pb[0], pb[1])) b.position = {pb[0], pb[1], map_.heightAt(pb[0], pb[1])};
    };
    std::unordered_map<int64_t, std::vector<size_t>> grid;
    auto cellOf = [&](const Vec3& p) { return std::pair<int64_t, int64_t>{int64_t(std::floor(p[0] / cell)), int64_t(std::floor(p[1] / cell))}; };
    auto keyOf = [](int64_t x, int64_t y) { return (x << 32) ^ (y & 0xffffffff); };
    auto isBig = [&](size_t i) { return units_[i].radius > kBig; };
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].structure || isBig(i)) continue;
        const auto [cx, cy] = cellOf(units_[i].position);
        grid[keyOf(cx, cy)].push_back(i);
    }
    std::vector<size_t> near;
    for (size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].structure) continue;
        near.clear();
        if (isBig(i)) {
            // Against every unit after it, and every small one before it (small units skip bigs).
            for (size_t j = 0; j < units_.size(); ++j)
                if (j != i && !units_[j].structure && (j > i || !isBig(j))) near.push_back(j);
        } else {
            const auto [cx, cy] = cellOf(units_[i].position);
            for (int64_t dx = -1; dx <= 1; ++dx)
                for (int64_t dy = -1; dy <= 1; ++dy)
                    if (auto it = grid.find(keyOf(cx + dx, cy + dy)); it != grid.end())
                        for (size_t j : it->second)
                            if (j > i) near.push_back(j);
        }
        std::sort(near.begin(), near.end()); // same pair order every run
        for (size_t j : near) pushApart(std::min(i, j), std::max(i, j));
    }
}

float World::revealRange(const GameObjectType& t) const {
    if (auto it = revealRanges_.find(&t); it != revealRanges_.end()) return it->second;
    const float r = float(t.number("Land_FOW_Reveal_Range").value_or(150));
    revealRanges_[&t] = r;
    return r;
}

uint8_t World::visibility(int player, float x, float y) const {
    if (!fog_) return kVisible;
    if (player < 0 || size_t(player) >= fogGrids_.size() || fogW_ <= 0) return kUnexplored;
    const int cx = std::clamp(int(x / fogCell_), 0, fogW_ - 1), cy = std::clamp(int(y / fogCell_), 0, fogH_ - 1);
    return fogGrids_[size_t(player)][size_t(cy) * size_t(fogW_) + size_t(cx)];
}

void World::updateFog() {
    if (!fog_) return;
    // Twice a second; units move about 1-4 cells in that time.
    if (fogTick_ >= 0 && int64_t(ticks_) - fogTick_ < kTicksPerSecond / 2 && fogGrids_.size() == players_.size()) return;
    fogTick_ = int64_t(ticks_);
    fogW_ = std::max(1, int(std::ceil(map_.sizeX() / fogCell_)));
    fogH_ = std::max(1, int(std::ceil(map_.sizeY() / fogCell_)));
    const size_t cells = size_t(fogW_) * size_t(fogH_);
    fogGrids_.resize(players_.size());
    // Each player's own sight this update.
    std::vector<std::vector<uint8_t>> sight(players_.size(), std::vector<uint8_t>(cells, 0));
    for (const Unit& u : units_) {
        if (u.owner < 0 || size_t(u.owner) >= players_.size() || !u.alive() || u.inside) continue;
        float r = revealRange(*u.type);
        if (!u.attributes.empty()) {
            static const std::string kMult = "MAX_LAND_FOW_REVEAL_MULT_COEFF", kAdd = "MAX_LAND_FOW_REVEAL_ADD_COEFF";
            r = r * std::max(0.f, 1 + u.attributeUpper(kMult)) + u.attributeUpper(kAdd);
        }
        if (u.underConstruction) r = std::min(r, std::max(u.radius * 2, 80.f));
        const int cx = int(u.position[0] / fogCell_), cy = int(u.position[1] / fogCell_);
        const int rc = int(std::ceil(r / fogCell_));
        const float r2 = (r / fogCell_) * (r / fogCell_);
        std::vector<uint8_t>& g = sight[size_t(u.owner)];
        for (int dy = -rc; dy <= rc; ++dy) {
            const int y = cy + dy;
            if (y < 0 || y >= fogH_) continue;
            for (int dx = -rc; dx <= rc; ++dx) {
                const int x = cx + dx;
                if (x < 0 || x >= fogW_ || float(dx * dx + dy * dy) > r2) continue;
                g[size_t(y) * size_t(fogW_) + size_t(x)] = 1;
            }
        }
    }
    // Allies share what they see; seen cells stay explored.
    for (size_t p = 0; p < players_.size(); ++p) {
        std::vector<uint8_t>& g = fogGrids_[p];
        g.resize(cells, kUnexplored);
        for (uint8_t& c : g)
            if (c == kVisible) c = kExplored;
        for (size_t q = 0; q < players_.size(); ++q) {
            if (q != p && enemies(int(p), int(q))) continue;
            const std::vector<uint8_t>& s = sight[q];
            for (size_t i = 0; i < cells; ++i)
                if (s[i]) g[i] = kVisible;
        }
    }
}

void World::tick() {
    // UAW_PROFILE=MS: print the phases of any tick slower than MS milliseconds.
    static const double profile = std::getenv("UAW_PROFILE") ? std::atof(std::getenv("UAW_PROFILE")) : 0;
    using Clock = std::chrono::steady_clock;
    Clock::time_point marks[16];
    int nm = 0;
    auto mark = [&]() { if (profile > 0 && nm < 16) marks[nm++] = Clock::now(); };
    mark();
    if (navDirty_) rebuildNav();
    mark();
    updateFog();
    mark();
    for (size_t i = 0; i < units_.size(); ++i) updateConstruction(units_[i]);
    for (size_t i = 0; i < units_.size(); ++i) updateProduction(units_[i]); // may spawn (index loop on purpose)
    mark();
    updateEconomy();
    updateResearch();
    mark();
    updateAbilities();
    mark();
    updateSuperweapons();
    for (Unit& u : units_) updateTargeting(u);
    mark();
    for (Unit& u : units_) {
        updateFlow(u);
        // Guarding: when idle and the friend has moved off, follow (every half second).
        if (u.guarding && (ticks_ + u.id) % 15 == 0) {
            const Unit* f = unit(u.guarding);
            if (!f || !f->alive() || f->inside) {
                u.guarding = 0;
            } else if (!u.target && !u.attackOrder) {
                const float keep = std::max(60.f, f->radius + u.radius + 40);
                if (dist2d(u.position, f->position) > keep && (u.path.empty() || dist2d(u.goal, f->position) > keep)) {
                    u.goal = f->position;
                    u.path = planPath(u, f->position);
                }
            }
        }
        // Arrived with Shift-queued moves left: on to the next.
        if (!u.queuedMoves.empty() && !u.moving() && !u.attackMove && !u.attackOrder && !u.inside) {
            u.goal = u.queuedMoves.front();
            u.queuedMoves.erase(u.queuedMoves.begin());
            u.path = planPath(u, u.goal);
        }
        moveUnit(u);
    }
    mark();
    updateGarrisons();
    separate();
    mark();
    for (Unit& u : units_) fireWeapons(u);
    updateProjectiles();
    mark();
    removeDead();
    mark();
    if (ticks_ % (10 * kTicksPerSecond) == 0) { // every player's army strength, every ten seconds
        std::array<float, 8> v{};
        for (const Unit& u : units_) {
            if (u.structure || u.owner < 0 || u.owner >= 8 || u.inside) continue;
            float dps = 0;
            for (const auto& ws : u.weapons) dps += ws.spec.damagePerSecond();
            v[size_t(u.owner)] += std::sqrt(std::max(0.f, u.health) * dps);
        }
        armyHistory_.push_back(v);
    }
    ++ticks_;
    if (profile > 0) {
        const double total = std::chrono::duration<double, std::milli>(marks[nm - 1] - marks[0]).count();
        if (total > profile) {
            static const char* names[] = {"nav", "fog", "build/produce", "economy", "abilities", "targeting", "move", "separate", "fire", "deaths"};
            std::fprintf(stderr, "tick %llu: %.1f ms (%zu units):", (unsigned long long)ticks_, total, units_.size());
            for (int k = 1; k < nm; ++k)
                std::fprintf(stderr, " %s %.1f", names[k - 1], std::chrono::duration<double, std::milli>(marks[k] - marks[k - 1]).count());
            std::fprintf(stderr, "\n");
        }
    }
}

void World::apply(const Command& c) {
    const uint32_t first = c.ids.empty() ? 0 : c.ids[0];
    switch (c.kind) {
    case Command::Kind::Move: commandMove(c.ids, c.point, c.index == 1); break;
    case Command::Kind::Stop: commandStop(c.ids); break;
    case Command::Kind::AttackMove: commandAttackMove(c.ids, c.point); break;
    case Command::Kind::Patrol: commandPatrol(c.ids, c.point); break;
    case Command::Kind::Guard: commandGuard(c.ids, c.target); break;
    case Command::Kind::Attack: commandAttack(c.ids, c.target); break;
    case Command::Kind::Queue: queueProduction(first, c.name); break;
    case Command::Kind::CancelQueue: cancelProduction(first); break;
    case Command::Kind::Rally: setRally(first, c.point); break;
    case Command::Kind::Construct: commandConstruct(first, c.name, c.point); break;
    case Command::Kind::Superweapon: fireSuperweapon(first, c.point); break;
    case Command::Kind::Research: startResearch(c.owner, c.name, c.index); break;
    case Command::Kind::Ability: useAbility(first, size_t(c.index), c.target, c.point); break;
    case Command::Kind::Board: commandBoard(c.ids, c.target); break;
    case Command::Kind::Unload: unload(first); break;
    case Command::Kind::Sell: sell(first); break;
    case Command::Kind::Pause: break; // the network game's, not the world's
    }
}

uint64_t World::stateHash() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto mixf = [&](float f) { uint32_t b; std::memcpy(&b, &f, 4); mix(b); };
    mix(ticks_);
    for (const Unit& u : units_) {
        mix(u.id);
        mix(uint64_t(u.owner + 1));
        mixf(u.health);
        mixf(u.position[0]);
        mixf(u.position[1]);
    }
    for (const Player& p : players_) mixf(p.credits);
    return h;
}

std::vector<uint32_t> World::unitsNear(Vec3 p, float radius) const {
    std::vector<std::pair<float, uint32_t>> hits;
    for (const Unit& u : units_) {
        if (u.inside) continue;
        const float d = dist2d(u.position, p);
        if (d <= radius + u.radius) hits.push_back({d, u.id});
    }
    std::sort(hits.begin(), hits.end());
    std::vector<uint32_t> out;
    for (auto& h : hits) out.push_back(h.second);
    return out;
}

} // namespace alamo
