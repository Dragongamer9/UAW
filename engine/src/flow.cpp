// Novus flow: units with the FLOW behaviour travel along their side's power network. A move whose
// start is near one powered node (generator or transmitter) and whose end is near another goes
// there by walking to the first node (at most Maximum_Run_To_Flow, 400), riding the network to the
// second, and walking the rest, when that is clearly shorter than walking all the way. While
// flowing the unit is out of the battle (hidden, untargetable), like a passenger.
#include "alamo/sim.hpp"

#include <algorithm>
#include <cmath>

namespace alamo {

namespace {
constexpr float kRunToFlow = 400;   // GameConstants Maximum_Run_To_Flow
constexpr float kFlowSpeed = 25;    // units per tick along the network
constexpr float kMinSaving = 0.65f; // flow when the walking parts are under this share of the walk

float flat(Vec3 a, Vec3 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }
} // namespace

bool World::canFlow(const GameObjectType& t) const {
    if (auto it = flowCache_.find(&t); it != flowCache_.end()) return it->second;
    const std::string* lb = t.find("LandBehavior");
    bool yes = false;
    if (lb) {
        std::string s = *lb;
        for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
        for (size_t p = s.find("FLOW"); p != std::string::npos; p = s.find("FLOW", p + 1)) {
            const bool startOk = p == 0 || !std::isalnum(static_cast<unsigned char>(s[p - 1])) && s[p - 1] != '_';
            const bool endOk = p + 4 >= s.size() || (!std::isalnum(static_cast<unsigned char>(s[p + 4])) && s[p + 4] != '_');
            if (startOk && endOk) { yes = true; break; }
        }
    }
    return flowCache_[&t] = yes;
}

bool World::planFlow(Unit& u, Vec3 goal) {
    u.flowPlan = false;
    if (u.structure || u.flying() || u.inside || !canFlow(*u.type)) return false;
    const float walk = flat(u.position, goal);
    if (walk < 600) return false;
    using P = EconomySpec::Power;
    const Unit* in = nullptr;
    const Unit* out = nullptr;
    float bestIn = kRunToFlow, bestOut = kRunToFlow;
    for (const Unit& n : units_) {
        if (n.owner != u.owner || !n.powered || n.underConstruction || !n.alive()) continue;
        const P role = economy(*n.type).power;
        if (role != P::Generator && role != P::Transmitter) continue;
        const float a = flat(n.position, u.position) - n.radius, b = flat(n.position, goal) - n.radius;
        if (a < bestIn) { bestIn = a; in = &n; }
        if (b < bestOut) { bestOut = b; out = &n; }
    }
    if (!in || !out || in == out) return false;
    if (std::max(0.f, bestIn) + std::max(0.f, bestOut) > kMinSaving * walk) return false;
    u.flowPlan = true;
    u.flowFrom = in->position;
    u.flowTo = out->position;
    u.flowGoal = goal;
    u.goal = in->position;
    u.path = planPath(u, in->position);
    return true;
}

void World::updateFlow(Unit& u) {
    if (u.flowTicks > 0) { // under way along the network
        const float t = float(++u.flowTick) / float(u.flowTicks);
        for (int k = 0; k < 2; ++k) u.position[size_t(k)] = u.flowFrom[size_t(k)] + (u.flowTo[size_t(k)] - u.flowFrom[size_t(k)]) * std::min(t, 1.f);
        if (u.flowTick >= u.flowTicks) {
            u.flowTicks = 0;
            u.inside = 0;
            const Vec3 p = nav_.nearestPassable(u.flowTo);
            u.position = {p[0], p[1], map_.heightAt(p[0], p[1])};
            u.goal = u.flowGoal;
            u.path = planPath(u, u.flowGoal);
        }
        return;
    }
    if (!u.flowPlan) return;
    if (u.attackOrder || u.attackMove || u.boarding) { // fighting on the way: walk
        u.flowPlan = false;
        if (u.attackMove) { u.goal = u.flowGoal; u.path = planPath(u, u.flowGoal); }
        return;
    }
    // Close enough to the node: in.
    if (flat(u.position, u.flowFrom) < 70 || (!u.moving() && flat(u.position, u.flowFrom) < 140)) {
        u.flowPlan = false;
        u.path.clear();
        u.speed = 0;
        u.target = 0;
        u.inside = u.id; // out of the battle while flowing
        u.flowTick = 0;
        u.flowTicks = std::max(1, int(flat(u.flowFrom, u.flowTo) / kFlowSpeed));
        u.position[0] = u.flowFrom[0], u.position[1] = u.flowFrom[1];
    } else if (!u.moving()) {
        u.flowPlan = false; // couldn't get there: walk instead
        u.goal = u.flowGoal;
        u.path = planPath(u, u.flowGoal);
    }
}

} // namespace alamo
