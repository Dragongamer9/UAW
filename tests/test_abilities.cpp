// Tests for effects, effect generators, abilities, research and the Lua bytecode VM,
// on synthetic game data.
#include "alamo/combat.hpp"
#include "alamo/effects.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/luavm.hpp"
#include "alamo/research.hpp"
#include "alamo/sim.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
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

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static Map openMap() {
    Map m;
    m.width = m.height = 50;
    m.spacing = 20;
    m.heights.assign(2500, 0.f);
    m.layerIndex.assign(2500, 0);
    m.vertexFlags.assign(2500, 0x01);
    return m;
}

struct Data {
    GameDatabase db;
    EffectLibrary lib;
};

static Data testData(const fs::path& root) {
    fs::remove_all(root);
    write(root / "Data/MegaFiles.xml", "<Mega_Files></Mega_Files>");
    write(root / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
    write(root / "Data/XML/Units.xml", R"(<Units>
  <X_Units Name="Dummy"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>1000</Tactical_Health>
    <CategoryMask>Small, Organic</CategoryMask></X_Units>
  <X_Units Name="Healer"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <Effect_Generator_Data SubObjectList="Yes">
      <Effect_Generator Name="HealAura"><Starts_Active>true</Starts_Active>
        <Modal_Refresh_Frequency>1.0</Modal_Refresh_Frequency>
        <Effect_Generator_Entry Name="Entry1"><Effect_Name>HealEffect</Effect_Name><Duration>-1</Duration>
          <Target_Gathering_Mask>SelfAE</Target_Gathering_Mask><Area_Effect_Radius>100</Area_Effect_Radius>
          <Target_Filter_C>Is_Friendly_Unit_Damaged</Target_Filter_C>
          <Target_Filter_Categories>Small + Organic</Target_Filter_Categories></Effect_Generator_Entry>
      </Effect_Generator></Effect_Generator_Data></X_Units>
  <X_Units Name="Hexer"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <Unit_Abilities_Data SubObjectList="Yes">
      <Unit_Ability Name="Hex"><Type>EFFECT_GENERATOR</Type><Recharge_Seconds>10</Recharge_Seconds>
        <GUI_Activated_Ability_Name>Hex_Ability</GUI_Activated_Ability_Name>
        <Effect_Generator_Name>HexGen</Effect_Generator_Name><Targeting_Type>ENEMY_OBJECT</Targeting_Type></Unit_Ability>
    </Unit_Abilities_Data>
    <Abilities SubObjectList="Yes">
      <Effect_Generator_Ability Name="Hex_Ability"><Activation_Style>USER_INPUT</Activation_Style>
        <Activation_Max_Range>150</Activation_Max_Range><Effect_Generator_Name>HexGen</Effect_Generator_Name></Effect_Generator_Ability>
    </Abilities>
    <Effect_Generator_Data SubObjectList="Yes">
      <Effect_Generator Name="HexGen"><Should_Deactivate_Manually>false</Should_Deactivate_Manually>
        <Effect_Generator_Entry Name="Entry1"><Effect_Name>HexEffect</Effect_Name><Duration>5</Duration>
          <Target_Gathering_Mask>Target</Target_Gathering_Mask><Target_Filter_C>Is_Enemy</Target_Filter_C></Effect_Generator_Entry>
      </Effect_Generator></Effect_Generator_Data></X_Units>
  <X_Units Name="Charmer"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <Unit_Abilities_Data SubObjectList="Yes">
      <Unit_Ability Name="Charm"><Type>EFFECT_GENERATOR</Type><Recharge_Seconds>20</Recharge_Seconds>
        <Effect_Generator_Name>CharmGen</Effect_Generator_Name><Targeting_Type>ENEMY_OBJECT</Targeting_Type></Unit_Ability>
    </Unit_Abilities_Data>
    <Effect_Generator_Data SubObjectList="Yes">
      <Effect_Generator Name="CharmGen">
        <Effect_Generator_Entry Name="Entry1"><Effect_Name>CharmEffect</Effect_Name><Duration>3</Duration>
          <Target_Gathering_Mask>Target</Target_Gathering_Mask><Target_Filter_C>Is_Enemy</Target_Filter_C></Effect_Generator_Entry>
      </Effect_Generator></Effect_Generator_Data></X_Units>
  <X_Units Name="Brawler"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>300</Tactical_Health>
    <Abilities SubObjectList="Yes">
      <Generic_Attack_Ability Name="Punch"><Activation_Max_Range>20</Activation_Max_Range>
        <Damage_Amount>25</Damage_Amount><Damage_Type>Damage_Fist</Damage_Type>
        <Applicable_Unit_Categories>Small | Piloted</Applicable_Unit_Categories>
        <Excluded_Unit_Categories>Flying</Excluded_Unit_Categories></Generic_Attack_Ability>
    </Abilities></X_Units>
  <X_Units Name="Squad"><MovementClass>Infantry</MovementClass><Company_Units>Dummy, Dummy,Brawler</Company_Units></X_Units>
  <X_Units Name="Moth_Fire"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>200</Tactical_Health><CategoryMask>Small</CategoryMask>
    <BehaviorType Name="ElementalModeType"><Elemental_Object_Types>Fire, Moth_Fire</Elemental_Object_Types>
      <Elemental_Object_Types>Ice, Moth_Ice</Elemental_Object_Types></BehaviorType></X_Units>
  <X_Units Name="Moth_Ice"><Variant_Of_Existing_Type>Moth_Fire</Variant_Of_Existing_Type><Tactical_Health>300</Tactical_Health>
    <Attributes><DMA_Max>100</DMA_Max></Attributes></X_Units>
  <X_Units Name="Shielded"><MovementClass>Infantry</MovementClass><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>100</Tactical_Health>
    <BehaviorType Name="ShieldBehaviorType"><Shield_Points>100</Shield_Points><Shield_Refresh_Rate>30</Shield_Refresh_Rate></BehaviorType></X_Units>
  <X_Units Name="Jumper"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <Unit_Abilities_Data SubObjectList="Yes">
      <Unit_Ability Name="Jump"><Type>LEAP</Type><Recharge_Seconds>10</Recharge_Seconds><Targeting_Type>ENEMY_OBJECT</Targeting_Type>
        <GUI_Activated_Ability_Name>Jump_Ability</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>
    <Abilities SubObjectList="Yes"><Leap_Activation_Ability Name="Jump_Ability"><Activation_Style>USER_INPUT</Activation_Style>
      <Activation_Max_Range>200</Activation_Max_Range></Leap_Activation_Ability></Abilities>
    <BehaviorType Name="LeapBehaviorType"><Self_Destruct_On_Landing>YES</Self_Destruct_On_Landing>
      <Damage_Applied_On_Self_Destruct>50</Damage_Applied_On_Self_Destruct>
      <Radius_Affected_By_Self_Destruction>30</Radius_Affected_By_Self_Destruction></BehaviorType></X_Units>
  <X_Units Name="Phoenix"><MovementClass>Infantry</MovementClass><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>50</Tactical_Health>
    <BehaviorType Name="DeathFXBehaviorType"><On_Death_Spawn_Object_Type>Brawler</On_Death_Spawn_Object_Type></BehaviorType></X_Units>
  <X_Units Name="Shorty"><MovementClass>Infantry</MovementClass><Tactical_Health>100</Tactical_Health>
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Short</Projectile_Types>
      <Targeting_Max_Attack_Distance>100</Targeting_Max_Attack_Distance></BehaviorType></X_Units>
  <Projectile Name="Proj_Short"><Max_Speed>20</Max_Speed>
    <BehaviorType Name="ProjectileBehaviorType"><Projectile_Damage>3</Projectile_Damage>
      <Projectile_Max_Flight_Distance>80</Projectile_Max_Flight_Distance></BehaviorType></Projectile>
  <X_Units Name="Ghost"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <Effect_Generator_Data SubObjectList="Yes">
      <Effect_Generator Name="CloakGen"><Starts_Active>true</Starts_Active>
        <Effect_Generator_Entry Name="Entry1"><Effect_Name>CloakEffect</Effect_Name><Duration>-1</Duration>
          <Target_Gathering_Mask>Self</Target_Gathering_Mask></Effect_Generator_Entry>
      </Effect_Generator></Effect_Generator_Data></X_Units>
  <X_Units Name="Blinker"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>100</Tactical_Health>
    <Abilities SubObjectList="Yes"><Teleport_Move_Ability Name="Blink"><Activation_Style>USER_INPUT</Activation_Style>
      <Teleport_Pre_Delay_Time>1.0</Teleport_Pre_Delay_Time><Maximum_Teleport_Range>500</Maximum_Teleport_Range>
      <Minimum_Teleport_Range>100</Minimum_Teleport_Range></Teleport_Move_Ability></Abilities></X_Units>
  <X_Units Name="Setter"><MovementClass>Infantry</MovementClass><Tactical_Health>100</Tactical_Health>
    <Fire_Weapon_When_Deployed>Yes</Fire_Weapon_When_Deployed><Fire_Weapon_When_Undeployed>No</Fire_Weapon_When_Undeployed>
    <BehaviorType Name="DeployBehaviorType"><Deployment_Anim_Rate>1.5</Deployment_Anim_Rate></BehaviorType>
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Short</Projectile_Types>
      <Targeting_Max_Attack_Distance>100</Targeting_Max_Attack_Distance></BehaviorType>
    <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability Name="Setup"><Type>DEPLOY</Type></Unit_Ability></Unit_Abilities_Data></X_Units>
  <X_Units Name="Mirror"><MovementClass>Infantry</MovementClass><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>100</Tactical_Health>
    <BehaviorType Name="ShieldBehaviorType"><Shield_Points>100</Shield_Points><Shield_Refresh_Rate>30</Shield_Refresh_Rate></BehaviorType>
    <Abilities SubObjectList="Yes"><Redirect_Projectile_Ability Name="Bounce"><Activation_Style>Take_Damage</Activation_Style>
      <Block_Chance>1.0</Block_Chance><Redirect_If_Shield_Hit_Only>Yes</Redirect_If_Shield_Hit_Only></Redirect_Projectile_Ability></Abilities></X_Units>
  <X_Structures Name="Lab"><MovementClass>Building</MovementClass><Tactical_Health>500</Tactical_Health>
    <Custom_Hard_XExtent>20</Custom_Hard_XExtent>
    <BehaviorType Name="GroundStructureBehaviorType"><Enables_Research>Yes</Enables_Research></BehaviorType></X_Structures>
</Units>)");
    write(root / "Data/XML/Factions.xml", R"(<Factions><Faction Name="Novus"></Faction><Faction Name="Alien"></Faction></Factions>)");
    write(root / "Data/XML/Effects/EffectSystemFiles.xml", "<EffectSystemFiles><File>Test_Effects.xml</File></EffectSystemFiles>");
    write(root / "Data/XML/Effects/Test_Effects.xml", R"(<Effects>
  <Effect Name="HealEffect"><Effect_Class>Healing</Effect_Class><Duration>-1</Duration>
    <Effect_Component Type="HealOverTime"><Amount_Per_Second>30</Amount_Per_Second></Effect_Component>
    <Effect_Component Type="ParticleSystem"><Particle_System_Type>Sparkles</Particle_System_Type></Effect_Component></Effect>
  <Effect Name="HexEffect"><Effect_Class>Burden</Effect_Class><Duration>5</Duration>
    <Effect_Component Type="AttributeModifier"><Attribute>Max_Speed_Mult_Coeff</Attribute><Modifier_Value>-0.5</Modifier_Value></Effect_Component>
    <Effect_Component Type="AttributeModifier"><Attribute>Damage_Taken_Mult_Coeff</Attribute><Modifier_Value>1.0</Modifier_Value></Effect_Component>
    <Effect_Component Type="AttributeModifier"><Attribute>Category_Mask</Attribute><Modifier_Type>Bitwise_Or</Modifier_Type></Effect_Component></Effect>
  <Effect Name="CharmEffect"><Effect_Class>MindControl</Effect_Class><Duration>3</Duration>
    <Effect_Component Type="SwitchFaction"><Use_Source_Faction>true</Use_Source_Faction></Effect_Component></Effect>
  <Effect Name="CloakEffect"><Effect_Class>Cloaking</Effect_Class><Duration>-1</Duration>
    <Effect_Component Type="Cloak"><Break_Mask>Movement | Damage</Break_Mask></Effect_Component></Effect>
  <Effect Name="WardEffect"><Effect_Class>Ward</Effect_Class><Duration>10</Duration>
    <Effect_Component Type="BlockEffect"><Block_Effect_Class>Burden</Block_Effect_Class></Effect_Component></Effect>
</Effects>)");
    MegVfs vfs;
    mountGame(vfs, root);
    Data d;
    d.db.load(vfs);
    d.lib.load(vfs);
    return d;
}

static void testParsing(const Data& d) {
    CHECK(d.lib.size() == 5);
    const EffectDef* hex = d.lib.find("hexeffect");
    CHECK(hex && hex->effectClass == "Burden" && hex->duration == 5 && hex->components.size() == 3);
    if (hex) {
        CHECK(hex->components[0].op == EffectOp::AttributeModifier && hex->components[0].attribute == "MAX_SPEED_MULT_COEFF");
        CHECK(hex->components[0].value == -0.5f);
        CHECK(hex->components[2].attribute.empty()); // bitwise: not summed
        CHECK(hex->hasGameplay());
    }
    const GameObjectType* healer = d.db.find("Healer");
    CHECK(healer != nullptr);
    if (healer) {
        const auto gens = generatorsFor(*healer);
        CHECK(gens.size() == 1 && gens[0].startsActive && gens[0].refreshSeconds == 1.0f);
        CHECK(gens.size() == 1 && gens[0].entries.size() == 1 && gens[0].entries[0].mask == GatherSelfArea &&
              gens[0].entries[0].radius == 100 && gens[0].entries[0].filters.size() == 1);
    }
    const GameObjectType* hexer = d.db.find("Hexer");
    if (hexer) {
        const auto abs = abilitiesFor(*hexer, d.db, generatorsFor(*hexer));
        CHECK(abs.size() == 1 && abs[0].kind == AbilitySpec::Kind::Generator && abs[0].name == "Hex" && abs[0].specialName == "Hex_Ability");
        CHECK(abs.size() == 1 && !abs[0].autonomous && abs[0].needsTarget && abs[0].maxRange == 150 && abs[0].rechargeTicks == 300);
    }
    const GameObjectType* brawler = d.db.find("Brawler");
    if (brawler) {
        const auto abs = abilitiesFor(*brawler, d.db, {});
        CHECK(abs.size() == 1 && abs[0].kind == AbilitySpec::Kind::Attack && abs[0].autonomous && abs[0].damage == 25);
    }
    // A projectile that can't fly the weapon's range shortens it (Grunt shotgun pellets).
    if (const GameObjectType* shorty = d.db.find("Shorty")) {
        const auto ws = weaponsFor(*shorty, d.db);
        CHECK(ws.size() == 1 && ws[0].maxRange == 70);
    }
    const std::vector<std::string> cats = {"SMALL", "ORGANIC"};
    CHECK(matchesCategories(cats, "Small + Organic"));
    CHECK(!matchesCategories(cats, "Small + ~Organic"));
    CHECK(matchesCategories(cats, "Piloted | Organic"));
    CHECK(matchesCategories(cats, ""));
    CHECK(!hasAnyCategory(cats, ""));
    CHECK(hasAnyCategory(cats, "Flying, Small"));
}

static void testAura(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int p = w.addPlayer("Novus", 0);
    w.spawn("Healer", p, {200, 200, 0});
    const uint32_t nearId = w.spawn("Dummy", p, {260, 200, 0});
    const uint32_t farId = w.spawn("Dummy", p, {700, 700, 0});
    w.unit(nearId)->health = 500;
    w.unit(farId)->health = 500;
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    CHECK(w.unit(nearId)->health > 560 && w.unit(nearId)->health < 600); // ~30/s for ~3 s
    CHECK(w.unit(farId)->health == 500);
    CHECK(w.hasEffectClass(*w.unit(nearId), "Healing"));
    // Leaves the aura: the effect goes at the next refresh and healing stops.
    w.unit(nearId)->position = {900, 200, 0};
    for (int i = 0; i < 2 * 30; ++i) w.tick();
    const float h = w.unit(nearId)->health;
    CHECK(!w.hasEffectClass(*w.unit(nearId), "Healing"));
    for (int i = 0; i < 30; ++i) w.tick();
    CHECK(w.unit(nearId)->health == h);
}

static void testCloak(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t ghost = w.spawn("Ghost", a, {200, 200, 0});
    w.tick();
    w.tick();
    w.spawn("Shorty", b, {260, 200, 0}); // arrives once the ghost is cloaked
    for (int i = 0; i < 2 * 30; ++i) w.tick();
    CHECK(w.unit(ghost)->cloaked);
    CHECK(w.unit(ghost)->health == 100); // nobody shoots what they can't see
    CHECK(!w.canSee(b, *w.unit(ghost)) && w.canSee(a, *w.unit(ghost)));
    w.commandMove({ghost}, {230, 260, 0}); // moving breaks it
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    CHECK(w.unit(ghost) && !w.unit(ghost)->cloaked);
    CHECK(w.unit(ghost) && w.unit(ghost)->health < 100);
}

static void testTeleport(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int a = w.addPlayer("Novus", 0);
    const uint32_t b = w.spawn("Blinker", a, {200, 200, 0});
    w.tick();
    CHECK(w.unit(b)->abilities.size() == 1);
    CHECK(w.useAbility(b, 0, 0, {250, 200, 0}) == OrderResult::NotAllowed); // too close
    CHECK(w.useAbility(b, 0, 0, {800, 200, 0}) == OrderResult::NotAllowed); // too far
    CHECK(w.useAbility(b, 0, 0, {500, 300, 0}) == OrderResult::Ok);
    for (int i = 0; i < 20; ++i) w.tick();
    CHECK(w.unit(b)->position[0] < 210); // still charging up
    for (int i = 0; i < 20; ++i) w.tick();
    CHECK(std::fabs(w.unit(b)->position[0] - 500) < 15 && std::fabs(w.unit(b)->position[1] - 300) < 15);
}

static void testDeploy(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t gun = w.spawn("Setter", a, {200, 200, 0});
    const uint32_t victim = w.spawn("Dummy", b, {260, 200, 0});
    CHECK(w.unit(gun)->deployNeed == 30);
    for (int i = 0; i < 20; ++i) w.tick();
    CHECK(w.unit(victim)->health == w.unit(victim)->maxHealth); // still setting up
    for (int i = 0; i < 40; ++i) w.tick();
    CHECK(w.unit(victim)->health < w.unit(victim)->maxHealth);
}

static void testRedirect(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t mirror = w.spawn("Mirror", a, {200, 200, 0});
    const uint32_t shooter = w.spawn("Shorty", b, {260, 200, 0});
    for (int i = 0; i < 10 * 30; ++i) w.tick();
    CHECK(w.unit(mirror) && w.unit(mirror)->health == 100 && w.unit(mirror)->shield == 100); // every shot bounced
    CHECK(!w.unit(shooter) || w.unit(shooter)->health < 100);                             // back at the shooter
}

static void testHexAndCharm(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t hexer = w.spawn("Hexer", a, {200, 200, 0});
    const uint32_t victim = w.spawn("Dummy", b, {300, 200, 0});
    w.tick();
    CHECK(w.unit(hexer)->abilities.size() == 1);
    uint32_t target = 0;
    CHECK(w.abilityTarget(*w.unit(hexer), 0, target) && target == victim);
    CHECK(w.useAbility(hexer, 0, target) == OrderResult::Ok);
    CHECK(w.useAbility(hexer, 0, target) == OrderResult::NotAllowed); // recharging
    for (int i = 0; i < 5; ++i) w.tick();
    CHECK(std::fabs(w.unit(victim)->maxSpeed - 1.0f) < 1e-4f);
    CHECK(std::fabs(w.unit(victim)->damageTaken - 2.0f) < 1e-4f);
    for (int i = 0; i < 6 * 30; ++i) w.tick();
    CHECK(std::fabs(w.unit(victim)->maxSpeed - 2.0f) < 1e-4f && w.unit(victim)->effects.empty());

    // A BlockEffect for the class keeps it off.
    if (const EffectDef* ward = d.lib.find("WardEffect")) w.applyEffect(*w.unit(victim), *ward, nullptr, b, 300);
    w.unit(hexer)->abilities[0].cooldown = 0;
    CHECK(w.useAbility(hexer, 0, victim) == OrderResult::Ok);
    for (int i = 0; i < 5; ++i) w.tick();
    CHECK(!w.hasEffectClass(*w.unit(victim), "Burden"));

    // Mind control: the target changes sides for the effect's duration.
    const uint32_t charmer = w.spawn("Charmer", a, {200, 260, 0});
    const uint32_t other = w.spawn("Dummy", b, {260, 300, 0});
    w.tick();
    CHECK(w.useAbility(charmer, 0, other) == OrderResult::Ok);
    for (int i = 0; i < 5; ++i) w.tick();
    CHECK(w.unit(other)->owner == a);
    for (int i = 0; i < 4 * 30; ++i) w.tick();
    CHECK(w.unit(other)->owner == b);
}

static void testMelee(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t brawler = w.spawn("Brawler", a, {200, 200, 0});
    const uint32_t victim = w.spawn("Dummy", b, {214, 200, 0});
    const uint32_t far = w.spawn("Dummy", b, {400, 200, 0});
    for (int i = 0; i < 2 * 30 + 2; ++i) w.tick();
    CHECK(w.unit(brawler) != nullptr);
    CHECK(w.unit(victim)->health <= 1000 - 50 && w.unit(victim)->health >= 1000 - 75); // a swing per second
    CHECK(w.unit(far)->health == 1000);
}

static void testSquads(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    const int p = w.addPlayer("Novus", 0);
    const uint32_t first = w.spawn("Squad", p, {400, 400, 0});
    CHECK(first != 0 && w.spawned().size() == 3 && w.units().size() == 3);
    CHECK(w.unit(first) && w.unit(first)->type->name == "Dummy");
    CHECK(squadMembers(*d.db.find("Squad"), d.db).size() == 3 && squadMembers(*d.db.find("Dummy"), d.db).empty());
}

static void testModesAndDefences(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t moth = w.spawn("Moth_Fire", a, {200, 200, 0});
    w.unit(moth)->health = 100;
    w.tick();
    CHECK(w.unit(moth)->abilities.size() == 1 && w.dmaMax(*w.unit(moth)) == 0);
    CHECK(w.useAbility(moth, 0) == OrderResult::Ok);
    CHECK(w.unit(moth)->type->name == "Moth_Ice" && std::fabs(w.unit(moth)->health - 150) < 0.01f);
    CHECK(w.dmaMax(*w.unit(moth)) == 100 && w.unit(moth)->abilities[0].cooldown > 0);
    w.unit(moth)->dma = 40;
    w.applyEffect(*w.unit(moth), *d.lib.find("HexEffect"), nullptr, b, 1); // damage taken x2 for a tick
    w.unit(moth)->abilities[0].cooldown = 0;
    CHECK(w.useAbility(moth, 0) == OrderResult::Ok && w.unit(moth)->type->name == "Moth_Fire");
    CHECK(w.unit(moth)->dma == 0 && !w.unit(moth)->effects.empty()); // effects survive the switch

    const uint32_t ice = w.spawn("Moth_Ice", a, {300, 200, 0});
    CHECK(w.unit(ice)->dma == 100);                 // starts with full armor
    const uint32_t shielded = w.spawn("Shielded", a, {400, 200, 0});
    CHECK(w.unit(shielded)->shield == 100);

    // A melee brawler hits both: the armor/shield soak it first.
    const uint32_t brawler = w.spawn("Brawler", b, {300, 214, 0});
    for (int i = 0; i < 2 * 30 + 2; ++i) w.tick();
    CHECK(w.unit(ice)->health == 300 && std::fabs(w.unit(ice)->dma - 25) < 0.01f); // three swings of 25 soaked
    w.unit(brawler)->health = 0;
    w.tick();

    // Shields regenerate.
    const uint32_t s2 = w.spawn("Shielded", a, {500, 500, 0});
    w.unit(s2)->shield = 0;
    for (int i = 0; i < 30; ++i) w.tick();
    CHECK(w.unit(s2)->shield > 28 && w.unit(s2)->shield < 32);
}

static void testDeathSpawn(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    const int p = w.addPlayer("Novus", 0);
    const uint32_t phoenix = w.spawn("Phoenix", p, {300, 300, 0});
    w.unit(phoenix)->health = 0;
    w.tick();
    w.tick();
    bool reborn = false;
    for (const Unit& u : w.units()) reborn = reborn || (u.type->name == "Brawler" && u.owner == p);
    CHECK(!w.unit(phoenix) && reborn);
}

static void testLeap(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t jumper = w.spawn("Jumper", a, {200, 200, 0});
    const uint32_t victim = w.spawn("Dummy", b, {350, 200, 0});
    const uint32_t bystander = w.spawn("Dummy", b, {360, 215, 0});
    w.tick();
    uint32_t target = 0;
    CHECK(w.abilityTarget(*w.unit(jumper), 0, target) && target == victim);
    CHECK(w.useAbility(jumper, 0, victim) == OrderResult::Ok && w.unit(jumper)->leaping);
    for (int i = 0; i < 10; ++i) w.tick();
    CHECK(w.unit(jumper) && w.unit(jumper)->position[0] > 220 && w.unit(jumper)->position[2] > 5); // in the air
    for (int i = 0; i < 60; ++i) w.tick();
    CHECK(!w.unit(jumper));                                   // blew up on landing
    CHECK(w.unit(victim)->health == 950 && w.unit(bystander)->health == 950);
}

static void testResearch(const Data& d) {
    std::vector<ResearchTree> trees(1);
    trees[0].faction = "NOVUS";
    ResearchSuite a1;
    a1.path = "A";
    a1.index = 1;
    a1.cost = 300;
    a1.seconds = 2;
    a1.changes.push_back({ResearchChange::Kind::Generator, "HealAura", "", false});
    a1.changes.push_back({ResearchChange::Kind::Type, "Brawler", "", false});
    ResearchSuite a2 = a1;
    a2.index = 2;
    a2.changes = {{ResearchChange::Kind::UnitAbility, "Hex", "Hexer", true}};
    trees[0].suites = {a1, a2};

    ResearchLocks l = ResearchLocks::initial(trees[0]);
    CHECK(l.generatorLocked("healaura") && l.types.count("BRAWLER") && l.abilities.empty());

    const Map m = openMap();
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    w.setResearchTrees(&trees);
    const int p = w.addPlayer("Novus", 1000);
    CHECK(w.typeLocked(p, *d.db.find("Brawler")));
    CHECK(w.researchSlots(p) == 1);
    w.spawn("Lab", p, {500, 500, 0});
    CHECK(w.researchSlots(p) == 2);
    w.spawn("Healer", p, {200, 200, 0});
    const uint32_t hurt = w.spawn("Dummy", p, {240, 200, 0});
    w.unit(hurt)->health = 500;
    CHECK(w.availableResearch(p).size() == 1);
    CHECK(w.startResearch(p, "A", 2) == OrderResult::NotAllowed);
    CHECK(w.startResearch(p, "A", 1) == OrderResult::Ok && w.player(p)->credits == 700);
    for (int i = 0; i < 30; ++i) w.tick();
    CHECK(w.unit(hurt)->health == 500); // aura still locked
    for (int i = 0; i < 2 * 30; ++i) w.tick();
    CHECK(w.player(p)->researched.count("A1") && !w.typeLocked(p, *d.db.find("Brawler")));
    for (int i = 0; i < 30; ++i) w.tick();
    CHECK(w.unit(hurt)->health > 500);
    CHECK(w.availableResearch(p).size() == 1 && w.availableResearch(p)[0]->index == 2);
    // A2 locks the Hexer's ability.
    const uint32_t hexer = w.spawn("Hexer", p, {300, 300, 0});
    CHECK(w.startResearch(p, "A", 2) == OrderResult::Ok);
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    const auto& spec = w.abilitiesOf(*w.unit(hexer)->type).abilities[0];
    CHECK(w.abilityLocked(*w.unit(hexer), spec));
}

// Hand-assembled Lua 5.1 chunk in the game's format ('p', 4-byte floats, extra header int):
//   x = 2 + 3;  t = {"a", "b"};  s = 0; for i = 1, 4 do s = s + i end
static std::vector<uint8_t> testChunk() {
    std::vector<uint8_t> c = {0x1b, 'L', 'u', 'a', 0x51, 'p', 1, 4, 4, 4, 4, 0};
    auto i32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) c.push_back(uint8_t(v >> (8 * k))); };
    auto str = [&](const char* s) { i32(uint32_t(std::strlen(s) + 1)); for (const char* p = s; *p; ++p) c.push_back(uint8_t(*p)); c.push_back(0); };
    auto num = [&](float f) { c.push_back(3); uint32_t v; std::memcpy(&v, &f, 4); i32(v); };
    auto abc = [](uint32_t op, uint32_t a, uint32_t b, uint32_t cc) { return op | a << 6 | cc << 14 | b << 23; };
    auto abx = [](uint32_t op, uint32_t a, uint32_t bx) { return op | a << 6 | bx << 14; };
    auto asbx = [](uint32_t op, uint32_t a, int sbx) { return op | a << 6 | uint32_t(sbx + 131071) << 14; };
    const std::vector<uint32_t> code = {
        abc(12, 0, 256 + 0, 256 + 1), // ADD R0 K0 K1
        abx(7, 0, 2),                 // SETGLOBAL x
        abc(10, 0, 2, 0),             // NEWTABLE R0
        abx(1, 1, 3),                 // LOADK R1 "a"
        abx(1, 2, 4),                 // LOADK R2 "b"
        abc(34, 0, 2, 1),             // SETLIST R0 2 1
        abx(7, 0, 5),                 // SETGLOBAL t
        abx(1, 0, 6),                 // LOADK R0 0
        abx(1, 1, 7),                 // LOADK R1 1
        abx(1, 2, 8),                 // LOADK R2 4
        abx(1, 3, 7),                 // LOADK R3 1
        asbx(32, 1, 1),               // FORPREP R1 -> FORLOOP
        abc(12, 0, 0, 4),             // ADD R0 R0 R4
        asbx(31, 1, -2),              // FORLOOP R1 -> ADD
        abx(7, 0, 9),                 // SETGLOBAL s
        abc(30, 0, 1, 0),             // RETURN
    };
    i32(0); i32(0); i32(0); i32(0);   // source, linedefined, lastlinedefined, extra
    c.insert(c.end(), {0, 0, 2, 6});  // upvalues, params, vararg, maxstack
    i32(uint32_t(code.size()));
    for (uint32_t ins : code) i32(ins);
    i32(10);
    num(2); num(3);
    c.push_back(4); str("x");
    c.push_back(4); str("a");
    c.push_back(4); str("b");
    c.push_back(4); str("t");
    num(0); num(1); num(4);
    c.push_back(4); str("s");
    i32(0); i32(0); i32(0); i32(0);   // protos, lines, locals, upvalue names
    return c;
}

static void testLua() {
    LuaVM vm;
    std::string err;
    CHECK(vm.run(testChunk(), &err));
    CHECK(vm.global("x").type == LuaValue::Type::Number && vm.global("x").n == 5);
    const LuaValue t = vm.global("t");
    CHECK(t.isTable() && t.t->length() == 2 && t.get(1.0).str() == "a" && t.get(2.0).str() == "b");
    CHECK(vm.global("s").n == 10);
    CHECK(!vm.run({0x1b, 'L', 'u', 'a', 0x50}, &err));

    // Plain source (as mods ship it): literal assignments only.
    LuaVM src;
    src.loadDataAssignments(R"(
        -- comment
        --[[ block
             Branch_X = { broken ]]
        LUA_PREP = true
        function Init()
            Branch_X = {}
            Branch_X =
            {
                Suite1 = { Path = "A", Index = 1, ResearchCost = 1000.5, Enabled = false,
                           UnlocksTypes = { [1] = { ObjectType = Find_Object_Type('Hero'), Note = [[long]] }, },
                           Weird = 2 * 3, Neg = -4, },
                "positional"; 7
            }
            local y = x + 1
        end)");
    const LuaValue b = src.global("Branch_X");
    CHECK(src.global("LUA_PREP").truthy());
    CHECK(b.isTable() && b.get("Suite1").get("Path").str() == "A" && b.get("Suite1").get("ResearchCost").n == 1000.5);
    CHECK(b.get("Suite1").get("UnlocksTypes").get(1.0).get("ObjectType").str() == "Hero");
    CHECK(b.get("Suite1").get("UnlocksTypes").get(1.0).get("Note").str() == "long");
    CHECK(b.get("Suite1").get("Weird").isNil() && b.get("Suite1").get("Neg").n == -4);
    CHECK(b.get(1.0).str() == "positional" && b.get(2.0).n == 7);
    CHECK(src.global("y").isNil());
}

int main() {
    const fs::path root = fs::temp_directory_path() / "uaw_native_test_abilities";
    const Data d = testData(root);
    testParsing(d);
    testAura(d);
    testCloak(d);
    testTeleport(d);
    testRedirect(d);
    testDeploy(d);
    testHexAndCharm(d);
    testMelee(d);
    testSquads(d);
    testModesAndDefences(d);
    testLeap(d);
    testDeathSpawn(d);
    testResearch(d);
    testLua();
    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all effect/ability/research/lua tests passed\n");
    return 0;
}
