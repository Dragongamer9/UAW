// Tests for navigation and the tactical simulation on a synthetic map.
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

int main() {
    GameDatabase db = testDb(fs::temp_directory_path() / "uaw_native_test_sim");
    testNav();
    testMovement(db);
    testGroups(db);
    fs::remove_all(fs::temp_directory_path() / "uaw_native_test_sim");
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all nav/sim tests passed\n");
    return 0;
}
