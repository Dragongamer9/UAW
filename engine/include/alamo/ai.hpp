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
//      grows with game time), regrouping when the attack wave is spent;
//   4. researches the next suite of its tree whenever credits allow (suites that unlock
//      units first, then the lowest tier, paths in turn);
//   5. fires superweapons where they catch the most enemy value;
//   6. uses its units' ordered abilities when World::abilityTarget finds a worthwhile
//      target (respecting the data's Unit_Ability_AI_Only_At_Health_Lower hints).
// Its random choices use its own seeded generator, so matches replay exactly.
#pragma once

#include "alamo/sim.hpp"

#include <string>
#include <unordered_map>
#include <map>
#include <unordered_set>
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
    enum class Role { Economy, Power, Production, Defense, Tech, Research, Other };
    Role roleOf(const GameObjectType& t) const;
    bool isCombatUnit(const GameObjectType& t) const;
    bool isBuilderType(const GameObjectType& t) const;
    bool isUtilityUnit(const GameObjectType& t) const;
    std::string pickUnit(const Unit& structure, const std::vector<std::string>& options, float credits);
    bool unlocksUnit(const std::string& upgrade) const;
    bool unlockerOffered(const std::vector<std::string>& options) const;
    std::string pickUpgrade(const Unit& structure, const std::vector<std::string>& options, float credits);
    void manageConstruction();
    void manageProduction();
    void manageArmy();
    void manageReapers();
    void manageResearch();
    void manageAbilities();
    void manageSuperweapons();
    bool tryConstruct(Unit& builder, const std::string& type);
    bool findSite(const GameObjectType& t, Vec3& out);
    bool coveredByPower(Vec3 p, float extra) const;
    bool joinsNetwork(Vec3 p, float reach) const;
    Vec3 baseCentre() const;
    Vec3 rallyPoint() const;
    uint32_t rand(uint32_t n);
    float armyValue(const std::vector<uint32_t>& army) const;
    float enemyStrength() const;
    // Fog of war: what this player knows. Enemy structures seen (and the enemies' starting
    // buildings, as start positions are known), enemy units seen, until it learns they are gone.
    void updateIntel();
    bool sees(const Unit& e) const;
    bool intelReady_ = false;
    std::map<uint32_t, Vec3> knownStructures_;
    std::map<uint32_t, float> knownUnits_; // id -> strength when last seen
    int scoutLeg_ = 0;
    // A wave's target that takes no damage for a minute is given up on (out of reach) for a while.
    float targetSince_ = 0, targetHealth_ = 0;
    std::map<uint32_t, float> givenUp_; // target id -> until when (seconds)
    float scoutSince_ = 0;
    float wealth() const;
    float typeStrength(const GameObjectType& t) const; // sqrt(health x dps), cached
    bool hitsAir(const GameObjectType& t) const;        // some weapon may target aircraft, cached
    mutable std::unordered_map<const GameObjectType*, bool> hitsAir_;

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
    std::unordered_map<const GameObjectType*, float> strength_;
    std::vector<std::string> producers_; // structure types that produce some combat unit
    std::unordered_set<const GameObjectType*> lineFactories_; // ...non-hero units only (not HQs)
    bool networkNeeded_ = false;              // Novus: a receiver found no powered site
    std::string lastAction_;
    std::unordered_map<std::string, float> blockedUntil_; // structure types that found no site
    std::unordered_map<std::string, bool> unlocks_;
    std::unordered_map<const GameObjectType*, std::string> squadOf_; // squad member -> squad type
    std::unordered_map<std::string, int> made_; // units queued so far, by type (roster rotation)
    float reserve_ = 0;                         // credits kept for the next structure
    float researchReserve_ = 0;                 // ...and for the next research suite
    uint32_t savingFor_ = 0;                    // structure saving up for an expensive unit
    float savingSince_ = 0, unitReserve_ = 0;
};

// A player is out when it has no structures and no unit able to construct one.
bool defeated(const World& w, int player);

} // namespace alamo
