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
  <X_Units Name="Switcher"><MovementClass>Infantry</MovementClass><Max_Speed>2.0</Max_Speed>
    <Custom_Hard_XExtent>5</Custom_Hard_XExtent><Tactical_Health>300</Tactical_Health>
    <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability Name="Heavy_Mode"><Type>ATTACK_SUBSTITUTE</Type>
      <Recharge_Seconds>0.1</Recharge_Seconds><GUI_Activated_Ability_Name>Heavy_Punch</GUI_Activated_Ability_Name>
      <Targeting_Type>ENEMY_OBJECT</Targeting_Type></Unit_Ability></Unit_Abilities_Data>
    <Abilities SubObjectList="Yes">
      <Generic_Attack_Ability Name="Light_Punch"><Activation_Style>SPECIAL_ATTACK</Activation_Style><Activation_Max_Range>20</Activation_Max_Range>
        <Damage_Amount>10</Damage_Amount></Generic_Attack_Ability>
      <Generic_Attack_Ability Name="Heavy_Punch"><Activation_Style>SPECIAL_ATTACK</Activation_Style><Activation_Max_Range>20</Activation_Max_Range>
        <Damage_Amount>100</Damage_Amount></Generic_Attack_Ability>
    </Abilities></X_Units>
  <X_Units Name="Waller"><MovementClass>Infantry</MovementClass><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>300</Tactical_Health>
    <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability Name="Wall"><Type>BUILDING_PLACEMENT</Type>
      <Recharge_Seconds>20</Recharge_Seconds></Unit_Ability></Unit_Abilities_Data>
    <Abilities SubObjectList="Yes"><Building_Placement_Ability Name="Wall_Place"><Building_Type>Lab</Building_Type>
      <Activation_Max_Range>135</Activation_Max_Range></Building_Placement_Ability></Abilities></X_Units>
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
  <X_Units Name="AAGun"><MovementClass>Infantry</MovementClass><Tactical_Health>100</Tactical_Health>
    <Attack_Category_Restrictions> ~Flying </Attack_Category_Restrictions>
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Short</Projectile_Types>
      <Targeting_Max_Attack_Distance>100</Targeting_Max_Attack_Distance></BehaviorType></X_Units>
  <X_Units Name="Machine"><MovementClass>Infantry</MovementClass><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>1000</Tactical_Health><CategoryMask>Medium</CategoryMask></X_Units>
  <X_Units Name="FooBall"><MovementClass>Flying</MovementClass><Tactical_Health>75</Tactical_Health>
    <Attack_Category_Restrictions> Organic </Attack_Category_Restrictions>
    <BehaviorType Name="FooFighterBehaviorType"><Foo_Fighter_Damage_Type>Damage_Default</Foo_Fighter_Damage_Type>
      <Foo_Fighter_Damage_Amount>0.5</Foo_Fighter_Damage_Amount><Foo_Fighter_Damage_Interval>0.1</Foo_Fighter_Damage_Interval>
      <Foo_Fighter_Heal_Friendly_Amount>0.5</Foo_Fighter_Heal_Friendly_Amount><Foo_Fighter_Heal_Friendly_Interval>0.1</Foo_Fighter_Heal_Friendly_Interval>
    </BehaviorType></X_Units>
  <X_Units Name="Saucer"><MovementClass>Infantry</MovementClass><Custom_Hard_XExtent>5</Custom_Hard_XExtent>
    <Tactical_Health>400</Tactical_Health>
    <BehaviorType Name="FooCoreBehaviorType"><Foo_Core_Fighter_Type_Name>FooBall</Foo_Core_Fighter_Type_Name>
      <Foo_Core_Attack_Range>200</Foo_Core_Attack_Range></BehaviorType></X_Units>
  <X_Units Name="Picky"><MovementClass>Infantry</MovementClass><Tactical_Health>100</Tactical_Health>
    <BehaviorType Name="WeaponBehaviorType"><Projectile_Types>Proj_Short</Projectile_Types>
      <Targeting_Priority_Set>Prefer_Machines</Targeting_Priority_Set>
      <Targeting_Max_Attack_Distance>100</Targeting_Max_Attack_Distance></BehaviorType></X_Units>
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

static void testRestrictionsAndFoo(const Data& d) {
    const Map m = openMap();
    {
        World w(m, d.db);
        w.setEffectLibrary(&d.lib);
        const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
        w.spawn("AAGun", a, {200, 200, 0});
        const uint32_t ground = w.spawn("Dummy", b, {260, 200, 0});
        for (int i = 0; i < 90; ++i) w.tick();
        CHECK(w.unit(ground)->health == w.unit(ground)->maxHealth); // an anti-air gun leaves ground units alone
    }
    {
        World w(m, d.db);
        w.setEffectLibrary(&d.lib);
        const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
        const uint32_t saucer = w.spawn("Saucer", a, {200, 200, 0});
        CHECK(w.unit(saucer)->weapons.size() == 1 && w.unit(saucer)->weapons[0].spec.fooFighters == 3);
        const uint32_t man = w.spawn("Dummy", b, {300, 200, 0});    // organic: the fighters won't
        const uint32_t tank = w.spawn("Machine", b, {300, 260, 0});
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.unit(man)->health == w.unit(man)->maxHealth);
        CHECK(std::fabs(w.unit(tank)->health - (1000 - 15)) < 2.5f); // 3 fighters x 0.5 x 10 a second
        // Heal mode: they mend a hurt friendly machine instead.
        w.unit(saucer)->fooHeal = true;
        const uint32_t mine = w.spawn("Machine", a, {200, 260, 0});
        w.unit(mine)->health = 500;
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.unit(mine)->health > 510);
    }
}

static void testPriorities(const Data& d) {
    const Map m = openMap();
    TargetPriorities pri;
    TargetPrioritySet set;
    set.priorities = {{"MEDIUM", 1}, {"SMALL", 3}};
    set.typeExclusions = {"AAGUN"};
    pri.set("Prefer_Machines", set);
    World w(m, d.db);
    w.setEffectLibrary(&d.lib);
    w.setTargetPriorities(&pri);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t picky = w.spawn("Picky", a, {200, 200, 0});
    const uint32_t man = w.spawn("Dummy", b, {240, 200, 0});     // nearer, but Small
    const uint32_t tank = w.spawn("Machine", b, {200, 255, 0});  // Medium: preferred
    w.tick();
    CHECK(w.targetPriority(*w.unit(picky), *w.unit(tank)) == 1 && w.targetPriority(*w.unit(picky), *w.unit(man)) == 3);
    CHECK(w.unit(picky)->target == tank);
    const uint32_t gun = w.spawn("AAGun", b, {200, 190, 0});      // excluded: not picked on its own
    w.unit(tank)->health = 0;
    for (int i = 0; i < 20; ++i) w.tick();
    CHECK(w.unit(picky)->target == man);
    CHECK(w.targetPriority(*w.unit(picky), *w.unit(gun)) == -1);
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

static void testSubstitute(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    const int a = w.addPlayer("Novus", 0), b = w.addPlayer("Alien", 0);
    const uint32_t sw = w.spawn("Switcher", a, {200, 200, 0});
    const uint32_t victim = w.spawn("Dummy", b, {214, 200, 0});
    w.tick();
    size_t mode = 99;
    const World::TypeAbilities& ta = w.abilitiesOf(*w.unit(sw)->type);
    for (size_t i = 0; i < w.unit(sw)->abilities.size(); ++i)
        if (ta.abilities[w.unit(sw)->abilities[i].index].substitute) mode = i;
    CHECK(mode != 99);
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    const float light = 1000 - w.unit(victim)->health;
    CHECK(light > 0 && light <= 40); // light punches only
    CHECK(w.useAbility(sw, mode, 0) == OrderResult::Ok);
    for (int i = 0; i < 3 * 30; ++i) w.tick();
    CHECK(1000 - w.unit(victim)->health - light >= 200); // heavy punches while the mode is on
}

static void testPlacement(const Data& d) {
    const Map m = openMap();
    World w(m, d.db);
    const int a = w.addPlayer("Novus", 0);
    const uint32_t k = w.spawn("Waller", a, {200, 200, 0});
    w.tick();
    CHECK(w.unit(k)->abilities.size() == 1);
    CHECK(w.useAbility(k, 0, 0, {500, 200, 0}) == OrderResult::NotAllowed); // too far
    CHECK(w.useAbility(k, 0, 0, {300, 200, 0}) == OrderResult::Ok);
    w.tick();
    int labs = 0;
    for (const Unit& u : w.units()) labs += u.type->name == "Lab" && u.owner == a;
    CHECK(labs == 1);
    CHECK(w.useAbility(k, 0, 0, {200, 300, 0}) == OrderResult::NotAllowed); // recharging
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

// Metatables:  d = {hp = 7}; mt = {__index = d, __newindex = log, __add = addf}; log = {}
//   o = setmetatable({}, mt); v = o.hp; o.x = 5; r = o + 1
static std::vector<uint8_t> metaChunk() {
    std::vector<uint8_t> c = {0x1b, 'L', 'u', 'a', 0x51, 'p', 1, 4, 4, 4, 4, 0};
    auto i32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) c.push_back(uint8_t(v >> (8 * k))); };
    auto str = [&](const char* s) { c.push_back(4); i32(uint32_t(std::strlen(s) + 1)); for (const char* p = s; *p; ++p) c.push_back(uint8_t(*p)); c.push_back(0); };
    auto num = [&](float f) { c.push_back(3); uint32_t v; std::memcpy(&v, &f, 4); i32(v); };
    auto abc = [](uint32_t op, uint32_t a, uint32_t b, uint32_t cc) { return op | a << 6 | cc << 14 | b << 23; };
    auto abx = [](uint32_t op, uint32_t a, uint32_t bx) { return op | a << 6 | bx << 14; };
    const uint32_t K = 256;
    const std::vector<uint32_t> code = {
        abc(10, 0, 0, 0),           // NEWTABLE R0            d
        abc(9, 0, K + 1, K + 2),    // SETTABLE R0 "hp" 7
        abc(10, 1, 0, 0),           // NEWTABLE R1            mt
        abc(9, 1, K + 3, 0),        // SETTABLE R1 "__index" R0
        abc(10, 2, 0, 0),           // NEWTABLE R2            log
        abc(9, 1, K + 5, 2),        // SETTABLE R1 "__newindex" R2
        abx(5, 3, 11),              // GETGLOBAL R3 addf
        abc(9, 1, K + 10, 3),       // SETTABLE R1 "__add" R3
        abx(7, 2, 8),               // SETGLOBAL log
        abx(5, 3, 0),               // GETGLOBAL R3 setmetatable
        abc(10, 4, 0, 0),           // NEWTABLE R4
        abc(0, 5, 1, 0),            // MOVE R5 R1
        abc(28, 3, 3, 2),           // CALL R3(R4, R5) -> R3
        abx(7, 3, 9),               // SETGLOBAL o
        abc(6, 4, 3, K + 1),        // GETTABLE R4 o "hp"
        abx(7, 4, 4),               // SETGLOBAL v
        abc(9, 3, K + 6, K + 7),    // SETTABLE o "x" 5
        abc(12, 4, 3, K + 12),      // ADD R4 o 1
        abx(7, 4, 13),              // SETGLOBAL r
        abc(30, 0, 1, 0),           // RETURN
    };
    i32(0); i32(0); i32(0); i32(0);   // source, linedefined, lastlinedefined, extra
    c.insert(c.end(), {0, 0, 2, 6});  // upvalues, params, vararg, maxstack
    i32(uint32_t(code.size()));
    for (uint32_t ins : code) i32(ins);
    i32(14);
    str("setmetatable"); str("hp"); num(7); str("__index"); str("v"); str("__newindex"); str("x"); num(5);
    str("log"); str("o"); str("__add"); str("addf"); num(1); str("r");
    i32(0); i32(0); i32(0); i32(0);   // protos, lines, locals, upvalue names
    return c;
}

// Coroutines: a body (here a native function) that yields twice, then returns.
static void testLuaCoroutines() {
    LuaVM vm;
    const LuaValue co = vm.global("coroutine");
    const LuaValue yield = co.get("yield");
    std::vector<double> seen;
    const LuaValue body = LuaValue::function([&](const std::vector<LuaValue>& a) {
        seen.push_back(a.empty() ? -1 : a[0].n);                       // 1: resume's first arguments
        std::vector<LuaValue> r = vm.call(yield, {LuaValue::number(10)});
        seen.push_back(r.empty() ? -1 : r[0].n);                       // 2: what the second resume passed
        r = vm.call(yield, {LuaValue::number(20), LuaValue::number(21)});
        seen.push_back(r.empty() ? -1 : r[0].n);
        return std::vector<LuaValue>{LuaValue::string("done")};
    });
    const LuaValue c = vm.call(co.get("create"), {body})[0];
    CHECK(c.type == LuaValue::Type::Thread);
    CHECK(vm.call(co.get("status"), {c})[0].str() == "suspended");
    auto r = vm.call(co.get("resume"), {c, LuaValue::number(1)});
    CHECK(r.size() == 2 && r[0].truthy() && r[1].n == 10);
    CHECK(vm.call(co.get("status"), {c})[0].str() == "suspended");
    r = vm.call(co.get("resume"), {c, LuaValue::number(2)});
    CHECK(r.size() == 3 && r[1].n == 20 && r[2].n == 21);
    r = vm.call(co.get("resume"), {c, LuaValue::number(3)});
    CHECK(r.size() == 2 && r[0].truthy() && r[1].str() == "done");
    CHECK(vm.call(co.get("status"), {c})[0].str() == "dead");
    r = vm.call(co.get("resume"), {c});
    CHECK(!r.empty() && !r[0].truthy()); // dead
    CHECK(seen.size() == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3);
    // wrap, and a coroutine dropped while suspended (its thread is woken and unwinds).
    const LuaValue w = vm.call(co.get("wrap"), {body})[0];
    seen.clear();
    r = vm.call(w, {LuaValue::number(5)});
    CHECK(r.size() == 1 && r[0].n == 10 && seen.size() == 1 && seen[0] == 5);
    CHECK(vm.call(yield, {LuaValue::number(1)}).empty()); // outside any coroutine: nothing happens
}

static void testLuaMeta() {
    LuaVM vm;
    vm.setFunction("addf", [](const std::vector<LuaValue>& a) {
        CHECK(a.size() == 2 && a[0].isTable() && a[1].n == 1);
        return std::vector<LuaValue>{LuaValue::number(42)};
    });
    std::string err;
    CHECK(vm.run(metaChunk(), &err));
    CHECK(vm.global("v").n == 7);                                       // __index table
    CHECK(vm.global("o").isTable() && vm.global("o").t->get(LuaValue::string("x")).isNil()); // __newindex
    CHECK(vm.global("log").get("x").n == 5);
    CHECK(vm.global("r").n == 42);                                      // __add
    const auto mt = vm.call(vm.global("getmetatable"), {vm.global("o")});
    CHECK(!mt.empty() && mt[0].isTable() && mt[0].get("__index").get("hp").n == 7);
}


// Lua source compiled by compileLua: every construct the mods' scripts use, checked by the script
// itself (each `ok(cond, name)` must pass).
static void testLuaSource() {
    LuaVM vm;
    std::vector<std::string> failed;
    int passed = 0;
    vm.setFunction("ok", [&](const std::vector<LuaValue>& a) {
        if (!a.empty() && a[0].truthy()) ++passed;
        else failed.push_back(a.size() > 1 ? a[1].str() : "?");
        return std::vector<LuaValue>{};
    });
    const char* src = R"LUA(
-- comments, [[long]] strings and numbers
--[[ a long
comment ]]
local a, b, c = 1, 2.5e1, 0x10
ok(a + b + c == 42, "numbers")
ok([[x
y]] == "x\ny" and [==[a]]b]==] == "a]]b", "long strings")
ok("tab\t" .. 'q\'' .. "\65" == "tab\tq'A", "escapes")
ok(2 ^ 3 ^ 2 == 512 and -2 ^ 2 == -4 and 7 % 3 == 1 and 1 + 2 * 3 == 7, "precedence")
ok(not nil and not false and (nil or 5) == 5 and (false and 1) == false and (1 and 2) == 2, "and/or")
ok(1 < 2 and 2 <= 2 and 3 > 2 and 3 >= 3 and 1 ~= 2 and "a" < "b", "comparisons")
local s = "x" .. 1 .. "y"
ok(s == "x1y" and #s == 3, "concat and length")
-- tables
local t = {10, 20, 30, x = 1, ["y z"] = 2, [5] = 50; 40}
ok(#t == 5 and t[4] == 40 and t.x == 1 and t["y z"] == 2 and t[5] == 50, "constructor")
t.deep = {inner = {v = 7}}
ok(t.deep.inner.v == 7, "nested index")
local big = {}
for i = 1, 120 do big[#big + 1] = i end
local list = {unpack(big)}
ok(#list == 120 and list[120] == 120, "setlist with many items")
-- control flow
local sum = 0
for i = 10, 1, -2 do sum = sum + i end
ok(sum == 30, "numeric for down")
local n = 0
while true do n = n + 1; if n >= 5 then break end end
ok(n == 5, "while/break")
repeat local k = n; n = n - 1 until k <= 2
ok(n == 1, "repeat sees body locals")
local keys = 0
for k, v in pairs({a = 1, b = 2, c = 3}) do keys = keys + v end
ok(keys == 6, "pairs")
local idx = 0
for i, v in ipairs({5, 6, 7}) do idx = idx + i * v end
ok(idx == 5 + 12 + 21, "ipairs")
local grade
if n > 5 then grade = "high" elseif n > 0 then grade = "low" else grade = "none" end
ok(grade == "low", "elseif")
-- functions, varargs, multiple results
local function fact(x) if x <= 1 then return 1 end return x * fact(x - 1) end
ok(fact(5) == 120, "recursive local function")
local function three() return 1, 2, 3 end
local p, q, r, z = three()
ok(p == 1 and q == 2 and r == 3 and z == nil, "multiple results")
ok(select("#", three()) == 3 and select("#", (three())) == 1, "parenthesized call keeps one")
local function count(...) return select("#", ...) end
ok(count() == 0 and count(nil, nil) == 2 and count(three()) == 3 and count(three(), 9) == 2, "varargs")
local function pack(...) return {...} end
ok(#pack(4, 5, 6) == 3, "vararg table")
local x, y = 1, 2
x, y = y, x
ok(x == 2 and y == 1, "swap")
-- closures: each iteration has its own variable
local fns = {}
for i = 1, 3 do fns[i] = function() return i end end
ok(fns[1]() == 1 and fns[3]() == 3, "closures per iteration")
local function counter()
  local c = 0
  return function() c = c + 1; return c end
end
local c1, c2 = counter(), counter()
c1(); c1()
ok(c1() == 3 and c2() == 1, "upvalues")
local function outer() local v = 1; local function mid() return function() v = v + 1; return v end end; return mid()() end
ok(outer() == 2, "upvalue of upvalue")
-- methods and metatables
Account = {balance = 0}
Account.__index = Account
function Account.new(b) return setmetatable({balance = b}, Account) end
function Account:deposit(v) self.balance = self.balance + v; return self end
local acc = Account.new(10):deposit(5)
ok(acc.balance == 15 and getmetatable(acc) == Account, "methods")
local V = {}
V.__add = function(l, r) return l.v + r.v end
local v1, v2 = setmetatable({v = 1}, V), setmetatable({v = 2}, V)
ok(v1 + v2 == 3, "metamethods")
Global_Value = "set"
function Module_Fn(a, b) return a * b end
-- the string, table and math libraries
ok(string.format("%d|%5.2f|%s|%x|%q|%%", 42, 3.14159, "hi", 255, 'a"b') == '42| 3.14|hi|ff|"a\\"b"|%', "format")
ok(("abc"):upper() == "ABC" and string.len("abc") == 3 and ("hello"):sub(2, -2) == "ell" and ("x"):rep(3) == "xxx", "string basics")
ok(string.byte("A") == 65 and string.char(72, 105) == "Hi", "byte/char")
ok(string.find("hello world", "wor") == 7 and string.find("a.b", ".", 1, true) == 2, "find plain")
local s1, e1, cap = string.find("key = value", "(%w+)%s*=")
ok(s1 == 1 and e1 == 5 and cap == "key", "find captures")
ok(string.match("Unit_42_alpha", "_(%d+)_") == "42" and string.match("  trim  ", "^%s*(.-)%s*$") == "trim", "match")
local words = {}
for w in string.gmatch("one two  three", "%a+") do words[#words + 1] = w end
ok(#words == 3 and words[3] == "three", "gmatch")
local g, count = string.gsub("hello world", "o", "0")
ok(g == "hell0 w0rld" and count == 2, "gsub string")
ok(string.gsub("$name is $age", "%$(%w+)", {name = "Ann", age = 7}) == "Ann is 7", "gsub table")
ok(string.gsub("abc", "%w", function(c) return c:upper() .. "." end) == "A.B.C.", "gsub function")
ok(string.match("[tag]", "%b[]") == "[tag]" and string.match("THE (quick) fox", "%f[%a]%a+", 5) == "quick", "balance and frontier")
local arr = {5, 1, 4, 2, 3}
table.sort(arr)
ok(table.concat(arr, ",") == "1,2,3,4,5", "sort and concat")
table.sort(arr, function(x, y) return x > y end)
ok(arr[1] == 5 and table.remove(arr) == 1 and #arr == 4 and table.remove(arr, 1) == 5 and arr[1] == 4, "sort with comparator, remove")
ok(math.abs(-3) == 3 and math.ceil(1.2) == 2 and math.sqrt(16) == 4 and math.floor(math.pi) == 3, "math")
local r = math.random(1, 6)
ok(r >= 1 and r <= 6 and r == math.floor(r), "random")
)LUA";
    std::string err;
    CHECK(vm.runSource(src, &err));
    if (!err.empty()) std::fprintf(stderr, "lua source: %s\n", err.c_str());
    for (const std::string& f : failed) std::fprintf(stderr, "lua source check failed: %s\n", f.c_str());
    CHECK(failed.empty() && passed == 42);
    CHECK(vm.global("Global_Value").str() == "set");
    const auto r = vm.call(vm.global("Module_Fn"), {LuaValue::number(6), LuaValue::number(7)});
    CHECK(r.size() == 1 && r[0].n == 42);
    CHECK(!compileLua("x = = 1", &err) && err.find("line 1") != std::string::npos);
    CHECK(!compileLua("if x then", &err));
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
    testLuaMeta();
    testLuaCoroutines();
    testParsing(d);
    testAura(d);
    testCloak(d);
    testTeleport(d);
    testRedirect(d);
    testDeploy(d);
    testPriorities(d);
    testSubstitute(d);
    testPlacement(d);
    testRestrictionsAndFoo(d);
    testHexAndCharm(d);
    testMelee(d);
    testSquads(d);
    testModesAndDefences(d);
    testLeap(d);
    testDeathSpawn(d);
    testResearch(d);
    testLua();
    testLuaSource();
    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all effect/ability/research/lua tests passed\n");
    return 0;
}
