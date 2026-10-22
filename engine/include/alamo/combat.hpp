// Weapons, projectiles and the damage-versus-armor table.
//
// Weapon fields are read from a unit's WeaponBehaviorType (or the same fields at
// top level), and from every hardpoint listed in its Attached_Hard_Points
// ("HP_Type, bone" pairs):
//   Projectile_Types, Targeting_Min/Max_Attack_Distance, Projectile_Fire_Recharge_Seconds,
//   Projectile_Fire_Pulse_Count, Projectile_Fire_Pulse_Delay_Seconds,
//   Projectile_Fire_Spread_Amount (pellets per pulse) / _Angle (cone, degrees),
//   Targeting_Fire_Inaccuracy
// Projectile fields: Max_Speed (units per frame), Damage_Type, Projectile_Damage,
//   Projectile_Blast_Area_Damage / _Range, Projectile_Max_Flight_Distance.
// Damage dealt = damage x DamageToArmor.xml modifier (1.0 when not listed).
#pragma once

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace alamo {

class DamageTable {
public:
    void set(const std::string& damage, const std::string& armor, float mod);
    float modifier(const std::string& damage, const std::string& armor) const; // 1.0 if unknown
    size_t size() const { return mods_.size(); }

private:
    std::unordered_map<std::string, float> mods_;
};

// Reads Data/XML/DamageToArmor.xml; an empty table if the file is missing.
DamageTable loadDamageTable(MegVfs& vfs);

// Targeting priority sets (GroundUnitTargetingPriorities.xml), named by a unit's
// Targeting_Priority_Set: which enemies it picks on its own. Lower priority numbers first; enemies
// not listed come after the listed ones; exclusions are never picked on its own (but may still be
// attacked when ordered); restrictions are never attacked at all.
struct TargetPrioritySet {
    std::vector<std::pair<std::string, int>> priorities; // category or type name (upper case)
    std::string categoryExclusions, categoryRestrictions; // category lists as written
    std::vector<std::string> typeExclusions;              // upper case
};
class TargetPriorities {
public:
    void set(const std::string& name, TargetPrioritySet s);
    const TargetPrioritySet* find(const std::string& name) const;
    size_t size() const { return sets_.size(); }

private:
    std::unordered_map<std::string, TargetPrioritySet> sets_; // by upper-case name
};
TargetPriorities loadTargetPriorities(MegVfs& vfs);

struct WeaponSpec {
    const GameObjectType* source = nullptr;     // unit or hardpoint carrying the weapon
    const GameObjectType* projectile = nullptr;
    std::string damageType;
    float minRange = 0, maxRange = 100;
    int rechargeTicks = 30;
    int pulseCount = 1;
    int pulseDelayTicks = 0;
    int pellets = 1;
    float spreadRadians = 0;
    float inaccuracy = 0;                       // world units of aim scatter
    float projectileSpeed = 10;                 // units per tick
    float damage = 0;
    float blastDamage = 0, blastRange = 0;
    float maxFlight = 500;
    // Attack_Category_Restrictions: categories this weapon won't fire at ("Flying | Resource",
    // "~Flying" = air only). Points into the type's data (stable), or null.
    const std::string* restrictions = nullptr;
    // Foo Core (FooCoreBehaviorType): the damage is done by the core's foo fighters, which fly out
    // to the target; fooFighters of them, each hitting for `damage` every recharge.
    int fooFighters = 0;
    float fooHeal = 0;                          // heal mode: health given per fighter per recharge

    float damagePerSecond() const;              // for tooltips/balancing checks
};

// Every weapon a unit type fires: its own plus its hardpoints'.
std::vector<WeaponSpec> weaponsFor(const GameObjectType& unit, const GameDatabase& db);

} // namespace alamo
