// Effects, effect generators and abilities at run time (data model in effects.hpp).
//
// Each tick, before targeting:
//   1. every unit's active effects tick: durations run down, damage/heal over time is dealt,
//      expired effects are removed (running ApplyEffectOnRemove and undoing mind control,
//      MaxHealth bonuses and created objects);
//   2. modifiers are re-summed from AttributeModifier components (speed, damage dealt and
//      taken, weapon rate and range, ability recharge) and DisableBehavior/ModifyArmor;
//   3. active generators gather targets and apply their entries' effects, at Base_Delay and
//      then every Modal_Refresh_Frequency; effects with Duration -1 stay while the generator
//      keeps re-applying them and are dropped as soon as it stops (the target left the area,
//      the generator was switched off, the source died);
//   4. autonomous abilities (SPECIAL_ATTACK: melee swings, grenades, auto-cast generators)
//      fire when they are ready and something valid is in range.
// Target filters (Target_Filter_C) are named C++ functions in the original; the common ones
// are implemented below, any other name rejects the target (so unknown effects stay off).
#include "alamo/sim.hpp"
#include "alamo/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace alamo {

namespace {

float distXY(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

std::string upperS(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

int toTicks(float seconds) { return int(std::lround(seconds * kTicksPerSecond)); }

uint64_t generatorKey(uint32_t unit, size_t gen, size_t entry) {
    return (uint64_t(unit) << 24) | (uint64_t(gen & 0xFFFF) << 8) | uint64_t(entry & 0xFF) | (1ull << 63);
}
bool sameGenerator(uint64_t key, uint32_t unit, size_t gen) {
    return key && (key & ~0xFFull) == (generatorKey(unit, gen, 0) & ~0xFFull);
}

bool entryPersistent(const GeneratorEntry& e, const EffectDef* def) {
    const float d = e.duration == -2 ? (def ? def->duration : -1) : e.duration;
    return d < 0 && e.durationAttribute.empty();
}

} // namespace

namespace {
// A type's <Attributes> block, parsed once (upper-case name -> value).
const std::unordered_map<std::string, float>& baseAttributes(const GameObjectType* t) {
    static std::unordered_map<const GameObjectType*, std::unordered_map<std::string, float>> cache;
    auto it = cache.find(t);
    if (it != cache.end()) return it->second;
    std::unordered_map<std::string, float> m;
    if (t)
        for (const ObjectField& f : t->fields())
            if (f.path.size() > 11 && upperS(f.path.substr(0, 11)) == "ATTRIBUTES." && !f.values.empty())
                m[upperS(f.path.substr(11))] = float(std::atof(trim(f.values.back()).c_str()));
    return cache.emplace(t, std::move(m)).first->second;
}
} // namespace

float Unit::attributeUpper(const std::string& key) const {
    float v = 0;
    if (!attributes.empty())
        if (auto it = attributes.find(key); it != attributes.end()) v += it->second;
    const auto& base = baseAttributes(type);
    if (!base.empty())
        if (auto it = base.find(key); it != base.end()) v += it->second;
    return v;
}

float Unit::attribute(std::string_view name) const { return attributeUpper(upperS(trim(name))); }

const World::TypeAbilities& World::abilitiesOf(const GameObjectType& t) const {
    auto it = abilityCache_.find(&t);
    if (it != abilityCache_.end()) return it->second;
    TypeAbilities a;
    a.generators = generatorsFor(t);
    a.abilities = abilitiesFor(t, db_, a.generators);
    a.categories = categoriesOf(t);
    if (const std::string* g = t.find("Impact_Effect_Generator_Name")) a.impactGenerator = trim(*g);
    return abilityCache_.emplace(&t, std::move(a)).first->second;
}

bool World::generatorLocked(int owner, std::string_view name) const {
    return owner >= 0 && owner < int(players_.size()) && players_[size_t(owner)].locks.generatorLocked(trim(name));
}
bool World::effectLocked(int owner, std::string_view name) const {
    return owner >= 0 && owner < int(players_.size()) && players_[size_t(owner)].locks.effectLocked(trim(name));
}
bool World::typeLocked(int owner, const GameObjectType& t) const {
    return owner >= 0 && owner < int(players_.size()) && players_[size_t(owner)].locks.typeLocked(t, db_);
}
bool World::abilityLocked(const Unit& u, const AbilitySpec& s) const {
    if (u.owner < 0 || u.owner >= int(players_.size())) return false;
    const ResearchLocks& l = players_[size_t(u.owner)].locks;
    return l.abilityLocked(*u.type, s.name, db_) || l.abilityLocked(*u.type, s.specialName, db_) ||
           (!s.generator.empty() && l.generatorLocked(s.generator));
}

Unit* World::anyUnit(uint32_t id) {
    if (!id) return nullptr;
    if (Unit* u = unit(id)) return u;
    for (Unit& o : effectObjects_)
        if (o.id == id) return &o;
    return nullptr;
}

void World::initAbilities(Unit& u) {
    u.baseMaxSpeed = u.maxSpeed;
    if (u.originalOwner < 0) u.originalOwner = u.owner;
    const TypeAbilities& ta = abilitiesOf(*u.type);
    u.generators.clear();
    for (size_t i = 0; i < ta.generators.size(); ++i) {
        Unit::GeneratorState g;
        g.index = uint16_t(i);
        const GeneratorDef& def = ta.generators[i];
        if (def.startsActive) { // research locks are checked as it runs
            g.active = true;
            g.age = -toTicks(def.startDelay) - 1;
        }
        u.generators.push_back(g);
    }
    u.abilities.clear();
    for (size_t i = 0; i < ta.abilities.size(); ++i) {
        const AbilitySpec& s = ta.abilities[i];
        if (s.kind == AbilitySpec::Kind::Unsupported) continue;
        u.abilities.push_back({uint16_t(i), 0.f});
    }
}

bool World::hasEffectClass(const Unit& u, std::string_view cls) const {
    for (const auto& e : u.effects)
        if (iequals(e.def->effectClass, cls)) return true;
    return false;
}

// ---------------------------------------------------------------- applying effects

void World::applyEffect(Unit& t, const EffectDef& def, const Unit* source, int sourceOwner, int duration, uint64_t genKey) {
    if (!t.alive()) return;
    // Blocked by an active BlockEffect of this class?
    if (!def.effectClass.empty())
        for (const auto& e : t.effects)
            for (const EffectComponent& c : e.def->components)
                if (c.op == EffectOp::BlockEffect)
                    if (const std::string* b = c.get("Block_Effect_Class"); b && iequals(*b, def.effectClass)) return;
    const uint32_t sourceId = source ? (source->actsFor ? source->actsFor : source->id) : 0;
    for (auto& e : t.effects) {
        if (e.def != &def) continue;
        if (e.source == sourceId ? def.sameSourceStacking : def.sameSourceOnly) continue; // adds a copy
        if (duration < 0) {
            e.remaining = -1;
        } else if (e.remaining >= 0 && def.extendsDuration) {
            e.remaining = std::max(e.remaining, duration);
        }
        if (genKey) { e.genKey = genKey; e.stamp = ticks_; }
        return;
    }

    Unit::ActiveEffect ae;
    ae.def = &def;
    ae.source = sourceId;
    ae.sourceOwner = sourceOwner;
    ae.remaining = duration < 0 ? -1 : std::max(duration, 1);
    ae.genKey = duration < 0 ? genKey : 0;
    ae.stamp = ticks_;
    static const std::string kMult = "EFFECT_DAMAGE_AMOUNT_MULT_COEFF", kAdd = "EFFECT_DAMAGE_AMOUNT_ADD_COEFF";
    const float srcMult = source ? source->damageCast * std::max(0.f, 1 + source->attributeUpper(kMult)) : 1.f;
    const float srcAdd = source ? source->attributeUpper(kAdd) : 0.f;
    bool destroy = false;
    for (const EffectComponent& c : def.components) {
        if (c.op == EffectOp::Damage) {
            const float amount = (c.number("Damage_Amount") + srcAdd) * srcMult + c.number("Damage_Percentage") * t.maxHealth;
            if (amount > 0) applyDamage(t, amount, c.get("Damage_Type") ? *c.get("Damage_Type") : std::string());
        } else if (c.op == EffectOp::Heal) {
            t.health = std::min(t.maxHealth, t.health + c.number("Amount") + c.number("Percent_Heal") * t.maxHealth);
        } else if (c.op == EffectOp::RemoveEffect) {
            if (const std::string* cls = c.get("Remove_Effect_Class"))
                for (size_t i = t.effects.size(); i-- > 0;)
                    if (iequals(t.effects[i].def->effectClass, *cls)) removeEffect(t, i, false);
        } else if (c.op == EffectOp::Destroy) {
            destroy = true;
        } else if (c.op == EffectOp::SwitchFaction) {
            if (c.flag("Use_Source_Faction", true) && sourceOwner >= 0 && t.owner != sourceOwner && !t.structure) {
                ae.ownerBefore = t.owner;
                t.owner = sourceOwner;
                t.target = t.attackOrder = 0;
                t.attackMove = false;
                t.path.clear();
            }
        } else if (c.op == EffectOp::MaxHealth) {
            ae.healthBonus = t.maxHealth * c.number("Modifier_Value");
            t.maxHealth += ae.healthBonus;
            t.health = std::max(1.f, t.health + ae.healthBonus);
        } else if (c.op == EffectOp::CreateObject) {
            const std::string* name = c.get("Create_Object_Type");
            const GameObjectType* ot = name ? db_.find(trim(*name)) : nullptr;
            if (!ot) continue;
            const int owner = c.flag("Use_Source_Player_ID", true) && sourceOwner >= 0 ? sourceOwner : t.owner;
            const int count = std::max(1, int(c.number("Number_To_Create", 1)));
            const TypeAbilities& ota = abilitiesOf(*ot);
            bool gameplay = false;
            for (const GeneratorDef& g : ota.generators)
                for (const GeneratorEntry& e : g.entries)
                    if (const EffectDef* d = effectLib_ ? effectLib_->find(e.effect) : nullptr; d && d->hasGameplay()) gameplay = true;
            const bool realUnit = !weaponsFor(*ot, db_).empty() && ot->number("Tactical_Health").value_or(0) > 0;
            if (!gameplay && !realUnit) continue; // visual props
            for (int n = 0; n < count; ++n) {
                Unit o;
                o.id = nextId_++;
                o.type = ot;
                o.owner = owner;
                o.originalOwner = owner;
                o.position = t.position;
                o.facing = t.facing;
                o.maxSpeed = 0;
                o.maxHealth = o.health = std::max(1.f, float(ot->number("Tactical_Health").value_or(1)));
                o.radius = 1;
                // Particle objects (radiation fields) last as long as their particle effect.
                const float frames = float(ot->number("Particle_Lifetime_Frames").value_or(0));
                o.lifeTicks = frames > 0 ? int(frames) : c.flag("Destroy_On_Remove") ? -1 : 60 * kTicksPerSecond;
                if (c.flag("Destroy_On_Remove")) ae.created.push_back(o.id);
                if (realUnit) {
                    pendingSpawns_.push_back({ot->name, owner, t.position});
                    continue;
                }
                initAbilities(o);
                pendingObjects_.push_back(std::move(o));
            }
        } else if (c.op == EffectOp::ApplyGenerator || c.op == EffectOp::ActivateGenerator) {
            const std::string* gname = c.get("Effect_Generator_Name");
            if (!gname) continue;
            const TypeAbilities& tta = abilitiesOf(*t.type);
            for (size_t gi = 0; gi < tta.generators.size(); ++gi)
                if (iequals(tta.generators[gi].name, trim(*gname))) activateGenerator(t, gi, 0, t.position, ae.remaining);
        }
    }
    t.effects.push_back(std::move(ae));
    if (destroy) t.health = 0;
}

void World::removeEffect(Unit& u, size_t i, bool expired) {
    if (i >= u.effects.size()) return;
    Unit::ActiveEffect ae = std::move(u.effects[i]);
    u.effects.erase(u.effects.begin() + long(i));
    const EffectDef& def = *ae.def;
    if (ae.ownerBefore >= 0 && u.owner == ae.sourceOwner) {
        u.owner = ae.ownerBefore;
        u.target = u.attackOrder = 0;
        u.attackMove = false;
        u.path.clear();
    }
    if (ae.healthBonus != 0) {
        const float frac = u.maxHealth > 0 ? u.health / u.maxHealth : 1;
        u.maxHealth = std::max(1.f, u.maxHealth - ae.healthBonus);
        u.health = std::min(u.health, std::max(1.f, frac * u.maxHealth));
    }
    for (uint32_t id : ae.created)
        if (Unit* o = anyUnit(id)) o->health = 0;
    for (const EffectComponent& c : def.components) {
        if (c.op == EffectOp::ApplyEffectOnRemove) {
            if (c.flag("Apply_On_Expiration_Only") && !expired) continue;
            if (const std::string* n = c.get("Effect_Name"))
                if (const EffectDef* next = effectLib_ ? effectLib_->find(*n) : nullptr) {
                    const float d = next->duration;
                    applyEffect(u, *next, anyUnit(ae.source), ae.sourceOwner, d < 0 ? toTicks(5) : toTicks(d));
                }
        } else if (c.op == EffectOp::ApplyGenerator || c.op == EffectOp::ActivateGenerator) {
            const std::string* gname = c.get("Effect_Generator_Name");
            if (!gname) continue;
            const TypeAbilities& ta = abilitiesOf(*u.type);
            for (auto& g : u.generators)
                if (g.active && iequals(ta.generators[g.index].name, trim(*gname)) && !ta.generators[g.index].startsActive)
                    deactivateGenerator(u, g);
        }
    }
}

void World::updateEffects(Unit& u) {
    const size_t n = u.effects.size();
    std::vector<size_t> expired, dropped;
    for (size_t i = 0; i < n; ++i) {
        Unit::ActiveEffect& e = u.effects[i];
        for (const EffectComponent& c : e.def->components) {
            if (c.op == EffectOp::DamageOverTime) {
                const float a = c.number("Amount_Per_Second") + c.number("Percentage_Per_Second") * u.maxHealth;
                if (a > 0) applyDamage(u, a / kTicksPerSecond, c.get("Damage_Type") ? *c.get("Damage_Type") : std::string());
            } else if (c.op == EffectOp::HealOverTime) {
                const float a = c.number("Amount_Per_Second") + c.number("Percentage_Per_Second") * u.maxHealth;
                if (a > 0 && u.alive()) u.health = std::min(u.maxHealth, u.health + a / kTicksPerSecond);
            }
        }
        if (e.remaining > 0) {
            if (--e.remaining == 0) expired.push_back(i);
        } else if (e.remaining < 0 && e.genKey) {
            // Kept alive by a generator: gone when its source is.
            if (e.source && !anyUnit(e.source)) dropped.push_back(i);
        }
    }
    std::vector<std::pair<size_t, bool>> gone;
    for (size_t i : expired) gone.push_back({i, true});
    for (size_t i : dropped) gone.push_back({i, false});
    std::sort(gone.begin(), gone.end());
    for (size_t k = gone.size(); k-- > 0;) removeEffect(u, gone[k].first, gone[k].second);
}

void World::recomputeModifiers(Unit& u) {
    u.attributes.clear();
    u.armorMods.clear();
    u.immobile = u.disarmed = u.silenced = false;
    for (const auto& e : u.effects)
        for (const EffectComponent& c : e.def->components) {
            if (c.op == EffectOp::AttributeModifier) {
                if (!c.attribute.empty()) u.attributes[c.attribute] += c.value;
            } else if (c.op == EffectOp::DisableBehavior) {
                const std::string b = c.get("Behaviors") ? upperS(*c.get("Behaviors")) : "";
                if (b.find("LOCOMOTOR") != std::string::npos) u.immobile = true;
                if (b.find("WEAPON") != std::string::npos || b.find("TARGETING") != std::string::npos) u.disarmed = true;
                if (b.find("UNIT_ABILITY") != std::string::npos) u.silenced = true;
            } else if (c.op == EffectOp::DisableAbility) {
                u.silenced = true;
            } else if (c.op == EffectOp::ModifyArmor) {
                if (const std::string* d = c.get("Damage_Type")) u.armorMods.push_back({trim(*d), c.number("Amount")});
            }
        }
    auto sum = [&](const char* name) {
        auto it = u.attributes.find(name);
        return it == u.attributes.end() ? 0.f : it->second;
    };
    if (!u.structure) {
        u.maxSpeed = std::max(0.f, u.baseMaxSpeed * std::max(0.f, 1 + sum("MAX_SPEED_MULT_COEFF")) + sum("MAX_SPEED_ADD_COEFF"));
        if (u.immobile) u.maxSpeed = 0;
    }
    u.damageCast = std::max(0.f, 1 + sum("DAMAGE_CAST_MULT_COEFF"));
    u.damageTaken = std::max(0.f, 1 + sum("DAMAGE_TAKEN_MULT_COEFF"));
    u.fireRate = std::max(0.05f, 1 + sum("WEAPON_RATE_MULTIPLIER")) / std::max(0.05f, 1 + sum("WEAPON_DELAY_MULTIPLIER"));
    u.rangeScale = std::max(0.f, 1 + sum("WEAPON_RANGE_MULT_COEFF"));
    u.rangeAdd = sum("WEAPON_RANGE_ADD_COEFF");
    u.abilityRate = std::max(0.05f, 1 + sum("UNIT_ABILITY_COUNTDOWN_RATE_MULTIPLIER")) /
                    std::max(0.05f, 1 + sum("UNIT_ABILITY_COUNTDOWN_DELAY_MULTIPLIER"));
}

// ---------------------------------------------------------------- generators

bool World::filterPasses(std::string_view f, const Unit& s, int so, const Unit& t) const {
    const std::string n = upperS(trim(f));
    const bool same = t.owner == so;
    const bool enemy = so >= 0 && t.owner >= 0 && enemies(so, t.owner);
    const bool damaged = t.health < t.maxHealth - 0.01f;
    const std::string tn = upperS(t.type->name);
    auto endsWith = [&](const char* suf) { const size_t l = std::strlen(suf); return tn.size() >= l && tn.compare(tn.size() - l, l, suf) == 0; };
    if (n == "IS_ENEMY" || n == "IS_FOW_AWARE_ENEMY") return enemy;
    if (n == "IS_ENEMY_NOT_IMMUNE_TO_MC") return enemy && !t.structure && t.attribute("Is_Mind_Controlled") == 0;
    if (n == "IS_ALLY" || n == "IS_SAME_OWNER" || n == "IS_OWN_FLEET") return same;
    if (n == "IS_ALLY_NOT_SELF") return same && t.id != s.id;
    if (n == "IS_NOT_ALLY" || n == "IS_NOT_SAME_OWNER") return !same;
    if (n == "IS_FRIENDLY_UNIT_DAMAGED") return same && damaged && !t.structure;
    if (n == "IS_DAMAGED_AND_POWERED") return same && damaged && t.powered;
    if (n == "IS_NOT_FLYING") return !t.flying();
    if (n == "IS_POWERED") return t.powered;
    if (n == "IS_NOT_MIND_CONTROLLED") return t.attribute("Is_Mind_Controlled") == 0;
    if (n == "IS_ORIGINAL_OWNER") return t.owner == t.originalOwner;
    if (n == "IS_NOT_ORIGINAL_PLAYER") return t.owner != t.originalOwner;
    if (n == "IN_ICE_MODE") return endsWith("_ICE");
    if (n == "IN_FIRE_MODE") return endsWith("_FIRE");
    if (n == "IS_FACTION_MASARI") return so >= 0 && so < int(players_.size()) && iequals(players_[size_t(so)].faction, "Masari");
    if (n == "IS_SOURCE_FACTION_ALIEN") return so >= 0 && so < int(players_.size()) && iequals(players_[size_t(so)].faction, "Alien");
    return false; // unknown filter: don't apply
}

std::vector<Unit*> World::gather(Unit& source, const GeneratorEntry& e, const Unit::GeneratorState& g) {
    std::vector<std::pair<float, Unit*>> found;
    const TypeAbilities* ta = nullptr;
    auto consider = [&](Unit* t, Vec3 centre) {
        if (!t || !t->alive()) return;
        for (const auto& f : found)
            if (f.second == t) return;
        ta = &abilitiesOf(*t->type);
        if (!e.categories.empty() && !matchesCategories(ta->categories, e.categories)) return;
        for (const std::string& f : e.filters)
            if (!filterPasses(f, source, source.owner, *t)) return;
        found.push_back({distXY(t->position, centre), t});
    };
    if (e.mask & GatherSelf) consider(&source, source.position);
    Unit* target = g.target ? anyUnit(g.target) : nullptr;
    if (e.mask & GatherTarget) consider(target, source.position);
    auto area = [&](Vec3 centre) {
        if (e.radius <= 0) return;
        for (Unit& o : units_)
            if (distXY(o.position, centre) <= e.radius + o.radius) consider(&o, centre);
    };
    if (e.mask & GatherSelfArea) area(source.position);
    if (e.mask & GatherTargetArea) area(target ? target->position : g.point);
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (e.maxTargets > 0 && int(found.size()) > e.maxTargets) found.resize(size_t(e.maxTargets));
    std::vector<Unit*> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

void World::activateGenerator(Unit& u, size_t index, uint32_t target, Vec3 point, int life) {
    const TypeAbilities& ta = abilitiesOf(*u.type);
    if (index >= ta.generators.size() || generatorLocked(u.owner, ta.generators[index].name)) return;
    for (auto& g : u.generators) {
        if (g.index != index) continue;
        g.active = true;
        g.age = -1;
        g.target = target;
        g.point = point;
        g.life = life;
        return;
    }
}

void World::deactivateGenerator(Unit& u, Unit::GeneratorState& g) {
    g.active = false;
    g.target = 0;
    purgeGenerator(u, g);
}

void World::purgeGenerator(Unit& u, Unit::GeneratorState& g) {
    auto purge = [&](Unit& o) {
        for (size_t i = o.effects.size(); i-- > 0;)
            if (sameGenerator(o.effects[i].genKey, u.id, g.index) && o.effects[i].remaining < 0) removeEffect(o, i, false);
    };
    for (Unit& o : units_) purge(o);
    for (Unit& o : effectObjects_) purge(o);
}

void World::runGeneratorEntries(Unit& u, Unit::GeneratorState& g, bool force) {
    const GeneratorDef& def = abilitiesOf(*u.type).generators[g.index];
    for (size_t j = 0; j < def.entries.size(); ++j) {
        const GeneratorEntry& e = def.entries[j];
        const EffectDef* eff = effectLib_->find(e.effect);
        if (!eff || !eff->hasGameplay()) continue;
        const int delay = toTicks(e.baseDelay);
        if (g.age < delay) continue;
        const bool persistent = entryPersistent(e, eff);
        const int period = def.refreshSeconds > 0 ? std::max(1, toTicks(def.refreshSeconds))
                           : (persistent && (e.mask & (GatherSelfArea | GatherTargetArea))) ? kTicksPerSecond : 0;
        const bool due = force || g.age == delay || (period > 0 && (g.age - delay) % period == 0);
        if (!due) continue;
        const uint64_t key = generatorKey(u.id, g.index, j);
        const bool disabled = (!e.disableAttribute.empty() && u.attributeUpper(e.disableAttribute) != 0) ||
                              effectLockedUpper(u.owner, e.effectUpper);
        if (!disabled) {
            int ticks = -1;
            if (!persistent) {
                float d = e.duration == -2 ? eff->duration : e.duration;
                if (!e.durationAttribute.empty()) d = u.attribute(e.durationAttribute);
                ticks = std::max(1, toTicks(d));
            }
            for (Unit* t : gather(u, e, g)) {
                if (e.probability < 1 && random01() > e.probability) continue;
                applyEffect(*t, *eff, &u, u.owner, ticks, persistent ? key : 0);
            }
        }
        if (persistent) // drop it from whoever was not re-applied this time
            for (std::vector<Unit>* list : {&units_, &effectObjects_})
                for (Unit& o : *list)
                    for (size_t i = o.effects.size(); i-- > 0;)
                        if (o.effects[i].genKey == key && o.effects[i].stamp != ticks_) removeEffect(o, i, false);
    }
}

void World::updateGenerators(Unit& u) {
    if (!effectLib_ || u.generators.empty()) return;
    const TypeAbilities& ta = abilitiesOf(*u.type);
    for (size_t k = 0; k < u.generators.size(); ++k) {
        Unit::GeneratorState& g = u.generators[k];
        if (!g.active) continue;
        const GeneratorDef& def = ta.generators[g.index];
        if ((!def.disableAttribute.empty() && u.attributeUpper(def.disableAttribute) != 0) || generatorLockedUpper(u.owner, def.nameUpper)) {
            if (g.age >= 0) purgeGenerator(u, g);
            g.age = -1;
            continue;
        }
        ++g.age;
        if (g.age < 0) continue;
        if (g.life > 0 && --g.life == 0) {
            deactivateGenerator(u, g);
            continue;
        }
        if (g.target && !anyUnit(g.target)) { // the target is gone
            deactivateGenerator(u, g);
            continue;
        }
        if (!def.signals.empty()) {
            bool fired = false;
            for (const std::string& s : def.signals) {
                if (iequals(s, "OBJECT_DAMAGED")) fired = fired || u.damagedThisTick;
                if (iequals(s, "OBJECT_FIRED_WEAPON")) fired = fired || u.firedThisTick;
            }
            if (fired) runGeneratorEntries(u, g, true);
            continue;
        }
        runGeneratorEntries(u, g, false);
    }
}

// ---------------------------------------------------------------- abilities

bool World::abilityTarget(const Unit& u, size_t i, uint32_t& target) const {
    target = 0;
    if (i >= u.abilities.size()) return false;
    World& w = const_cast<World&>(*this);
    const TypeAbilities& ta = abilitiesOf(*u.type);
    const AbilitySpec& s = ta.abilities[u.abilities[i].index];
    if (abilityLocked(u, s)) return false;
    auto categoriesOk = [&](const Unit& t) {
        const auto& cats = abilitiesOf(*t.type).categories;
        if (!s.applicable.empty() && !hasAnyCategory(cats, s.applicable)) return false;
        if (!s.excluded.empty() && hasAnyCategory(cats, s.excluded)) return false;
        return true;
    };
    auto inRange = [&](const Unit& t, float maxR) {
        const float d = distXY(u.position, t.position) - t.radius - u.radius;
        return d <= maxR && d + t.radius >= s.minRange;
    };

    if (s.kind == AbilitySpec::Kind::Attack) {
        const float maxR = s.maxRange > 0 ? s.maxRange : 100;
        if (const Unit* t = u.target ? w.unit(u.target) : nullptr; t && t->alive() && enemies(u.owner, t->owner) && categoriesOk(*t) && inRange(*t, maxR)) {
            target = t->id;
            return true;
        }
        float best = 1e30f;
        for (const Unit& o : units_) {
            if (!o.alive() || !enemies(u.owner, o.owner) || !categoriesOk(o) || !inRange(o, maxR)) continue;
            const float d = distXY(u.position, o.position);
            if (d < best) { best = d; target = o.id; }
        }
        return target != 0;
    }
    if (s.kind != AbilitySpec::Kind::Generator || !effectLib_) return false;
    const GeneratorDef* gen = nullptr;
    for (const GeneratorDef& g : ta.generators)
        if (iequals(g.name, s.generator)) gen = &g;
    if (!gen || generatorLocked(u.owner, gen->name)) return false;
    std::vector<const GeneratorEntry*> useful;
    for (const GeneratorEntry& e : gen->entries)
        if (const EffectDef* d = effectLib_->find(e.effect);
            d && d->hasGameplay() && !(e.mask & GatherUnsupported) && !effectLocked(u.owner, e.effect))
            useful.push_back(&e);
    if (useful.empty()) return false;

    if (s.needsTarget) {
        const float maxR = s.maxRange > 0 ? s.maxRange : (s.aiRange > 0 ? s.aiRange : 200);
        float best = 1e30f;
        for (const Unit& o : units_) {
            if (!o.alive() || o.id == u.id || !categoriesOk(o) || !inRange(o, maxR)) continue;
            bool ok = false;
            for (const GeneratorEntry* e : useful) {
                if (!(e->mask & (GatherTarget | GatherTargetArea))) continue;
                bool pass = true;
                for (const std::string& f : e->filters) pass = pass && filterPasses(f, u, u.owner, o);
                if (e->mask & GatherTargetArea && e->filters.empty()) pass = s.targetsAllies ? !enemies(u.owner, o.owner) : enemies(u.owner, o.owner);
                if (pass && !e->categories.empty()) pass = matchesCategories(abilitiesOf(*o.type).categories, e->categories);
                ok = ok || pass;
            }
            if (!ok) continue;
            // Prefer the current target, then the most damaged ally / nearest enemy.
            float score = distXY(u.position, o.position);
            if (o.id == u.target) score -= 1e6f;
            if (s.targetsAllies) score = o.health / std::max(o.maxHealth, 1.f) * 1000 + score * 0.01f;
            if (score < best) { best = score; target = o.id; }
        }
        return target != 0;
    }
    // Self / around the unit: worth it when the area holds something the effects apply to,
    // or (pure self effects) when the unit is fighting.
    Unit::GeneratorState probe;
    for (const GeneratorEntry* e : useful) {
        if (e->mask & GatherSelfArea) {
            if (!w.gather(const_cast<Unit&>(u), *e, probe).empty()) return true;
        } else if (e->mask & GatherSelf) {
            bool pass = true;
            for (const std::string& f : e->filters) pass = pass && filterPasses(f, u, u.owner, u);
            if (pass && u.target) return true;
        }
    }
    return false;
}

void World::executeAbility(Unit& u, Unit::AbilityState& a, uint32_t target, Vec3 point) {
    const TypeAbilities& ta = abilitiesOf(*u.type);
    const AbilitySpec& s = ta.abilities[a.index];
    a.cooldown = float(std::max(s.rechargeTicks, kTicksPerSecond));
    if (Player* p = player(u.owner)) ++p->abilitiesUsed;
    static const bool log = std::getenv("UAW_ABILITY_LOG") != nullptr;
    if (log) std::fprintf(stderr, "%7.1fs %-28s %-40s -> %s\n", seconds(), u.type->name.c_str(), s.name.c_str(),
                          unit(target) ? unit(target)->type->name.c_str() : "-");
    if (s.kind == AbilitySpec::Kind::Attack) {
        Unit* t = unit(target);
        if (!t) return;
        if (!s.projectile) {
            applyDamage(*t, (s.damage + s.damagePercent * t->maxHealth) * u.damageCast, s.damageType);
            return;
        }
        const float speed = std::max(1.f, float(s.projectile->number("Max_Speed").value_or(10)));
        for (int n = 0; n < s.projectileCount; ++n) {
            Projectile pr;
            pr.owner = u.owner;
            pr.shooter = u.id;
            pr.target = t->id;
            pr.position = {u.position[0], u.position[1], u.position[2] + std::max(u.radius * 0.8f, 6.f)};
            const Vec3 aim{t->position[0], t->position[1], t->position[2] + std::max(t->radius * 0.6f, 4.f)};
            const float dx = aim[0] - pr.position[0], dy = aim[1] - pr.position[1], dz = aim[2] - pr.position[2];
            const float len = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
            pr.velocity = {dx / len * speed, dy / len * speed, dz / len * speed};
            pr.distance = len;
            const float blast = float(s.projectile->number("Projectile_Blast_Area_Damage").value_or(0));
            pr.damage = (blast > 0 ? float(s.projectile->number("Projectile_Damage").value_or(0)) : s.damage) * u.damageCast;
            pr.blastDamage = blast * u.damageCast;
            pr.blastRange = float(s.projectile->number("Projectile_Blast_Area_Range").value_or(0));
            pr.damageType = s.damageType;
            pr.type = s.projectile;
            projectiles_.push_back(std::move(pr));
        }
        return;
    }
    for (size_t gi = 0; gi < ta.generators.size(); ++gi) {
        const GeneratorDef& g = ta.generators[gi];
        if (!iequals(g.name, s.generator)) continue;
        int life = 0;
        bool persistent = false;
        for (const GeneratorEntry& e : g.entries) {
            life = std::max(life, toTicks(e.baseDelay) + 2);
            persistent = persistent || entryPersistent(e, effectLib_ ? effectLib_->find(e.effect) : nullptr);
        }
        if (g.manualDeactivate || persistent || g.refreshSeconds > 0)
            life = s.durationTicks > 0 ? s.durationTicks : 10 * kTicksPerSecond;
        activateGenerator(u, gi, target, point, life);
        return;
    }
}

OrderResult World::useAbility(uint32_t id, size_t i, uint32_t target, Vec3 point) {
    Unit* u = unit(id);
    if (!u || i >= u->abilities.size()) return OrderResult::InvalidUnit;
    if (u->silenced || u->underConstruction || u->abilities[i].cooldown > 0) return OrderResult::NotAllowed;
    const AbilitySpec& s = abilitiesOf(*u->type).abilities[u->abilities[i].index];
    if (s.kind == AbilitySpec::Kind::Unsupported || abilityLocked(*u, s)) return OrderResult::NotAllowed;
    if (s.needsTarget) {
        const Unit* t = unit(target);
        if (!t) return OrderResult::InvalidUnit;
        const float maxR = s.maxRange > 0 ? s.maxRange : (s.aiRange > 0 ? s.aiRange : 200);
        if (distXY(u->position, t->position) - t->radius - u->radius > maxR) return OrderResult::NotAllowed;
        point = t->position;
    }
    executeAbility(*u, u->abilities[i], target, point);
    return OrderResult::Ok;
}

void World::autonomousAbilities(Unit& u) {
    if (u.abilities.empty()) return;
    const TypeAbilities& ta = abilitiesOf(*u.type);
    for (size_t i = 0; i < u.abilities.size(); ++i) {
        Unit::AbilityState& a = u.abilities[i];
        if (a.cooldown > 0) a.cooldown = std::max(0.f, a.cooldown - u.abilityRate);
        if (a.cooldown > 0 || u.silenced || u.underConstruction || !ta.abilities[a.index].autonomous) continue;
        if (abilityLocked(u, ta.abilities[a.index])) continue;
        uint32_t target = 0;
        if (abilityTarget(u, i, target)) executeAbility(u, a, target, u.position);
    }
}

void World::projectileImpact(const Projectile& p, const Unit* hit) {
    if (!effectLib_ || !p.type) return;
    const TypeAbilities& pta = abilitiesOf(*p.type);
    if (pta.impactGenerator.empty()) return;
    const uint32_t target = hit ? hit->id : 0;
    auto lifeOf = [&](const GeneratorDef& g) {
        int life = 0;
        for (const GeneratorEntry& e : g.entries) life = std::max(life, toTicks(e.baseDelay) + 2);
        return life;
    };
    // Usually the generator is defined on the projectile: it runs from an invisible object at
    // the impact point, owned by the shooter's player.
    for (size_t gi = 0; gi < pta.generators.size(); ++gi) {
        if (!iequals(pta.generators[gi].name, pta.impactGenerator)) continue;
        Unit o;
        o.id = nextId_++;
        o.type = p.type;
        o.owner = o.originalOwner = p.owner;
        o.position = p.position;
        o.maxHealth = o.health = 1;
        o.radius = 1;
        o.actsFor = p.shooter;
        initAbilities(o);
        for (auto& g : o.generators) g.active = false; // only the impact generator runs
        const int life = lifeOf(pta.generators[gi]);
        activateGenerator(o, gi, target, p.position, life);
        o.lifeTicks = life + 2;
        pendingObjects_.push_back(std::move(o));
        return;
    }
    // ...otherwise on the unit that fired it.
    if (Unit* shooter = unit(p.shooter)) {
        const TypeAbilities& sta = abilitiesOf(*shooter->type);
        for (size_t gi = 0; gi < sta.generators.size(); ++gi)
            if (iequals(sta.generators[gi].name, pta.impactGenerator)) {
                activateGenerator(*shooter, gi, target, p.position, lifeOf(sta.generators[gi]));
                return;
            }
    }
}

void World::updateAbilities() {
    for (Unit& u : units_) updateEffects(u);
    for (Unit& o : effectObjects_) updateEffects(o);
    for (Unit& u : units_) recomputeModifiers(u);
    for (Unit& o : effectObjects_) recomputeModifiers(o);
    for (size_t i = 0; i < units_.size(); ++i) updateGenerators(units_[i]);
    for (size_t i = 0; i < effectObjects_.size(); ++i) updateGenerators(effectObjects_[i]);
    for (Unit& u : units_) {
        autonomousAbilities(u);
        u.damagedThisTick = u.firedThisTick = false;
    }
    // Effect objects: lifetime, then add the ones created this tick.
    for (Unit& o : effectObjects_)
        if (o.lifeTicks > 0 && --o.lifeTicks == 0) o.health = 0;
    effectObjects_.erase(std::remove_if(effectObjects_.begin(), effectObjects_.end(), [](const Unit& o) { return !o.alive(); }),
                         effectObjects_.end());
    for (Unit& o : pendingObjects_) effectObjects_.push_back(std::move(o));
    pendingObjects_.clear();
    std::vector<PendingSpawn> spawns;
    spawns.swap(pendingSpawns_);
    for (const PendingSpawn& p : spawns) spawn(p.type, p.owner, p.position);
}

} // namespace alamo

namespace alamo {

// ---------------------------------------------------------------- research

void World::setResearchTrees(const std::vector<ResearchTree>* trees) {
    trees_ = trees;
    for (Player& p : players_)
        if (const ResearchTree* t = researchTree(p.id)) p.locks = ResearchLocks::initial(*t);
}

const ResearchTree* World::researchTree(int owner) const {
    if (!trees_ || owner < 0 || owner >= int(players_.size())) return nullptr;
    const std::string f = upperS(players_[size_t(owner)].faction);
    for (const ResearchTree& t : *trees_)
        if (t.faction == f) return &t;
    return nullptr;
}

int World::researchFacilities(int owner) const {
    int n = 0;
    for (const Unit& u : units_) {
        if (u.owner != owner || !u.structure || u.underConstruction) continue;
        const std::string* r = u.type->find("Enables_Research");
        if (r && (iequals(trim(*r), "yes") || iequals(trim(*r), "true"))) ++n;
    }
    return n;
}

int World::researchSlots(int owner) const { return 1 + researchFacilities(owner); }

std::vector<const ResearchSuite*> World::availableResearch(int owner) const {
    std::vector<const ResearchSuite*> out;
    const ResearchTree* tree = researchTree(owner);
    if (!tree) return out;
    const Player& p = players_[size_t(owner)];
    if (int(p.researching.size()) >= researchSlots(owner)) return out;
    if (int(p.researched.size() + p.researching.size()) >= kMaxResearchSuites) return out;
    for (const ResearchSuite& s : tree->suites) {
        if (p.researched.count(s.id())) continue;
        if (s.index > 1 && !p.researched.count(s.path + std::to_string(s.index - 1))) continue;
        bool busy = false;
        for (const auto& r : p.researching) busy = busy || r.suite == &s;
        if (!busy) out.push_back(&s);
    }
    return out;
}

OrderResult World::startResearch(int owner, std::string_view path, int index) {
    Player* p = player(owner);
    const ResearchTree* tree = researchTree(owner);
    const ResearchSuite* s = tree ? tree->find(path, index) : nullptr;
    if (!p || !s) return OrderResult::InvalidUnit;
    bool ok = false;
    for (const ResearchSuite* a : availableResearch(owner)) ok = ok || a == s;
    if (!ok) return OrderResult::NotAllowed;
    if (p->credits < s->cost) return OrderResult::NotEnoughCredits;
    p->credits -= s->cost;
    p->researching.push_back({s, std::max(1, toTicks(s->seconds))});
    return OrderResult::Ok;
}

void World::updateResearch() {
    for (Player& p : players_) {
        if (p.researching.empty()) continue;
        // Losing facilities pauses the newest research beyond what the rest can carry.
        const int slots = researchSlots(p.id);
        for (size_t i = 0; i < p.researching.size(); ++i) {
            if (int(i) >= slots) break;
            Player::Research& r = p.researching[i];
            if (--r.ticksLeft > 0) continue;
            p.locks.apply(*r.suite);
            p.researched.insert(r.suite->id());
            r.suite = nullptr;
        }
        std::erase_if(p.researching, [](const Player::Research& r) { return r.suite == nullptr; });
    }
}

void GameRules::load(MegVfs& vfs) {
    try { effects.loadEffects(vfs); } catch (const std::exception&) {}
    effectLibrary.load(vfs);
    research = loadResearchTrees(vfs);
    balance = loadBalance(vfs);
}

void GameRules::configure(World& w) const {
    w.setEffects(&effects);
    w.setEffectLibrary(&effectLibrary);
    w.setResearchTrees(&research);
    w.setBalance(balance);
}

} // namespace alamo
