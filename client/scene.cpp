#include <cstdlib>
#include <cstdio>
#include "scene.hpp"

#include <algorithm>
#include <cstdio>

namespace uaw {

alamo::Vec4 ownerColor(int owner) {
    static const alamo::Vec4 colors[] = {{0.9f, 0.25f, 0.2f, 1}, {0.25f, 0.45f, 0.95f, 1}, {0.3f, 0.75f, 0.3f, 1},
                                         {0.75f, 0.75f, 0.72f, 1}, {0.55f, 0.55f, 0.52f, 1}, {0.92f, 0.78f, 0.18f, 1},
                                         {0.65f, 0.3f, 0.8f, 1},  {0.2f, 0.78f, 0.8f, 1}};
    return colors[std::clamp(owner, 0, 7)];
}

Scene::Scene(Renderer& r, const alamo::GameDatabase& db, const alamo::Map& map, bool includeAllGroups) : map_(map) {
    const int mp = map.groupIndex("Multiplayer");
    for (const alamo::MapObject& o : map.objects) {
        if (!includeAllGroups && mp >= 0 && !o.groups.empty() &&
            std::find(o.groups.begin(), o.groups.end(), uint32_t(mp)) == o.groups.end())
            continue;
        const alamo::GameObjectType* t = db.findByCrc(o.typeCrc);
        if (!t) continue;
        // Harvestable piles live in the simulation (they shrink and disappear); drawn by UnitControl.
        if (!includeAllGroups && t->number("Resource_Units").value_or(0) > 0) continue;

        SceneObject so;
        so.type = t;
        so.position = o.position;
        const std::string& cat = t->category;
        int slot = -1;
        if (std::sscanf(t->name.c_str(), "Marker_Player_%d_Start", &slot) == 1) {
            so.kind = SceneObject::Kind::Start;
            so.startSlot = slot;
            so.color = ownerColor(slot);
            so.transform = Mat4::translate(o.position[0], o.position[1], o.position[2]) * Mat4::scale3(6, 6, 160);
            objects_.push_back(so);
            ++stats_.starts;
            continue;
        }
        if (t->name.rfind("Marker_", 0) == 0) continue; // editor-only helpers

        if (const std::string* b = t->find("Behavior"); b && b->find("SKY_DOME") != std::string::npos) {
            // Sky domes go round the camera (Scene::drawSky), not on the map; the first one is used.
            if (const std::string* m = t->find("Land_Model_Name"); m && !sky_)
                if ((sky_ = r.model(alamo::trim(*m)))) ++stats_.objects;
            continue;
        }
        if (cat.find("Structure") != std::string::npos || cat.find("Walker") != std::string::npos) so.kind = SceneObject::Kind::Structure;
        else if (cat.find("Units") != std::string::npos || cat.find("Heroes") != std::string::npos) so.kind = SceneObject::Kind::Unit;
        else if (cat.find("Resource") != std::string::npos) so.kind = SceneObject::Kind::Resource;

        float typeScale = 1;
        if (auto s = t->number("Scale_Factor"); s && *s > 0) typeScale = float(*s);
        const float scale = o.scale * typeScale;
        const float yaw = o.rotation[2] * 3.14159265f / 180.f;
        so.transform = Mat4::translate(o.position[0], o.position[1], o.position[2]) * Mat4::rotateZ(yaw) * Mat4::scale(scale);

        if (const std::string* modelName = t->find("Land_Model_Name"); modelName && !alamo::trim(*modelName).empty())
            so.model = r.model(alamo::trim(*modelName));

        switch (so.kind) {
        case SceneObject::Kind::Structure: so.boxSize = 60; so.color = ownerColor(o.owner); break;
        case SceneObject::Kind::Unit: so.boxSize = 12; so.color = ownerColor(o.owner); break;
        case SceneObject::Kind::Resource: so.boxSize = 10; so.color = {0.45f, 0.85f, 0.95f, 1}; break;
        default: so.boxSize = 8; so.color = {0.55f, 0.5f, 0.45f, 1}; break;
        }
        if (so.model && std::getenv("UAW_BIG_PROPS")) {
            const float ext = std::max({so.model->boundsMax[0] - so.model->boundsMin[0], so.model->boundsMax[1] - so.model->boundsMin[1],
                                        so.model->boundsMax[2] - so.model->boundsMin[2]}) * scale;
            if (ext > 400) std::fprintf(stderr, "big prop %s (%.0f) at %.0f,%.0f\n", t->name.c_str(), ext, o.position[0], o.position[1]);
        }
        ++stats_.objects;
        if (so.model) ++stats_.withModel;
        else ++stats_.placeholders;
        objects_.push_back(so);
    }
}

void Scene::useEnvironmentSky(Renderer& r, const alamo::GameDatabase& db) {
    // No sky dome placed on the map: the one its lighting preset names.
    if (sky_ || map_.environment.skyDome.empty()) return;
    if (const alamo::GameObjectType* t = db.find(map_.environment.skyDome))
        if (const std::string* m = t->find("Land_Model_Name"); m && !alamo::trim(*m).empty()) sky_ = r.model(alamo::trim(*m));
}

void Scene::draw(Renderer& r, bool showPlaceholders, bool showStarts) const {
    r.drawTerrain();
    for (const SceneObject& o : objects_) {
        if (o.kind == SceneObject::Kind::Start) {
            if (showStarts) r.drawBox(o.transform, o.color);
            continue;
        }
        if (o.model) {
            r.drawModel(*o.model, o.transform);
        } else if (showPlaceholders) {
            const float s = o.boxSize;
            const float h = o.kind == SceneObject::Kind::Structure ? s * 0.6f : s;
            r.drawBox(Mat4::translate(o.position[0], o.position[1], o.position[2]) * Mat4::scale3(s, s, h), o.color);
        }
    }
}

alamo::Vec3 Scene::startPosition(int slot) const {
    for (const SceneObject& o : objects_)
        if (o.kind == SceneObject::Kind::Start && o.startSlot == slot) return o.position;
    return {map_.sizeX() / 2, map_.sizeY() / 2, map_.heightAt(map_.sizeX() / 2, map_.sizeY() / 2)};
}

} // namespace uaw
