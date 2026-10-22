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
    if (w.damage <= 0 && w.blastDamage <= 0) return std::nullopt; // effect-only projectiles (handled by abilities later)
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
    if (!vfs.exists(path)) return table;
    XmlDocument doc = parseXml(readText(vfs, path));
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
    if (const std::string* hps = unit.find("Attached_Hard_Points")) {
        // "HP_Type, bone" pairs, possibly across lines.
        const auto items = splitList(*hps);
        for (size_t i = 0; i < items.size(); i += 2) {
            const GameObjectType* hp = db.find(items[i]);
            if (!hp) continue;
            if (auto w = weaponFrom(*hp, db)) out.push_back(*w);
        }
    }
    return out;
}

} // namespace alamo
