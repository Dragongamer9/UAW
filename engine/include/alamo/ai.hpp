// Skirmish AI: one per computer player. Everything it knows about a faction comes from
// the same data the player uses (tech tree, costs, economy specs, weapons), so it works
// for every faction and every mod without per-faction scripts.
//
// Each decision step (twice a second) it
//   1. keeps builders busy: economy first (depots, Matter Engines, Supply, Reapers),
//      Novus power where needed, one of each production structure, then defences and
//      tech, a second production line when rich;
//   2. keeps every production structure queuing combat units (and a spare builder);
//   3. gathers the army at a rally point, defends the base when enemies come near and
//      attacks the nearest enemy structure once the army is big enough (the threshold
//      grows with game time), regrouping when the attack wave is spent.
// Its random choices use its own seeded generator, so matches replay exactly.
#pragma once

#include "alamo/sim.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace alamo {

class SkirmishAI {
public:
    SkirmishAI(World& world, int player, uint64_t seed = 1);

    // Call every tick (after or before World::tick); it acts every few ticks.
    void update();

    int player() const { return player_; }
    bool attacking() const { return attacking_; }
    const std::string& lastAction() const { return lastAction_; }

private:
    enum class Role { Economy, Power, Production, Defense, Tech, Other };
    Role roleOf(const GameObjectType& t) const;
    bool isCombatUnit(const GameObjectType& t) const;
    bool isBuilderType(const GameObjectType& t) const;
    void manageConstruction();
    void manageProduction();
    void manageArmy();
    void manageReapers();
    bool tryConstruct(Unit& builder, const std::string& type);
    bool findSite(const GameObjectType& t, Vec3& out);
    bool coveredByPower(Vec3 p, float extra) const;
    Vec3 baseCentre() const;
    Vec3 rallyPoint() const;
    uint32_t rand(uint32_t n);
    float armyValue(const std::vector<uint32_t>& army) const;
    float enemyStrength() const;
    float wealth() const;

    World& w_;
    const GameDatabase& db_;
    int player_;
    uint64_t rng_;
    int tick_ = 0;
    bool attacking_ = false;
    uint32_t attackTarget_ = 0;
    int waveStartSize_ = 0;
    float waveStartValue_ = 0;
    float scale_ = 10000;
    int siteSpin_ = 0;
    std::unordered_map<const GameObjectType*, Role> roles_;
    std::unordered_map<const GameObjectType*, bool> combat_, builder_;
    std::vector<std::string> producers_; // structure types that produce some combat unit
    std::string lastAction_;
};

// A player is out when it has no structures and no unit able to construct one.
bool defeated(const World& w, int player);

} // namespace alamo
