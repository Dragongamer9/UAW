// Research trees.
//
// Each faction's tree is a compiled script, Data\Scripts\Miscellaneous\<FACTION>_PACKAGED_
// RESEARCH_TREE.LUA. Running its Init_Research_Tree (in LuaVM) leaves one global table per
// branch (Branch_QUANTUM, ...) holding suites, each with a path ("A", "B", "C"), an index
// within the path (1 first), ResearchCost, TotalResearchTime and what it changes:
//   UnlocksTypes / UnlocksPatches      {ObjectType}             units, heroes, hardpoints, patches
//   UnlocksGenerators / LocksGenerators {EffectName}           effect generators by name
//   UnlocksEffects / LocksEffects       {EffectName, LockThisEffect}
//   UnlocksUnitAbilities / Locks...     {TypeName, AbilityName}  Unit_Abilities_Data entries
//   UnlocksSpecialAbilities / Locks...  {ObjectType, AbilityName} Abilities block entries
// Everything a suite unlocks starts locked; what it locks starts available (Research_Common
// locks the Unlocks* lists at game start). A suite needs the one before it in its path; a
// player researches one suite at a time (more with Enables_Research facilities, which are
// strategic-map buildings) and at most six per game.
#pragma once

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"

#include <set>
#include <string>
#include <vector>

namespace alamo {

struct ResearchChange {
    enum class Kind { Type, Generator, Effect, UnitAbility, SpecialAbility };
    Kind kind = Kind::Type;
    std::string name;          // object type, generator, effect or ability name
    std::string objectType;    // abilities: the type they belong to
    bool locks = false;        // true: researching turns it off (Locks* lists, LockThisEffect)
};

struct ResearchSuite {
    std::string branch;        // e.g. QUANTUM
    std::string path;          // "A", "B" or "C"
    int index = 1;             // tier within the path
    float cost = 0;
    float seconds = 0;
    std::string title;         // TitleTextID of the first change
    std::vector<ResearchChange> changes;
    std::string id() const { return path + std::to_string(index); }
};

struct ResearchTree {
    std::string faction;       // upper case: ALIEN, NOVUS, MASARI
    std::vector<ResearchSuite> suites; // by path, then index
    const ResearchSuite* find(std::string_view path, int index) const;
};

// Every faction's tree found in the VFS.
std::vector<ResearchTree> loadResearchTrees(MegVfs& vfs);

// What is locked for one player. Names are upper case; abilities are "TYPE|ABILITY".
struct ResearchLocks {
    std::set<std::string> types, generators, effects, abilities;
    static ResearchLocks initial(const ResearchTree& tree);
    void apply(const ResearchSuite& suite);
    bool empty() const { return types.empty() && generators.empty() && effects.empty() && abilities.empty(); }
    bool typeLocked(const GameObjectType& t, const GameDatabase& db) const;
    bool generatorLocked(std::string_view name) const;
    bool effectLocked(std::string_view name) const;
    // Same, for names already in upper case (hot paths).
    bool generatorLockedUpper(const std::string& n) const { return !generators.empty() && generators.count(n); }
    bool effectLockedUpper(const std::string& n) const { return !effects.empty() && effects.count(n); }
    // `type` or any type it is a variant of.
    bool abilityLocked(const GameObjectType& type, std::string_view ability, const GameDatabase& db) const;
};

} // namespace alamo
