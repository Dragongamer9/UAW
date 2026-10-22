// Tactical simulation: units on a map, ticked at a fixed rate.
//
// Movement parameters come straight from the unit XML and are read the way the
// data suggests (to be confirmed against the original game):
//   Max_Speed               world units per logic frame
//   Max_Rate_Of_Turn        degrees per logic frame while moving
//   Stopped_Rate_Of_Turn    degrees per logic frame while turning in place
//   OverrideAcceleration    fraction of Max_Speed gained per frame (default 0.1)
//   Custom_Hard_XExtent     collision radius (fallback: soft footprint radius, then 6)
// GameConstants.xml notes the game is tuned for 30 frames per second.
#pragma once

#include "alamo/combat.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/nav.hpp"
#include "alamo/ted.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alamo {

constexpr int kTicksPerSecond = 30;

enum class MoveClass { Infantry, Vehicle, Hover, Walker, LargeWalker, Air, Other };

struct Unit {
    uint32_t id = 0;
    const GameObjectType* type = nullptr;
    int owner = 0;
    Vec3 position{};
    float facing = 0;          // radians, 0 = +X (east), counter-clockwise
    float speed = 0;           // current, units per tick

    // From the XML.
    MoveClass moveClass = MoveClass::Other;
    float maxSpeed = 1;        // units per tick
    float turnRate = 0.5f;     // radians per tick while moving
    float stoppedTurnRate = 0.2f;
    float acceleration = 0.1f; // units per tick per tick
    float radius = 6;
    float health = 100, maxHealth = 100;

    std::string armorType;

    // Orders.
    std::vector<Vec3> path;    // remaining waypoints
    Vec3 goal{};
    int stuckTicks = 0;
    uint32_t attackOrder = 0;  // explicit attack target (chase it), 0 = none
    bool attackMove = false;   // heading to `goal` but stopping to fight anything in range
    uint32_t target = 0;       // what the weapons are currently shooting at
    int repathTicks = 0;
    bool moving() const { return !path.empty(); }

    struct WeaponState {
        WeaponSpec spec;
        int cooldown = 0;      // ticks until the next burst may start
        int pulsesLeft = 0;    // remaining pulses in the current burst
        int pulseTimer = 0;
    };
    std::vector<WeaponState> weapons;
    float maxRange() const;
    bool alive() const { return health > 0; }
};

struct Projectile {
    int owner = 0;
    uint32_t shooter = 0;
    uint32_t target = 0;
    Vec3 position{}, velocity{};
    float travelled = 0;
    float distance = 0;        // to the aim point; the shot lands when travelled reaches it
    float damage = 0, blastDamage = 0, blastRange = 0;
    std::string damageType;
    const GameObjectType* type = nullptr;
};

struct DeathEvent {
    uint32_t id;
    const GameObjectType* type;
    int owner;
    Vec3 position;
    uint64_t tick;
};

class World {
public:
    // `damage` may be null (every modifier is then 1.0).
    World(const Map& map, const GameDatabase& db, const DamageTable* damage = nullptr, uint64_t seed = 1);

    // Places a unit; returns its id, or 0 if the type is unknown.
    uint32_t spawn(const std::string& typeName, int owner, Vec3 position, float facingRadians = 0);
    Unit* unit(uint32_t id);
    const std::vector<Unit>& units() const { return units_; }

    // Move a group: they spread into a formation around `target` and each path-finds there.
    void commandMove(const std::vector<uint32_t>& ids, Vec3 target);
    void commandStop(const std::vector<uint32_t>& ids);
    // Move toward a point, halting to fight enemies that come into range, then carrying on.
    void commandAttackMove(const std::vector<uint32_t>& ids, Vec3 target);
    // Chase and shoot a specific unit until it dies.
    void commandAttack(const std::vector<uint32_t>& ids, uint32_t target);

    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<DeathEvent>& deaths() const { return deaths_; } // everything that has died
    bool enemies(int ownerA, int ownerB) const { return ownerA != ownerB; }

    void tick();
    uint64_t tickCount() const { return ticks_; }
    float seconds() const { return float(ticks_) / kTicksPerSecond; }

    const NavGrid& nav() const { return nav_; }
    const Map& map() const { return map_; }

    // Unit ids within `radius` of a ground point, nearest first.
    std::vector<uint32_t> unitsNear(Vec3 p, float radius) const;

private:
    void moveUnit(Unit& u);
    void separate();
    void updateTargeting(Unit& u);
    void fireWeapons(Unit& u);
    void updateProjectiles();
    void applyDamage(Unit& victim, float amount, const std::string& damageType);
    void removeDead();
    float random01();

    const Map& map_;
    const GameDatabase& db_;
    NavGrid nav_;
    const DamageTable* damage_;
    std::vector<Unit> units_;
    std::vector<Projectile> projectiles_;
    std::vector<DeathEvent> deaths_;
    uint32_t nextId_ = 1;
    uint64_t ticks_ = 0;
    uint64_t rng_;
};

float angleDiff(float from, float to); // shortest signed difference, radians

} // namespace alamo
