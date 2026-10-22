#include "alamo/combat.hpp"

#include "alamo/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace alamo {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == '\n' || c == '\r' || c == '\t') {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

int toTicks(double seconds) { return std::max(0, int(std::lround(seconds * 30.0))); }

std::optional<WeaponSpec> weaponFrom(const GameObjectType& t, const GameDatabase& db) {
    const std::string* projList = t.find("Projectile_Types");
    if (!projList) return std::nullopt;
    const auto names = splitList(*projList);
    if (names.empty()) return std::nullopt;
    const GameObjectType* proj = db.find(names.front());
    if (!proj) return std::nullopt;

    WeaponSpec w;
    w.source = &t;
    w.projectile = proj;
    w.minRange = float(t.number("Targeting_Min_Attack_Distance").value_or(0));
    w.maxRange = float(t.number("Targeting_Max_Attack_Distance").value_or(100));
    w.rechargeTicks = std::max(1, toTicks(t.number("Projectile_Fire_Recharge_Seconds").value_or(1.0)));
    w.pulseCount = std::max(1, int(t.number("Projectile_Fire_Pulse_Count").value_or(1)));
    w.pulseDelayTicks = toTicks(t.number("Projectile_Fire_Pulse_Delay_Seconds").value_or(0));
    w.pellets = std::clamp(int(t.number("Projectile_Fire_Spread_Amount").value_or(1)), 1, 32);
    w.spreadRadians = float(t.number("Projectile_Fire_Spread_Angle").value_or(0)) * 3.14159265f / 180.f;
    w.inaccuracy = float(t.number("Targeting_Fire_Inaccuracy").value_or(0)) * 4.f;

    if (const std::string* dt = proj->find("Damage_Type")) w.damageType = trim(*dt);
    w.projectileSpeed = std::max(1.f, float(proj->number("Max_Speed").value_or(10)));
    w.damage = float(proj->number("Projectile_Damage").value_or(0));
    w.blastDamage = float(proj->number("Projectile_Blast_Area_Damage").value_or(0));
    w.blastRange = float(proj->number("Projectile_Blast_Area_Range").value_or(0));
    w.maxFlight = float(proj->number("Projectile_Max_Flight_Distance").value_or(std::max(500.0, double(w.maxRange) * 2)));
    // A projectile that can't fly the weapon's full range limits it (Grunt shotgun pellets:
    // range 100, pellets fly 80); keep a little margin for the target's own radius.
    if (w.maxFlight > 0 && w.maxFlight < w.maxRange + 10) w.maxRange = std::max(w.minRange, w.maxFlight - 10);
    if (w.damage <= 0 && w.blastDamage <= 0) return std::nullopt; // effect-only projectiles (handled by abilities later)
    return w;
}

// Beam weapons (RadiationManagerBehaviorType, fired by a Radiation_Weapon_Ability): the beam
// sweeps for Radiation_Sweep_Duration_In_Secs dealing Radiation_Direct_Damage every tick, then
// rests for Radiation_Cooldown_In_Secs. Simulated as an instant-hit weapon with that cycle;
// its range is the ability's Activation_Max_Range (else 60% of the beam length).
std::optional<WeaponSpec> beamFrom(const GameObjectType& t) {
    const auto perTick = t.number("Radiation_Direct_Damage");
    if (!perTick || *perTick <= 0) return std::nullopt;
    float range = 0;
    for (const ObjectField& f : t.fields()) {
        const std::string p = upper(f.path);
        if (p.find("RADIATION_WEAPON_ABILITY[") != std::string::npos && p.size() > 21 &&
            p.compare(p.size() - 21, 21, ".ACTIVATION_MAX_RANGE") == 0 && !f.values.empty())
            range = float(std::atof(f.values.back().c_str()));
    }
    if (range <= 0) range = float(t.number("Radiation_Beam_Max_Length").value_or(200)) * 0.6f;
    const int sweep = std::max(1, toTicks(t.number("Radiation_Sweep_Duration_In_Secs").value_or(1.0)));
    const int rest = toTicks(t.number("Radiation_Cooldown_In_Secs").value_or(0.5));
    WeaponSpec w;
    w.source = &t;
    w.maxRange = range;
    w.rechargeTicks = std::max(1, sweep + rest);
    w.projectileSpeed = 1e6f; // a beam: lands at once
    w.damage = float(*perTick) * float(sweep);
    if (const std::string* dt = t.find("Radiation_Damage_Type")) w.damageType = trim(*dt);
    w.maxFlight = range + 100;
    return w;
}

} // namespace

void DamageTable::set(const std::string& damage, const std::string& armor, float mod) {
    mods_[upper(damage) + "|" + upper(armor)] = mod;
}

float DamageTable::modifier(const std::string& damage, const std::string& armor) const {
    if (damage.empty() || armor.empty()) return 1.f;
    auto it = mods_.find(upper(damage) + "|" + upper(armor));
    return it == mods_.end() ? 1.f : it->second;
}

DamageTable loadDamageTable(MegVfs& vfs) {
    DamageTable table;
    const char* path = "Data\\XML\\DamageToArmor.xml";
    // Each layer's table on top of the one below: a mod's entry replaces the base one.
    for (const auto& v : vfs.readAllLayers(path)) {
        XmlDocument doc = parseXml(std::string(v.data.begin(), v.data.end()));
        std::vector<const XmlNode*> stack;
        for (const auto& c : doc.root.children) stack.push_back(c.get());
        while (!stack.empty()) {
            const XmlNode* n = stack.back();
            stack.pop_back();
            if (iequals(n->tag, "Damage_To_Armor_Mod")) {
                const auto parts = splitList(n->text);
                if (parts.size() == 3) table.set(parts[0], parts[1], float(std::atof(parts[2].c_str())));
            }
            for (const auto& c : n->children) stack.push_back(c.get());
        }
    }
    return table;
}

float WeaponSpec::damagePerSecond() const {
    const float perPulse = (damage + blastDamage) * float(pellets);
    const float cycle = float(rechargeTicks + pulseDelayTicks * (pulseCount - 1)) / 30.f;
    return cycle > 0 ? perPulse * float(pulseCount) / cycle : 0;
}

std::vector<WeaponSpec> weaponsFor(const GameObjectType& unit, const GameDatabase& db) {
    std::vector<WeaponSpec> out;
    if (auto w = weaponFrom(unit, db)) out.push_back(*w);
    else if (auto b = beamFrom(unit)) out.push_back(*b);
    if (const std::string* hps = unit.find("Attached_Hard_Points")) {
        // "HP_Type, bone" pairs, possibly across lines.
        const auto items = splitList(*hps);
        for (size_t i = 0; i < items.size(); i += 2) {
            const GameObjectType* hp = db.find(items[i]);
            if (!hp) continue;
            if (auto w = weaponFrom(*hp, db)) out.push_back(*w);
            else if (auto b = beamFrom(*hp)) out.push_back(*b);
        }
    }
    return out;
}

} // namespace alamo
