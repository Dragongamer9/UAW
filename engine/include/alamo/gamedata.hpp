// Game object database: every unit, structure, prop, projectile... defined
// in the files listed by Data/XML/GameObjectFiles.xml.
//
// Each object is flattened into path -> values, e.g.
//   TACTICAL_HEALTH                                         -> "300"
//   BEHAVIORTYPE[PRODUCTIONBEHAVIORTYPE].BUILD_COST_CREDITS -> "275"
//   ATTRIBUTES.TACTICAL_SELL_TIME_SECONDS                   -> "2.0"
// Named sub-objects become TAG[NAME]; unnamed containers become TAG.
// Variants (<Variant_Of_Existing_Type>) start from a copy of their base type
// and override whatever paths they define themselves.
#pragma once

#include "alamo/meg.hpp"
#include "alamo/xml.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace alamo {

struct ObjectField {
    std::string path;                 // display spelling, e.g. BehaviorType[ProductionBehaviorType].Build_Cost_Credits
    std::vector<std::string> values;  // repeated tags keep every value in order
};

class GameObjectType {
public:
    std::string name;
    std::string category;    // element tag, e.g. X10_Alien_Units
    std::string sourceFile;  // e.g. DATA\XML\UNITS\ALIEN_INF_GRUNT.XML
    size_t layer = 0;        // VFS layer it came from (0 = base game, then each mod)
    std::string variantOf;   // base type name, empty if none

    // Exact path lookup (case-insensitive). Returns the last value.
    const std::string* get(std::string_view path) const;
    // First field whose final segment matches (top-level fields first).
    const std::string* find(std::string_view leaf) const;
    std::optional<double> number(std::string_view leaf) const;

    const std::vector<ObjectField>& fields() const { return fields_; }

    void addValue(const std::string& path, std::string value);
    void overlay(const GameObjectType& other); // other's paths replace ours

private:
    std::vector<ObjectField> fields_;
    std::unordered_map<std::string, size_t> index_; // uppercase path -> fields_ index
    // find() results by upper-case leaf: fields_ index, -1 for none. Indices stay valid when the
    // object is copied (variants); any change to the fields clears it.
    mutable std::unordered_map<std::string, long> findCache_;
};

struct Faction {
    std::string name;
    bool humanPlayable = false;
    bool aiPlayable = false;
    float multiplayerCredits = 0;  // Multiplayer_Starting_Credits
    int multiplayerPopCap = 90;    // Pop_Cap_For_Multiplayer_Games
    float baselineBankCapacity = 0; // Baseline_Bank_Capacity (Masari: bank limit without Matter Engines)
};

class GameDatabase {
public:
    // Loads GameObjectFiles.xml and Factions.xml through the VFS.
    void load(MegVfs& vfs);
    // Loads the effect library (Data/XML/Effects/EffectSystemFiles.xml) instead: each
    // <Effect Name=...> becomes an entry, e.g. AlienHarvestResourcesEffect.
    void loadEffects(MegVfs& vfs);

    const GameObjectType* find(std::string_view name) const;
    // Maps refer to object types by crc32 of the uppercase name.
    const GameObjectType* findByCrc(uint32_t crc) const;
    const std::vector<GameObjectType>& objects() const { return objects_; }
    const std::vector<Faction>& factions() const { return factions_; }
    const Faction* faction(std::string_view name) const;
    const std::vector<std::string>& warnings() const { return warnings_; }
    size_t fileCount() const { return fileCount_; }
    size_t overriddenCount() const { return overridden_; } // objects a mod replaced

private:
    void loadFileList(MegVfs& vfs, const std::string& listPath, const char* rootTag, const std::string& dir);
    void loadObjectFile(MegVfs& vfs, const std::string& path);
    void loadObjectFile(MegVfs& vfs, const std::string& path, size_t layer, const std::string& text);
    void resolveVariants();

    std::vector<GameObjectType> objects_;
    std::unordered_map<std::string, size_t> byName_; // uppercase name
    std::unordered_map<uint32_t, size_t> byCrc_;
    std::vector<Faction> factions_;
    std::vector<std::string> warnings_;
    size_t fileCount_ = 0;
    size_t overridden_ = 0;
};

// Mounts every archive listed in <gameRoot>/Data/MegaFiles.xml, in order, and
// uses gameRoot for loose-file overrides. Returns archives that are listed but missing.
// Mounts the base game, then each mod as a layer on top (bottom to top, so the last
// mod wins where they overlap). Returns archives listed but not found.
std::vector<std::string> mountGame(MegVfs& vfs, const std::filesystem::path& gameRoot,
                                   const std::vector<std::filesystem::path>& mods);
std::vector<std::string> mountGame(MegVfs& vfs, const std::filesystem::path& gameRoot,
                                   const std::filesystem::path& modRoot = {});

// Mod folders (those with Data\MegaFiles.xml) under <game>\Mods or <game>\..\Mods,
// sorted by name: the default load order, bottom to top.
std::vector<std::filesystem::path> installedMods(const std::filesystem::path& gameRoot);

// Resolves --mod: a folder path, or a mod name (or prefix) under the game's Mods folder.
// Returns empty when nothing matches.
std::filesystem::path findMod(const std::filesystem::path& gameRoot, const std::string& mod);

// Reads a text file through the VFS (bytes as-is; game data is ASCII/Latin-1).
std::string readText(MegVfs& vfs, std::string_view path);

} // namespace alamo
