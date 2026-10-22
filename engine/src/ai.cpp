#include "alamo/ai.hpp"

#include <algorithm>
#include <cmath>

namespace alamo {

namespace {

float dist2(Vec3 a, Vec3 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

constexpr int kThinkEvery = kTicksPerSecond / 2;

} // namespace

bool defeated(const World& w, int player) {
    for (const Unit& u : w.units()) {
        if (u.owner != player) continue;
        if (u.structure) return false;
        if (w.tech() && !w.tech()->constructibleBy(u.type->name, [](const std::string&) { return true; }).empty())
            return false;
    }
    return true;
}

SkirmishAI::SkirmishAI(World& world, int player, uint64_t seed)
    : w_(world), db_(world.db()), player_(player), rng_(seed * 0x2545F4914F6CDD1Dull + uint64_t(player) * 0x9E3779B97F4A7C15ull + 1) {
    // Price level of this faction: its starting credits (Novus 10,000, Masari 1,000 in the
    // retail data, matching their unit prices). Spending thresholds are fractions of it.
    if (const Player* p = w_.player(player_)) scale_ = std::max(100.f, p->credits);
    // Structures that produce at least one combat unit: the production buildings.
    if (const TechTree* tech = w_.tech()) {
        for (const std::string& obj : tech->objects()) {
            const GameObjectType* t = db_.find(obj);
            if (!t || !isCombatUnit(*t)) continue;
            for (const BuildOption& o : tech->options(obj))
                for (const std::string& b : o.builders) {
                    const GameObjectType* bt = db_.find(b);
                    // Structures, and units that produce (Hierarchy walkers).
                    if (bt && std::find(producers_.begin(), producers_.end(), bt->name) == producers_.end())
                        producers_.push_back(bt->name);
                }
        }
    }
}

uint32_t SkirmishAI::rand(uint32_t n) {
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    return n ? uint32_t((rng_ * 0x2545F4914F6CDD1Dull) >> 33) % n : 0;
}

bool SkirmishAI::isCombatUnit(const GameObjectType& t) const {
    auto it = combat_.find(&t);
    if (it != combat_.end()) return it->second;
    bool c = !isStructureType(t) && buildInfo(t, db_).spawnsUnit && !weaponsFor(t, db_).empty();
    const EconomySpec& e = w_.economy(t);
    if (e.collector || e.reaper) c = false;
    const_cast<SkirmishAI*>(this)->combat_[&t] = c;
    return c;
}

bool SkirmishAI::isBuilderType(const GameObjectType& t) const {
    auto it = builder_.find(&t);
    if (it != builder_.end()) return it->second;
    const bool b = !isStructureType(t) && w_.tech() &&
                   !w_.tech()->constructibleBy(t.name, [](const std::string&) { return true; }).empty();
    const_cast<SkirmishAI*>(this)->builder_[&t] = b;
    return b;
}

SkirmishAI::Role SkirmishAI::roleOf(const GameObjectType& t) const {
    auto it = roles_.find(&t);
    if (it != roles_.end()) return it->second;
    Role r = Role::Other;
    const EconomySpec& e = w_.economy(t);
    const Player* p = const_cast<World&>(w_).player(player_);
    const bool passive = e.passivePerSecond > 0 && (e.passiveFaction.empty() || (p && iequals(e.passiveFaction, p->faction)));
    if (e.drones > 0 || passive || e.reaper) r = Role::Economy;
    else if (e.power == EconomySpec::Power::Generator || e.power == EconomySpec::Power::Transmitter) r = Role::Power;
    else if (std::find(producers_.begin(), producers_.end(), t.name) != producers_.end()) r = Role::Production;
    else if (isStructureType(t) && !weaponsFor(t, db_).empty()) r = Role::Defense;
    else if (isStructureType(t)) r = Role::Tech;
    const_cast<SkirmishAI*>(this)->roles_[&t] = r;
    return r;
}

Vec3 SkirmishAI::baseCentre() const {
    // The first structure we own (the HQ), else our first unit.
    for (const Unit& u : w_.units())
        if (u.owner == player_ && u.structure) return u.position;
    for (const Unit& u : w_.units())
        if (u.owner == player_) return u.position;
    return {w_.map().sizeX() / 2, w_.map().sizeY() / 2, 0};
}

Vec3 SkirmishAI::rallyPoint() const {
    const Vec3 b = baseCentre();
    const float cx = w_.map().sizeX() / 2, cy = w_.map().sizeY() / 2;
    const float dx = cx - b[0], dy = cy - b[1], d = std::max(1.f, std::hypot(dx, dy));
    const float out = std::min(450.f, d * 0.4f);
    return w_.nav().nearestPassable({b[0] + dx / d * out, b[1] + dy / d * out, 0});
}

bool SkirmishAI::coveredByPower(Vec3 p, float extra) const {
    for (const Unit& u : w_.units()) {
        if (u.owner != player_ || !u.powered || u.underConstruction) continue;
        const EconomySpec& e = w_.economy(*u.type);
        if ((e.power == EconomySpec::Power::Generator || e.power == EconomySpec::Power::Transmitter) &&
            dist2(u.position, p) + extra <= e.powerRadius)
            return true;
    }
    return false;
}

bool SkirmishAI::findSite(const GameObjectType& t, Vec3& out) {
    const Vec3 base = baseCentre();
    const float cx = w_.map().sizeX() / 2, cy = w_.map().sizeY() / 2;
    const float toCentre = std::atan2(cy - base[1], cx - base[0]);
    const EconomySpec& e = w_.economy(t);
    const bool needsPower = e.power == EconomySpec::Power::Receiver;
    const bool isNode = e.power == EconomySpec::Power::Generator || e.power == EconomySpec::Power::Transmitter;
    const float extent = float(t.number("Custom_Hard_XExtent").value_or(30));
    // Spiral outward from the base, starting on the side facing the map centre.
    for (int ring = 0; ring < 9; ++ring) {
        const float radius = 200 + ring * 95 + (isNode ? 120 : 0);
        const int steps = 10 + ring * 3;
        for (int k = 0; k < steps; ++k) {
            const int j = (k + siteSpin_) % steps;
            const float a = toCentre + (j % 2 ? 1 : -1) * float((j + 1) / 2) * (6.2831853f / float(steps));
            const Vec3 p{base[0] + std::cos(a) * radius, base[1] + std::sin(a) * radius, 0};
            if (p[0] < 100 || p[1] < 100 || p[0] > w_.map().sizeX() - 100 || p[1] > w_.map().sizeY() - 100) continue;
            if (needsPower && !coveredByPower(p, extent * 0.5f)) continue;
            if (!w_.siteClear(t, p)) continue;
            out = p;
            ++siteSpin_;
            return true;
        }
    }
    return false;
}

bool SkirmishAI::tryConstruct(Unit& builder, const std::string& type) {
    const GameObjectType* t = db_.find(type);
    if (!t) return false;
    Vec3 site;
    if (!findSite(*t, site)) return false;
    const OrderResult r = w_.commandConstruct(builder.id, type, site);
    if (r == OrderResult::Ok) lastAction_ = "construct " + type;
    return r == OrderResult::Ok;
}

float SkirmishAI::wealth() const {
    // Credits as a fraction of the faction's price level; near a bank cap counts as rich.
    const Player* me = const_cast<World&>(w_).player(player_);
    if (!me) return 0;
    float r = me->credits / scale_;
    const float cap = w_.bankCap(player_);
    if (cap > 0 && me->credits >= 0.75f * cap) r = std::max(r, 1.f);
    return r;
}

void SkirmishAI::manageConstruction() {
    const Player* me = w_.player(player_);
    if (!me) return;
    const float rich = wealth();
    const float minutes = w_.seconds() / 60;
    // What we have (built or being built), by type and role.
    std::unordered_map<std::string, int> have;
    int roleCount[6] = {0, 0, 0, 0, 0, 0};
    bool unpoweredReceiver = false;
    for (const Unit& u : w_.units()) {
        if (u.owner != player_) continue;
        ++have[u.type->name];
        if (u.structure || w_.economy(*u.type).reaper || roleOf(*u.type) == Role::Production) ++roleCount[int(roleOf(*u.type))];
        if (u.structure && !u.underConstruction && !u.powered) unpoweredReceiver = true;
    }
    for (Unit& b : const_cast<std::vector<Unit>&>(w_.units())) {
        if (b.owner != player_ || b.structure || b.underConstruction || b.constructing || !b.path.empty()) continue;
        const auto options = w_.constructible(b.id);
        if (options.empty()) continue;
        // Desired counts per role, growing with time.
        const int wantEconomy = 2 + int(minutes / 2.5f);
        const int wantDefense = minutes > 4 ? 1 + int((minutes - 4) / 4) : 0;
        auto pick = [&](Role role, int limitOfType) -> std::string {
            std::string best;
            int bestHave = 1 << 30;
            float bestCost = 0;
            for (const std::string& o : options) {
                const GameObjectType* t = db_.find(o);
                if (!t || roleOf(*t) != role) continue;
                const int n = have.count(o) ? have.at(o) : 0;
                if (n >= limitOfType) continue;
                const float cost = buildInfo(*t, db_).cost;
                if (cost > me->credits) continue;
                // Spread across types; among equals take the cheaper (usually the basic unit factory).
                if (n < bestHave || (n == bestHave && cost < bestCost)) { bestHave = n; bestCost = cost; best = o; }
            }
            return best;
        };
        std::string choice;
        // Novus: a generator first, and another node whenever a receiver sits unpowered.
        if (roleCount[int(Role::Power)] == 0 || unpoweredReceiver) choice = pick(Role::Power, 99);
        if (choice.empty() && roleCount[int(Role::Economy)] < wantEconomy) choice = pick(Role::Economy, 99);
        if (choice.empty()) choice = pick(Role::Production, 1);
        if (choice.empty() && roleCount[int(Role::Defense)] < wantDefense) choice = pick(Role::Defense, 99);
        if (choice.empty() && rich > 0.15f) choice = pick(Role::Tech, 1);
        // Rich: more production lines (and more economy) to turn credits into an army.
        if (choice.empty() && minutes > 2 && rich > 0.25f) choice = pick(Role::Production, 1 + int(rich / 0.2f));
        if (choice.empty() && minutes > 3 && rich > 0.4f) choice = pick(Role::Economy, 99);
        if (choice.empty()) continue;
        if (tryConstruct(b, choice)) {
            ++have[choice];
            ++roleCount[int(roleOf(*db_.find(choice)))];
        }
    }
}

void SkirmishAI::manageProduction() {
    const Player* me = w_.player(player_);
    if (!me) return;
    int builders = 0;
    for (const Unit& u : w_.units())
        if (u.owner == player_ && isBuilderType(*u.type)) ++builders;
    const Vec3 rally = rallyPoint();
    for (const Unit& s : w_.units()) {
        // Queue deeper when rich so credits don't pile up.
        const float rich = wealth();
        const size_t depth = rich > 0.6f ? 4 : rich > 0.2f ? 2 : 1;
        if (s.owner != player_ || s.underConstruction || s.queue.size() >= depth || !s.powered) continue;
        if (!s.structure && roleOf(*s.type) != Role::Production) continue; // walkers produce too
        const auto options = w_.producible(s.id);
        if (options.empty()) continue;
        if (!s.hasRally) w_.setRally(s.id, rally);
        // A spare builder first (up to 2), then a random affordable combat unit,
        // weighted toward what we can buy several of.
        std::string choice;
        if (builders < 2)
            for (const auto& o : options)
                if (const GameObjectType* t = db_.find(o); t && isBuilderType(*t) && buildInfo(*t, db_).cost <= me->credits) {
                    choice = o;
                    ++builders;
                    break;
                }
        if (choice.empty()) {
            std::vector<std::string> combat;
            for (const auto& o : options) {
                const GameObjectType* t = db_.find(o);
                if (!t || !isCombatUnit(*t)) continue;
                const float cost = buildInfo(*t, db_).cost;
                if (cost > me->credits) continue;
                // Heroes once in a while only (they have a build limit anyway).
                if (t->category.find("Hero") != std::string::npos && rand(6) != 0) continue;
                combat.push_back(o);
            }
            if (!combat.empty()) {
                // Rich: buy from the pricier half (stronger units) so credits don't pile up.
                if (rich > 0.5f && combat.size() > 1) {
                    std::sort(combat.begin(), combat.end(), [&](const std::string& x, const std::string& y) {
                        const float cx = buildInfo(*db_.find(x), db_).cost, cy = buildInfo(*db_.find(y), db_).cost;
                        return cx != cy ? cx > cy : x < y;
                    });
                    combat.resize((combat.size() + 1) / 2);
                }
                choice = combat[rand(uint32_t(combat.size()))];
            }
        }
        if (!choice.empty() && w_.queueProduction(s.id, choice) == OrderResult::Ok) lastAction_ = "queue " + choice;
    }
}

// Combat strength: sqrt(health x damage per second), summed. Comparable across factions
// whatever their prices (Masari units cost a tenth of the others').
float unitStrength(const Unit& u) {
    float dps = 0;
    for (const auto& w : u.weapons) dps += w.spec.damagePerSecond();
    return std::sqrt(std::max(0.f, u.health) * dps);
}

float SkirmishAI::armyValue(const std::vector<uint32_t>& army) const {
    float v = 0;
    for (uint32_t id : army)
        if (const Unit* u = const_cast<World&>(w_).unit(id)) v += unitStrength(*u);
    return v;
}

float SkirmishAI::enemyStrength() const {
    float v = 0;
    for (const Unit& u : w_.units())
        if (w_.enemies(player_, u.owner) && !u.structure && !u.underConstruction) v += unitStrength(u);
    return v;
}

void SkirmishAI::manageArmy() {
    std::vector<uint32_t> army;
    Vec3 centroid{0, 0, 0};
    for (const Unit& u : w_.units()) {
        if (u.owner != player_ || u.structure || u.underConstruction || !isCombatUnit(*u.type)) continue;
        army.push_back(u.id);
        centroid[0] += u.position[0];
        centroid[1] += u.position[1];
    }
    if (army.empty()) { attacking_ = false; return; }
    centroid[0] /= float(army.size());
    centroid[1] /= float(army.size());

    // Defend: enemies near any of our structures take priority.
    for (const Unit& s : w_.units()) {
        if (s.owner != player_ || !s.structure) continue;
        for (uint32_t id : w_.unitsNear(s.position, 650)) {
            const Unit* e = w_.unit(id);
            if (!e || !w_.enemies(player_, e->owner) || e->owner < 0) continue;
            if (!attacking_) {
                w_.commandAttackMove(army, e->position);
                lastAction_ = "defend";
            }
            return;
        }
    }

    const float minutes = w_.seconds() / 60;
    const Player* me = w_.player(player_);
    const bool capped = me && w_.popUsed(player_) >= me->popCap - 4 && me->popCap > 0;
    if (!attacking_) {
        // Go when clearly stronger than everything the enemy has in the field (and not
        // before minute 4 unless overwhelming), or when we can't grow any more.
        const float value = armyValue(army), enemy = enemyStrength();
        const bool ready = (minutes >= 4 && army.size() >= 6 && value >= 1.3f * enemy) || (minutes >= 3 && army.size() >= 4 && value >= 3 * enemy + 1) ||
                           capped || (minutes >= 12 && army.size() >= 10);
        if (!ready) {
            // Gather idle units at the rally point.
            const Vec3 rally = rallyPoint();
            std::vector<uint32_t> idle;
            for (uint32_t id : army)
                if (const Unit* u = w_.unit(id); u && !u->moving() && !u->attackOrder && dist2(u->position, rally) > 300) idle.push_back(id);
            if (!idle.empty()) w_.commandMove(idle, rally);
            return;
        }
        attacking_ = true;
        waveStartSize_ = int(army.size());
        waveStartValue_ = armyValue(army);
        attackTarget_ = 0;
    }
    // Attacking: pick the nearest enemy structure (else any enemy unit) to the army.
    const Unit* target = attackTarget_ ? w_.unit(attackTarget_) : nullptr;
    if (!target) {
        float best = 1e30f;
        for (int pass = 0; pass < 2 && !target; ++pass)
            for (const Unit& e : w_.units()) {
                if (!w_.enemies(player_, e.owner) || (pass == 0 && !e.structure)) continue;
                const float d = dist2(e.position, centroid);
                if (d < best) { best = d; target = &e; }
            }
        if (!target) { attacking_ = false; return; }
        attackTarget_ = target->id;
        w_.commandAttackMove(army, target->position);
        lastAction_ = "attack " + target->type->name;
        return;
    }
    // Wave spent: fall back and rebuild.
    if (armyValue(army) * 3 < waveStartValue_) {
        attacking_ = false;
        w_.commandMove(army, rallyPoint());
        lastAction_ = "retreat";
        return;
    }
    // Keep late units and idle ones heading for the target.
    std::vector<uint32_t> idle;
    for (uint32_t id : army)
        if (const Unit* u = w_.unit(id); u && !u->moving() && !u->target) idle.push_back(id);
    if (!idle.empty()) w_.commandAttackMove(idle, target->position);
}

void SkirmishAI::manageReapers() {
    // Reapers harvest on their own within their search radius; when nothing is left
    // there, walk them to the nearest pile.
    for (const Unit& u : w_.units()) {
        if (u.owner != player_ || u.underConstruction) continue;
        const EconomySpec& e = w_.economy(*u.type);
        if (!e.reaper || u.harvest != Unit::Harvest::None || u.moving()) continue;
        const Player* me = w_.player(player_);
        const ResourceNode* best = nullptr;
        float bestD = 1e30f;
        for (const ResourceNode& n : w_.resources()) {
            if (n.remaining <= 0 || !me || !n.usableBy(me->faction)) continue;
            const float d = dist2(n.position, u.position) + 0.5f * dist2(n.position, baseCentre());
            if (d < bestD) { bestD = d; best = &n; }
        }
        if (best) w_.commandMove({u.id}, best->position);
    }
}

void SkirmishAI::update() {
    if (++tick_ % kThinkEvery != player_ % kThinkEvery) return; // stagger players
    if (defeated(w_, player_)) return;
    manageConstruction();
    manageProduction();
    manageReapers();
    if (tick_ % (kThinkEvery * 4) < kThinkEvery) manageArmy(); // every 2 s
}

} // namespace alamo
