// Effect library, effect generators and ability specs (see effects.hpp).
#include "alamo/effects.hpp"

#include "alamo/combat.hpp"
#include "alamo/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_set>

namespace alamo {
namespace {

std::string up(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

bool truthy(std::string_view v) {
    const std::string t = up(trim(v));
    return t == "YES" || t == "TRUE" || t == "1";
}

float num(const std::string* v, float fallback) {
    return v && !trim(*v).empty() ? float(std::atof(trim(*v).c_str())) : fallback;
}

// Splits "A, B | C" style lists.
std::vector<std::string> splitList(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == '|' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Groups a type's fields under `prefix[NAME].` into NAME -> (rest of path -> value), keeping
// the order in which names first appear.
using FieldGroup = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;
FieldGroup groupFields(const GameObjectType& t, std::string_view prefix) {
    FieldGroup out;
    std::map<std::string, size_t> index;
    const std::string P = up(prefix) + "[";
    for (const ObjectField& f : t.fields()) {
        if (f.path.size() <= P.size() || up(f.path.substr(0, P.size())) != P) continue;
        const size_t close = f.path.find(']', P.size());
        if (close == std::string::npos || close + 1 >= f.path.size() || f.path[close + 1] != '.') continue;
        const std::string name = f.path.substr(P.size(), close - P.size());
        auto it = index.find(up(name));
        if (it == index.end()) {
            it = index.emplace(up(name), out.size()).first;
            out.push_back({name, {}});
        }
        out[it->second].second.push_back({f.path.substr(close + 2), f.values.empty() ? std::string() : f.values.back()});
    }
    return out;
}

const std::string* lookup(const std::vector<std::pair<std::string, std::string>>& kv, std::string_view key) {
    const std::string* r = nullptr;
    for (const auto& [k, v] : kv)
        if (iequals(k, key)) r = &v; // last wins
    return r;
}

uint32_t parseMask(std::string_view s) {
    uint32_t m = 0;
    for (const std::string& t : splitList(s)) {
        const std::string u = up(t);
        if (u == "SELF") m |= GatherSelf;
        else if (u == "TARGET") m |= GatherTarget;
        else if (u == "SELFAE") m |= GatherSelfArea;
        else if (u == "TARGETAE") m |= GatherTargetArea;
        else if (u == "SELFPARENT" || u == "SELFHPPARENT" || u == "TARGETHPPARENT") m |= GatherParent;
        else m |= GatherUnsupported;
    }
    return m;
}

} // namespace

// ---------------------------------------------------------------- components / effects

const std::string* EffectComponent::get(std::string_view key) const {
    for (const auto& [k, v] : fields)
        if (iequals(k, key)) return &v;
    return nullptr;
}

float EffectComponent::number(std::string_view key, float fallback) const { return num(get(key), fallback); }

bool EffectComponent::flag(std::string_view key, bool fallback) const {
    const std::string* v = get(key);
    return v ? truthy(*v) : fallback;
}

EffectOp effectOpFromName(std::string_view t) {
    static const std::pair<const char*, EffectOp> kOps[] = {
        {"AttributeModifier", EffectOp::AttributeModifier}, {"Damage", EffectOp::Damage},
        {"DamageOverTime", EffectOp::DamageOverTime},       {"Heal", EffectOp::Heal},
        {"HealOverTime", EffectOp::HealOverTime},           {"DisableBehavior", EffectOp::DisableBehavior},
        {"DisableAbility", EffectOp::DisableAbility},       {"RemoveEffect", EffectOp::RemoveEffect},
        {"BlockEffect", EffectOp::BlockEffect},             {"SwitchFaction", EffectOp::SwitchFaction},
        {"ModifyArmor", EffectOp::ModifyArmor},             {"Destroy", EffectOp::Destroy},
        {"MaxHealth", EffectOp::MaxHealth},                 {"ApplyEffectOnRemove", EffectOp::ApplyEffectOnRemove},
        {"ApplyGenerator", EffectOp::ApplyGenerator},       {"ActivateGenerator", EffectOp::ActivateGenerator},
        {"CreateObject", EffectOp::CreateObject},           {"Transfer", EffectOp::Transfer},
    };
    for (const auto& [n, op] : kOps)
        if (iequals(t, n)) return op;
    return EffectOp::Other;
}

bool EffectDef::hasGameplay() const {
    for (const EffectComponent& c : components)
        if (c.op != EffectOp::Other) return true;
    return false;
}

void EffectLibrary::add(EffectDef def) {
    const std::string key = up(def.name);
    effects_[key] = std::make_unique<EffectDef>(std::move(def));
}

void EffectLibrary::parse(std::string_view xml) {
    XmlDocument doc = parseXml(xml);
    std::vector<const XmlNode*> roots;
    for (const auto& r : doc.root.children) roots.push_back(r.get());
    for (const XmlNode* root : roots)
        for (const XmlNode* e : root->childrenNamed("Effect")) {
            const std::string* name = e->attr("Name");
            if (!name || trim(*name).empty()) continue;
            EffectDef def;
            def.name = trim(*name);
            for (const auto& c : e->children) {
                if (iequals(c->tag, "Effect_Class")) def.effectClass = trim(c->text);
                else if (iequals(c->tag, "Duration")) def.duration = float(std::atof(trim(c->text).c_str()));
                else if (iequals(c->tag, "Extends_Existing_Duration")) def.extendsDuration = truthy(c->text);
                else if (iequals(c->tag, "Allow_Same_Source_Stacking")) def.sameSourceStacking = truthy(c->text);
                else if (iequals(c->tag, "Extends_Same_Source_Duration_Only")) def.sameSourceOnly = truthy(c->text);
                else if (iequals(c->tag, "Effect_Component")) {
                    EffectComponent comp;
                    if (const std::string* t = c->attr("Type")) comp.type = trim(*t);
                    for (const auto& f : c->children) comp.fields.push_back({f->tag, trim(f->text)});
                    comp.op = effectOpFromName(comp.type);
                    if (comp.op == EffectOp::AttributeModifier) {
                        if (const std::string* a = comp.get("Attribute")) comp.attribute = up(trim(*a));
                        const std::string* mt = comp.get("Modifier_Type");
                        const std::string m = mt ? up(trim(*mt)) : "";
                        if (m.empty() || m == "ADDITIVE" || m == "MULTIPLICATIVE")
                            comp.value = comp.get("Modifier_Value") ? comp.number("Modifier_Value") : comp.number("Modifier_Integer_Value");
                        else
                            comp.attribute.clear(); // bitwise / replacement: not a number to sum
                        if (const std::string* ma = comp.get("Modifier_Attribute")) comp.modifierAttribute = up(trim(*ma));
                        comp.reset = comp.flag("Reset_Memoryless_Attribute");
                    }
                    def.components.push_back(std::move(comp));
                }
            }
            add(std::move(def));
        }
}

void EffectLibrary::load(MegVfs& vfs) {
    const std::string list = "Data\\XML\\Effects\\EffectSystemFiles.xml";
    std::vector<std::string> files;
    std::unordered_set<std::string> seen;
    for (const auto& v : vfs.readAllLayers(list)) {
        XmlDocument doc = parseXml(std::string(v.data.begin(), v.data.end()));
        for (const auto& r : doc.root.children)
            for (const XmlNode* f : r->childrenNamed("File")) {
                const std::string path = "Data\\XML\\Effects\\" + trim(f->text);
                if (seen.insert(up(path)).second) files.push_back(path);
            }
    }
    for (size_t layer = 0; layer < vfs.layerCount(); ++layer)
        for (const std::string& path : files) {
            auto data = vfs.readInLayer(layer, path);
            if (data) parse(std::string(data->begin(), data->end()));
        }
}

const EffectDef* EffectLibrary::find(std::string_view name) const {
    auto it = effects_.find(up(trim(name)));
    return it == effects_.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------- generators

std::vector<GeneratorDef> generatorsFor(const GameObjectType& type) {
    std::vector<GeneratorDef> out;
    for (const auto& [name, kv] : groupFields(type, "Effect_Generator_Data.Effect_Generator")) {
        GeneratorDef g;
        g.name = trim(name);
        g.nameUpper = up(g.name);
        std::vector<std::pair<std::string, std::string>> own;
        // Entries: split "Effect_Generator_Entry[E].Field" into per-entry lists.
        std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> entries;
        for (const auto& [k, v] : kv) {
            const std::string K = up(k);
            static const std::string kEntry = "EFFECT_GENERATOR_ENTRY[";
            if (K.rfind(kEntry, 0) == 0) {
                const size_t close = k.find(']');
                if (close == std::string::npos || close + 2 > k.size()) continue;
                const std::string en = up(k.substr(kEntry.size(), close - kEntry.size()));
                auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.first == en; });
                if (it == entries.end()) { entries.push_back({en, {}}); it = entries.end() - 1; }
                it->second.push_back({k.substr(close + 2), v});
            } else {
                own.push_back({k, v});
            }
        }
        if (const std::string* v = lookup(own, "Starts_Active")) g.startsActive = truthy(*v);
        if (const std::string* v = lookup(own, "Should_Deactivate_Manually")) g.manualDeactivate = truthy(*v);
        g.refreshSeconds = num(lookup(own, "Modal_Refresh_Frequency"), 0);
        g.startDelay = num(lookup(own, "Self_Start_Delay"), 0);
        if (const std::string* v = lookup(own, "Application_Signals")) g.signals = splitList(*v);
        if (const std::string* v = lookup(own, "Disable_Generator_Attribute")) g.disableAttribute = up(trim(*v));
        for (const auto& [en, ekv] : entries) {
            GeneratorEntry e;
            const std::string* eff = lookup(ekv, "Effect_Name");
            if (!eff || trim(*eff).empty()) continue;
            e.effect = trim(*eff);
            e.effectUpper = up(e.effect);
            e.duration = num(lookup(ekv, "Duration"), -2);
            if (const std::string* v = lookup(ekv, "Duration_Attribute")) e.durationAttribute = trim(*v);
            e.baseDelay = num(lookup(ekv, "Base_Delay"), 0);
            if (const std::string* v = lookup(ekv, "Target_Gathering_Mask")) e.mask = parseMask(*v);
            e.radius = num(lookup(ekv, "Area_Effect_Radius"), 0);
            for (const char* key : {"Target_Filter_C", "Target_Filer_C"}) // the data has the typo too
                if (const std::string* v = lookup(ekv, key))
                    for (const std::string& f : splitList(*v)) e.filters.push_back(f);
            if (const std::string* v = lookup(ekv, "Target_Filter_Categories")) e.categories = trim(*v);
            e.maxTargets = int(num(lookup(ekv, "Max_Targets"), 0));
            if (const std::string* v = lookup(ekv, "Disable_Entry_Attribute")) e.disableAttribute = up(trim(*v));
            e.probability = num(lookup(ekv, "Target_Application_Probability"), 1);
            g.entries.push_back(std::move(e));
        }
        out.push_back(std::move(g));
    }
    return out;
}

// ---------------------------------------------------------------- abilities

std::vector<AbilitySpec> abilitiesFor(const GameObjectType& type, const GameDatabase& db,
                                      const std::vector<GeneratorDef>& generators) {
    // The Abilities block, by entry name: element tag and fields.
    struct Raw { std::string kind; std::vector<std::pair<std::string, std::string>> kv; };
    std::map<std::string, Raw> raw; // upper-case name
    std::vector<std::string> rawOrder;
    {
        std::map<std::string, size_t> seen;
        for (const ObjectField& f : type.fields()) {
            if (f.path.size() < 11 || up(f.path.substr(0, 10)) != "ABILITIES.") continue;
            const size_t open = f.path.find('[', 10), close = f.path.find(']', 10);
            if (open == std::string::npos || close == std::string::npos || close < open || close + 2 > f.path.size()) continue;
            const std::string kind = f.path.substr(10, open - 10);
            const std::string name = f.path.substr(open + 1, close - open - 1);
            const std::string key = up(name);
            if (!raw.count(key)) { raw[key].kind = kind; rawOrder.push_back(key); }
            raw[key].kv.push_back({f.path.substr(close + 2), f.values.empty() ? std::string() : f.values.back()});
        }
    }
    auto generatorTargets = [&](const std::string& gen, bool& target, bool& ally) {
        for (const GeneratorDef& g : generators)
            if (iequals(g.name, gen))
                for (const GeneratorEntry& e : g.entries) {
                    if (e.mask & (GatherTarget | GatherTargetArea)) target = true;
                    for (const std::string& f : e.filters)
                        if (up(f).find("ALLY") != std::string::npos && up(f).find("NOT_ALLY") == std::string::npos) ally = true;
                    for (const std::string& f : e.filters)
                        if (up(f).find("DAMAGED") != std::string::npos) ally = true;
                }
    };

    // Fills a spec from an Abilities entry.
    auto fromRaw = [&](AbilitySpec& s, const Raw& r) {
        const auto& kv = r.kv;
        if (const std::string* v = lookup(kv, "Activation_Style"))
            s.autonomous = up(trim(*v)) == "SPECIAL_ATTACK";
        s.minRange = num(lookup(kv, "Activation_Min_Range"), 0);
        s.maxRange = num(lookup(kv, "Activation_Max_Range"), s.maxRange);
        if (const std::string* v = lookup(kv, "Activation_Recharge_Time"))
            s.rechargeTicks = std::max(s.rechargeTicks, int(num(v, 0) * 30));
        if (const std::string* v = lookup(kv, "Effect_Generator_Name"); v && s.generator.empty()) s.generator = trim(*v);
        if (const std::string* v = lookup(kv, "Applicable_Unit_Categories")) s.applicable = trim(*v);
        if (const std::string* v = lookup(kv, "Excluded_Unit_Categories")) s.excluded = trim(*v);
        if (const std::string* v = lookup(kv, "Can_Target_Allies")) s.targetsAllies = s.targetsAllies || truthy(*v);
        if (iequals(r.kind, "Generic_Attack_Ability")) {
            s.kind = AbilitySpec::Kind::Attack;
            s.needsTarget = true;
            s.damage = num(lookup(kv, "Damage_Amount"), 0);
            s.damagePercent = num(lookup(kv, "Damage_Percentage"), 0);
            if (const std::string* v = lookup(kv, "Damage_Type")) s.damageType = trim(*v);
            if (const std::string* v = lookup(kv, "Projectile_Type")) s.projectile = db.find(trim(*v));
            s.projectileCount = std::max(1, int(num(lookup(kv, "Projectile_Count"), 1)));
            if (s.rechargeTicks == 0) // melee: one swing per attack animation
                s.rechargeTicks = int(num(lookup(kv, "Animation_Duration_Override"), 1.0f) * 30);
            if (s.projectile && s.damage <= 0) {
                // Damage comes with the projectile.
                const std::string* pd = s.projectile->find("Projectile_Damage");
                s.damage = num(pd, 0);
                if (s.damageType.empty())
                    if (const std::string* dt = s.projectile->find("Damage_Type")) s.damageType = trim(*dt);
                const std::string* blast = s.projectile->find("Projectile_Blast_Area_Damage");
                s.damage = std::max(s.damage, num(blast, 0));
            }
            if (s.damage <= 0 && s.damagePercent <= 0 && !s.projectile) s.kind = AbilitySpec::Kind::Unsupported;
        } else if (iequals(r.kind, "Effect_Generator_Ability")) {
            s.kind = s.generator.empty() ? AbilitySpec::Kind::Unsupported : AbilitySpec::Kind::Generator;
        } else if (iequals(r.kind, "Crush_Attack_Ability")) {
            // Orlok's crush: a channelled generator on one target in front of him.
            s.kind = s.generator.empty() ? AbilitySpec::Kind::Unsupported : AbilitySpec::Kind::Generator;
            s.rechargeTicks = std::max(s.rechargeTicks, int(num(lookup(kv, "Cooldown_Time_Secs"), 1) * 30));
            if (const std::string* v = lookup(kv, "Can_Target_Allies")) s.targetsAllies = truthy(*v);
            if (const std::string* v = lookup(kv, "Check_Ammo_Full")) s.needsAmmoRoom = truthy(*v);
        } else if (iequals(r.kind, "Leap_Activation_Ability")) {
            s.kind = AbilitySpec::Kind::Leap;
        } else if (iequals(r.kind, "Radiation_Weapon_Ability")) {
            s.kind = AbilitySpec::Kind::Weapon;
        } else if (iequals(r.kind, "Spray_Projectiles_Ability")) {
            if (const std::string* v = lookup(kv, "Projectile_Type")) s.projectile = db.find(trim(*v));
            if (s.projectile) {
                s.kind = AbilitySpec::Kind::Burst;
                s.burstPulses = std::max(1, int(num(lookup(kv, "Projectile_Count"), 1)));
                s.burstInterval = std::max(1, int(num(lookup(kv, "Projectile_Fire_Interval"), 0.2f) * 30));
                s.burstRadius = num(lookup(kv, "Projectile_Radius"), s.maxRange > 0 ? s.maxRange : 150);
                s.burstDelay = int(num(lookup(kv, "Damage_Frame_Number"), 0)); // frames of the warm-up
                if (const std::string* dt = s.projectile->find("Damage_Type")) s.damageType = trim(*dt);
            }
        } else if (iequals(r.kind, "Dervish_Spin_Attack_Ability")) {
            s.kind = AbilitySpec::Kind::Burst;
            s.burstRadius = num(lookup(kv, "Attack_Radius"), 100);
            s.burstDelay = int(num(lookup(kv, "Spin_Up_Seconds"), 0) * 30);
            s.burstInterval = 15; // twice a second, like its arc generator
            s.burstPulses = std::max(1, int(num(lookup(kv, "Attack_Duration_Seconds"), 3) * 2));
            s.burstDamage = num(lookup(kv, "AOE_Damage_Per_Second"), 0) / 2;
            s.burstRandom = num(lookup(kv, "AOE_Additional_Random_Damage"), 0);
            if (const std::string* v = lookup(kv, "Damage_Type")) s.damageType = trim(*v);
            if (s.burstDamage + s.burstRandom <= 0) s.kind = AbilitySpec::Kind::Unsupported;
        }
    };

    std::vector<AbilitySpec> out;
    std::unordered_set<std::string> used; // Abilities entries claimed by a Unit_Ability
    for (const auto& [name, kv] : groupFields(type, "Unit_Abilities_Data.Unit_Ability")) {
        AbilitySpec s;
        s.name = trim(name);
        const std::string* t = lookup(kv, "Type");
        s.abilityType = t ? up(trim(*t)) : "";
        s.rechargeTicks = int(num(lookup(kv, "Recharge_Seconds"), 0) * 30);
        s.durationTicks = int(num(lookup(kv, "Expiration_Seconds"), 0) * 30);
        if (const std::string* v = lookup(kv, "Effect_Generator_Name")) s.generator = trim(*v);
        if (const std::string* v = lookup(kv, "Targeting_Type"))
            s.needsTarget = up(*v).find("OBJECT") != std::string::npos;
        if (const std::string* v = lookup(kv, "Targeting_Type"))
            s.targetsAllies = up(*v).find("ALLIED") != std::string::npos || up(*v).find("FRIENDLY") != std::string::npos;
        s.aiRange = num(lookup(kv, "Unit_Ability_AI_Range"), 0);
        s.aiHealthBelow = num(lookup(kv, "Unit_Ability_AI_Only_At_Health_Lower"), 0);
        if (const std::string* v = lookup(kv, "Unit_Ability_AI_Unit_Categories")) s.aiCategories = trim(*v);
        if (const std::string* v = lookup(kv, "Unit_Ability_AI_No_Autofire")) s.aiNoAutofire = truthy(*v);
        if (const std::string* v = lookup(kv, "GUI_Activated_Ability_Name")) {
            const std::string key = up(trim(*v));
            if (auto it = raw.find(key); it != raw.end()) {
                used.insert(key);
                s.specialName = trim(*v);
                fromRaw(s, it->second);
                s.autonomous = false; // a button: ordered
            }
        }
        if (s.kind != AbilitySpec::Kind::Attack && s.kind != AbilitySpec::Kind::Burst)
            s.kind = !s.generator.empty() && (s.abilityType == "EFFECT_GENERATOR" || s.kind == AbilitySpec::Kind::Generator)
                         ? AbilitySpec::Kind::Generator
                         : AbilitySpec::Kind::Unsupported;
        if (s.abilityType == "SWITCH_TYPE") {
            const std::string* a = lookup(kv, "Active_Type_Class");
            const std::string* in = lookup(kv, "Inactive_Type_Class");
            if (a && in && db.find(trim(*a)) && db.find(trim(*in))) {
                s.kind = AbilitySpec::Kind::Switch;
                s.switchTypes = {trim(*in), trim(*a)};
                s.needsTarget = false;
            }
        } else if (s.abilityType == "LEAP") {
            s.kind = AbilitySpec::Kind::Leap;
        } else if (s.abilityType == "SELF_DESTRUCT") {
            s.kind = AbilitySpec::Kind::SelfDestruct;
            s.needsTarget = false;
        } else if (s.abilityType == "SPAWN_CLONES") {
            if (const std::string* v = type.find("Spawn_Clones_Type_To_Spawn"); v && db.find(trim(*v))) {
                s.kind = AbilitySpec::Kind::SpawnClones;
                s.cloneType = trim(*v);
                s.cloneCount = std::max(1, int(num(type.find("Spawn_Clones_Number_Of_Clones"), 2)));
                s.cloneLifetime = num(type.find("Spawn_Clones_Clone_Lifetime_Seconds"), 15);
                s.needsTarget = false;
            }
        }
        if (s.kind == AbilitySpec::Kind::Leap || s.kind == AbilitySpec::Kind::SelfDestruct) {
            auto leapField = [&](const char* leaf) { return type.find(leaf); };
            if (const std::string* v = leapField("On_Landing_Effect_Generator_Name")) s.landingGenerator = trim(*v);
            if (const std::string* v = leapField("Self_Destruct_On_Landing")) s.selfDestructOnLanding = truthy(*v);
            if (const std::string* v = leapField("Self_Destruct_Damage_Type")) s.selfDestructDamageType = trim(*v);
            s.leapHeight = num(leapField("Leap_Max_Height"), 60);
            s.selfDestructDamage = num(leapField("Damage_Applied_On_Self_Destruct"), 0);
            s.selfDestructRadius = num(leapField("Radius_Affected_By_Self_Destruction"), 0);
            if (s.kind == AbilitySpec::Kind::Leap && s.maxRange <= 0) s.maxRange = 150;
            if (const std::string* v = lookup(kv, "Targeting_Type")) s.needsTarget = up(*v).find("OBJECT") != std::string::npos;
        }
        if (s.kind == AbilitySpec::Kind::Generator) {
            bool target = false, ally = false;
            generatorTargets(s.generator, target, ally);
            s.needsTarget = s.needsTarget || target;
            s.targetsAllies = s.targetsAllies || ally;
        }
        // An ATTACK_SUBSTITUTE that recharges like a weapon is an alternate fire mode (it replaces
        // the normal weapon while toggled on), not an extra attack.
        if (s.abilityType == "ATTACK_SUBSTITUTE" && s.rechargeTicks < 5 * 30) s.kind = AbilitySpec::Kind::Unsupported;
        out.push_back(std::move(s));
    }
    for (const std::string& key : rawOrder) {
        if (used.count(key)) continue;
        const Raw& r = raw[key];
        AbilitySpec s;
        // Keep the spelling from the data.
        for (const ObjectField& f : type.fields())
            if (const size_t o = f.path.find('['); o != std::string::npos && up(f.path.substr(0, 10)) == "ABILITIES." &&
                up(f.path.substr(o + 1, key.size())) == key) { s.name = f.path.substr(o + 1, key.size()); break; }
        s.abilityType = r.kind;
        s.specialName = s.name;
        fromRaw(s, r);
        // No button and no Activation_Style: part of the unit's normal attack (melee swings).
        if (!lookup(r.kv, "Activation_Style") && s.kind == AbilitySpec::Kind::Attack) s.autonomous = true;
        if (s.kind == AbilitySpec::Kind::Generator) {
            bool target = false, ally = false;
            generatorTargets(s.generator, target, ally);
            s.needsTarget = target;
            s.targetsAllies = s.targetsAllies || ally;
        }
        out.push_back(std::move(s));
    }
    // Masari elemental modes: every unit with two or more Elemental_Object_Types can switch.
    {
        AbilitySpec e;
        for (const ObjectField& f : type.fields()) {
            if (up(f.path).find("ELEMENTAL_OBJECT_TYPES") == std::string::npos) continue;
            for (const std::string& v : f.values) {
                const size_t comma = v.find(',');
                if (comma == std::string::npos) continue;
                const std::string t = trim(v.substr(comma + 1));
                if (db.find(t) && std::find(e.switchTypes.begin(), e.switchTypes.end(), t) == e.switchTypes.end())
                    e.switchTypes.push_back(t);
            }
        }
        if (e.switchTypes.size() >= 2) {
            e.kind = AbilitySpec::Kind::Switch;
            e.elemental = true;
            e.name = "Elemental_Mode";
            e.abilityType = "ELEMENTAL_MODE";
            e.rechargeTicks = 5 * 30;
            if (const std::string* g = type.find("Switch_Effect_Generator_Name")) e.generator = trim(*g);
            out.push_back(std::move(e));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const AbilitySpec& a, const AbilitySpec& b) { return a.autonomous > b.autonomous; });
    return out;
}

// ---------------------------------------------------------------- categories

std::vector<std::string> categoriesOf(const GameObjectType& type) {
    std::vector<std::string> out;
    if (const std::string* c = type.find("CategoryMask"))
        for (const std::string& t : splitList(*c)) out.push_back(up(t));
    return out;
}

namespace {
// "A | B" (also commas) is OR, "A + B" is AND, "~A" is NOT: "Small + Organic + ~RadiationHelps".
bool evalCategories(const std::vector<std::string>& cats, std::string_view expr, bool& any) {
    any = false;
    bool result = false;
    std::string group;
    auto evalGroup = [&](const std::string& g) {
        bool all = true, terms = false;
        std::string term;
        auto evalTerm = [&](std::string t) {
            t = trim(t);
            if (t.empty()) return;
            terms = true;
            const bool negate = t[0] == '~';
            if (negate) t = trim(t.substr(1));
            const bool has = std::find(cats.begin(), cats.end(), up(t)) != cats.end();
            all = all && (negate ? !has : has);
        };
        for (char c : g) {
            if (c == '+') { evalTerm(term); term.clear(); }
            else term += c;
        }
        evalTerm(term);
        if (terms) { any = true; result = result || all; }
    };
    for (char c : expr) {
        if (c == '|' || c == ',') { evalGroup(group); group.clear(); }
        else group += c;
    }
    evalGroup(group);
    return result;
}
} // namespace

bool hasAnyCategory(const std::vector<std::string>& cats, std::string_view list) {
    bool any = false;
    const bool r = evalCategories(cats, list, any);
    return any && r;
}

bool matchesCategories(const std::vector<std::string>& cats, std::string_view filter) {
    bool any = false;
    const bool r = evalCategories(cats, filter, any);
    return !any || r;
}

} // namespace alamo
