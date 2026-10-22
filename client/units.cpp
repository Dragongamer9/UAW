#include "units.hpp"
#include "particles.hpp"

#include <cctype>
#include <unordered_map>

#include <algorithm>
#include <cstring>
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
        if (u.owner != player_ || u.inside) continue;
        float sx, sy;
        if (v.toScreen(u.position, sx, sy) && sx >= lx && sx <= hx && sy >= ly && sy <= hy) selected_.insert(u.id);
    }
}

void UnitControl::selectAllVisible(alamo::World& w, const View& v) {
    boxSelect(w, v, 0, 0, float(v.width), float(v.height), false);
}

void UnitControl::selectSameType(alamo::World& w, const View& v, const Vec3& ground) {
    const alamo::GameObjectType* type = nullptr;
    float best = 1e30f;
    for (uint32_t id : w.unitsNear(ground, 12)) {
        const alamo::Unit* u = w.unit(id);
        if (!u || u->owner != player_) continue;
        const float d = std::hypot(u->position[0] - ground[0], u->position[1] - ground[1]);
        if (d < best) { best = d; type = u->type; }
    }
    if (!type) return;
    selected_.clear();
    for (const alamo::Unit& u : w.units()) {
        if (u.owner != player_ || u.type != type) continue;
        float sx, sy;
        if (v.toScreen(u.position, sx, sy) && sx >= 0 && sy >= 0 && sx <= float(v.width) && sy <= float(v.height)) selected_.insert(u.id);
    }
}

void UnitControl::storeGroup(int n) {
    if (n < 0 || n > 9) return;
    groups_[n] = selected_;
}

bool UnitControl::hasGroup(const alamo::World& w, int n) const {
    if (n < 0 || n > 9) return false;
    for (uint32_t id : groups_[n])
        if (const alamo::Unit* u = const_cast<alamo::World&>(w).unit(id); u && u->owner == player_) return true;
    return false;
}

bool UnitControl::recallGroup(alamo::World& w, int n) {
    if (!hasGroup(w, n)) return false;
    std::set<uint32_t> alive;
    for (uint32_t id : groups_[n])
        if (const alamo::Unit* u = w.unit(id); u && u->owner == player_) alive.insert(id);
    groups_[n] = alive;
    selected_ = alive;
    return true;
}

bool UnitControl::selectIdleBuilder(alamo::World& w, Vec3& at) {
    std::vector<uint32_t> idle;
    for (const alamo::Unit& u : w.units())
        if (u.owner == player_ && !u.structure && !u.inside && !u.moving() && !u.constructing && !w.constructible(u.id).empty())
            idle.push_back(u.id);
    if (idle.empty()) return false;
    std::sort(idle.begin(), idle.end());
    auto it = std::upper_bound(idle.begin(), idle.end(), lastIdleBuilder_);
    const uint32_t pick = it == idle.end() ? idle.front() : *it;
    lastIdleBuilder_ = pick;
    selected_ = {pick};
    at = w.unit(pick)->position;
    return true;
}

void UnitControl::selectArmy(alamo::World& w) {
    selected_.clear();
    for (const alamo::Unit& u : w.units())
        if (u.owner == player_ && !u.structure && !u.inside && !u.weapons.empty() && w.constructible(u.id).empty()) selected_.insert(u.id);
}

void UnitControl::selectAll(alamo::World& w) {
    selected_.clear();
    for (const alamo::Unit& u : w.units())
        if (u.owner == player_) selected_.insert(u.id);
}

void UnitControl::moveTo(alamo::World& w, const Vec3& ground, bool queued) {
    if (selected_.empty()) return;
    w.commandMove(std::vector<uint32_t>(selected_.begin(), selected_.end()), ground, queued);
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
    if (r == alamo::OrderResult::Ok) sound(w.db().find(item.type), "SFXEvent_Tactical_Build_Started");
    else sounds_.push_back("GUI_Generic_Bad_Sound");
}

void UnitControl::cancelQueue(alamo::World& w) {
    if (const uint32_t id = menuOwner(w)) {
        const alamo::Unit* s = w.unit(id);
        const alamo::GameObjectType* last = s && !s->queue.empty() ? s->queue.back() : nullptr;
        w.cancelProduction(id);
        say("Cancelled", w.seconds());
        sound(last, "SFXEvent_Tactical_Build_Cancelled");
    }
}

void UnitControl::place(alamo::World& w) {
    if (!placing()) return;
    const alamo::OrderResult r = w.commandConstruct(placingBuilder_, placingType_, ghost_);
    say(r == alamo::OrderResult::Ok ? "Building" : std::string("Can't build here: ") + alamo::toString(r), w.seconds());
    if (r == alamo::OrderResult::Ok) {
        sound(w.db().find(placingType_), "SFXEvent_Tactical_Build_Started");
        placingType_.clear();
    } else {
        sounds_.push_back("GUI_Generic_Bad_Sound");
    }
}

void UnitControl::command(alamo::World& w, const Vec3& ground, bool queued) {
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
        // Your own transport: the others get in.
        if (u && u->owner == player_ && w.garrisonOf(*u->type) && !selected_.count(id)) {
            std::vector<uint32_t> riders(selected_.begin(), selected_.end());
            if (w.commandBoard(riders, id) == alamo::OrderResult::Ok) {
                say("Boarding", w.seconds());
                lastOrder_ = u->position;
                lastOrderTime_ = w.seconds();
                attackOrder_ = false;
                return;
            }
        }
        if (u && w.enemies(player_, u->owner)) {
            w.commandAttack(std::vector<uint32_t>(selected_.begin(), selected_.end()), id);
            lastOrder_ = u->position;
            lastOrderTime_ = w.seconds();
            attackOrder_ = true;
            return;
        }
    }
    attackOrder_ = false;
    moveTo(w, ground, queued);
}

std::vector<AbilityButton> UnitControl::abilities(alamo::World& w) const {
    std::vector<AbilityButton> out;
    for (uint32_t id : selected_) {
        const alamo::Unit* u = w.unit(id);
        if (!u || u->owner != player_) continue;
        const alamo::World::TypeAbilities& ta = w.abilitiesOf(*u->type);
        for (const alamo::Unit::AbilityState& st : u->abilities) {
            const alamo::AbilitySpec& s = ta.abilities[st.index];
            if (s.autonomous || s.kind == alamo::AbilitySpec::Kind::Unsupported) continue;
            if (w.abilityLocked(*u, s)) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](const AbilityButton& b) { return b.name == s.name; });
            if (it == out.end()) {
                AbilityButton b;
                b.name = s.name;
                // "Novus_Hacker_Download_Ability" -> "Hacker Download"
                std::string l = s.name;
                for (const char* drop : {"_Unit_Ability", "_Special_Ability", "_Ability", "_ability", "_Toggle"})
                    if (const size_t p = l.rfind(drop); p != std::string::npos && p + std::strlen(drop) == l.size()) l.resize(p);
                for (const char* lead : {"Unit_Ability_", "Special_Ability_", "Ability_", "Novus_", "Alien_", "Masari_", "Military_"})
                    if (l.rfind(lead, 0) == 0) l = l.substr(std::strlen(lead));
                for (char& c : l) if (c == '_') c = ' ';
                b.label = l;
                b.wantsObject = s.needsTarget && !s.substitute; // fire modes just switch
                b.wantsPoint = !s.needsTarget && !s.substitute && (s.kind == alamo::AbilitySpec::Kind::Leap || s.maxRange > 0);
                b.cooldown = 1e9f;
                out.push_back(b);
                it = out.end() - 1;
            }
            it->ready = it->ready || st.cooldown <= 0;
            const int idx = int(&st - u->abilities.data());
            it->on = it->on || (s.substitute && u->substitute == idx) || (s.kind == alamo::AbilitySpec::Kind::FooMode && u->fooHeal);
            it->cooldown = std::min(it->cooldown, st.cooldown / float(alamo::kTicksPerSecond));
        }
    }
    if (out.size() > 8) out.resize(8);
    return out;
}

void UnitControl::fireAbility(alamo::World& w, const std::string& name, uint32_t target, const Vec3& point) {
    int used = 0, tooFar = 0;
    for (uint32_t id : std::vector<uint32_t>(selected_.begin(), selected_.end())) {
        const alamo::Unit* u = w.unit(id);
        if (!u || u->owner != player_) continue;
        const alamo::World::TypeAbilities& ta = w.abilitiesOf(*u->type);
        for (size_t i = 0; i < u->abilities.size(); ++i) {
            if (ta.abilities[u->abilities[i].index].name != name || u->abilities[i].cooldown > 0) continue;
            const alamo::OrderResult r = w.useAbility(id, i, target, point);
            if (r == alamo::OrderResult::Ok) ++used;
            else if (r == alamo::OrderResult::NotAllowed) ++tooFar;
            break;
        }
    }
    if (!used) say(tooFar ? "Out of range or not ready" : "No target", w.seconds());
}

void UnitControl::useAbility(alamo::World& w, size_t button) {
    const auto list = abilities(w);
    if (button >= list.size()) return;
    const AbilityButton& b = list[button];
    if (!b.ready) {
        say(b.label + " ready in " + std::to_string(int(b.cooldown + 0.99f)) + " s", w.seconds());
        return;
    }
    if (b.wantsObject || b.wantsPoint) {
        armedAbility_ = b.name;
        say(b.label + ": right-click the target", w.seconds());
        return;
    }
    fireAbility(w, b.name, 0, {});
}

void UnitControl::targetAbility(alamo::World& w, const Vec3& ground) {
    const std::string name = armedAbility_;
    armedAbility_.clear();
    if (name.empty()) return;
    // The unit nearest the click (any side; the ability's own filters decide), else the ground.
    uint32_t target = 0;
    for (uint32_t id : w.unitsNear(ground, 25))
        if (const alamo::Unit* u = w.unit(id); u && !selected_.count(id)) { target = id; break; }
    fireAbility(w, name, target, ground);
}

bool UnitControl::fireSuperweapon(alamo::World& w, const Vec3& ground) {
    for (uint32_t id : selected_)
        if (const alamo::Unit* u = w.unit(id); u && u->owner == player_ && w.superweaponCountdown(id) == 0)
            return w.fireSuperweapon(id, ground) == alamo::OrderResult::Ok;
    return false;
}

void UnitControl::attackMove(alamo::World& w, const Vec3& ground) {
    if (selected_.empty()) return;
    w.commandAttackMove(std::vector<uint32_t>(selected_.begin(), selected_.end()), ground);
    lastOrder_ = ground;
    lastOrderTime_ = w.seconds();
    attackOrder_ = true;
}

void UnitControl::patrol(alamo::World& w, const Vec3& ground) {
    if (selected_.empty()) return;
    w.commandPatrol(std::vector<uint32_t>(selected_.begin(), selected_.end()), ground);
    lastOrder_ = ground;
    lastOrderTime_ = w.seconds();
    attackOrder_ = true;
}

void UnitControl::stop(alamo::World& w) { w.commandStop(std::vector<uint32_t>(selected_.begin(), selected_.end())); }

// ------------------------------------------------------------------ drawing

void UnitControl::unload(alamo::World& w) {
    int n = 0;
    for (uint32_t id : selected_)
        if (w.unload(id) == alamo::OrderResult::Ok) ++n;
    if (n) say("Unloading", w.seconds());
}

namespace {

// Attached_Hard_Points: pairs of (hardpoint type, bone of the parent model), cached per type.
struct Attachment {
    const alamo::GameObjectType* type;
    std::string bone;
};
const std::vector<Attachment>& attachmentsOf(const alamo::GameObjectType& t, const alamo::GameDatabase& db) {
    static std::unordered_map<const alamo::GameObjectType*, std::vector<Attachment>> cache;
    auto it = cache.find(&t);
    if (it != cache.end()) return it->second;
    std::vector<Attachment> out;
    if (const std::string* list = t.find("Attached_Hard_Points")) {
        std::vector<std::string> words;
        std::string cur;
        for (char c : *list + ",") {
            if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        for (size_t i = 0; i + 1 < words.size(); i += 2)
            if (const alamo::GameObjectType* hp = db.find(words[i])) out.push_back({hp, words[i + 1]});
    }
    return cache.emplace(&t, std::move(out)).first->second;
}

int boneIndex(const GpuModel& m, const std::string& name) {
    for (size_t b = 0; b < m.skeleton.bones.size(); ++b) {
        const std::string& n = m.skeleton.bones[b].name;
        if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char c) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(c));
            }))
            return int(b);
    }
    return -1;
}

alamo::Mat34 mul34(const alamo::Mat34& a, const alamo::Mat34& b) {
    alamo::Mat34 c{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) c[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j];
        c[i * 4 + 3] += a[i * 4 + 3];
    }
    return c;
}

} // namespace

const UnitControl::Fallen& UnitControl::fallenOf(Renderer& r, const alamo::GameObjectType& t, uint32_t id) const {
    // Per type and death-animation variant (the id picks one of _DIE_00.._DIE_03 that exist).
    const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(&t)) * 4 + id % 4;
    if (auto it = fallen_.find(key); it != fallen_.end()) return it->second;
    Fallen f;
    const alamo::GameObjectType* shown = &t;
    if (db_) {
        // Every "Damage_X, Clone_Type" pair (the tag repeats): the Damage_Default one, else the first.
        const alamo::GameObjectType* first = nullptr, *byDefault = nullptr;
        for (const alamo::ObjectField& fld : t.fields()) {
            if (fld.path.size() < 11 || !alamo::iequals(std::string_view(fld.path).substr(fld.path.size() - 11), "Death_Clone")) continue;
            for (const std::string& v : fld.values) {
                const size_t comma = v.find(',');
                if (comma == std::string::npos) continue;
                const std::string damage = alamo::trim(v.substr(0, comma)), clone = alamo::trim(v.substr(comma + 1));
                const alamo::GameObjectType* c = db_->find(clone);
                if (!c || !c->find("Land_Model_Name")) continue;
                if (!first) first = c;
                if (!byDefault && alamo::iequals(damage, "Damage_Default")) byDefault = c;
            }
        }
        if (byDefault || first) shown = byDefault ? byDefault : first;
    }
    if (const std::string* name = shown->find("Land_Model_Name"); name && !alamo::trim(*name).empty()) {
        f.model = r.model(alamo::trim(*name));
        std::string stem = alamo::trim(*name);
        if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
        int variants = 0;
        const alamo::Animation* found[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int v = 0; v < 4; ++v)
            if (const alamo::Animation* a = r.animation(stem + "_DIE_0" + std::to_string(v) + ".ALA"); a && a->frameCount)
                found[variants++] = a;
        if (variants) f.anim = found[id % uint32_t(variants)];
        f.scale = float(shown->number("Scale_Factor").value_or(t.number("Scale_Factor").value_or(1.0)));
        if (f.model) f.height = (f.model->boundsMax[2] - f.model->boundsMin[2]) * f.scale;
    }
    f.persist = std::clamp(float(t.number("Death_Persistence_Duration").value_or(2.0)), 2.f, 12.f);
    f.wreck = shown != &t; // a separate wreck model is shown even when it has no animation
    return fallen_.emplace(key, f).first->second;
}

int damageAlternate(const alamo::GameObjectType& t, float healthFraction) {
    // Land_Damage_Thresholds (falling health fractions) and Land_Damage_Alternates (the model's
    // _ALTn meshes shown below each); cached per type.
    static std::unordered_map<const alamo::GameObjectType*, std::pair<std::vector<float>, std::vector<int>>> cache;
    auto it = cache.find(&t);
    if (it == cache.end()) {
        auto numbers = [](const std::string* v) {
            std::vector<float> out;
            if (!v) return out;
            std::string cur;
            for (char c : *v + ",") {
                if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
                    if (!cur.empty()) out.push_back(float(std::atof(cur.c_str())));
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            return out;
        };
        const std::vector<float> thr = numbers(t.find("Land_Damage_Thresholds"));
        std::vector<int> alts;
        for (float a : numbers(t.find("Land_Damage_Alternates"))) alts.push_back(int(a));
        it = cache.emplace(&t, std::make_pair(thr, alts)).first;
    }
    const auto& [thr, alts] = it->second;
    int idx = -1;
    for (size_t i = 0; i < thr.size(); ++i)
        if (healthFraction <= thr[i] + 1e-4f) idx = int(i);
    if (idx < 0) return 0;
    if (healthFraction <= 0 && idx > 0 && thr[size_t(idx)] <= 0) --idx; // the last entry is "destroyed"
    return size_t(idx) < alts.size() ? alts[size_t(idx)] : idx;
}

Vec4 playerColor(int player) {
    // The game's multiplayer colours (GameConstants.xml MP_Color_*): blue, red, green, orange, cyan,
    // purple, yellow, grey.
    static const Vec4 c[] = {{78 / 255.f, 150 / 255.f, 1, 1},          {1, 23 / 255.f, 23 / 255.f, 1},
                             {119 / 255.f, 1, 78 / 255.f, 1},          {1, 149 / 255.f, 23 / 255.f, 1},
                             {111 / 255.f, 217 / 255.f, 224 / 255.f, 1}, {1, 111 / 255.f, 209 / 255.f, 1},
                             {224 / 255.f, 223 / 255.f, 45 / 255.f, 1}, {150 / 255.f, 150 / 255.f, 150 / 255.f, 1}};
    return player < 0 ? Vec4{1, 1, 1, 1} : c[player % 8];
}

bool UnitControl::aimTurret(const GpuModel& m, const Mat4& xf, const alamo::GameObjectType& t, const alamo::Vec3* target, uint64_t key,
                            float time, Pose& pose) const {
    const std::string* boneName = t.find("Turret_Bone_Name");
    if (!boneName || m.skeleton.bones.empty()) return false;
    if (t.number("Apply_Z_Turret_Rotate_To_Axis").value_or(2) != 2) return false; // turned about another axis
    const int tb = boneIndex(m, alamo::trim(*boneName));
    if (tb < 0) return false;
    const size_t nb = m.skeleton.bones.size();
    if (pose.size() < nb) pose.resize(nb, alamo::Mat34{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    // The turret's pivot (posed) in model space.
    const alamo::Mat34 rest = m.skeleton.boneWorld(uint32_t(tb));
    const alamo::Mat34& P = pose[size_t(tb)];
    alamo::Vec3 pivot;
    for (int i = 0; i < 3; ++i) pivot[i] = P[i * 4] * rest[3] + P[i * 4 + 1] * rest[7] + P[i * 4 + 2] * rest[11] + P[i * 4 + 3];
    // Wanted angle: the target's direction in model space, from the model's forward (-Y).
    float want = 0;
    if (target) {
        alamo::Vec3 pw;
        for (int i = 0; i < 3; ++i) pw[i] = xf.at(i, 0) * pivot[0] + xf.at(i, 1) * pivot[1] + xf.at(i, 2) * pivot[2] + xf.at(i, 3);
        const float dw[3] = {(*target)[0] - pw[0], (*target)[1] - pw[1], (*target)[2] - pw[2]};
        float dm[3]; // inverse of the (rotation x uniform scale) part: transpose, the scale cancels in atan2
        for (int j = 0; j < 3; ++j) dm[j] = xf.at(0, j) * dw[0] + xf.at(1, j) * dw[1] + xf.at(2, j) * dw[2];
        if (dm[0] * dm[0] + dm[1] * dm[1] > 1e-6f) want = std::remainder(std::atan2(dm[1], dm[0]) + 1.5707963f, 6.2831853f);
    }
    const float extent = float(t.number("Turret_Rotate_Extent_Degrees").value_or(360.0)) * 3.14159265f / 180.f;
    if (extent < 6.2f) want = std::clamp(want, -extent, extent);
    // Turned at Turret_Rotate_Speed (degrees per 1/30 s), from where it was last frame.
    if (turrets_.size() > 20000) turrets_.clear(); // dead units' turrets: start afresh now and then
    auto [it, fresh] = turrets_.try_emplace(key, want, time);
    float& angle = it->second.first;
    if (!fresh) {
        const float dt = std::clamp(time - it->second.second, 0.f, 0.25f);
        const float step = float(t.number("Turret_Rotate_Speed").value_or(5.0)) * 3.14159265f / 180.f * 30.f * dt;
        const float diff = std::remainder(want - angle, 6.2831853f);
        angle = std::fabs(diff) <= step ? want : angle + (diff > 0 ? step : -step);
        angle = std::remainder(angle, 6.2831853f);
    }
    it->second.second = time;
    if (std::fabs(angle) < 1e-4f) return true;
    // The turret bone and everything below it, turned about model Z through the pivot.
    const float c = std::cos(angle), sn = std::sin(angle);
    const alamo::Mat34 R{c, -sn, 0, pivot[0] - (c * pivot[0] - sn * pivot[1]), sn, c, 0, pivot[1] - (sn * pivot[0] + c * pivot[1]), 0, 0, 1, 0};
    std::vector<char> below(nb, 0);
    below[size_t(tb)] = 1;
    for (int pass = 0; pass < 4; ++pass) // parents normally come first; a few passes cover the rest
        for (size_t b = 0; b < nb; ++b) {
            const int32_t par = m.skeleton.bones[b].parent;
            if (!below[b] && par >= 0 && size_t(par) < nb && below[size_t(par)]) below[b] = 1;
        }
    for (size_t b = 0; b < nb; ++b)
        if (below[b]) pose[b] = mul34(R, pose[b]);
    return true;
}

void UnitControl::drawHardpoints(Renderer& r, const alamo::Unit& u, const alamo::GameObjectType& type, const GpuModel& model,
                                 const Mat4& xf, const Pose* pose, int depth) const {
    // Walker legs, crowns and their weapons: each hardpoint's own model on its bone of the parent
    // (posed with the parent's animation), as long as the unit still has one of that type alive.
    if (depth > 3 || !db_) return;
    for (const Attachment& a : attachmentsOf(type, *db_)) {
        bool alive = false;
        float hpFraction = 1;
        for (const alamo::Unit::HardPoint& hp : u.hardpoints)
            if (hp.type == a.type && hp.alive() && !alive) {
                alive = true;
                hpFraction = hp.maxHealth > 0 ? hp.health / hp.maxHealth : 1.f;
            }
        if (!alive) continue;
        const std::string* name = a.type->find("Land_Model_Name");
        const GpuModel* hm = name && !alamo::trim(*name).empty() ? r.model(alamo::trim(*name)) : nullptr;
        const int b = boneIndex(model, a.bone);
        if (b < 0) continue;
        Mat4 bone = Mat4::fromAlamo(model.skeleton.boneWorld(uint32_t(b)));
        if (pose && size_t(b) < pose->size()) bone = Mat4::fromAlamo((*pose)[size_t(b)]) * bone;
        if (!hm) continue;
        Mat4 at = xf * bone * Mat4::scale(float(a.type->number("Scale_Factor").value_or(1.0)));
        // The hardpoint model's own attachment bone (Tactical_Build_Hard_Point_Attachment_Bone, e.g. a
        // walker leg's HP_Attach at the hip) goes on the parent's bone, not the model's origin.
        if (const std::string* ab = a.type->find("Tactical_Build_Hard_Point_Attachment_Bone"))
            if (const int hb = boneIndex(*hm, alamo::trim(*ab)); hb >= 0 && size_t(hb) < hm->restInverse.size())
                at = at * Mat4::fromAlamo(hm->restInverse[size_t(hb)]);
        Pose hpPose;
        const alamo::Unit* tgt = (db_ && world_) ? world_->unit(u.target) : nullptr;
        const alamo::Vec3* aimAt = tgt && tgt->alive() ? &tgt->position : nullptr;
        const uint64_t key = (uint64_t(u.id) << 16) | (uint64_t(depth) << 12) | uint64_t(&a - attachmentsOf(type, *db_).data());
        const bool turret = aimTurret(*hm, at, *a.type, aimAt, key, turretTime_, hpPose);
        r.drawModel(*hm, at, {1, 1, 1, 1}, turret ? &hpPose : nullptr, damageAlternate(*a.type, hpFraction));
        drawHardpoints(r, u, *a.type, *hm, at, turret ? &hpPose : nullptr, depth + 1);
    }
}

void UnitControl::sell(alamo::World& w) {
    float refund = 0;
    int n = 0;
    for (uint32_t id : selected_)
        if (const alamo::Unit* u = w.unit(id); u && u->owner == player_ && w.canSell(*u)) {
            refund += w.sellRefund(*u);
            if (w.sell(id) == alamo::OrderResult::Ok) ++n;
        }
    if (n) say("Selling " + std::to_string(n) + (n == 1 ? " structure" : " structures") + " for about " + std::to_string(int(refund)), w.seconds());
    else say("Nothing selected can be sold", w.seconds());
}

bool UnitControl::shown(const alamo::World& w, const alamo::Unit& u) const {
    if (u.inside) return false;
    if (player_ < 0 || !w.enemies(player_, u.owner)) return true;
    if (u.cloaked && fog) return false;
    if (!fog || !w.fogOfWar()) return true;
    if (w.visibleTo(player_, u.position)) {
        if (u.structure) seenStructures_.insert(u.id);
        return true;
    }
    return u.structure && seenStructures_.count(u.id);
}

// A Foo Core's fighters: swarming the target (or the friend they mend), else hovering by the core.
void UnitControl::drawFooFighters(Renderer& r, alamo::World& w, const alamo::Unit& u, float time) const {
    const alamo::Unit::WeaponState* foo = nullptr;
    for (const alamo::Unit::WeaponState& ws : u.weapons)
        if (ws.spec.fooFighters > 0) foo = &ws;
    if (!foo || u.underConstruction) return;
    const alamo::GameObjectType* ft = nullptr;
    if (const std::string* n = u.type->find("Foo_Core_Fighter_Type_Name")) ft = w.db().find(alamo::trim(*n));
    const GpuModel* m = nullptr;
    if (ft)
        if (const std::string* mn = ft->find("Land_Model_Name"); mn && !alamo::trim(*mn).empty()) m = r.model(alamo::trim(*mn));
    const float scale = ft ? float(ft->number("Scale_Factor").value_or(1.0)) : 1.f;
    // Where they are: at work on someone in range, or home.
    const alamo::Unit* at = nullptr;
    if (u.fooHeal) at = w.unit(u.fooTarget);
    else if (const alamo::Unit* t = w.unit(u.target); t && foo->cooldown > 0) at = t;
    if (at && std::hypot(at->position[0] - u.position[0], at->position[1] - u.position[1]) > u.weaponRange(*foo) + at->radius + 30) at = nullptr;
    const alamo::Vec3 c = at ? at->position : u.position;
    const float orbit = at ? std::max(at->radius * 0.9f, 8.f) : std::max(u.radius * 0.7f, 6.f);
    const float lift = at ? std::max(at->radius * 0.8f, 8.f) : u.radius * 0.5f + 4;
    for (int k = 0; k < foo->spec.fooFighters; ++k) {
        const float a = time * (at ? 4.f : 1.2f) + 6.2831853f * float(k) / float(foo->spec.fooFighters) + float(u.id);
        const float z = c[2] + lift + std::sin(time * 3.f + float(k)) * (at ? 3.f : 1.f);
        const Mat4 xf = Mat4::translate(c[0] + std::cos(a) * orbit, c[1] + std::sin(a) * orbit, z);
        // The fighter is all particles: the model's Idle / Attack / Heal proxy.
        const char* want = !at ? "IDLE" : u.fooHeal ? "HEAL" : "ATTACK";
        const std::string* proxy = nullptr;
        if (m)
            for (const GpuModel::Proxy& px : m->proxies) {
                std::string up = px.name;
                for (char& ch : up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                if (up.find(want) != std::string::npos) { proxy = &px.name; break; }
            }
        if (effects && proxy) {
            // "pp_..._m" is a stub; the effect itself is the name without "_m".
            std::string fx = *proxy;
            if (fx.size() > 2 && (fx.compare(fx.size() - 2, 2, "_m") == 0 || fx.compare(fx.size() - 2, 2, "_M") == 0)) fx.resize(fx.size() - 2);
            effects->follow((uint64_t(u.id) << 8 | uint64_t(0xF0 + k)) | (1ull << 63), fx, {xf.at(0, 3), xf.at(1, 3), xf.at(2, 3)}, scale);
        }
        else
            r.drawBox(xf * Mat4::scale(2.5f), u.fooHeal ? Vec4{0.4f, 1, 0.5f, 1} : Vec4{1, 0.5f, 0.9f, 1});
    }
}

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
        if (!shown(w, u)) continue;
        const bool sel = selected_.count(u.id) != 0;
        const Vec4 col = playerColor(u.owner);
        r.setTeamColor(col);
        const float rad = u.radius;
        const Mat4 base = Mat4::translate(u.position[0], u.position[1], u.position[2]) * Mat4::rotateZ(u.facing);

        // Body: the real model when we have it, otherwise a box stretched along the facing with a nose.
        const GpuModel* model = nullptr;
        if (const std::string* name = u.type->find("Land_Model_Name"); name && !alamo::trim(*name).empty())
            model = r.model(alamo::trim(*name));
        if (model) {
            const float scale = float(u.type->number("Scale_Factor").value_or(1.0));
            // Animated: <model>_MOVE_00 while moving, _ATTACK_00 while shooting, else _IDLE_00
            // (falling back to idle), each unit at its own phase.
            const Pose* pose = nullptr;
            Pose posed;
            if (animate && (!u.structure || u.maxSpeed > 0)) { // walkers too
                std::string stem = alamo::trim(*u.type->find("Land_Model_Name"));
                if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
                const char* kind = u.moving() ? "_MOVE_00.ALA" : (u.target && !u.weapons.empty()) ? "_ATTACK_00.ALA" : "_IDLE_00.ALA";
                const alamo::Animation* anim = r.animation(stem + kind);
                if (!anim) anim = r.animation(stem + "_IDLE_00.ALA");
                if (anim) {
                    posed = r.pose(*model, *anim, time + float(u.id % 97) * 0.37f);
                    pose = &posed;
                }
            }
            // Alamo models face -Y in model space; the simulation's facing 0 is +X.
            const Mat4 xf = base * Mat4::rotateZ(1.5707963f) * Mat4::scale(scale);
            {
                const alamo::Unit* tgt = w.unit(u.target);
                const alamo::Vec3* aimAt = tgt && tgt->alive() && !u.underConstruction ? &tgt->position : nullptr;
                if (aimTurret(*model, xf, *u.type, aimAt, uint64_t(u.id) << 16 | 0xFFFF, time, posed)) pose = &posed;
            }
            if (u.underConstruction) // rising out of the ground as it is built
                r.setClipHeight(u.position[2] + std::max(2.f, model->boundsMax[2] * scale * std::clamp(u.buildProgress, 0.f, 1.f)));
            const int alt = damageAlternate(*u.type, u.maxHealth > 0 ? u.health / u.maxHealth : 1.f);
            r.drawModel(*model, xf, {1, 1, 1, 1}, pose, alt);
            if (u.underConstruction) r.setClipHeight(1e9f);
            world_ = &w;
            turretTime_ = time;
            if (!u.hardpoints.empty()) drawHardpoints(r, u, *u.type, *model, xf, pose, 0);
            drawFooFighters(r, w, u, time);
            // Attached effects (engine glows, flames, lightning cores) near the camera.
            if (effects && !model->proxies.empty() && !u.underConstruction) {
                const alamo::Vec3& cp = r.cameraPosition();
                const float dx = cp[0] - u.position[0], dy = cp[1] - u.position[1];
                if (dx * dx + dy * dy < 2200.f * 2200.f)
                    for (size_t k = 0; k < model->proxies.size() && k < 32; ++k) {
                        const GpuModel::Proxy& px = model->proxies[k];
                        if (px.hidden || (px.alt >= 0 && px.alt != alt)) continue; // fires of a damage state only then
                        const alamo::Vec3& p = px.position;
                        const alamo::Vec3 wp{xf.at(0, 0) * p[0] + xf.at(0, 1) * p[1] + xf.at(0, 2) * p[2] + xf.at(0, 3),
                                             xf.at(1, 0) * p[0] + xf.at(1, 1) * p[1] + xf.at(1, 2) * p[2] + xf.at(1, 3),
                                             xf.at(2, 0) * p[0] + xf.at(2, 1) * p[1] + xf.at(2, 2) * p[2] + xf.at(2, 3)};
                        effects->follow((uint64_t(u.id) << 8 | k) | (1ull << 63), px.name, wp, scale);
                    }
            }
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
            // A thin square frame on the ground (a filled plate hides the unit's surroundings).
            const float s = rad * 2.6f + 4, t = std::max(1.2f, s * 0.04f), z = u.position[2] + 0.6f;
            const Vec4 green{0.3f, 1.f, 0.4f, 1};
            const float cx = u.position[0], cy = u.position[1], h = s / 2;
            r.drawBox(Mat4::translate(cx, cy + h, z) * Mat4::scale3(s, t, 0.6f), green);
            r.drawBox(Mat4::translate(cx, cy - h, z) * Mat4::scale3(s, t, 0.6f), green);
            r.drawBox(Mat4::translate(cx + h, cy, z) * Mat4::scale3(t, s, 0.6f), green);
            r.drawBox(Mat4::translate(cx - h, cy, z) * Mat4::scale3(t, s, 0.6f), green);
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
    // The power network (Novus): while placing a structure or with a node selected, the links between
    // your powered generators and transmitters.
    {
        using P = alamo::EconomySpec::Power;
        bool show = placing();
        for (uint32_t id : selected_)
            if (const alamo::Unit* u = w.unit(id); u && u->owner == player_ && w.economy(*u->type).power != P::None) show = true;
        if (show) {
            std::vector<const alamo::Unit*> nodes;
            for (const alamo::Unit& u : w.units()) {
                if (u.owner != player_ || u.underConstruction || !u.powered) continue;
                const P role = w.economy(*u.type).power;
                if (role == P::Generator || role == P::Transmitter) nodes.push_back(&u);
            }
            for (size_t i = 0; i < nodes.size(); ++i)
                for (size_t j = i + 1; j < nodes.size(); ++j) {
                    const alamo::Unit& a = *nodes[i];
                    const alamo::Unit& b = *nodes[j];
                    const float reach = std::max(w.economy(*a.type).transmitRadius, w.economy(*b.type).transmitRadius);
                    const float dx = b.position[0] - a.position[0], dy = b.position[1] - a.position[1], len = std::hypot(dx, dy);
                    if (len > reach || len < 1) continue;
                    const float z = std::max(a.position[2], b.position[2]) + 18;
                    r.drawBox(Mat4::translate((a.position[0] + b.position[0]) / 2, (a.position[1] + b.position[1]) / 2, z) *
                                  Mat4::rotateZ(std::atan2(dy, dx)) * Mat4::scale3(len, 1.2f, 1.2f),
                              {0.35f, 0.85f, 1.f, 1});
                }
        }
    }
    // Units riding the Novus power network: a bright mote travelling along it.
    for (const alamo::Unit& u : w.units()) {
        if (u.flowTicks <= 0) continue;
        if (player_ >= 0 && w.enemies(player_, u.owner) && fog && w.fogOfWar() && !w.visibleTo(player_, u.position)) continue;
        const float z = w.map().heightAt(u.position[0], u.position[1]) + 14;
        r.drawBox(Mat4::translate(u.position[0], u.position[1], z) * Mat4::scale(5), {0.45f, 0.9f, 1.f, 1});
    }
    // Construction sites: a translucent-looking frame rising with progress; rally flags of selected structures.
    for (const alamo::Unit& u : w.units()) {
        if (!shown(w, u)) continue;
        if (u.structure && u.underConstruction) {
            // The building rises (cut off at its progress); a glowing band marks the cut, over a
            // low frame of the site.
            float top = 8 + 40 * u.buildProgress;
            if (const std::string* name = u.type->find("Land_Model_Name"); name && !alamo::trim(*name).empty())
                if (const GpuModel* m = r.model(alamo::trim(*name)))
                    top = std::max(2.f, m->boundsMax[2] * float(u.type->number("Scale_Factor").value_or(1.0)) * std::clamp(u.buildProgress, 0.f, 1.f));
            const float fx = u.footprintX * 2, fy = u.footprintY * 2;
            auto frame = [&](float z, float t, const Vec4& c) { // the site's outline at height z
                r.drawBox(Mat4::translate(u.position[0], u.position[1] + fy / 2, z) * Mat4::scale3(fx, t, t), c);
                r.drawBox(Mat4::translate(u.position[0], u.position[1] - fy / 2, z) * Mat4::scale3(fx, t, t), c);
                r.drawBox(Mat4::translate(u.position[0] + fx / 2, u.position[1], z) * Mat4::scale3(t, fy, t), c);
                r.drawBox(Mat4::translate(u.position[0] - fx / 2, u.position[1], z) * Mat4::scale3(t, fy, t), c);
            };
            frame(u.position[2] + 0.5f, 1.f, {0.35f, 0.4f, 0.45f, 1});
            frame(u.position[2] + top, 1.2f, {0.5f, 0.9f, 1.f, 1});
        }
        if (u.structure && u.hasRally && selected_.count(u.id)) {
            const float z = w.map().heightAt(u.rally[0], u.rally[1]);
            r.drawBox(Mat4::translate(u.rally[0], u.rally[1], z) * Mat4::scale3(2, 2, 40), {1, 1, 1, 1});
            r.drawBox(Mat4::translate(u.rally[0] + 7, u.rally[1], z + 30) * Mat4::scale3(12, 1.5f, 10), {0.3f, 1.f, 0.4f, 1});
        }
        // Shift-queued moves of selected units: small markers, joined by thin lines.
        if (!u.queuedMoves.empty() && selected_.count(u.id)) {
            alamo::Vec3 from = u.goal;
            for (const alamo::Vec3& q : u.queuedMoves) {
                const float z = w.map().heightAt(q[0], q[1]);
                r.drawBox(Mat4::translate(q[0], q[1], z + 1) * Mat4::scale3(5, 5, 2), {0.3f, 1.f, 0.4f, 1});
                const float dx = q[0] - from[0], dy = q[1] - from[1], len = std::hypot(dx, dy);
                if (len > 1)
                    r.drawBox(Mat4::translate((q[0] + from[0]) / 2, (q[1] + from[1]) / 2, z + 1.5f) * Mat4::rotateZ(std::atan2(dy, dx)) *
                                  Mat4::scale3(len, 0.8f, 0.8f),
                              {0.3f, 0.8f, 0.4f, 1});
                from = q;
            }
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
    r.setTeamColor({1, 1, 1, 1});
    for (const alamo::Projectile& p : w.projectiles()) {
        if (fog && w.fogOfWar() && player_ >= 0 && !w.visibleTo(player_, p.position)) continue;
        if (p.type)
            if (const std::string* m = p.type->find("Land_Model_Name"); m && !alamo::trim(*m).empty()) {
                if (effects && effects->system(alamo::trim(*m))) continue; // its trail is the picture
                if (const GpuModel* gm = r.model(alamo::trim(*m))) {      // a rocket or shell: the model, nose first
                    const float yaw = std::atan2(p.velocity[1], p.velocity[0]);
                    const float scale = float(p.type->number("Scale_Factor").value_or(1.0));
                    r.drawModel(*gm, Mat4::translate(p.position[0], p.position[1], p.position[2]) * Mat4::rotateZ(yaw + 1.5707963f) *
                                         Mat4::scale(scale));
                    continue;
                }
            }
        const float sz = p.blastDamage > 0 ? 6.f : 3.5f;
        r.drawBox(Mat4::translate(p.position[0], p.position[1], p.position[2] - sz / 2) * Mat4::scale(sz), shot[std::clamp(p.owner, 0, 3)]);
    }
    // The fallen: the type's death clone (DeathFXBehaviorType Death_Clone, e.g. a structure's
    // collapsing NB_..._DC model) or else the unit itself, playing its death animation
    // (<model>_DIE_0N) where it died; the last frame holds for Death_Persistence_Duration (at least
    // two seconds) and then sinks into the ground.
    const uint64_t now = w.tickCount();
    if (animate)
        for (auto it = w.deaths().rbegin(); it != w.deaths().rend(); ++it) {
            const alamo::DeathEvent& d = *it;
            const float age = float(now - d.tick) / float(alamo::kTicksPerSecond);
            if (age > 16.f) break; // deaths are in time order
            if (d.sold || !d.type) continue;
            if (fog && w.fogOfWar() && player_ >= 0 && !w.visibleTo(player_, d.position)) continue;
            const Fallen& f = fallenOf(r, *d.type, d.id);
            if (!f.model || (!f.anim && !f.wreck)) continue;
            const float animLen = f.anim ? float(f.anim->frameCount - 1) / std::max(f.anim->fps, 1.f) : 0.f;
            const float hold = std::max(animLen, f.persist), gone = hold + 2.5f;
            if (age > gone) continue;
            const Pose pose = f.anim ? r.pose(*f.model, *f.anim, std::min(age, animLen)) : Pose{};
            const float sink = age > hold ? (age - hold) * std::max(12.f, f.height / 2.5f) : 0;
            r.setTeamColor(playerColor(d.owner));
            r.drawModel(*f.model,
                        Mat4::translate(d.position[0], d.position[1], d.position[2] - sink) * Mat4::rotateZ(d.facing) *
                            Mat4::rotateZ(1.5707963f) * Mat4::scale(f.scale),
                        {1, 1, 1, 1}, f.anim ? &pose : nullptr);
        }
    r.setTeamColor({1, 1, 1, 1});
    // Wrecks: a dark scorch plate for a few seconds, only when there are no particle effects to show
    // the explosion (they replace it).
    if (!effects)
        for (const alamo::DeathEvent& d : w.deaths()) {
            const float age = float(now - d.tick) / float(alamo::kTicksPerSecond);
            if (age > 6 || (fog && w.fogOfWar() && player_ >= 0 && w.visibility(player_, d.position[0], d.position[1]) == 0)) continue;
            const float k = 1 - age / 6;
            const float s = 10 + 6 * std::min(age * 3, 1.f);
            r.drawBox(Mat4::translate(d.position[0], d.position[1], d.position[2] + 0.4f) * Mat4::scale3(s, s, 1.0f * k + 0.2f),
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
