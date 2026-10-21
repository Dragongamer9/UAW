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
};

struct Faction {
    std::string name;
    bool humanPlayable = false;
    bool aiPlayable = false;
};

class GameDatabase {
public:
    // Loads GameObjectFiles.xml and Factions.xml through the VFS.
    void load(MegVfs& vfs);

    const GameObjectType* find(std::string_view name) const;
    // Maps refer to object types by crc32 of the uppercase name.
    const GameObjectType* findByCrc(uint32_t crc) const;
    const std::vector<GameObjectType>& objects() const { return objects_; }
    const std::vector<Faction>& factions() const { return factions_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    size_t fileCount() const { return fileCount_; }

private:
    void loadObjectFile(MegVfs& vfs, const std::string& path);
    void resolveVariants();

    std::vector<GameObjectType> objects_;
    std::unordered_map<std::string, size_t> byName_; // uppercase name
    std::unordered_map<uint32_t, size_t> byCrc_;
    std::vector<Faction> factions_;
    std::vector<std::string> warnings_;
    size_t fileCount_ = 0;
};

// Mounts every archive listed in <gameRoot>/Data/MegaFiles.xml, in order, and
// uses gameRoot for loose-file overrides. Returns archives that are listed but missing.
std::vector<std::string> mountGame(MegVfs& vfs, const std::filesystem::path& gameRoot);

// Reads a text file through the VFS (bytes as-is; game data is ASCII/Latin-1).
std::string readText(MegVfs& vfs, std::string_view path);

} // namespace alamo
