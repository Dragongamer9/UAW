// A map plus the objects placed on it, ready to draw.
#pragma once

#include "renderer.hpp"

#include <string>
#include <vector>

namespace uaw {

struct SceneObject {
    const alamo::GameObjectType* type = nullptr;
    const GpuModel* model = nullptr; // nullptr: draw a placeholder box
    Mat4 transform;
    alamo::Vec3 position{};
    alamo::Vec4 color{1, 1, 1, 1};   // placeholder colour / start beacon colour
    float boxSize = 10;
    enum class Kind { Prop, Unit, Structure, Resource, Start, Sky } kind = Kind::Prop;
    int startSlot = -1;
};

struct SceneStats {
    size_t objects = 0, withModel = 0, placeholders = 0, starts = 0;
};

class Scene {
public:
    // includeAllGroups=false keeps skirmish objects: those in the "Multiplayer"
    // editor group or in no group (campaign mission groups are left out).
    Scene(Renderer& r, const alamo::GameDatabase& db, const alamo::Map& map, bool includeAllGroups);

    void draw(Renderer& r, bool showPlaceholders) const;
    void drawSky(Renderer& r) const { if (sky_) r.drawSky(*sky_); }

    const alamo::Map& map() const { return map_; }
    const std::vector<SceneObject>& objects() const { return objects_; }
    const SceneStats& stats() const { return stats_; }
    alamo::Vec3 startPosition(int slot) const; // falls back to the map centre

private:
    const alamo::Map& map_;
    std::vector<SceneObject> objects_;
    const GpuModel* sky_ = nullptr;
    SceneStats stats_;
};

alamo::Vec4 ownerColor(int owner);

} // namespace uaw
