#include "units.hpp"

#include <algorithm>
#include <cmath>

namespace uaw {

using alamo::Vec3;
using alamo::Vec4;

// ------------------------------------------------------------------ View

bool View::toScreen(const Vec3& p, float& sx, float& sy) const {
    const Mat4& m = viewProj;
    const float x = m.at(0, 0) * p[0] + m.at(0, 1) * p[1] + m.at(0, 2) * p[2] + m.at(0, 3);
    const float y = m.at(1, 0) * p[0] + m.at(1, 1) * p[1] + m.at(1, 2) * p[2] + m.at(1, 3);
    const float w = m.at(3, 0) * p[0] + m.at(3, 1) * p[1] + m.at(3, 2) * p[2] + m.at(3, 3);
    if (w <= 1e-4f) return false;
    sx = (x / w * 0.5f + 0.5f) * float(width);
    sy = (1 - (y / w * 0.5f + 0.5f)) * float(height);
    return true;
}

bool View::groundAt(const alamo::Map& m, float px, float py, Vec3& out) const {
    const Vec3 f = Mat4::normalize(Mat4::sub(target, eye));
    const Vec3 r = Mat4::normalize(Mat4::cross(f, {0, 0, 1}));
    const Vec3 u = Mat4::cross(r, f);
    const float t = std::tan(fovY / 2);
    const float nx = (2 * px / float(width) - 1) * t * float(width) / float(height);
    const float ny = (1 - 2 * py / float(height)) * t;
    const Vec3 d = Mat4::normalize({f[0] + r[0] * nx + u[0] * ny, f[1] + r[1] * nx + u[1] * ny, f[2] + r[2] * nx + u[2] * ny});
    // March until below the terrain, then bisect.
    float prev = 0, step = 8;
    const float maxDist = 40000;
    for (float s = step; s < maxDist; s += step) {
        const Vec3 p{eye[0] + d[0] * s, eye[1] + d[1] * s, eye[2] + d[2] * s};
        if (p[2] <= m.heightAt(p[0], p[1])) {
            float lo = prev, hi = s;
            for (int i = 0; i < 20; ++i) {
                const float mid = (lo + hi) / 2;
                const Vec3 q{eye[0] + d[0] * mid, eye[1] + d[1] * mid, eye[2] + d[2] * mid};
                (q[2] <= m.heightAt(q[0], q[1]) ? hi : lo) = mid;
            }
            out = {eye[0] + d[0] * hi, eye[1] + d[1] * hi, 0};
            out[2] = m.heightAt(out[0], out[1]);
            return out[0] >= 0 && out[1] >= 0 && out[0] <= m.sizeX() && out[1] <= m.sizeY();
        }
        prev = s;
        step = std::min(step * 1.05f, 60.f);
    }
    return false;
}

// ------------------------------------------------------------------ squads

namespace {
const std::vector<SquadSpec> kSquads[3] = {
        {{"Novus_Robotic_Infantry", 6}, {"Novus_Variant", 2}, {"Novus_Antimatter_Tank", 1}, {"Novus_Hero_Mech", 1}},
        {{"Alien_Grunt", 6}, {"Alien_Recon_Tank", 2}, {"Alien_Defiler", 1}, {"Alien_Hero_Orlok", 1}},
        {{"Masari_Disciple", 6}, {"Masari_Seeker", 2}, {"Masari_Peacebringer_Fire", 1}, {"Masari_Hero_Charos", 1}},
};

void placeSquad(alamo::World& world, int player, const Vec3& at, float facing, float ahead) {
    const float fx = std::cos(facing), fy = std::sin(facing);
    int i = 0;
    for (const SquadSpec& spec : kSquads[player % 3]) {
        for (int k = 0; k < spec.count; ++k, ++i) {
            const float row = float(i / 5), col = float(i % 5) - 2;
            const Vec3 p{at[0] + fx * (ahead - row * 40) - fy * col * 40, at[1] + fy * (ahead - row * 40) + fx * col * 40, 0};
            world.spawn(spec.type, player, p, facing);
        }
    }
}
} // namespace

void spawnBattle(alamo::World& world, const Vec3& centre, float gap) {
    placeSquad(world, 0, {centre[0] - gap / 2, centre[1], 0}, 0.f, 0.f);
    placeSquad(world, 1, {centre[0] + gap / 2, centre[1], 0}, 3.14159265f, 0.f);
    // Both sides attack-move through each other's starting line.
    std::vector<uint32_t> a, b;
    for (const alamo::Unit& u : world.units()) (u.owner == 0 ? a : b).push_back(u.id);
    world.commandAttackMove(a, {centre[0] + gap / 2, centre[1], 0});
    world.commandAttackMove(b, {centre[0] - gap / 2, centre[1], 0});
}

void spawnTestSquads(alamo::World& world, const std::vector<Vec3>& starts, const alamo::Map& map) {
    for (size_t p = 0; p < starts.size(); ++p) {
        const Vec3& s = starts[p];
        const float toCentre = std::atan2(map.sizeY() / 2 - s[1], map.sizeX() / 2 - s[0]);
        // A little in front of the start marker, in rows.
        placeSquad(world, int(p), s, toCentre, 160.f);
    }
}

// ------------------------------------------------------------------ selection and orders

void UnitControl::clickSelect(alamo::World& w, const Vec3& ground, bool add) {
    if (!add) selected_.clear();
    for (uint32_t id : w.unitsNear(ground, 12)) {
        const alamo::Unit* u = w.unit(id);
        if (u && u->owner == player_) {
            if (add && selected_.count(id)) selected_.erase(id);
            else selected_.insert(id);
            return;
        }
    }
}

void UnitControl::boxSelect(alamo::World& w, const View& v, float x0, float y0, float x1, float y1, bool add) {
    if (!add) selected_.clear();
    const float lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
    for (const alamo::Unit& u : w.units()) {
        if (u.owner != player_) continue;
        float sx, sy;
        if (v.toScreen(u.position, sx, sy) && sx >= lx && sx <= hx && sy >= ly && sy <= hy) selected_.insert(u.id);
    }
}

void UnitControl::selectAllVisible(alamo::World& w, const View& v) {
    boxSelect(w, v, 0, 0, float(v.width), float(v.height), false);
}

void UnitControl::selectAll(alamo::World& w) {
    selected_.clear();
    for (const alamo::Unit& u : w.units())
        if (u.owner == player_) selected_.insert(u.id);
}

void UnitControl::moveTo(alamo::World& w, const Vec3& ground) {
    if (selected_.empty()) return;
    w.commandMove(std::vector<uint32_t>(selected_.begin(), selected_.end()), ground);
    lastOrder_ = ground;
    lastOrderTime_ = w.seconds();
}

uint32_t UnitControl::menuOwner(alamo::World& w) const {
    uint32_t fallback = 0;
    for (uint32_t id : selected_) {
        const alamo::Unit* u = w.unit(id);
        if (!u || u->owner != player_) continue;
        if (!w.producible(id).empty() && u->structure) return id; // prefer structures
        if (!fallback && (!w.producible(id).empty() || !w.constructible(id).empty())) fallback = id;
    }
    return fallback;
}

std::vector<MenuItem> UnitControl::menu(alamo::World& w, const alamo::GameDatabase& db, const alamo::StringTable& text) const {
    std::vector<MenuItem> items;
    const uint32_t id = menuOwner(w);
    const alamo::Unit* u = w.unit(id);
    if (!u) return items;
    const alamo::Player* p = w.player(u->owner);
    auto add = [&](const std::string& type, bool construct) {
        const alamo::GameObjectType* t = db.find(type);
        if (!t) return;
        // Novus patches are activated abilities (cooldowns), not things you buy here.
        if (t->category.find("Patch") != std::string::npos) return;
        const alamo::BuildInfo info = alamo::buildInfo(*t, db);
        MenuItem m;
        m.type = t->name;
        m.label = text.displayName(*t);
        m.cost = info.cost;
        m.seconds = float(info.ticks) / alamo::kTicksPerSecond;
        m.construct = construct;
        m.upgrade = !construct && !info.spawnsUnit;
        m.affordable = !p || p->credits >= info.cost;
        items.push_back(std::move(m));
    };
    for (const auto& t : w.constructible(id)) add(t, true);
    for (const auto& t : w.producible(id)) add(t, false);
    // Units first, then upgrades; keep data order otherwise.
    std::stable_sort(items.begin(), items.end(), [](const MenuItem& a, const MenuItem& b) { return a.upgrade < b.upgrade; });
    return items;
}

void UnitControl::activate(alamo::World& w, const MenuItem& item) {
    const uint32_t id = menuOwner(w);
    if (!id) return;
    if (item.construct) {
        placingType_ = item.type;
        placingBuilder_ = id;
        say("Place " + item.label + " (click / A to place, right click / B to cancel)", w.seconds());
        return;
    }
    const alamo::OrderResult r = w.queueProduction(id, item.type);
    say(r == alamo::OrderResult::Ok ? "Queued " + item.label : item.label + ": " + alamo::toString(r), w.seconds());
}

void UnitControl::cancelQueue(alamo::World& w) {
    if (const uint32_t id = menuOwner(w)) {
        w.cancelProduction(id);
        say("Cancelled", w.seconds());
    }
}

void UnitControl::place(alamo::World& w) {
    if (!placing()) return;
    const alamo::OrderResult r = w.commandConstruct(placingBuilder_, placingType_, ghost_);
    say(r == alamo::OrderResult::Ok ? "Building" : std::string("Can't build here: ") + alamo::toString(r), w.seconds());
    if (r == alamo::OrderResult::Ok) placingType_.clear();
}

void UnitControl::command(alamo::World& w, const Vec3& ground) {
    if (selected_.empty()) return;
    // Selected structures take the click as their rally point.
    bool anyMobile = false;
    for (uint32_t id : selected_)
        if (const alamo::Unit* u = w.unit(id); u && u->structure) w.setRally(id, ground);
        else if (u) anyMobile = true;
    if (!anyMobile) {
        lastOrder_ = ground;
        lastOrderTime_ = w.seconds();
        attackOrder_ = false;
        say("Rally point set", w.seconds());
        return;
    }
    for (uint32_t id : w.unitsNear(ground, 10)) {
        const alamo::Unit* u = w.unit(id);
        if (u && w.enemies(player_, u->owner)) {
            w.commandAttack(std::vector<uint32_t>(selected_.begin(), selected_.end()), id);
            lastOrder_ = u->position;
            lastOrderTime_ = w.seconds();
            attackOrder_ = true;
            return;
        }
    }
    attackOrder_ = false;
    moveTo(w, ground);
}

void UnitControl::attackMove(alamo::World& w, const Vec3& ground) {
    if (selected_.empty()) return;
    w.commandAttackMove(std::vector<uint32_t>(selected_.begin(), selected_.end()), ground);
    lastOrder_ = ground;
    lastOrderTime_ = w.seconds();
    attackOrder_ = true;
}

void UnitControl::stop(alamo::World& w) { w.commandStop(std::vector<uint32_t>(selected_.begin(), selected_.end())); }

// ------------------------------------------------------------------ drawing

void UnitControl::draw(Renderer& r, alamo::World& w, float time) const {
    // Scrap piles: the model when available, else a gold heap that shrinks as it is mined.
    for (const alamo::ResourceNode& n : w.resources()) {
        if (n.remaining <= 0 || !showResources) continue;
        const float frac = std::clamp(n.remaining / std::max(n.initial, 1.f), 0.f, 1.f);
        const GpuModel* model = nullptr;
        if (const std::string* name = n.type->find("Land_Model_Name"); name && !alamo::trim(*name).empty())
            model = r.model(alamo::trim(*name));
        const Mat4 at = Mat4::translate(n.position[0], n.position[1], n.position[2]);
        if (model) {
            r.drawModel(*model, at * Mat4::scale(float(n.type->number("Scale_Factor").value_or(1.0)) * (0.5f + 0.5f * frac)));
        } else {
            const float s = n.radius * 1.4f * (0.4f + 0.6f * std::sqrt(frac));
            r.drawBox(at * Mat4::scale3(s, s, std::max(2.f, s * 0.45f)), {0.85f, 0.68f, 0.25f, 1});
            r.drawBox(at * Mat4::translate(s * 0.15f, -s * 0.1f, s * 0.3f) * Mat4::scale3(s * 0.5f, s * 0.45f, s * 0.35f),
                      {0.6f, 0.62f, 0.66f, 1});
        }
    }
    for (const alamo::Unit& u : w.units()) {
        const bool sel = selected_.count(u.id) != 0;
        const Vec4 col = [&] {
            static const Vec4 c[] = {{0.25f, 0.55f, 1.f, 1}, {0.95f, 0.25f, 0.2f, 1}, {0.35f, 0.85f, 0.35f, 1}, {0.95f, 0.8f, 0.2f, 1}};
            return c[std::clamp(u.owner, 0, 3)];
        }();
        const float rad = u.radius;
        const Mat4 base = Mat4::translate(u.position[0], u.position[1], u.position[2]) * Mat4::rotateZ(u.facing);

        // Body: the real model when we have it, otherwise a box stretched along the facing with a nose.
        const GpuModel* model = nullptr;
        if (const std::string* name = u.type->find("Land_Model_Name"); name && !alamo::trim(*name).empty())
            model = r.model(alamo::trim(*name));
        if (model) {
            const float scale = float(u.type->number("Scale_Factor").value_or(1.0));
            // Alamo models face -Y in model space; the simulation's facing 0 is +X.
            r.drawModel(*model, base * Mat4::rotateZ(1.5707963f) * Mat4::scale(scale));
        } else {
            const bool infantry = u.moveClass == alamo::MoveClass::Infantry;
            const float h = infantry ? std::max(rad * 2.2f, 9.f) : std::max(rad * 0.9f, 6.f);
            r.drawBox(base * Mat4::scale3(rad * 1.8f, rad * 1.3f, h), col);
            r.drawBox(base * Mat4::translate(rad * 0.9f, 0, h * 0.35f) * Mat4::scale3(rad * 0.6f, rad * 0.4f, h * 0.3f),
                      {col[0] * 0.5f, col[1] * 0.5f, col[2] * 0.5f, 1});
        }

        if (u.carried > 0) {
            // Collectors show their load as a small crate underneath.
            const float c = 3 + 4 * std::min(1.f, u.carried / 220.f);
            r.drawBox(Mat4::translate(u.position[0], u.position[1], u.position[2] - c) * Mat4::scale(c), {0.9f, 0.72f, 0.25f, 1});
        }
        if (sel) {
            const float s = rad * 2.6f + 4;
            r.drawBox(Mat4::translate(u.position[0], u.position[1], u.position[2] + 0.5f) * Mat4::scale3(s, s, 0.8f),
                      {0.3f, 1.f, 0.4f, 1});
        }
        if (sel || u.health < u.maxHealth) {
            // Health bar above selected or damaged units.
            const float frac = std::clamp(u.health / std::max(u.maxHealth, 1.f), 0.f, 1.f);
            const float barW = std::min(std::max(rad * 2.5f, 14.f), 110.f);
            const float top = u.structure ? 45.f : std::max(rad * 2.4f, 16.f) + 8;
            r.drawBox(Mat4::translate(u.position[0], u.position[1], u.position[2] + top) * Mat4::scale3(barW, 2.5f, 2.5f),
                      {0.1f, 0.1f, 0.1f, 1});
            r.drawBox(Mat4::translate(u.position[0] - barW * (1 - frac) / 2, u.position[1], u.position[2] + top + 0.3f) *
                          Mat4::scale3(barW * frac, 2.8f, 2.6f),
                      {1 - frac, 0.85f * frac + 0.15f, 0.2f, 1});
        }
    }
    // Construction sites: a translucent-looking frame rising with progress; rally flags of selected structures.
    for (const alamo::Unit& u : w.units()) {
        if (u.structure && u.underConstruction) {
            const float h = 8 + 40 * u.buildProgress;
            r.drawBox(Mat4::translate(u.position[0], u.position[1], u.position[2]) * Mat4::scale3(u.footprintX * 2, u.footprintY * 2, h),
                      {0.55f, 0.6f, 0.65f, 1});
        }
        if (u.structure && u.hasRally && selected_.count(u.id)) {
            const float z = w.map().heightAt(u.rally[0], u.rally[1]);
            r.drawBox(Mat4::translate(u.rally[0], u.rally[1], z) * Mat4::scale3(2, 2, 40), {1, 1, 1, 1});
            r.drawBox(Mat4::translate(u.rally[0] + 7, u.rally[1], z + 30) * Mat4::scale3(12, 1.5f, 10), {0.3f, 1.f, 0.4f, 1});
        }
    }
    // Placement ghost: green where the site is clear, red where it's blocked.
    if (placing()) {
        if (const alamo::GameObjectType* t = w.db().find(placingType_)) {
            const float fx = float(t->number("Custom_Hard_XExtent").value_or(20));
            const float fy = float(t->number("Custom_Hard_YExtent").value_or(fx));
            const bool ok = w.siteClear(*t, ghost_);
            const float pulse = 0.75f + 0.25f * std::sin(time * 6);
            r.drawBox(Mat4::translate(ghost_[0], ghost_[1], ghost_[2]) * Mat4::scale3(fx * 2, fy * 2, 6),
                      ok ? Vec4{0.2f * pulse, 0.9f * pulse, 0.3f * pulse, 1} : Vec4{0.95f * pulse, 0.2f, 0.15f, 1});
        }
    }

    // Shots in flight: small glowing blocks in the shooter's colour.
    static const Vec4 shot[] = {{0.5f, 0.85f, 1.f, 1}, {1.f, 0.55f, 0.25f, 1}, {0.6f, 1.f, 0.5f, 1}, {1.f, 0.9f, 0.4f, 1}};
    for (const alamo::Projectile& p : w.projectiles()) {
        const float sz = p.blastDamage > 0 ? 6.f : 3.5f;
        r.drawBox(Mat4::translate(p.position[0], p.position[1], p.position[2] - sz / 2) * Mat4::scale(sz), shot[std::clamp(p.owner, 0, 3)]);
    }
    // Wrecks: dark scorch plates that fade over 20 seconds.
    const uint64_t now = w.tickCount();
    for (const alamo::DeathEvent& d : w.deaths()) {
        const float age = float(now - d.tick) / float(alamo::kTicksPerSecond);
        if (age > 20) continue;
        const float k = 1 - age / 20;
        const float s = 12 + 10 * std::min(age * 3, 1.f);
        r.drawBox(Mat4::translate(d.position[0], d.position[1], d.position[2] + 0.4f) * Mat4::scale3(s, s, 1.5f * k + 0.3f),
                  {0.12f + 0.5f * k * (age < 0.5f), 0.1f, 0.08f, 1});
    }
    // Order marker: shrinking plate for one second (red for attack orders).
    const float age = time - lastOrderTime_;
    if (age >= 0 && age < 1) {
        const float s = 30 * (1 - age) + 6;
        const Vec4 c = attackOrder_ ? Vec4{1.f, 0.3f, 0.25f, 1} : Vec4{0.3f, 1.f, 0.4f, 1};
        r.drawBox(Mat4::translate(lastOrder_[0], lastOrder_[1], lastOrder_[2] + 0.6f) * Mat4::scale3(s, s, 0.8f), c);
    }
}

void UnitControl::drawBox(Renderer& r, const View&, float x0, float y0, float x1, float y1) const {
    const float lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
    r.drawOverlayRect(lx, ly, hx, hy, {0.3f, 1.f, 0.4f, 0.15f});
    const Vec4 edge{0.3f, 1.f, 0.4f, 0.9f};
    r.drawOverlayRect(lx, ly, hx, ly + 1.5f, edge);
    r.drawOverlayRect(lx, hy - 1.5f, hx, hy, edge);
    r.drawOverlayRect(lx, ly, lx + 1.5f, hy, edge);
    r.drawOverlayRect(hx - 1.5f, ly, hx, hy, edge);
}

} // namespace uaw
