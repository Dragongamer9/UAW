#include "alamo/ai.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace alamo {

namespace {

float dist2(Vec3 a, Vec3 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

constexpr int kThinkEvery = kTicksPerSecond / 2;
constexpr int kMaxArmy = 160; // combat units alive

} // namespace

float unitStrength(const Unit& u); // below

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
    std::unordered_map<const GameObjectType*, bool> massProducers_, heroProducers;
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
                    // Line factories make the army; the HQs that train heroes are not duplicated.
                    if (bt) (t->category.find("Hero") == std::string::npos ? massProducers_ : heroProducers)[bt] = true;
                }
        }
    }
    // Member type -> squad type, for counting squads that builders place (Hierarchy science teams).
    if (const TechTree* tech = w_.tech())
        for (const std::string& obj : tech->objects())
            if (const GameObjectType* t = db_.find(obj))
                for (const GameObjectType* m : squadMembers(*t, db_)) squadOf_.emplace(m, t->name);
    for (const auto& [t, yes] : massProducers_)
        if (!heroProducers.count(t)) lineFactories_.insert(t);
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
    bool armed = !weaponsFor(t, db_).empty();
    for (const GameObjectType* m : squadMembers(t, db_)) armed = armed || !weaponsFor(*m, db_).empty();
    bool c = !isStructureType(t) && buildInfo(t, db_).spawnsUnit && armed;
    const EconomySpec& e = w_.economy(t);
    if (e.collector || e.reaper) c = false;
    const_cast<SkirmishAI*>(this)->combat_[&t] = c;
    return c;
}

bool SkirmishAI::isBuilderType(const GameObjectType& t) const {
    auto it = builder_.find(&t);
    if (it != builder_.end()) return it->second;
    bool b = !isStructureType(t) && w_.tech() &&
             !w_.tech()->constructibleBy(t.name, [](const std::string&) { return true; }).empty();
    // ...or a mobile unit listed as the Tactical_Builder of a structure (it builds on site: the
    // Military Build Bot).
    if (!b && !isStructureType(t) && w_.tech())
        for (const std::string& obj : w_.tech()->objects()) {
            const GameObjectType* ot = db_.find(obj);
            if (!ot || !isStructureType(*ot)) continue;
            for (const BuildOption& o : w_.tech()->options(obj))
                for (const std::string& name : o.builders) b = b || iequals(name, t.name);
            if (b) break;
        }
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
    else if (const std::string* er = t.find("Enables_Research"); er && (iequals(trim(*er), "yes") || iequals(trim(*er), "true")))
        r = Role::Research;
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

bool SkirmishAI::joinsNetwork(Vec3 p, float reach) const {
    bool anyNode = false;
    for (const Unit& u : w_.units()) {
        if (u.owner != player_ || u.underConstruction) continue;
        const EconomySpec& e = w_.economy(*u.type);
        if (e.power != EconomySpec::Power::Generator && e.power != EconomySpec::Power::Transmitter) continue;
        anyNode = true;
        if (u.powered && dist2(u.position, p) <= std::max(reach, e.transmitRadius) - 10) return true;
    }
    return !anyNode;
}

bool SkirmishAI::findSite(const GameObjectType& t, Vec3& out) {
    const Vec3 base = baseCentre();
    const float cx = w_.map().sizeX() / 2, cy = w_.map().sizeY() / 2;
    const float toCentre = std::atan2(cy - base[1], cx - base[0]);
    const EconomySpec& e = w_.economy(t);
    const bool needsPower = e.power == EconomySpec::Power::Receiver;
    const bool isNode = e.power == EconomySpec::Power::Generator || e.power == EconomySpec::Power::Transmitter;
    const float extent = float(t.number("Custom_Hard_XExtent").value_or(30));
    if (e.power == EconomySpec::Power::Transmitter && !joinsNetwork({-1e6f, -1e6f, 0}, 0)) {
        // Extend the network: a ring around each powered node, inside its transmit reach,
        // nodes nearest an unpowered receiver (else the map centre) first.
        Vec3 aim{cx, cy, 0};
        for (const Unit& u : w_.units())
            if (u.owner == player_ && u.structure && !u.underConstruction && !u.powered &&
                w_.economy(*u.type).power == EconomySpec::Power::Receiver) { aim = u.position; break; }
        std::vector<const Unit*> nodes;
        for (const Unit& u : w_.units()) {
            const EconomySpec& ne = w_.economy(*u.type);
            if (u.owner == player_ && u.powered && !u.underConstruction &&
                (ne.power == EconomySpec::Power::Generator || ne.power == EconomySpec::Power::Transmitter))
                nodes.push_back(&u);
        }
        std::sort(nodes.begin(), nodes.end(), [&](const Unit* a, const Unit* b) { return dist2(a->position, aim) < dist2(b->position, aim); });
        for (const Unit* n : nodes) {
            const float reach = std::max(e.transmitRadius, w_.economy(*n->type).transmitRadius) - 20;
            const float toAim = std::atan2(aim[1] - n->position[1], aim[0] - n->position[0]);
            for (float frac : {0.55f, 0.8f, 0.35f})
                for (int k = 0; k < 12; ++k) {
                    const float a = toAim + (k % 2 ? 1 : -1) * float((k + 1) / 2) * 0.5236f;
                    const Vec3 p{n->position[0] + std::cos(a) * reach * frac, n->position[1] + std::sin(a) * reach * frac, 0};
                    if (p[0] < 100 || p[1] < 100 || p[0] > w_.map().sizeX() - 100 || p[1] > w_.map().sizeY() - 100) continue;
                    if (w_.siteClear(t, p)) { out = p; return true; }
                }
        }
        return false;
    }
    if (needsPower) {
        // Receivers: inside a powered node's field, nodes nearest the base first.
        std::vector<const Unit*> nodes;
        for (const Unit& u : w_.units()) {
            const EconomySpec& ne = w_.economy(*u.type);
            if (u.owner == player_ && u.powered && !u.underConstruction && ne.powerRadius > 0 &&
                (ne.power == EconomySpec::Power::Generator || ne.power == EconomySpec::Power::Transmitter))
                nodes.push_back(&u);
        }
        std::sort(nodes.begin(), nodes.end(), [&](const Unit* a, const Unit* b) { return dist2(a->position, base) < dist2(b->position, base); });
        for (const Unit* n : nodes) {
            const float r = w_.economy(*n->type).powerRadius;
            for (float frac : {0.45f, 0.7f, 0.9f})
                for (int k = 0; k < 12; ++k) {
                    const int j = (k + siteSpin_) % 12;
                    const float a = toCentre + float(j) * 0.5236f;
                    const Vec3 p{n->position[0] + std::cos(a) * r * frac, n->position[1] + std::sin(a) * r * frac, 0};
                    if (p[0] < 100 || p[1] < 100 || p[0] > w_.map().sizeX() - 100 || p[1] > w_.map().sizeY() - 100) continue;
                    if (!coveredByPower(p, extent * 0.5f) || !w_.siteClear(t, p)) continue;
                    out = p;
                    ++siteSpin_;
                    return true;
                }
        }
        return false;
    }
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
            // A transmitter has to reach the powered network (or there is no network yet).
            if (e.power == EconomySpec::Power::Transmitter && !joinsNetwork(p, e.transmitRadius)) continue;
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
    if (std::getenv("UAW_AI_DEBUG")) std::fprintf(stderr, "[%.0fs p%d] construct %s: %d\n", w_.seconds(), player_, type.c_str(), int(r));
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
    int roleCount[7] = {0, 0, 0, 0, 0, 0, 0};
    bool unpoweredReceiver = false, haveGenerator = false, nodeComing = false;
    for (const Unit& u : w_.units()) {
        if (u.owner != player_) continue;
        ++have[u.type->name];
        // Squads appear as their members: count them as the squad type too.
        if (auto sq = squadOf_.find(u.type); sq != squadOf_.end()) ++have[sq->second];
        if (u.structure || w_.economy(*u.type).reaper || roleOf(*u.type) == Role::Production) ++roleCount[int(roleOf(*u.type))];
        if (w_.economy(*u.type).power == EconomySpec::Power::Generator) haveGenerator = true;
        if (u.structure && u.underConstruction && roleOf(*u.type) == Role::Power) nodeComing = true;
        if (u.structure && !u.underConstruction && !u.powered && w_.economy(*u.type).power == EconomySpec::Power::Receiver)
            unpoweredReceiver = true;
    }
    bool planned = false, anyBuilder = false;
    for (const Unit& u : w_.units()) anyBuilder = anyBuilder || (u.owner == player_ && isBuilderType(*u.type));
    if (!anyBuilder) reserve_ = 0;
    // Ids first: constructing spawns the site, which may reallocate the unit list.
    std::vector<uint32_t> idle;
    for (const Unit& u : w_.units())
        if (u.owner == player_ && !u.structure && !u.underConstruction && !u.constructing && u.path.empty()) idle.push_back(u.id);
    for (uint32_t bid : idle) {
        Unit* bp = w_.unit(bid);
        if (!bp) continue;
        Unit& b = *bp;
        const auto options = w_.constructible(b.id);
        if (options.empty()) continue;
        // Desired counts per role, growing with time.
        const int wantEconomy = 2 + int(minutes / 2.5f);
        // Passive income structures that pay for themselves quickly (Military Supply, Masari Matter
        // Engines) are worth many more, early: all the more the sooner they pay back (under 45 s
        // fully, none past 90 s).
        float bestPayback = 1e9f;
        for (const std::string& o : options) {
            const GameObjectType* t = db_.find(o);
            if (!t || roleOf(*t) != Role::Economy) continue;
            const EconomySpec& e = w_.economy(*t);
            if (e.passivePerSecond <= 0 || (!e.passiveFaction.empty() && !iequals(e.passiveFaction, me->faction))) continue;
            const float rate = e.passivePerSecond * w_.balance().of(me->faction).income * me->incomeBonus;
            if (rate > 0) bestPayback = std::min(bestPayback, buildInfo(*t, db_).cost / rate);
        }
        const int wantQuickEconomy = int((3 + minutes * 1.5f) * std::clamp((90 - bestPayback) / 45, 0.f, 1.f));
        const int wantDefense = minutes > 4 ? 1 + int((minutes - 4) / 4) : 0;
        bool planning = false; // true: ignore credits (what would we build next?)
        auto pick = [&](Role role, int limitOfType, bool generatorOnly = false, bool lineOnly = false) -> std::string {
            std::string best;
            int bestHave = 1 << 30;
            float bestCost = 0;
            for (const std::string& o : options) {
                const GameObjectType* t = db_.find(o);
                if (!t || roleOf(*t) != role) continue;
                if (generatorOnly && w_.economy(*t).power != EconomySpec::Power::Generator) continue;
                if (lineOnly && !lineFactories_.count(t)) continue;
                const int n = have.count(o) ? have.at(o) : 0;
                if (n >= limitOfType) continue;
                if (auto bl = blockedUntil_.find(o); bl != blockedUntil_.end() && bl->second > w_.seconds()) continue;
                const float cost = buildInfo(*t, db_).cost;
                if (cost > me->credits && !planning) continue;
                // Spread across types; among equals take the cheaper (usually the basic unit factory).
                // Tech structures that something else requires (the way to new units) go first.
                bool opens = false;
                if (role == Role::Tech) {
                    const std::string* into = t->find("Tactical_Buildable_Constructed");
                    opens = unlocksUnit(into ? trim(*into) : o);
                }
                const float rankCost = opens ? cost - 1e6f : cost;
                if (n < bestHave || (n == bestHave && rankCost < bestCost)) { bestHave = n; bestCost = rankCost; best = o; }
            }
            return best;
        };
        // The build order, as a priority list. With `planning` set it ignores credits.
        auto next = [&]() {
            std::string c;
            // Novus: a generator first, and another node whenever a receiver sits unpowered.
            if (!haveGenerator) c = pick(Role::Power, 99, true);
            // One node at a time: wait for the one being built before adding another.
            if (c.empty() && !nodeComing && (roleCount[int(Role::Power)] == 0 || unpoweredReceiver || networkNeeded_))
                c = pick(Role::Power, 99);
            if (c.empty() && roleCount[int(Role::Economy)] < std::min(wantEconomy, 3)) c = pick(Role::Economy, 99);
            if (c.empty()) c = pick(Role::Production, 1);
            if (c.empty() && roleCount[int(Role::Economy)] < wantQuickEconomy) c = pick(Role::Economy, 99);
            if (c.empty() && minutes > 4 && roleCount[int(Role::Defense)] < 1) c = pick(Role::Defense, 99);
            // Then every tech structure once (labs, advanced factories, detectors, superweapons),
            // cheapest first, so the whole roster comes into play.
            if (c.empty() && minutes > 4) c = pick(Role::Tech, 1);
            // Units builders place in the field (Hierarchy scan drones), one each.
            if (c.empty() && minutes > 4) c = pick(Role::Other, 1);
            // Research facilities (where a mod makes them buildable) research a second suite at once.
            if (c.empty() && w_.researchTree(player_) && roleCount[int(Role::Research)] < 1 && minutes > 5) c = pick(Role::Research, 1);
            if (c.empty() && roleCount[int(Role::Economy)] < wantEconomy) c = pick(Role::Economy, 99);
            if (c.empty() && roleCount[int(Role::Defense)] < wantDefense) c = pick(Role::Defense, 99);
            return c;
        };
        // The next structure in the build order, whatever it costs. While it is unaffordable nothing
        // cheaper further down the list jumps the queue, and production keeps its price in the bank
        // (see manageProduction), so economy, tech, defences and superweapons all get their turn.
        planning = true;
        const std::string want = next();
        planning = false;
        const GameObjectType* wt = want.empty() ? nullptr : db_.find(want);
        if (!planned) {
            planned = true;
            reserve_ = wt ? buildInfo(*wt, db_).cost : 0;
        }
        std::string choice;
        if (wt) {
            if (buildInfo(*wt, db_).cost <= me->credits) choice = want;
        } else {
            // Build order done: more factories as the game goes on (the mods' population costs are
            // 0, so armies are limited by how fast they can be built), and more economy when rich.
            if (minutes > 2 && rich > 0.35f) choice = pick(Role::Production, 1 + int(minutes / 3), false, true);
            if (choice.empty() && minutes > 2 && rich > 0.25f) choice = pick(Role::Production, 1 + int(rich / 0.2f), false, true);
            if (choice.empty() && minutes > 3 && rich > 0.4f) choice = pick(Role::Economy, 99);
        }
        if (choice.empty()) continue;
        if (!tryConstruct(b, choice)) {
            // No site (or not allowed): try something else for half a minute. A receiver with no
            // powered ground left asks for the network to be extended.
            blockedUntil_[choice] = w_.seconds() + 30;
            if (std::getenv("UAW_AI_DEBUG")) std::fprintf(stderr, "[%.0fs p%d] no site for %s\n", w_.seconds(), player_, choice.c_str());
            if (w_.economy(*db_.find(choice)).power == EconomySpec::Power::Receiver) networkNeeded_ = true;
        } else {
            if (roleOf(*db_.find(choice)) == Role::Power) {
                networkNeeded_ = false;
                nodeComing = true;
            }
            reserve_ = 0;
            ++have[choice];
            ++roleCount[int(roleOf(*db_.find(choice)))];
        }
    }
}

void SkirmishAI::manageProduction() {
    const Player* me = w_.player(player_);
    if (!me) return;
    int builders = 0, army = 0;
    for (const Unit& u : w_.units()) {
        if (u.owner != player_) continue;
        if (isBuilderType(*u.type)) ++builders;
        else if (!u.structure && isCombatUnit(*u.type)) ++army;
    }
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
        // A spare builder first, then a random affordable combat unit,
        // weighted toward what we can buy several of.
        std::string choice;
        // Spare builders: two, more while money piles up faster than they can spend it.
        const int wantBuilders = rich > 0.8f ? 4 : rich > 0.5f ? 3 : 2;
        if (builders < wantBuilders)
            for (const auto& o : options)
                if (const GameObjectType* t = db_.find(o); t && isBuilderType(*t) && buildInfo(*t, db_).cost <= me->credits) {
                    choice = o;
                    ++builders;
                    break;
                }
        // Units and upgrades leave the bank's reserves alone once a defence is standing: the next
        // structure, the next research suite and an expensive unit another structure saves for.
        if (savingFor_ && !w_.unit(savingFor_)) savingFor_ = 0;
        const float unitReserve = savingFor_ && savingFor_ != s.id ? unitReserve_ : 0;
        float reserve = army >= 4 || w_.seconds() > 240 ? std::max({reserve_, researchReserve_, unitReserve}) : 0;
        // A capped bank (Masari) can't hold more than its cap: never reserve past it.
        if (const float cap = w_.bankCap(player_); cap > 0) reserve = std::min(reserve, cap);
        const float spend = me->credits - reserve;
        // Now and then an upgrade instead of a unit (only with nothing else in the queue).
        if (choice.empty() && s.queue.empty() && (rand(3) == 0 || unlockerOffered(options))) choice = pickUpgrade(s, options, spend);
        // The mods set most population costs to 0, so cap the army ourselves: past this the
        // simulation slows down and nothing is gained.
        if (choice.empty() && army < kMaxArmy) choice = pickUnit(s, options, spend);
        if (!choice.empty()) {
            const OrderResult r = w_.queueProduction(s.id, choice);
            if (r == OrderResult::Ok) {
                lastAction_ = "queue " + choice;
                ++made_[choice];
            } else {
                // Refused (a build limit, a unique hero already out...): its turn passes, so the
                // rotation doesn't stall on it.
                ++made_[choice];
                if (std::getenv("UAW_AI_DEBUG"))
                    std::fprintf(stderr, "[%.0fs p%d] %s refused %s: %s\n", w_.seconds(), player_, s.type->name.c_str(), choice.c_str(), toString(r));
            }
        }
    }
}

bool SkirmishAI::isUtilityUnit(const GameObjectType& t) const {
    // Unarmed units with a job (scouts, hackers, field inverters, science teams): a few each.
    if (isStructureType(t) || isCombatUnit(t) || isBuilderType(t) || !buildInfo(t, db_).spawnsUnit) return false;
    const EconomySpec& e = w_.economy(t);
    return !e.collector && !e.reaper && e.drones == 0;
}

bool SkirmishAI::hitsAir(const GameObjectType& t) const {
    if (auto it = hitsAir_.find(&t); it != hitsAir_.end()) return it->second;
    static const std::vector<std::string> aircraft{"FLYING", "MEDIUM"};
    bool air = false;
    auto check = [&](const GameObjectType& u) {
        for (const WeaponSpec& w : weaponsFor(u, db_)) air = air || !w.restrictions || !hasAnyCategory(aircraft, *w.restrictions);
    };
    check(t);
    for (const GameObjectType* m : squadMembers(t, db_)) check(*m);
    return hitsAir_[&t] = air;
}

float SkirmishAI::armorEffect(const GameObjectType& t, const std::string& armor) const {
    const std::string key = t.name + "|" + armor;
    if (auto it = armorEffect_.find(key); it != armorEffect_.end()) return it->second;
    float sum = 0, dpsAll = 0;
    auto add = [&](const GameObjectType& u) {
        for (const WeaponSpec& w : weaponsFor(u, db_)) {
            const float dps = w.damagePerSecond();
            dpsAll += dps;
            sum += dps * (w_.damageTable() ? w_.damageTable()->modifier(w.damageType, armor) : 1.f);
        }
    };
    if (const auto members = squadMembers(t, db_); !members.empty())
        for (const GameObjectType* m : members) add(*m);
    else
        add(t);
    return armorEffect_[key] = dpsAll > 0 ? sum / dpsAll : 1.f;
}

std::string SkirmishAI::pickUnit(const Unit& s, const std::vector<std::string>& options, float credits) {
    // Whole roster in rotation: each type's turn comes round in proportion to its combat value
    // per credit (never below a quarter of the best), so every unit gets built and the efficient
    // ones most. Utility units are kept at one or two each. An expensive type whose turn has come
    // is saved for (one structure at a time, for up to a minute) instead of skipped forever.
    const float rich = wealth();
    std::unordered_map<std::string, int> alive;
    float enemyAir = 0, enemyAll = 0; // the enemy's air power as far as we know it: counter it
    std::map<std::string, float> enemyArmor; // and its armor: what our weapons hurt most (ordered: sums must not depend on hashing)
    for (const Unit& u : w_.units()) {
        if (u.owner == player_) ++alive[u.type->name];
        else if (w_.enemies(player_, u.owner) && u.owner >= 0 && !u.structure &&
                 (!w_.fogOfWar() || knownUnits_.count(u.id))) {
            const float v = unitStrength(u);
            enemyAll += v;
            if (u.flying()) enemyAir += v;
            if (!u.armorType.empty()) enemyArmor[u.armorType] += v;
        }
    }
    const float airShare = enemyAll > 0 ? enemyAir / enemyAll : 0;
    struct Cand { std::string name; float cost, perCredit; bool utility; };
    std::vector<Cand> cands;
    float bestPerCredit = 0;
    for (const auto& o : options) {
        const GameObjectType* t = db_.find(o);
        if (!t) continue;
        const bool combat = isCombatUnit(*t), utility = !combat && isUtilityUnit(*t);
        if (!combat && !utility) continue;
        if (utility && (alive[o] >= (rich > 0.5f ? 2 : 1) || w_.seconds() < 240)) continue;
        if (const auto limit = t->number("Build_Limit_Current_Per_Player"); limit && *limit > 0 && w_.countOwned(player_, *t) >= int(*limit))
            continue; // a hero already out, etc.
        const float cost = buildInfo(*t, db_).cost;
        const float perCredit = combat ? typeStrength(*t) / std::max(1.f, cost) : 0;
        bestPerCredit = std::max(bestPerCredit, perCredit);
        cands.push_back({o, cost, perCredit, utility});
    }
    if (cands.empty()) return {};
    // Against the armor we have seen: each combat type's damage modifier, averaged over the enemy
    // army by strength, relative to the average candidate (x0.5 to x2).
    std::unordered_map<std::string, float> counter;
    if (enemyAll > 0 && !enemyArmor.empty() && w_.damageTable()) {
        float mean = 0;
        int n = 0;
        for (const Cand& c : cands) {
            if (c.utility) continue;
            const GameObjectType* t = db_.find(c.name);
            if (!t) continue;
            float e = 0;
            for (const auto& [armor, v] : enemyArmor) e += v * armorEffect(*t, armor);
            e /= enemyAll;
            counter[c.name] = e;
            mean += e;
            ++n;
        }
        if (n > 0 && mean > 0) {
            mean /= float(n);
            for (auto& [k, e] : counter) e = std::clamp(e / mean, 0.5f, 2.f);
        }
    }
    auto key = [&](const Cand& c) {
        float weight = c.utility ? 0.5f : std::clamp(c.perCredit / std::max(1e-6f, bestPerCredit), 0.25f, 1.f);
        if (auto it = counter.find(c.name); it != counter.end()) weight *= it->second;
        if (!c.utility && airShare > 0.2f) { // aircraft about: favour what can shoot them down
            const GameObjectType* t = db_.find(c.name);
            weight *= t && hitsAir(*t) ? 1.f + 2.f * airShare : 1.f - 0.6f * airShare;
        }
        const auto it = made_.find(c.name);
        return float((it == made_.end() ? 0 : it->second) + 1) / weight;
    };
    std::sort(cands.begin(), cands.end(), [&](const Cand& x, const Cand& y) {
        const float kx = key(x), ky = key(y);
        return kx != ky ? kx < ky : x.name < y.name;
    });
    const Cand& turn = cands.front();
    if (std::getenv("UAW_AI_DEBUG"))
        std::fprintf(stderr, "[%.0fs p%d] %s: turn %s (%.0f), spend %.0f, saving %u\n", w_.seconds(), player_, s.type->name.c_str(),
                     turn.name.c_str(), turn.cost, credits, savingFor_);
    if (turn.cost <= credits) {
        if (savingFor_ == s.id) { savingFor_ = 0; unitReserve_ = 0; }
        return turn.name;
    }
    // Save up for it when that is realistic (within three times the faction's starting bank); the
    // other structures then only spend what is above it. Give up after a minute and a half.
    const bool canSave = turn.cost <= 3 * scale_ && (savingFor_ == 0 || savingFor_ == s.id);
    if (canSave) {
        if (savingFor_ != s.id) { savingFor_ = s.id; savingSince_ = w_.seconds(); unitReserve_ = turn.cost; }
        if (w_.seconds() - savingSince_ < 90) return {};
        made_[turn.name] += 1; // gave up this time; its next turn comes later
        savingFor_ = 0;
        unitReserve_ = 0;
    }
    for (const Cand& c : cands)
        if (c.cost <= credits) return c.name;
    return {};
}

bool SkirmishAI::unlockerOffered(const std::vector<std::string>& options) const {
    for (const auto& o : options)
        if (const GameObjectType* t = db_.find(o); t && !buildInfo(*t, db_).spawnsUnit && unlocksUnit(o) && !w_.owns(player_, o)) return true;
    return false;
}

bool SkirmishAI::unlocksUnit(const std::string& upgrade) const {
    // Some unit of ours lists it among its requirements.
    auto it = unlocks_.find(upgrade);
    if (it != unlocks_.end()) return it->second;
    bool r = false;
    if (const TechTree* tech = w_.tech())
        for (const std::string& obj : tech->objects()) {
            for (const BuildOption& o : tech->options(obj))
                for (const std::string& req : o.required) r = r || iequals(req, upgrade);
            if (r) break;
        }
    const_cast<SkirmishAI*>(this)->unlocks_[upgrade] = r;
    return r;
}

std::string SkirmishAI::pickUpgrade(const Unit& s, const std::vector<std::string>& options, float credits) {
    // Upgrades and hardpoints bought at this structure (or walker): once the army is under way
    // and credits allow, the cheapest one not yet owned.
    // Upgrades that some unit needs (Instance Generator -> Reflex Trooper) come first and need
    // only the money; the rest wait for a comfortable bank.
    if (w_.seconds() < 180) return {};
    const bool comfortable = w_.seconds() >= 300 && wealth() >= 0.3f;
    std::string best;
    float bestCost = 1e30f;
    bool bestUnlocks = false;
    for (const auto& o : options) {
        const GameObjectType* t = db_.find(o);
        if (!t || buildInfo(*t, db_).spawnsUnit) continue;
        if (s.structure && w_.owns(player_, o)) continue; // walkers each fill their own sockets
        bool owned = false;
        for (const GameObjectType* u : s.upgrades) owned = owned || u == t;
        if (owned) continue;
        const bool unlocks = unlocksUnit(o);
        if (!unlocks && !comfortable) continue;
        const float cost = buildInfo(*t, db_).cost;
        if (cost > credits * (unlocks ? 1.f : 0.6f)) continue;
        if ((unlocks && !bestUnlocks) || (unlocks == bestUnlocks && cost < bestCost)) { bestCost = cost; best = o; bestUnlocks = unlocks; }
    }
    return best;
}

float SkirmishAI::typeStrength(const GameObjectType& t) const {
    auto it = strength_.find(&t);
    if (it != strength_.end()) return it->second;
    float v = 0;
    if (const auto members = squadMembers(t, db_); !members.empty()) {
        for (const GameObjectType* m : members) v += typeStrength(*m);
    } else {
        float dps = 0;
        for (const WeaponSpec& w : weaponsFor(t, db_)) dps += w.damagePerSecond();
        const float health = float(t.number("Tactical_Health").value_or(100));
        v = std::sqrt(std::max(0.f, std::min(health, 20000.f)) * dps);
    }
    const_cast<SkirmishAI*>(this)->strength_[&t] = v;
    return v;
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

bool SkirmishAI::sees(const Unit& e) const {
    return !w_.fogOfWar() || w_.visibleTo(player_, e.position);
}

void SkirmishAI::updateIntel() {
    if (!w_.fogOfWar()) return;
    World& w = w_;
    for (const Unit& e : w_.units()) {
        if (!w_.enemies(player_, e.owner) || e.owner < 0 || e.inside) continue;
        // At the start: where the enemies' bases are (start positions are no secret).
        if ((!intelReady_ && e.structure) || (sees(e) && !e.cloaked)) {
            if (e.structure) knownStructures_[e.id] = e.position;
            else knownUnits_[e.id] = unitStrength(e);
        }
    }
    intelReady_ = true;
    // Forget what is gone: dead, or not where it was when we look there again.
    for (auto it = knownStructures_.begin(); it != knownStructures_.end();) {
        const Unit* u = w.unit(it->first);
        const bool gone = !u || !u->alive() || (w_.visibleTo(player_, it->second) && !w_.enemies(player_, u->owner));
        it = gone ? knownStructures_.erase(it) : std::next(it);
    }
    for (auto it = knownUnits_.begin(); it != knownUnits_.end();) {
        const Unit* u = w.unit(it->first);
        it = !u || !u->alive() || !w_.enemies(player_, u->owner) ? knownUnits_.erase(it) : std::next(it);
    }
}

float SkirmishAI::enemyStrength() const {
    if (w_.fogOfWar()) { // what it has seen of the enemy's forces
        float v = 0;
        for (const auto& [id, s] : knownUnits_) v += s;
        return v;
    }
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

    // A wave abroad comes home when a real force (not a straggler) is wrecking the base.
    if (attacking_) {
        float threat = 0;
        Vec3 where{};
        std::unordered_set<uint32_t> counted;
        for (const Unit& s : w_.units()) {
            if (s.owner != player_ || !s.structure) continue;
            for (uint32_t id : w_.unitsNear(s.position, 650)) {
                const Unit* e = w_.unit(id);
                if (!e || !w_.enemies(player_, e->owner) || e->owner < 0 || e->structure || !sees(*e) || !counted.insert(id).second) continue;
                const float v = unitStrength(*e);
                if (v > 0) where = e->position;
                threat += v;
            }
        }
        if (threat > 0.4f * armyValue(army)) {
            attacking_ = false;
            w_.commandAttackMove(army, where);
            lastAction_ = "recall";
            return;
        }
    }
    // Defend: enemies near any of our structures take priority (a wave already under way carries on
    // unless recalled above; the base's own defences and new units deal with stragglers).
    if (!attacking_)
        for (const Unit& s : w_.units()) {
            if (s.owner != player_ || !s.structure) continue;
            for (uint32_t id : w_.unitsNear(s.position, 650)) {
                const Unit* e = w_.unit(id);
                if (!e || !w_.enemies(player_, e->owner) || e->owner < 0 || !sees(*e) || e->cloaked) continue;
                w_.commandAttackMove(army, e->position);
                lastAction_ = "defend";
                return;
            }
        }

    const float minutes = w_.seconds() / 60;
    const Player* me = w_.player(player_);
    const bool capped = me && w_.popUsed(player_) >= me->popCap - 4 && me->popCap > 0;
    if (!attacking_) {
        // Go when clearly stronger than everything the enemy has in the field (and not
        // before minute 4 unless overwhelming), or when we can't grow any more.
        const float value = armyValue(army);
        // Under fog, an enemy army never seen isn't no army: assume a fair match until scouted.
        float enemy = enemyStrength();
        if (w_.fogOfWar() && knownUnits_.empty()) enemy = std::max(enemy, 0.75f * value);
        // A wave lost makes it more careful (x1.4 each, up to x2.5), easing back over five minutes.
        caution_ = std::max(1.f, caution_ - (w_.seconds() - cautionAt_) / 300.f);
        cautionAt_ = w_.seconds();
        const bool ready = (minutes >= 4 && army.size() >= 6 && value >= 1.3f * caution_ * enemy) ||
                           (minutes >= 3 && army.size() >= 4 && value >= 3 * enemy + 1) ||
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
    if (target && w_.seconds() - targetSince_ > 60) { // no harm done in a minute: can't get at it
        if (target->health >= targetHealth_) {
            givenUp_[target->id] = w_.seconds() + 180;
            target = nullptr;
        } else {
            targetSince_ = w_.seconds();
            targetHealth_ = target->health;
        }
    }
    if (!target || (w_.fogOfWar() && !target->structure && !sees(*target))) {
        target = nullptr;
        float best = 1e30f;
        for (int pass = 0; pass < 2 && !target; ++pass)
            for (const Unit& e : w_.units()) {
                if (!w_.enemies(player_, e.owner) || (pass == 0 && !e.structure)) continue;
                if (auto g = givenUp_.find(e.id); g != givenUp_.end() && g->second > w_.seconds()) continue;
                if (w_.fogOfWar() && !(e.structure ? knownStructures_.count(e.id) != 0 : sees(e) && !e.cloaked)) continue;
                const float d = dist2(e.position, centroid);
                if (d < best) { best = d; target = &e; }
            }
        if (!target && w_.fogOfWar()) {
            // Nothing known: sweep the map, a 3x3 grid of points, moving on when the army gets close to
            // one or after 40 s; every unit attack-moves there.
            const float sx = w_.map().sizeX(), sy = w_.map().sizeY();
            static const int order[9] = {4, 0, 2, 8, 6, 1, 5, 7, 3};
            auto leg = [&](int k) {
                const int c = order[k % 9];
                return Vec3{sx * (1 + 2 * float(c % 3)) / 6, sy * (1 + 2 * float(c / 3)) / 6, 0};
            };
            const float now = w_.seconds();
            if (scoutLeg_ == 0 || dist2(centroid, leg(scoutLeg_ - 1)) < 400 || now - scoutSince_ > 40) {
                w_.commandAttackMove(army, leg(scoutLeg_));
                ++scoutLeg_;
                scoutSince_ = now;
                lastAction_ = "scout";
            }
            return;
        }
        if (!target) { attacking_ = false; return; }
        attackTarget_ = target->id;
        targetSince_ = w_.seconds();
        targetHealth_ = target->health;
        w_.commandAttackMove(army, target->position);
        lastAction_ = "attack " + target->type->name;
        return;
    }
    // Wave spent: fall back and rebuild.
    if (armyValue(army) * 3 < waveStartValue_) {
        attacking_ = false;
        caution_ = std::min(2.5f, caution_ * 1.4f);
        cautionAt_ = w_.seconds();
        w_.commandMove(army, rallyPoint());
        lastAction_ = "retreat";
        return;
    }
    // Keep late units and idle ones heading for the target; those standing at it attack it outright
    // (units don't pick some targets on their own, e.g. Masari collectors).
    std::vector<uint32_t> idle, there;
    for (uint32_t id : army)
        if (const Unit* u = w_.unit(id); u && !u->moving() && !u->target)
            (dist2(u->position, target->position) < target->radius + u->maxRange() + 120 ? there : idle).push_back(id);
    if (!idle.empty()) w_.commandAttackMove(idle, target->position);
    if (!there.empty()) w_.commandAttack(there, target->id);
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
    updateIntel();
    static const double profile = std::getenv("UAW_PROFILE") ? std::atof(std::getenv("UAW_PROFILE")) : 0;
    using Clock = std::chrono::steady_clock;
    const char* names[8];
    Clock::time_point marks[9];
    int nm = 0;
    auto mark = [&](const char* n) { if (profile > 0) { names[nm] = n; marks[nm++] = Clock::now(); } };
    mark("construction");
    manageConstruction();
    mark("production");
    manageProduction();
    mark("reapers");
    manageReapers();
    mark("abilities");
    manageAbilities();
    if (tick_ % (kThinkEvery * 4) < kThinkEvery) { // every 2 s
        mark("army");
        manageArmy();
        mark("research");
        manageResearch();
        mark("superweapons");
        manageSuperweapons();
    }
    mark("end");
    if (profile > 0) {
        const double total = std::chrono::duration<double, std::milli>(marks[nm - 1] - marks[0]).count();
        if (total > profile) {
            std::fprintf(stderr, "ai %d at tick %llu: %.1f ms:", player_, (unsigned long long)w_.tickCount(), total);
            for (int k = 0; k + 1 < nm; ++k)
                std::fprintf(stderr, " %s %.1f", names[k], std::chrono::duration<double, std::milli>(marks[k + 1] - marks[k]).count());
            std::fprintf(stderr, "\n");
        }
    }
}

void SkirmishAI::manageSuperweapons() {
    // Fire each ready superweapon where it catches the most enemy value (build cost of what is
    // inside its radius), less what of ours would be caught; never at nothing.
    for (const Unit& e : w_.units()) {
        if (e.owner != player_ || !e.structure || w_.superweaponCountdown(e.id) != 0) continue;
        const SuperweaponSpec* spec = w_.superweaponOf(*e.type);
        if (!spec) continue;
        const float r = spec->radius;
        Vec3 best{};
        float bestScore = 0;
        for (const Unit& c : w_.units()) {
            if (!w_.enemies(player_, c.owner) || c.owner < 0) continue;
            if (w_.fogOfWar() && !(c.structure ? knownStructures_.count(c.id) != 0 : sees(c))) continue; // aims at what it knows
            float score = 0;
            for (const Unit& o : w_.units()) {
                if (dist2(o.position, c.position) > r + o.radius) continue;
                const float v = std::max(50.f, buildInfo(*o.type, db_).cost);
                if (w_.enemies(player_, o.owner) && o.owner >= 0) score += v;
                else if (!w_.enemies(player_, o.owner)) score -= 2 * v; // own and allied
            }
            if (score > bestScore) { bestScore = score; best = c.position; }
        }
        // Worth a shot: about a factory's worth of enemy stuff (prices scale with the faction).
        if (bestScore >= 0.15f * scale_ && w_.fireSuperweapon(e.id, best) == OrderResult::Ok)
            lastAction_ = "superweapon " + spec->weapon->name;
    }
}

void SkirmishAI::manageResearch() {
    const Player* me = w_.player(player_);
    if (!me || w_.seconds() < 120) return; // the economy first
    // ...and an army: research is a luxury until there is something to defend with.
    int army = 0;
    for (const Unit& u : w_.units())
        if (u.owner == player_ && !u.structure && isCombatUnit(*u.type)) ++army;
    if (army < 6 && w_.seconds() < 480) return;
    const auto options = w_.availableResearch(player_);
    researchReserve_ = 0;
    if (options.empty()) return;
    // Suites that unlock units (heroes) first, then the lowest tier; ties in turn (seeded) so the
    // tree fills out evenly. Placeholder suites that change nothing are skipped.
    auto unlocksUnits = [](const ResearchSuite* s) {
        for (const ResearchChange& c : s->changes)
            if (c.kind == ResearchChange::Kind::Type && !c.locks) return true;
        return false;
    };
    int best = 1 << 30;
    auto rank = [&](const ResearchSuite* s) { return s->index * 2 - (unlocksUnits(s) ? 3 : 0); };
    for (const ResearchSuite* s : options)
        if (!s->changes.empty()) best = std::min(best, rank(s));
    std::vector<const ResearchSuite*> tier;
    for (const ResearchSuite* s : options)
        if (!s->changes.empty() && rank(s) == best) tier.push_back(s);
    if (tier.empty()) return;
    const ResearchSuite* pick = tier[rand(uint32_t(tier.size()))];
    // Production keeps the suite's price in the bank until it can be started.
    researchReserve_ = pick->cost;
    if (me->credits < pick->cost) return;
    if (w_.startResearch(player_, pick->path, pick->index) == OrderResult::Ok) {
        lastAction_ = "research " + pick->id() + " " + pick->branch;
        researchReserve_ = 0;
    }
}

void SkirmishAI::manageAbilities() {
    const EffectLibrary* lib = w_.effectLibrary();
    std::vector<std::pair<uint32_t, size_t>> todo;
    for (const Unit& u : w_.units()) {
        if (u.owner != player_ || u.abilities.empty() || u.silenced || u.underConstruction) continue;
        const World::TypeAbilities& ta = w_.abilitiesOf(*u.type);
        for (size_t i = 0; i < u.abilities.size(); ++i) {
            const AbilitySpec& s = ta.abilities[u.abilities[i].index];
            if (s.autonomous || u.abilities[i].cooldown > 0 || s.kind == AbilitySpec::Kind::Unsupported || u.leaping) continue;
            if (s.kind == AbilitySpec::Kind::FooMode) {
                // Saucers: fight when there is an enemy in reach, else mend hurt machines nearby.
                bool mend = false;
                if (!u.target)
                    for (const Unit::WeaponState& ws : u.weapons) {
                        if (ws.spec.fooFighters == 0) continue;
                        for (uint32_t id : w_.unitsNear(u.position, u.weaponRange(ws))) {
                            const Unit* o = w_.unit(id);
                            if (o && o->id != u.id && o->owner == u.owner && !o->structure && o->health < 0.8f * o->maxHealth && w_.canHit(ws, *o)) {
                                mend = true;
                                break;
                            }
                        }
                    }
                if (mend != u.fooHeal) todo.push_back({u.id, i});
                continue;
            }
            if (s.kind == AbilitySpec::Kind::Place || s.substitute) continue; // default fire modes
            if (s.aiHealthBelow > 0 && u.health >= s.aiHealthBelow * u.maxHealth) continue;
            if (s.kind == AbilitySpec::Kind::Switch) {
                // Masari: dark (ice) mode's armor when hurt, light (fire) mode's damage otherwise.
                // Other mode switches (Orlok's siege mode, shield modes) are left alone for now.
                if (!s.elemental) continue;
                std::string name = u.type->name;
                for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
                const bool ice = name.size() > 4 && name.compare(name.size() - 4, 4, "_ICE") == 0;
                const float hp = u.health / std::max(1.f, u.maxHealth);
                if ((!ice && hp < 0.5f && u.target) || (ice && hp > 0.9f && !u.target)) todo.push_back({u.id, i});
                continue;
            }
            // Blowing up only where the data's AI hints call for it (low health).
            if ((s.kind == AbilitySpec::Kind::SelfDestruct || (s.kind == AbilitySpec::Kind::Leap && s.selfDestructOnLanding)) &&
                s.aiHealthBelow <= 0)
                continue;
            // Never sacrifice or cripple the unit (generators that destroy their user or switch off its
            // weapons / movement: weapon-mode toggles, deploy modes).
            bool selfDestroy = false;
            if (lib && s.kind == AbilitySpec::Kind::Generator)
                for (const GeneratorDef& g : ta.generators)
                    if (iequals(g.name, s.generator))
                        for (const GeneratorEntry& e : g.entries)
                            if (e.mask & GatherSelf)
                                if (const EffectDef* d = lib->find(e.effect))
                                    for (const EffectComponent& c : d->components)
                                        selfDestroy = selfDestroy || c.op == EffectOp::Destroy || c.op == EffectOp::DisableBehavior;
            if (selfDestroy) continue;
            // Offensive and self abilities only in a fight.
            if (!s.targetsAllies && !u.target) continue;
            todo.push_back({u.id, i});
        }
    }
    for (const auto& [id, i] : todo) {
        const Unit* u = w_.unit(id);
        if (!u) continue;
        uint32_t target = 0;
        if (!w_.abilityTarget(*u, i, target)) continue;
        Vec3 point = u->position;
        if (const Unit* t = w_.unit(target)) point = t->position;
        w_.useAbility(id, i, target, point);
    }
}

} // namespace alamo
