// Player control of simulated units in the viewer: selection, orders, drawing.
#pragma once

#include "camera.hpp"
#include "renderer.hpp"

#include "alamo/sim.hpp"

#include <set>
#include <vector>

namespace uaw {

// Screen <-> world helpers for the current frame.
struct View {
    alamo::Vec3 eye, target;
    float fovY = 0.75f;
    int width = 1, height = 1;
    Mat4 viewProj;

    // Ray through a pixel, intersected with the terrain. False if it misses.
    bool groundAt(const alamo::Map& m, float px, float py, alamo::Vec3& out) const;
    // World point to pixel; false if behind the camera.
    bool toScreen(const alamo::Vec3& p, float& sx, float& sy) const;
};

struct SquadSpec {
    const char* type;
    int count;
};

// Spawns a small mixed squad for each start position (player 1 Novus, player 2
// Hierarchy, player 3 Masari, then repeating). Skips types missing from the data.
void spawnTestSquads(alamo::World& world, const std::vector<alamo::Vec3>& starts, const alamo::Map& map);
// Player 1 and player 2 squads facing each other across `centre`, `gap` units apart.
void spawnBattle(alamo::World& world, const alamo::Vec3& centre, float gap);

class UnitControl {
public:
    explicit UnitControl(int player) : player_(player) {}

    int player() const { return player_; }
    const std::set<uint32_t>& selection() const { return selected_; }

    void clear() { selected_.clear(); }
    // Click: pick the own unit nearest the ground point (within its radius + slack).
    void clickSelect(alamo::World& w, const alamo::Vec3& ground, bool add);
    // Drag box in screen space.
    void boxSelect(alamo::World& w, const View& v, float x0, float y0, float x1, float y1, bool add);
    void selectAllVisible(alamo::World& w, const View& v);
    void selectAll(alamo::World& w);
    void moveTo(alamo::World& w, const alamo::Vec3& ground);
    // Right click / RB: attack an enemy under the point, otherwise move there.
    void command(alamo::World& w, const alamo::Vec3& ground);
    void attackMove(alamo::World& w, const alamo::Vec3& ground);
    void stop(alamo::World& w);

    void draw(Renderer& r, alamo::World& w, float time) const;
    void drawBox(Renderer& r, const View& v, float x0, float y0, float x1, float y1) const; // drag rectangle

private:
    int player_;
    std::set<uint32_t> selected_;
    alamo::Vec3 lastOrder_{};
    float lastOrderTime_ = -100;
    bool attackOrder_ = false;
};

} // namespace uaw
