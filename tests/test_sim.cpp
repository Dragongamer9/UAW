// Tests for navigation and the tactical simulation on a synthetic map.
#include "alamo/combat.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/nav.hpp"
#include "alamo/ai.hpp"
#include "alamo/sim.hpp"
#include "alamo/techtree.hpp"

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

static Map openMap() {
    Map m = wallMap();
    m.vertexFlags.assign(2500, 0x01);
    return m;
}

static Balance unitIncome() {
    Balance b = Balance::defaults();
    for (auto& [name, f] : b.factions) f.income = 1;
    return b;
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
  <X_Units Name="Builder"><MovementClass>Infantry</MovementClass><Max_Speed>3.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <BehaviorType Name="FleetBehaviorType"><Unit_Pop_Cap>2</Unit_Pop_Cap></BehaviorType>
    <BehaviorType Name="BuildableObjectBehaviorType"><Tactical_Build_Cost_Multiplayer>200</Tactical_Build_Cost_Multiplayer>
      <Tactical_Build_Time_Seconds>4</Tactical_Build_Time_Seconds></BehaviorType></X_Units>
  <X_Structures Name="HQ"><MovementClass>Building</MovementClass><Tactical_Health>2000</Tactical_Health>
    <Custom_Hard_XExtent>40</Custom_Hard_XExtent><Custom_Hard_YExtent>30</Custom_Hard_YExtent></X_Structures>
  <X_Structures Name="Barracks"><MovementClass>Building</MovementClass><Tactical_Health>1000</Tactical_Health>
    <Custom_Hard_XExtent>30</Custom_Hard_XExtent>
    <BehaviorType Name="BuildableObjectBehaviorType">
      <Tactical_Under_Construction_Object_Type>Barracks_Construction</Tactical_Under_Construction_Object_Type></BehaviorType></X_Structures>
  <X_Structures Name="Barracks_Construction"><MovementClass>Building</MovementClass>
    <BehaviorType Name="BuildableObjectBehaviorType"><Tactical_Build_Cost_Multiplayer>500</Tactical_Build_Cost_Multiplayer>
      <Tactical_Build_Time_Seconds>10</Tactical_Build_Time_Seconds></BehaviorType></X_Structures>
  <HardPoint Name="Barracks_Upgrade"><BehaviorType Name="BuildableObjectBehaviorType">
      <Tactical_Build_Cost_Multiplayer>300</Tactical_Build_Cost_Multiplayer>
      <Tactical_Build_Time_Seconds>3</Tactical_Build_Time_Seconds></BehaviorType></HardPoint>
  <X_Units Name="Tank"><MovementClass>Tracked</MovementClass><Max_Speed>3.0</Max_Speed>
    <Max_Rate_Of_Turn>4</Max_Rate_Of_Turn><Stopped_Rate_Of_Turn>3</Stopped_Rate_Of_Turn>
    <OverrideAcceleration>0.1</OverrideAcceleration><Custom_Hard_XExtent>14</Custom_Hard_XExtent></X_Units>
  <X_Structures Name="Depot"><MovementClass>Building</MovementClass><Custom_Hard_XExtent>20</Custom_Hard_XExtent>
    <BehaviorType Name="ResourceDepotBehaviorType"><Resource_Depot_Active_Drones>2</Resource_Depot_Active_Drones>
      <Resource_Depot_Seconds_To_Build_Drone>1</Resource_Depot_Seconds_To_Build_Drone>
      <Resource_Depot_Collection_Radius>2000</Resource_Depot_Collection_Radius></BehaviorType>
    <BehaviorType Name="PoweredBehaviorType"><Role>Receiver</Role><Radius_Attribute>Powerup</Radius_Attribute></BehaviorType>
    <Attributes><Powerup>0</Powerup></Attributes></X_Structures>
  <X_Structures Name="Router"><MovementClass>Building</MovementClass><Custom_Hard_XExtent>10</Custom_Hard_XExtent>
    <BehaviorType Name="PoweredBehaviorType"><Role>Generator</Role><Radius_Attribute>Powerup</Radius_Attribute>
      <Transmit_Radius_Attribute>Transmit</Transmit_Radius_Attribute></BehaviorType>
    <Attributes><Powerup>150</Powerup><Transmit>400</Transmit></Attributes></X_Structures>
  <X_Units Name="Drone"><MovementClass>Flying</MovementClass><Max_Speed>4.0</Max_Speed><Custom_Hard_XExtent>4</Custom_Hard_XExtent>
    <Is_Resource_Collector>true</Is_Resource_Collector>
    <BehaviorType Name="AIBehaviorType"><Drone_Capacity>200</Drone_Capacity><Resources_Per_Grab>100</Resources_Per_Grab>
      <Resources_Seconds_Per_Grab>1</Resources_Seconds_Per_Grab><Resource_Gather_Range>30</Resource_Gather_Range>
      <Resource_Depot_Type>Depot</Resource_Depot_Type></BehaviorType></X_Units>
  <X_Units Name="Reaper"><MovementClass>Walker</MovementClass><Max_Speed>1.0</Max_Speed><Custom_Hard_XExtent>12</Custom_Hard_XExtent>
    <Is_Resource_Collector>true</Is_Resource_Collector>
    <BehaviorType Name="LuaScriptBehaviorType"><Lua_Land_Behaviors>AlienReaperTurret</Lua_Land_Behaviors>
      <Lua_Script_Data>ResourceFaction = "Alien"  resource_drone_search_radius = 300.0  resource_drone_harvest_distance = 60.0</Lua_Script_Data></BehaviorType></X_Units>
  <X_Structures Name="Engine"><MovementClass>Building</MovementClass><Custom_Hard_XExtent>15</Custom_Hard_XExtent>
    <BehaviorType Name="LuaScriptBehaviorType"><Lua_Land_Behaviors>MasariMatterEngine</Lua_Land_Behaviors>
      <Lua_Script_Data>MatterEngine = {}  MatterEngine.ResourcesPerTimeBlock = 6.0  MatterEngine.ResourcesTimeSeconds = 2.0
        MatterEngine.ResourcesStorage = 100.0</Lua_Script_Data></BehaviorType></X_Structures>
  <X_Structures Name="Refinery"><MovementClass>Building</MovementClass><Custom_Hard_XExtent>15</Custom_Hard_XExtent>
    <BehaviorType Name="LuaScriptBehaviorType"><Lua_Land_Behaviors>NeutralRefinery, MasariMatterEngine</Lua_Land_Behaviors>
      <Lua_Script_Data>ResourcesPerSecond = 10.0 MatterEngine = {} MatterEngine.MustBeOfFaction = "Masari"</Lua_Script_Data></BehaviorType></X_Structures>
  <X_Resource_Objects Name="Scrap"><CategoryMask>Resource</CategoryMask><Custom_Hard_XExtent>10</Custom_Hard_XExtent>
    <BehaviorType Name="ResourceBehaviorType"><Resource_Units>1000</Resource_Units>
      <Resource_For_Faction>Novus, Alien</Resource_For_Faction></BehaviorType></X_Resource_Objects>
</Units>)");
    write(root / "Data/XML/Factions.xml", R"(<Factions>
  <Faction Name="Masari"><Baseline_Bank_Capacity>400</Baseline_Bank_Capacity></Faction>
  <Faction Name="Novus"></Faction><Faction Name="Alien"></Faction></Factions>)");
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

static void testEconomy(const GameDatabase& db) {
    Map m = wallMap();
    TechTree tech;
    tech.add("Builder", {{"HQ"}, {}});
    tech.add("Barracks", {{}, {"Builder"}});
    tech.add("Barracks_Upgrade", {{"Barracks"}, {}});
    tech.add("Gunner", {{"Barracks"}, {"Barracks_Upgrade"}}); // needs the upgrade first

    World w(m, db, nullptr, 1, &tech);
    const int p = w.addPlayer("Test", 1200, 6);
    const uint32_t hq = w.spawn("HQ", p, {200, 200, 0});
    CHECK(w.unit(hq)->structure);
    w.tick(); // footprint applied to navigation
    CHECK(!w.nav().passableAt(200, 200) && !w.nav().passableAt(235, 225) && w.nav().passableAt(260, 200));

    // HQ makes builders; builders make barracks; nothing else yet.
    CHECK(w.producible(hq) == std::vector<std::string>{"Builder"});
    CHECK(w.queueProduction(hq, "Gunner") == OrderResult::NotAllowed);
    CHECK(w.queueProduction(hq, "Builder") == OrderResult::Ok);
    CHECK(w.player(p)->credits == 1000);
    CHECK(w.popUsed(p) == 2); // queued units count against the cap
    for (int i = 0; i < 4 * 30; ++i) w.tick();
    uint32_t builder = 0;
    for (const Unit& u : w.units())
        if (u.type->name == "Builder") builder = u.id;
    CHECK(builder != 0);
    if (!builder) return;
    const Unit* bu = w.unit(builder);
    CHECK(std::fabs(bu->position[0] - 200) > 40 || std::fabs(bu->position[1] - 200) > 30); // came out beside the HQ

    CHECK(w.constructible(hq).empty()); // structures don't construct
    CHECK(w.constructible(builder) == std::vector<std::string>{"Barracks"});
    CHECK(w.commandConstruct(builder, "Barracks", {210, 210, 0}) == OrderResult::BlockedSite); // on the HQ
    CHECK(w.commandConstruct(builder, "Barracks", {300, 600, 0}) == OrderResult::Ok);
    CHECK(w.player(p)->credits == 500); // cost from Barracks_Construction
    uint32_t barracks = 0;
    for (const Unit& u : w.units())
        if (u.type->name == "Barracks") barracks = u.id;
    CHECK(w.unit(barracks) && w.unit(barracks)->underConstruction);
    if (!w.unit(barracks)) return;
    CHECK(w.producible(barracks).empty()); // not finished
    int ticks = 0;
    while (w.unit(barracks)->underConstruction && ticks < 60 * 30) { w.tick(); ++ticks; }
    CHECK(!w.unit(barracks)->underConstruction);
    // A few seconds to walk ~400 units at 3/tick, then 10 s of building.
    CHECK(ticks > 10 * 30 && ticks < 16 * 30);
    CHECK(w.unit(barracks)->health > 900);

    // The gunner needs the upgrade; the upgrade is researched, not spawned, and only once.
    CHECK(w.producible(barracks) == std::vector<std::string>{"Barracks_Upgrade"});
    const size_t unitsBefore = w.units().size();
    CHECK(w.queueProduction(barracks, "Barracks_Upgrade") == OrderResult::Ok);
    CHECK(w.queueProduction(barracks, "Barracks_Upgrade") == OrderResult::NotAllowed);
    for (int i = 0; i < 3 * 30 + 1; ++i) w.tick();
    CHECK(w.units().size() == unitsBefore);
    CHECK(w.owns(p, "Barracks_Upgrade"));
    auto prod = w.producible(barracks);
    CHECK(std::find(prod.begin(), prod.end(), "Gunner") != prod.end());
    CHECK(w.player(p)->credits == 200);
    CHECK(w.queueProduction(barracks, "Gunner") == OrderResult::Ok); // Gunner has no cost set: free
    w.cancelProduction(barracks);
    CHECK(w.unit(barracks)->queue.empty());

    // Pop cap 6: builder (2) + two more builders (4) = 6, a third is refused.
    w.player(p)->credits = 5000;
    CHECK(w.queueProduction(hq, "Builder") == OrderResult::Ok);
    CHECK(w.queueProduction(hq, "Builder") == OrderResult::Ok);
    CHECK(w.queueProduction(hq, "Builder") == OrderResult::PopulationCap);

    // Destroying a structure frees its footprint.
    w.unit(hq)->health = 0;
    w.tick();
    w.tick();
    CHECK(w.nav().passableAt(200, 200));
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

static void testStartingForces() {
    const char* lua = R"(
    StartingForces = {
        ["ALIEN"] = { "ALIEN_ARRIVAL_SITE", "ALIEN_GLYPH_CARVER", },
        -- a comment with "quotes" in it
        ["MILITARY"] = {
            "Military_Bunker",
            'Military_Build_Bot',
        },
    }
    Other = { ["X"] = { "Y" } })";
    const StartingForces f = parseStartingForces(lua);
    CHECK(f.size() == 2);
    CHECK(f.count("MILITARY") && f.at("MILITARY").size() == 2 && f.at("MILITARY")[1] == "Military_Build_Bot");
    CHECK(f.count("ALIEN") && f.at("ALIEN")[0] == "ALIEN_ARRIVAL_SITE");
    CHECK(parseStartingForces("nothing here").empty());
    CHECK(defaultStartingForces().count("NOVUS"));
}

static void testResources(const GameDatabase& db) {
    Map m = openMap(); // flat, all passable
    // Script data parsing.
    const auto sd = parseScriptData("ResourcesPerSecond = 16.0      MatterEngine = {}  MatterEngine.MustBeOfFaction = \"Masari\" a=1");
    CHECK(sd.count("RESOURCESPERSECOND") && sd.at("RESOURCESPERSECOND") == "16.0");
    CHECK(sd.count("MATTERENGINE.MUSTBEOFFACTION") && sd.at("MATTERENGINE.MUSTBEOFFACTION") == "Masari");
    CHECK(sd.count("A") && sd.at("A") == "1");

    // Collectors: an unpowered depot launches nothing; with a router nearby it launches two drones
    // that bring the pile home 200 at a time.
    {
        World w(m, db);
        w.setBalance(unitIncome()); // test the mechanics, not the tuned multipliers
        const int nv = w.addPlayer("Novus", 0);
        const uint32_t depot = w.spawn("Depot", nv, {200, 200, 0});
        const uint32_t pile = w.addResource("Scrap", {600, 200, 0});
        CHECK(depot && pile && w.resource(pile) && w.resource(pile)->remaining == 1000);
        for (int i = 0; i < 5 * 30; ++i) w.tick();
        int drones = 0;
        for (const Unit& u : w.units()) drones += u.depot == depot;
        CHECK(drones == 0); // no power
        w.spawn("Router", nv, {260, 260, 0});
        for (int i = 0; i < 60 * 30; ++i) w.tick();
        drones = 0;
        for (const Unit& u : w.units()) drones += u.depot == depot;
        CHECK(drones == 2);
        const float credits = w.player(nv)->credits;
        CHECK(credits >= 200 && std::fmod(credits, 100.f) == 0); // whole grabs delivered
        CHECK(w.resource(pile)->remaining + credits + 400 >= 1000 - 0.01f); // conserved (minus loads in flight)
        for (int i = 0; i < 120 * 30; ++i) w.tick();
        CHECK(w.resource(pile)->remaining == 0 && w.player(nv)->credits == 1000);
    }
    // Faction filter: Masari can't take Novus/Alien scrap; Reapers drain piles in reach.
    {
        World w(m, db);
        w.setBalance(unitIncome()); // test the mechanics, not the tuned multipliers
        const int al = w.addPlayer("Alien", 0);
        const uint32_t pile = w.addResource("Scrap", {400, 200, 0});
        w.spawn("Reaper", al, {200, 200, 0});
        for (int i = 0; i < 10 * 30; ++i) w.tick();
        CHECK(w.player(al)->credits > 0 && w.resource(pile)->remaining < 1000);
        // Each scrap unit is worth 10 credits to the Hierarchy (Balance default): same income rate, pile lasts 10x.
        CHECK(std::fabs(w.player(al)->credits / 10 + w.resource(pile)->remaining - 1000) < 0.5f);
        Balance flat;
        World w2(m, db);
        w2.setBalance(flat);
        const int al2 = w2.addPlayer("Alien", 0);
        const uint32_t pile2 = w2.addResource("Scrap", {400, 200, 0});
        w2.spawn("Reaper", al2, {200, 200, 0});
        for (int i = 0; i < 10 * 30; ++i) w2.tick();
        CHECK(std::fabs(w2.player(al2)->credits - w.player(al)->credits) < 1);           // same earning speed
        CHECK(std::fabs(w2.player(al2)->credits + w2.resource(pile2)->remaining - 1000) < 0.5f); // 1:1 without it
    }
    // Matter Engines: 3/s each, Masari bank capped at baseline + storage; refinery pays anyone.
    {
        World w(m, db);
        w.setBalance(unitIncome()); // test the mechanics, not the tuned multipliers
        const int ma = w.addPlayer("Masari", 0), nv = w.addPlayer("Novus", 0);
        w.spawn("Engine", ma, {200, 200, 0});
        w.spawn("Refinery", nv, {600, 600, 0});
        CHECK(std::fabs(w.incomePerSecond(ma) - 3) < 1e-4f && std::fabs(w.incomePerSecond(nv) - 10) < 1e-4f);
        CHECK(w.bankCap(ma) == 500 && w.bankCap(nv) == 0);
        for (int i = 0; i < 10 * 30; ++i) w.tick();
        CHECK(std::fabs(w.player(ma)->credits - 30) < 0.5f && std::fabs(w.player(nv)->credits - 100) < 0.5f);
        for (int i = 0; i < 300 * 30; ++i) w.tick();
        CHECK(std::fabs(w.player(ma)->credits - 500) < 0.01f); // capped
    }
}

// Two AIs on an open map with a tiny tech tree: they build, produce, fight, and the
// whole match replays exactly.
static void testAI(const GameDatabase& db) {
    Map m = openMap();
    DamageTable dmg;
    TechTree tech;
    tech.add("Builder", {{"HQ"}, {}});
    tech.add("Barracks", {{}, {"Builder"}});
    tech.add("Refinery", {{}, {"Builder"}});
    tech.add("Gunner", {{"Barracks"}, {}});
    auto run = [&](uint64_t seed, std::string& summary) {
        World w(m, db, &dmg, seed, &tech);
        const int a = w.addPlayer("Novus", 3000, 40), b = w.addPlayer("Alien", 3000, 40);
        w.spawn("HQ", a, {150, 150, 0});
        w.spawn("Builder", a, {250, 150, 0});
        w.spawn("HQ", b, {830, 830, 0});
        w.spawn("Builder", b, {730, 830, 0});
        SkirmishAI A(w, a, seed), B(w, b, seed + 1);
        bool builtBarracks = false, producedGunner = false, fought = false;
        int t = 0;
        for (; t < 20 * 60 * 30 && !defeated(w, a) && !defeated(w, b); ++t) {
            A.update();
            B.update();
            w.tick();
            for (const Unit& u : w.units()) {
                builtBarracks = builtBarracks || (u.type->name == "Barracks" && !u.underConstruction);
                producedGunner = producedGunner || u.type->name == "Gunner";
            }
            fought = fought || !w.projectiles().empty();
        }
        CHECK(builtBarracks && producedGunner && fought);
        summary = std::to_string(t) + ":" + std::to_string(w.units().size()) + ":" + std::to_string(int(w.player(a)->credits)) + ":" +
                  std::to_string(int(w.player(b)->credits)) + ":" + std::to_string(defeated(w, a)) + std::to_string(defeated(w, b));
        for (const Unit& u : w.units()) summary += ":" + std::to_string(int(u.position[0])) + "," + std::to_string(int(u.position[1]));
    };
    std::string s1, s2;
    run(5, s1);
    run(5, s2);
    CHECK(s1 == s2);
    std::printf("ai match: %s\n", s1.substr(0, 40).c_str());
}

// Hierarchy-style walkers: carved by base builders though no Tactical_Builder names them, and
// hardpoints bought into sockets nested in the walker's hardpoint tree.
static void testWalkers() {
    const fs::path root = fs::temp_directory_path() / "uaw_native_test_walkers";
    fs::remove_all(root);
    write(root / "Data/MegaFiles.xml", "<Mega_Files></Mega_Files>");
    write(root / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
    write(root / "Data/XML/Units.xml", R"(<Units>
  <X_Units Name="Carver"><MovementClass>Infantry</MovementClass><Max_Speed>3.0</Max_Speed><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Affiliation>Alien</Affiliation><Is_Tactical_Base_Builder>Yes</Is_Tactical_Base_Builder></X_Units>
  <X_Structures Name="Core"><MovementClass>Building</MovementClass><Custom_Hard_XExtent>20</Custom_Hard_XExtent>
    <Affiliation>Alien</Affiliation></X_Structures>
  <X_Units Name="Walker"><MovementClass>Walker</MovementClass><Max_Speed>1.0</Max_Speed><Custom_Hard_XExtent>60</Custom_Hard_XExtent>
    <Tactical_Health>5000</Tactical_Health><Affiliation>Alien</Affiliation>
    <Attached_Hard_Points> Walker_Leg_00, B_Leg0
      Walker_Leg_01, B_Leg1 </Attached_Hard_Points></X_Units>
  <X_Units Name="Novus_Walker"><MovementClass>Walker</MovementClass><Affiliation>Novus</Affiliation></X_Units>
  <HardPoint Name="Walker_Leg_00"><Attached_Hard_Points> Leg_Socket_00, B_Socket </Attached_Hard_Points></HardPoint>
  <HardPoint Name="Walker_Leg_01"><Variant_Of_Existing_Type>Walker_Leg_00</Variant_Of_Existing_Type>
    <Attached_Hard_Points> Leg_Socket_01, B_Socket </Attached_Hard_Points></HardPoint>
  <HardPoint Name="Leg_Socket"></HardPoint>
  <HardPoint Name="Leg_Socket_00"><Variant_Of_Existing_Type>Leg_Socket</Variant_Of_Existing_Type></HardPoint>
  <HardPoint Name="Leg_Socket_01"><Variant_Of_Existing_Type>Leg_Socket</Variant_Of_Existing_Type></HardPoint>
  <HardPoint Name="HP_Gun"><BehaviorType Name="BuildableObjectBehaviorType"><Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds></BehaviorType>
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Bullet</Projectile_Types>
      <Targeting_Max_Attack_Distance>150</Targeting_Max_Attack_Distance></BehaviorType></HardPoint>
  <X_Units Name="Mutant"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed><Custom_Hard_XExtent>5</Custom_Hard_XExtent></X_Units>
  <Projectile Name="Proj_Bullet"><Max_Speed>30</Max_Speed>
    <BehaviorType Name="ProjectileBehaviorType"><Projectile_Damage>5</Projectile_Damage></BehaviorType></Projectile>
</Units>)");
    MegVfs vfs;
    mountGame(vfs, root);
    GameDatabase db;
    db.load(vfs);
    TechTree tech;
    tech.add("Core", {{}, {"Carver"}});
    tech.add("Walker", {{}, {"Core"}});       // Tactical_Builder null, only the core required
    tech.add("Novus_Walker", {{}, {"Core"}}); // another faction's: not ours to carve
    tech.add("HP_Gun", {{"Leg_Socket"}, {}});
    tech.add("Mutant", {{"Walker"}, {"HP_Gun"}});
    Map m = openMap();
    World w(m, db, nullptr, 1, &tech);
    const int p = w.addPlayer("Alien", 10000, 100);
    const uint32_t carver = w.spawn("Carver", p, {200, 200, 0});
    CHECK(w.constructible(carver) == std::vector<std::string>{"Core"});
    CHECK(w.commandConstruct(carver, "Core", {300, 300, 0}) == OrderResult::Ok);
    for (int i = 0; i < 60 * 30 && !w.owns(p, "Core"); ++i) w.tick();
    CHECK(w.owns(p, "Core"));
    const auto opts = w.constructible(carver);
    CHECK(std::find(opts.begin(), opts.end(), "Walker") != opts.end());
    CHECK(std::find(opts.begin(), opts.end(), "Novus_Walker") == opts.end());

    // Two leg sockets (both variants of Leg_Socket): two guns, then no more; the Mutant needs one.
    const uint32_t walker = w.spawn("Walker", p, {500, 500, 0});
    CHECK(w.producible(walker) == std::vector<std::string>{"HP_Gun"});
    CHECK(w.unit(walker)->weapons.empty());
    CHECK(w.queueProduction(walker, "HP_Gun") == OrderResult::Ok);
    for (int i = 0; i < 2 * 30; ++i) w.tick();
    CHECK(w.unit(walker)->weapons.size() == 1); // the hardpoint's gun came with it
    auto prod = w.producible(walker);
    CHECK(std::find(prod.begin(), prod.end(), "Mutant") != prod.end());
    CHECK(std::find(prod.begin(), prod.end(), "HP_Gun") != prod.end()); // second socket still free
    CHECK(w.queueProduction(walker, "HP_Gun") == OrderResult::Ok);
    prod = w.producible(walker);
    CHECK(std::find(prod.begin(), prod.end(), "HP_Gun") == prod.end()); // both sockets taken
    CHECK(w.queueProduction(walker, "HP_Gun") == OrderResult::NotAllowed);
    // A second walker has sockets of its own.
    const uint32_t walker2 = w.spawn("Walker", p, {800, 500, 0});
    CHECK(w.queueProduction(walker2, "HP_Gun") == OrderResult::Ok);
    fs::remove_all(root);
}

// A superweapon: armed after its warm-up, fired at a point, the weapon object's bombardment lands
// there, and it recharges.
static void testSuperweapon() {
    const fs::path root = fs::temp_directory_path() / "uaw_native_test_superweapon";
    fs::remove_all(root);
    write(root / "Data/MegaFiles.xml", "<Mega_Files></Mega_Files>");
    write(root / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
    write(root / "Data/XML/Units.xml", R"(<Units>
  <X_Structures Name="Silo"><MovementClass>Building</MovementClass><Custom_Hard_XExtent>20</Custom_Hard_XExtent><Tactical_Health>1000</Tactical_Health>
    <BehaviorType Name="LuaScriptBehaviorType"><Lua_Land_Behaviors> SuperWeaponEnablerBehavior </Lua_Land_Behaviors>
      <Lua_Script_Data> SWOwnerList = {} SWOwnerList[Find_Object_Type("Silo")]="SILO_WEAPON" </Lua_Script_Data></BehaviorType></X_Structures>
  <X_Superweapons Name="Silo_Weapon"><CategoryMask> Untargetable, Superweapon </CategoryMask>
    <BehaviorType Name="TacticalSuperweaponBehaviorType"><TSW_Cooldown_Countdown_Seconds> 20 </TSW_Cooldown_Countdown_Seconds>
      <TSW_Area_Effect_Radius> 100 </TSW_Area_Effect_Radius><TSW_Warm_Up_Seconds> 10 </TSW_Warm_Up_Seconds></BehaviorType>
    <BehaviorType Name="OrbitalBombardmentBehaviorType"><Bombardment_Projectile> Proj_Bomb </Bombardment_Projectile>
      <Bombardment_Number_Of_Salvos> 1 </Bombardment_Number_Of_Salvos><Bombardment_Shots_Per_Salvo> 1 </Bombardment_Shots_Per_Salvo>
      <Bombardment_Shot_Delay> 1 </Bombardment_Shot_Delay></BehaviorType></X_Superweapons>
  <Projectile Name="Proj_Bomb"><Max_Speed>100</Max_Speed>
    <BehaviorType Name="ProjectileBehaviorType"><Projectile_Blast_Area_Damage>500</Projectile_Blast_Area_Damage>
      <Projectile_Blast_Area_Range>100</Projectile_Blast_Area_Range></BehaviorType></Projectile>
  <X_Units Name="Grunt"><MovementClass>Infantry</MovementClass><Max_Speed>1.0</Max_Speed><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>300</Tactical_Health></X_Units>
</Units>)");
    MegVfs vfs;
    mountGame(vfs, root);
    GameDatabase db;
    db.load(vfs);
    Map m = openMap();
    World w(m, db, nullptr, 1, nullptr);
    const int a = w.addPlayer("Alien", 0, 100), b = w.addPlayer("Novus", 0, 100);
    const uint32_t silo = w.spawn("Silo", a, {150, 150, 0});
    const uint32_t near = w.spawn("Grunt", b, {700, 700, 0}), far = w.spawn("Grunt", b, {300, 800, 0});
    CHECK(w.superweaponOf(*db.find("Silo")) && w.superweaponOf(*db.find("Silo"))->weapon == db.find("Silo_Weapon"));
    CHECK(!w.superweaponOf(*db.find("Grunt")));
    w.tick();
    CHECK(std::fabs(w.superweaponCountdown(silo) - 10) < 0.1f); // warming up
    CHECK(w.fireSuperweapon(silo, {700, 700, 0}) == OrderResult::NotAllowed);
    for (int i = 0; i < 10 * 30; ++i) w.tick();
    CHECK(w.superweaponCountdown(silo) == 0);
    CHECK(w.fireSuperweapon(silo, {700, 700, 0}) == OrderResult::Ok);
    CHECK(w.superweaponCountdown(silo) > 19);
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    CHECK(!w.unit(near)); // in the blast (500 damage against 300 health)
    CHECK(w.unit(far) && w.unit(far)->health == 300);         // well outside it
    CHECK(w.fireSuperweapon(silo, {700, 700, 0}) == OrderResult::NotAllowed); // recharging
    fs::remove_all(root);
}

static void testBurst() {
    const fs::path root = fs::temp_directory_path() / "uaw_native_test_burst";
    fs::remove_all(root);
    write(root / "Data/MegaFiles.xml", "<Mega_Files></Mega_Files>");
    write(root / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
    write(root / "Data/XML/Units.xml", R"(<Units>
  <X_Units Name="Spinner"><MovementClass>Infantry</MovementClass><Max_Speed>1.0</Max_Speed><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>300</Tactical_Health>
    <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability Name="Spin_Unit"><Type> DERVISH_SPIN_ATTACK </Type>
      <Recharge_Seconds> 30 </Recharge_Seconds><GUI_Activated_Ability_Name> Spin </GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>
    <Abilities SubObjectList="Yes"><Dervish_Spin_Attack_Ability Name="Spin"><Activation_Style>USER_INPUT</Activation_Style>
      <Attack_Duration_Seconds> 2.0 </Attack_Duration_Seconds><Attack_Radius> 100.0 </Attack_Radius>
      <AOE_Damage_Per_Second> 40.0 </AOE_Damage_Per_Second><Spin_Up_Seconds> 1.0 </Spin_Up_Seconds></Dervish_Spin_Attack_Ability></Abilities></X_Units>
  <X_Units Name="Grunt"><MovementClass>Infantry</MovementClass><Max_Speed>1.0</Max_Speed><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>300</Tactical_Health></X_Units>
</Units>)");
    MegVfs vfs;
    mountGame(vfs, root);
    GameDatabase db;
    db.load(vfs);
    Map m = openMap();
    World w(m, db, nullptr, 1, nullptr);
    const int a = w.addPlayer("Novus", 0, 100), b = w.addPlayer("Alien", 0, 100);
    const uint32_t s = w.spawn("Spinner", a, {500, 500, 0});
    const uint32_t near = w.spawn("Grunt", b, {550, 500, 0}), far = w.spawn("Grunt", b, {800, 500, 0});
    w.tick();
    CHECK(w.unit(s)->abilities.size() == 1);
    CHECK(w.useAbility(s, 0) == OrderResult::Ok);
    for (int i = 0; i < 20; ++i) w.tick();
    CHECK(w.unit(near)->health == 300); // still spinning up
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    CHECK(std::fabs(w.unit(near)->health - 220) < 1); // 40 per second for two seconds
    CHECK(w.unit(far)->health == 300);
    CHECK(w.useAbility(s, 0) == OrderResult::NotAllowed); // recharging
    fs::remove_all(root);
}

int main() {
    testStartingForces();
    GameDatabase db = testDb(fs::temp_directory_path() / "uaw_native_test_sim");
    testNav();
    testMovement(db);
    testGroups(db);
    testCombat(db);
    testEconomy(db);
    testResources(db);
    testAI(db);
    testWalkers();
    testSuperweapon();
    testBurst();
    fs::remove_all(fs::temp_directory_path() / "uaw_native_test_sim");
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all nav/sim tests passed\n");
    return 0;
}
