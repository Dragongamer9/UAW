// Tactical tech tree from Data/XML/ProductionDependencies.xml.
//
// Each <Dependency> is one way to build <Built_Object>; entries for the same
// object are OR'd. Within an entry, every type in <Tactical_Builder> and in
// <Tactical_Required_Objects> must exist (AND). In the retail data:
//   - units list the structure that produces them as Tactical_Builder
//     (Novus_Robotic_Infantry <- Novus_Robotic_Assembly);
//   - structures list the builder unit among Tactical_Required_Objects
//     (Novus_Robotic_Assembly <- Novus_Power_Router + Novus_Constructor).
// Strategic_* fields belong to the Global Conquest mode and are ignored here.
#pragma once

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace alamo {

struct BuildOption {
    std::vector<std::string> builders; // structure types that produce the object
    std::vector<std::string> required; // types that must exist (often the builder unit for structures)
};

class TechTree {
public:
    void add(const std::string& object, BuildOption option);
    // Replaces every way of building `object` (a mod redefining it).
    void replace(const std::string& object, std::vector<BuildOption> options);

    // Options for an object (empty if it can't be built in a tactical battle).
    const std::vector<BuildOption>& options(const std::string& object) const;

    using Has = std::function<bool(const std::string& type)>; // does the player own one?

    // Objects `producer` can produce now (it is one of the builders and every other condition holds).
    std::vector<std::string> producibleBy(const std::string& producer, const Has& has) const;
    // Structures the builder unit `builder` can construct now (it is listed as required and the rest exist).
    std::vector<std::string> constructibleBy(const std::string& builder, const Has& has) const;

    size_t size() const { return byObject_.size(); }
    const std::vector<std::string>& objects() const { return order_; } // every buildable object

private:
    std::unordered_map<std::string, std::vector<BuildOption>> byObject_; // uppercase key
    std::vector<std::string> order_;                                     // insertion order, display spelling
};

TechTree loadTechTree(MegVfs& vfs);

} // namespace alamo
