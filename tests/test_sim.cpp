// Tests for navigation and the tactical simulation on a synthetic map.
#include "alamo/combat.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/nav.hpp"
#include "alamo/sim.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>

using namespace alamo;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// 50x50 flat map, 20 units apart. A wall at x = 25 for y = 0..40 leaves a gap at the top.
static Map wallMap() {
    Map m;
    m.width = m.height = 50;
    m.spacing = 20;
    m.heights.assign(2500, 0.f);
    m.layerIndex.assign(2500, 0);
    m.vertexFlags.assign(2500, 0x01);
    for (int y = 0; y <= 40; ++y) m.vertexFlags[size_t(y) * 50 + 25] = 0x02;
    return m;
}

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static GameDatabase testDb(const fs::path& root) {
    fs::remove_all(root);
    write(root / "Data/MegaFiles.xml", "<Mega_Files></Mega_Files>");
    write(root / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
    write(root / "Data/XML/Units.xml", R"(<Units>
  <X_Units Name="Trooper"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Max_Rate_Of_Turn>30</Max_Rate_Of_Turn><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>250</Tactical_Health></X_Units>
  <X_Units Name="Gunner"><MovementClass>Infantry</MovementClass><Max_Speed>1.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <BehaviorType Name="ArmorAndDamageBehaviorType"><Armor_Type>Armor_Soft</Armor_Type></BehaviorType>
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Bullet</Projectile_Types>
      <Targeting_Max_Attack_Distance>150</Targeting_Max_Attack_Distance>
      <Projectile_Fire_Recharge_Seconds>1.0</Projectile_Fire_Recharge_Seconds>
      <Projectile_Fire_Pulse_Count>2</Projectile_Fire_Pulse_Count>
      <Projectile_Fire_Pulse_Delay_Seconds>0.1</Projectile_Fire_Pulse_Delay_Seconds></BehaviorType></X_Units>
  <X_Units Name="Turtle"><MovementClass>Tracked</MovementClass><Max_Speed>1.0</Max_Speed>
    <Custom_Hard_XExtent>10</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <BehaviorType Name="ArmorAndDamageBehaviorType"><Armor_Type>Armor_Hard</Armor_Type></BehaviorType>
    <Attached_Hard_Points> HP_Mortar, Bone_01 </Attached_Hard_Points></X_Units>
  <HardPoint Name="HP_Mortar">
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Shell</Projectile_Types>
      <Targeting_Max_Attack_Distance>300</Targeting_Max_Attack_Distance>
      <Projectile_Fire_Recharge_Seconds>2.0</Projectile_Fire_Recharge_Seconds></BehaviorType></HardPoint>
  <Projectile Name="Proj_Bullet"><Max_Speed>30</Max_Speed>
    <BehaviorType Name="ArmorAndDamageBehaviorType"><Damage_Type>Damage_Bullet</Damage_Type></BehaviorType>
    <BehaviorType Name="ProjectileBehaviorType"><Projectile_Damage>5</Projectile_Damage></BehaviorType></Projectile>
  <Projectile Name="Proj_Shell"><Max_Speed>10</Max_Speed>
    <BehaviorType Name="ArmorAndDamageBehaviorType"><Damage_Type>Damage_Shell</Damage_Type></BehaviorType>
    <BehaviorType Name="ProjectileBehaviorType"><Projectile_Blast_Area_Damage>20</Projectile_Blast_Area_Damage>
      <Projectile_Blast_Area_Range>40</Projectile_Blast_Area_Range></BehaviorType></Projectile>
  <X_Units Name="Tank"><MovementClass>Tracked</MovementClass><Max_Speed>3.0</Max_Speed>
    <Max_Rate_Of_Turn>4</Max_Rate_Of_Turn><Stopped_Rate_Of_Turn>3</Stopped_Rate_Of_Turn>
    <OverrideAcceleration>0.1</OverrideAcceleration><Custom_Hard_XExtent>14</Custom_Hard_XExtent></X_Units>
</Units>)");
    MegVfs vfs;
    mountGame(vfs, root);
    GameDatabase db;
    db.load(vfs);
    return db;
}

static void testNav() {
    Map m = wallMap();
    NavGrid nav(m);
    CHECK(!nav.passable(25, 10) && nav.passable(25, 45) && nav.passable(24, 10));
    auto path = nav.findPath({100, 100, 0}, {900, 100, 0});
    CHECK(!path.empty());
    bool throughGap = false;
    Vec3 prev{100, 100, 0};
    for (const Vec3& p : path) {
        throughGap |= p[1] > 800;
        CHECK(nav.lineClear(prev, p)); // every segment walkable
        prev = p;
    }
    CHECK(throughGap);
    CHECK(!path.empty() && std::hypot(path.back()[0] - 900, path.back()[1] - 100) < 1);

    // Target inside the wall: ends next to it.
    auto blocked = nav.findPath({100, 100, 0}, {500, 200, 0});
    CHECK(!blocked.empty() && std::fabs(blocked.back()[0] - 500) <= 20.5f);

    // Fully walled-off start: no path.
    Map boxed = wallMap();
    for (int y = 0; y < 50; ++y) boxed.vertexFlags[size_t(y) * 50 + 25] = 0x20;
    NavGrid nb(boxed);
    auto none = nb.findPath({100, 100, 0}, {900, 100, 0});
    CHECK(none.empty() || none.back()[0] < 500);
}

static void testMovement(const GameDatabase& db) {
    Map m = wallMap();
    World w(m, db);
    const uint32_t id = w.spawn("Trooper", 0, {100, 100, 0});
    CHECK(id != 0);
    CHECK(w.spawn("NoSuchUnit", 0, {0, 0, 0}) == 0);
    w.commandMove({id}, {900, 100, 0});
    bool everBlocked = false;
    int ticks = 0;
    while (w.unit(id)->moving() && ticks < 5000) {
        w.tick();
        ++ticks;
        everBlocked |= !w.nav().passableAt(w.unit(id)->position[0], w.unit(id)->position[1]);
    }
    const Unit* u = w.unit(id);
    CHECK(!u->moving());
    CHECK(std::hypot(u->position[0] - 900, u->position[1] - 100) < 5);
    CHECK(!everBlocked);
    // Path is ~1700 units long at 2 units/tick: roughly 850 ticks plus turning/acceleration.
    CHECK(ticks > 800 && ticks < 1200);
    std::printf("trooper arrived after %d ticks (%.1f s)\n", ticks, ticks / float(kTicksPerSecond));

    // A tracked vehicle reversing direction turns in place first.
    const uint32_t tank = w.spawn("Tank", 1, {200, 600, 0}, 0.f);
    w.commandMove({tank}, {100, 600, 0});
    w.tick();
    const Unit* t = w.unit(tank);
    CHECK(std::fabs(t->position[0] - 200) < 0.5f); // still turning, not driving backwards
}

static void testGroups(const GameDatabase& db) {
    Map m = wallMap();
    auto run = [&]() {
        World w(m, db);
        std::vector<uint32_t> ids;
        for (int i = 0; i < 9; ++i) ids.push_back(w.spawn("Trooper", 0, {100.f + (i % 3) * 15, 100.f + (i / 3) * 15, 0}));
        w.commandMove(ids, {700, 300, 0});
        for (int t = 0; t < 1500; ++t) w.tick();
        return w;
    };
    World a = run(), b = run();
    float minGap = 1e30f;
    for (size_t i = 0; i < a.units().size(); ++i) {
        const Unit& u = a.units()[i];
        CHECK(!u.moving());
        CHECK(std::hypot(u.position[0] - 700, u.position[1] - 300) < 80);
        if (u.moving()) std::fprintf(stderr, "  unit %u at (%.0f,%.0f) goal (%.0f,%.0f) waypoints %zu next (%.0f,%.0f) speed %.2f\n", u.id,
                                     u.position[0], u.position[1], u.goal[0], u.goal[1], u.path.size(), u.path[0][0], u.path[0][1], u.speed);
        for (size_t j = i + 1; j < a.units().size(); ++j)
            minGap = std::min(minGap, std::hypot(u.position[0] - a.units()[j].position[0], u.position[1] - a.units()[j].position[1]));
        // Same inputs, same result: required for lockstep multiplayer later.
        CHECK(u.position == b.units()[i].position);
    }
    CHECK(minGap > 7); // radius 5 each: no stacking
}

static void testCombat(const GameDatabase& db) {
    Map m = wallMap();
    // Weapons from the unit itself and from an attached hardpoint.
    const auto gw = weaponsFor(*db.find("Gunner"), db);
    CHECK(gw.size() == 1 && gw[0].pulseCount == 2 && gw[0].rechargeTicks == 30 && gw[0].pulseDelayTicks == 3);
    const auto tw = weaponsFor(*db.find("Turtle"), db);
    CHECK(tw.size() == 1 && tw[0].source->name == "HP_Mortar" && tw[0].blastDamage == 20);

    DamageTable table;
    table.set("Damage_Bullet", "Armor_Hard", 2.0f);
    CHECK(table.modifier("damage_bullet", "ARMOR_HARD") == 2.0f && table.modifier("Damage_Bullet", "Armor_Soft") == 1.0f);

    // Gunner vs Turtle at 120 units: bullets do 5 x2 = 10 per shot against hard armor,
    // two per burst every second -> 100 health gone in about 5 seconds.
    World w(m, db, &table, 7);
    const uint32_t gunner = w.spawn("Gunner", 0, {200, 400, 0});
    const uint32_t turtle = w.spawn("Turtle", 1, {320, 400, 0});
    // The turtle's mortar (blast 20, 1.0x vs soft) fights back: 100 health in 5 shells, one per 2 s.
    int ticks = 0;
    while (w.unit(turtle) && w.unit(gunner) && ticks < 30 * 30) {
        w.tick();
        ++ticks;
    }
    CHECK(!w.unit(turtle));            // the gunner wins the race
    CHECK(w.unit(gunner) != nullptr);
    CHECK(ticks > 4 * 30 && ticks < 6 * 30);
    CHECK(w.deaths().size() == 1 && w.deaths()[0].id == turtle);
    if (const Unit* g = w.unit(gunner)) CHECK(g->health < 100 && g->health > 0); // took mortar hits
    std::printf("gunner beat turtle in %d ticks (%.1f s)\n", ticks, ticks / 30.f);

    // Out of range: nobody shoots.
    World far(m, db, &table, 7);
    far.spawn("Gunner", 0, {100, 100, 0});
    far.spawn("Gunner", 1, {600, 100, 0});
    for (int i = 0; i < 90; ++i) far.tick();
    CHECK(far.projectiles().empty() && far.units()[0].health == 100);

    // Attack order: chase into range, then kill.
    World chase(m, db, &table, 7);
    const uint32_t hunter = chase.spawn("Gunner", 0, {100, 300, 0});
    const uint32_t prey = chase.spawn("Tank", 1, {400, 300, 0}); // unarmed
    chase.commandAttack({hunter}, prey);
    for (int i = 0; i < 60 * 30 && chase.unit(prey); ++i) chase.tick();
    CHECK(!chase.unit(prey));

    // Same seed, same battle.
    auto battle = [&](uint64_t seed) {
        World b(m, db, &table, seed);
        for (int i = 0; i < 4; ++i) {
            b.spawn("Gunner", 0, {200.f, 200.f + i * 25, 0});
            b.spawn("Gunner", 1, {320.f, 200.f + i * 25, 0});
        }
        for (int i = 0; i < 20 * 30; ++i) b.tick();
        std::vector<float> hp;
        for (const Unit& u : b.units()) hp.push_back(u.health);
        return hp;
    };
    CHECK(battle(3) == battle(3));
}

int main() {
    GameDatabase db = testDb(fs::temp_directory_path() / "uaw_native_test_sim");
    testNav();
    testMovement(db);
    testGroups(db);
    testCombat(db);
    fs::remove_all(fs::temp_directory_path() / "uaw_native_test_sim");
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all nav/sim tests passed\n");
    return 0;
}
