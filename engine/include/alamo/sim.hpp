// Tactical simulation: units on a map, ticked at a fixed rate.
//
// Movement parameters come straight from the unit XML and are read the way the
// data suggests (to be confirmed against the original game):
//   Max_Speed               world units per logic frame
//   Max_Rate_Of_Turn        degrees per logic frame while moving
//   Stopped_Rate_Of_Turn    degrees per logic frame while turning in place
//   OverrideAcceleration    fraction of Max_Speed gained per frame (default 0.1)
//   Custom_Hard_XExtent     collision radius (fallback: soft footprint radius, then 6)
// GameConstants.xml notes the game is tuned for 30 frames per second.
#pragma once

#include "alamo/combat.hpp"
#include "alamo/effects.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/nav.hpp"
#include "alamo/research.hpp"
#include "alamo/techtree.hpp"
#include "alamo/ted.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <string_view>
#include <memory>
#include <string>
#include <vector>

namespace alamo {

constexpr int kTicksPerSecond = 30;

enum class MoveClass { Infantry, Vehicle, Hover, Walker, LargeWalker, Air, Other };

struct Unit {
    uint32_t id = 0;
    const GameObjectType* type = nullptr;
    int owner = 0;
    Vec3 position{};
    float facing = 0;          // radians, 0 = +X (east), counter-clockwise
    float speed = 0;           // current, units per tick

    // From the XML.
    MoveClass moveClass = MoveClass::Other;
    float maxSpeed = 1;        // units per tick
    float turnRate = 0.5f;     // radians per tick while moving
    float stoppedTurnRate = 0.2f;
    float acceleration = 0.1f; // units per tick per tick
    float radius = 6;
    float health = 100, maxHealth = 100;

    std::string armorType;

    // Orders.
    std::vector<Vec3> path;    // remaining waypoints
    std::vector<Vec3> queuedMoves; // Shift-queued move orders still to come, in order
    Vec3 goal{};
    int stuckTicks = 0;
    uint32_t attackOrder = 0;  // explicit attack target (chase it), 0 = none
    bool attackMove = false;   // heading to `goal` but stopping to fight anything in range
    uint32_t target = 0;       // what the weapons are currently shooting at
    int repathTicks = 0;
    bool moving() const { return !path.empty(); }

    struct WeaponState {
        WeaponSpec spec;
        int cooldown = 0;      // ticks until the next burst may start
        int pulsesLeft = 0;    // remaining pulses in the current burst
        int pulseTimer = 0;
    };
    std::vector<WeaponState> weapons;
    float maxRange() const;          // with range modifiers

    // Hardpoints with their own health (Attached_Hard_Points). When the body itself is
    // invulnerable (Hierarchy walkers: Tactical_Health 999999) damage goes to the leg
    // sockets first, then to the hardpoint whose loss destroys the parent (the core);
    // `health`/`maxHealth` then track what is left of those.
    struct HardPoint {
        const GameObjectType* type = nullptr;
        float health = 0, maxHealth = 0;
        std::string armorType;
        bool leg = false;            // CategoryMask LegSocketHP
        bool destroysParent = false; // HP_Single_Destruction_Destroys_Parent
        bool alive() const { return health > 0; }
    };
    std::vector<HardPoint> hardpoints;
    bool shieldedByHardpoints = false;
    bool alive() const { return health > 0; }

    // Structures and production.
    bool structure = false;            // immobile, blocks the ground under its footprint
    float footprintX = 0, footprintY = 0; // half extents
    int pop = 0;                       // Unit_Pop_Cap
    bool underConstruction = false;
    float buildProgress = 1;           // 0..1 while under construction
    int buildTicks = 0;                // total construction time
    std::vector<const GameObjectType*> queue; // production queue (front is in progress)
    int queueTicks = 0;                // progress on queue.front()
    Vec3 rally{};
    bool hasRally = false;
    uint32_t constructing = 0;         // builders: structure they are working on
    int builderTicks = 0;              // construction ticks that need a builder present (the rest run alone)
    std::vector<const GameObjectType*> upgrades; // finished upgrades/hardpoints bought at this structure
    std::vector<const GameObjectType*> socketsUsed; // walkers: socket hardpoints filled (queued or bought)

    // Economy.
    enum class Harvest : uint8_t { None, ToNode, Gathering, Returning };
    Harvest harvest = Harvest::None;   // collectors and reapers (player orders reset it)
    uint32_t harvestNode = 0;
    uint32_t depot = 0;                // collectors: the depot that launched them
    float carried = 0;
    int harvestTimer = 0;
    int droneTimer = 0;                // depots: progress on the next drone
    bool powered = true;               // Novus structures: on the power network (others always true)
    bool flying() const { return moveClass == MoveClass::Air; }

    // Effects and abilities (abilities.cpp).
    struct ActiveEffect {
        const EffectDef* def = nullptr;
        uint32_t source = 0;           // unit (or effect object) that applied it
        int sourceOwner = -1;
        int remaining = -1;            // ticks left; -1 = while its generator keeps it (genKey)
        uint64_t genKey = 0;           // generator entry that keeps a persistent effect alive
        uint64_t stamp = 0;            // tick it was last (re)applied by that generator
        float healthBonus = 0;         // MaxHealth component: what it added
        int ownerBefore = -1;          // SwitchFaction: owner to return to
        std::vector<uint32_t> created; // CreateObject with Destroy_On_Remove
    };
    std::vector<ActiveEffect> effects;
    struct GeneratorState {
        uint16_t index = 0;            // into the type's generators
        bool active = false;
        uint32_t target = 0;           // ability target (object), if any
        Vec3 point{};                  // ...or ground point
        int age = 0;                   // ticks since activation
        int life = -1;                 // ticks left; -1 = until switched off
        bool signalled = false;        // an Application_Signals event this tick
    };
    std::vector<GeneratorState> generators;
    struct AbilityState {
        uint16_t index = 0;            // into the type's abilities
        float cooldown = 0;            // ticks until ready
    };
    std::vector<AbilityState> abilities;
    // Summed AttributeModifier values from active effects (upper-case attribute name).
    std::unordered_map<std::string, float> attributes;
    float attribute(std::string_view name) const;
    float attributeUpper(const std::string& upperName) const; // base (type's <Attributes>) + modifiers
    float baseMaxSpeed = 1;
    float damageCast = 1, damageTaken = 1; // multipliers
    float fireRate = 1;                // weapon recharge speed multiplier
    float rangeScale = 1, rangeAdd = 0;
    float abilityRate = 1;             // ability recharge speed multiplier
    std::vector<std::pair<std::string, float>> armorMods; // ModifyArmor: damage type -> added modifier
    int substitute = -1;               // index into abilities of the alternate fire mode switched on, or -1
    bool fooHeal = false;
    uint32_t fooTarget = 0;            // who the fighters are mending (heal mode; for drawing)              // Foo Core: fighters heal friends instead of attacking
    int deployNeed = 0, deployed = 0;  // DEPLOY units: ticks of standing still before the weapon fires
    bool immobile = false, disarmed = false, silenced = false; // DisableBehavior / DisableAbility
    bool damagedThisTick = false, firedThisTick = false;
    bool cloaked = false;              // a Cloak/Mirage effect hides it from enemies
    // Transports (GarrisonableBehaviorType): riders are carried off the map until unloaded.
    uint32_t inside = 0;               // the transport carrying this unit
    uint32_t boarding = 0;             // walking to this transport to get in
    std::vector<uint32_t> passengers;  // a transport's riders
    int sellTicks = -1;                // being sold (TacticalSellBehaviorType): ticks left, -1 = no
    bool sold = false;                 // gone by selling (no wreck, no death explosion)
    int originalOwner = -1;
    int lifeTicks = -1;                // effect objects: removed at 0
    int64_t superweaponReadyTick = -1; // superweapon enablers: tick it can fire (-1: not armed yet)
    // Defences that soak damage before health. Shields (ShieldBehaviorType): Shield_Points,
    // regenerating Shield_Refresh_Rate per second, hit with Shield_Armor_Type modifiers.
    // Masari dark-matter armor: up to the DMA_Max attribute, set by Current_DMA_Level effects
    // (regeneration, reset in fire mode, filled on switching to ice).
    float shield = 0, shieldMax = 0, shieldRegen = 0;
    std::string shieldArmor;
    float dma = 0;
    // Energy ammo (Orlok): Energy_Ammo_Capacity cartridges, filled by Transfer effects whose
    // Source_Effect is AmmoUnits (his crush), spent by mode abilities.
    float ammo = 0, ammoMax = 0;
    // Leaping (LEAP abilities): flying from leapFrom to leapTo, then landing effects.
    bool leaping = false;
    Vec3 leapFrom{}, leapTo{};
    int leapTick = 0, leapTicks = 0;
    uint16_t leapAbility = 0;
    uint32_t leapTarget = 0;
    uint32_t actsFor = 0;              // impact objects: the shooter, as the source of their effects
    float weaponRange(const WeaponState& w) const { return w.spec.maxRange * rangeScale + rangeAdd; }
};

// Cost and time to build an object. Structures and walkers keep these on their
// Tactical_Under_Construction_Object_Type (e.g. Novus_Power_Router_Construction);
// units keep them on themselves. Hierarchy glyphs add a carving phase
// (Required_Pre_Build_Time_In_Secs) that needs the Glyph Carver; after that the
// walker finishes on its own.
struct BuildInfo {
    float cost = 0;
    int ticks = 30;
    int builderTicks = 30;             // part of `ticks` that needs a builder present
    bool spawnsUnit = true;            // false for upgrades/patches researched at a structure
};
BuildInfo buildInfo(const GameObjectType& type, const GameDatabase& db);

struct Player {
    int id = 0;
    std::string faction;               // "Novus", "Alien", "Masari", ...
    float credits = 0;
    int popCap = 90;
    float baselineBank = 0;            // Factions.xml Baseline_Bank_Capacity (Masari); 0 = no cap
    float earned = 0;                  // total income so far
    // Research (research.hpp): what is still locked, what is in progress, what is done.
    ResearchLocks locks;
    struct Research { const ResearchSuite* suite = nullptr; int ticksLeft = 0; };
    std::vector<Research> researching;
    std::set<std::string> researched;  // suite ids, e.g. "A1"
    int abilitiesUsed = 0;             // statistics
    int team = -1;                     // players on the same team (>= 0) are allies
    float incomeBonus = 1;             // handicap: all income times this (computer difficulty)
};

// A pile of scrap (or any object with ResourceBehaviorType) placed on the map.
struct ResourceNode {
    uint32_t id = 0;                   // high bit set, so it never collides with unit ids
    const GameObjectType* type = nullptr;
    Vec3 position{};
    float radius = 15;
    float remaining = 0, initial = 0;  // Resource_Units
    std::vector<std::string> factions; // Resource_For_Faction (empty = anyone)
    bool usableBy(const std::string& faction) const;
};

// Economy parameters of one object type, read from its XML (see economy.cpp).
struct EconomySpec {
    // Collector drones (Is_Resource_Collector + Resource_Depot_Type), e.g. Novus_Collector.
    bool collector = false;
    std::string depotType;
    float capacity = 0, perGrab = 0, gatherRange = 80;
    int grabTicks = 30;
    // Reaper-style harvesters (Lua behavior AlienReaperTurret): drain piles in reach directly.
    bool reaper = false;
    float reaperRate = 0;              // resource units per second (AlienHarvestResourcesEffect)
    float reaperSearch = 200, reaperReach = 100;
    // Depots (ResourceDepotBehaviorType): launch and keep `drones` collectors.
    int drones = 0, droneBuildTicks = 90;
    float collectionRadius = 0;
    const GameObjectType* droneType = nullptr;
    // Passive income (Lua behaviors MasariMatterEngine / NeutralRefinery).
    float passivePerSecond = 0;
    std::string passiveFaction;        // MatterEngine.MustBeOfFaction
    bool matterEngine = false;         // counts toward the Masari bank cap
    float storage = 0;                 // MatterEngine.ResourcesStorage
    // Novus power (PoweredBehaviorType): generators (Power Router) and transmitters
    // (Signal Tower) chain within Transmit radius and power receivers within Radius.
    enum class Power : uint8_t { None, Generator, Transmitter, Receiver, SelfPowered };
    Power power = Power::None;
    float powerRadius = 0, transmitRadius = 0;
};

// Engine-side balance knobs on top of the game data, per faction (upper-case name).
// Defaults are built in; Data/XML/UAW_Balance.xml (any layer, later wins) overrides:
//   <UAW_Balance><Faction Name="Novus"><Income_Multiplier>0.5</Income_Multiplier>
//     <Resource_Unit_Value>1</Resource_Unit_Value></Faction></UAW_Balance>
// Income_Multiplier scales every credit a faction earns. Resource_Unit_Value is how many
// credits one unit of a scrap pile is worth to that faction: harvesting earns at the
// usual rate but piles last that many times longer.
struct Balance {
    struct FactionBalance {
        float income = 1;
        float resourceValue = 1;
    };
    std::map<std::string, FactionBalance> factions;
    const FactionBalance& of(const std::string& faction) const;
    static Balance defaults();
};
Balance loadBalance(MegVfs& vfs);

// Reads `Key = value` pairs from a Lua_Script_Data block (keys upper-cased, quotes stripped).
std::unordered_map<std::string, std::string> parseScriptData(std::string_view text);

enum class OrderResult { Ok, NotAllowed, NotEnoughCredits, PopulationCap, BlockedSite, InvalidUnit, BuildLimit };
const char* toString(OrderResult r);

struct Projectile {
    uint32_t id = 0;           // set when it first moves (for visuals that follow it)
    int owner = 0;
    uint32_t shooter = 0;
    uint32_t target = 0;
    Vec3 position{}, velocity{};
    float travelled = 0;
    float distance = 0;        // to the aim point; the shot lands when travelled reaches it
    float damage = 0, blastDamage = 0, blastRange = 0;
    std::string damageType;
    const GameObjectType* type = nullptr;
};

// A player order as given to the World (for saved games and replays: the simulation is
// deterministic, so a game is its setup plus the orders given and when).
struct Command {
    enum class Kind : uint8_t {
        Move, Stop, AttackMove, Attack, Queue, CancelQueue, Rally, Construct, Superweapon, Research, Ability, Board, Unload,
        Sell,
    };
    uint64_t tick = 0;              // given before this tick ran
    Kind kind = Kind::Move;
    std::vector<uint32_t> ids;      // units ordered (or the one structure / builder / user)
    uint32_t target = 0;            // unit targeted
    int owner = -1;                 // Research
    int index = 0;                  // Research suite index, ability slot
    Vec3 point{};
    std::string name;               // type to build or queue, research path
};

// A transport (GarrisonableBehaviorType): how many it carries and who may get in.
struct GarrisonSpec {
    int capacity = 0;                   // Max_Garrison_Capacity
    std::vector<std::string> required;  // Garrisonable_Attributes: riders need one of these attributes
    std::vector<std::string> forbidden; // Garrisonable_Anti_Attributes: riders with these can't
    float deathDamage = 0;              // Damage_Percent_On_Destruction: riders lose this share of health
};

// A tactical superweapon (TacticalSuperweaponBehaviorType). Its enabler structure names it in
// Lua_Script_Data (SWOwnerList[Find_Object_Type("Enabler")]="WEAPON"); firing creates the
// weapon object at the target point, which does the work: effect generators (EMP stun, Masari
// thermal vacuum), an orbital bombardment of projectiles (Hierarchy mass drop, Remix nuke), or
// an object spawned when it ends (the Novus gravity bomb's singularity).
struct SuperweaponSpec {
    const GameObjectType* weapon = nullptr;
    float cooldown = 180, warmUp = 180, radius = 150; // seconds; area of effect
    // ORBITAL_BOMBARDMENT
    std::vector<const GameObjectType*> projectiles;
    int salvos = 0, shotsPerSalvo = 0;
    float shotDelay = 0.5f, salvoDelay = 1, spread = 0;
    // LIGHTNING_BALL_EFFECT's Spawn_Object_On_Death and TornadoBehaviorType lifetime
    const GameObjectType* spawnOnEnd = nullptr;
    float lifetime = 20;              // seconds the weapon object lasts
};

// Where a projectile landed (for impact visuals).
struct ImpactEvent {
    const GameObjectType* type; // the projectile
    Vec3 position;
    bool hitUnit;
    uint64_t tick;
};

struct DeathEvent {
    uint32_t id;
    const GameObjectType* type;
    int owner;
    Vec3 position;
    uint64_t tick;
    bool sold = false; // sold, not destroyed
    float facing = 0;
};

class World {
public:
    // `damage` and `tech` may be null (all modifiers 1.0 / nothing buildable).
    World(const Map& map, const GameDatabase& db, const DamageTable* damage = nullptr, uint64_t seed = 1,
          const TechTree* tech = nullptr);

    // Players are numbered from 0 in the order added; unit `owner` refers to them.
    int addPlayer(const std::string& faction, float credits, int popCap = 90);
    Player* player(int id);
    const std::vector<Player>& players() const { return players_; }
    int popUsed(int owner) const;
    bool owns(int owner, const std::string& typeName) const; // a finished unit/structure of that type
    // Units/structures of the type the player has or is building (for Build_Limit_Current_Per_Player).
    int countOwned(int owner, const GameObjectType& type) const;

    // Production: queue a unit at one of the player's structures (pays up front).
    OrderResult queueProduction(uint32_t structure, const std::string& typeName);
    void cancelProduction(uint32_t structure);   // refunds the last queued item
    std::vector<std::string> producible(uint32_t structure) const;
    void setRally(uint32_t structure, Vec3 p);

    // Construction: the builder walks over and raises the structure (pays up front).
    OrderResult commandConstruct(uint32_t builder, const std::string& typeName, Vec3 site);
    std::vector<std::string> constructible(uint32_t builder) const;
    bool siteClear(const GameObjectType& type, Vec3 site) const;

    // Places a unit; returns its id, or 0 if the type is unknown. A squad type (Company_Units)
    // places its members around the point and returns the first; spawned() lists them all.
    uint32_t spawn(const std::string& typeName, int owner, Vec3 position, float facingRadians = 0);
    const std::vector<uint32_t>& spawned() const { return lastSpawned_; }
    Unit* unit(uint32_t id);
    const std::vector<Unit>& units() const { return units_; }

    // Move a group: they spread into a formation around `target` and each path-finds there.
    // queued (Shift): after the moves already given, instead of replacing them.
    void commandMove(const std::vector<uint32_t>& ids, Vec3 target, bool queued = false);
    void commandStop(const std::vector<uint32_t>& ids);
    // Move toward a point, halting to fight enemies that come into range, then carrying on.
    void commandAttackMove(const std::vector<uint32_t>& ids, Vec3 target);
    // Chase and shoot a specific unit until it dies.
    void commandAttack(const std::vector<uint32_t>& ids, uint32_t target);

    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<DeathEvent>& deaths() const { return deaths_; } // everything that has died
    const std::vector<ImpactEvent>& impacts() const { return impacts_; } // recent projectile landings (the last few thousand)
    bool enemies(int ownerA, int ownerB) const {
        if (ownerA == ownerB) return false;
        if (ownerA < 0 || ownerB < 0 || ownerA >= int(players_.size()) || ownerB >= int(players_.size())) return true;
        const int ta = players_[size_t(ownerA)].team;
        return ta < 0 || ta != players_[size_t(ownerB)].team;
    }
    // Fog of war (off unless switched on): each player sees what its units and its allies' units
    // reveal (Land_FOW_Reveal_Range); what they once saw stays explored. Units only pick targets
    // their player can see. The skirmish AI still knows where everything is.
    void setFogOfWar(bool on) { fog_ = on; fogTick_ = -1; }
    bool fogOfWar() const { return fog_; }
    enum : uint8_t { kUnexplored = 0, kExplored = 1, kVisible = 2 };
    uint8_t visibility(int player, float x, float y) const;
    // A unit as an enemy player sees it: not cloaked, and in sight when there is fog of war.
    bool canSee(int player, const Unit& u) const {
        if (!enemies(player, u.owner)) return true;
        return !u.cloaked && visibleTo(player, u.position);
    }
    bool visibleTo(int player, const Vec3& p) const { return !fog_ || player < 0 || visibility(player, p[0], p[1]) == kVisible; }
    // The player's grid (fogWidth x fogHeight cells of fogCell units, row 0 at y = 0), or nullptr.
    const std::vector<uint8_t>* fogGrid(int player) const {
        return fog_ && player >= 0 && size_t(player) < fogGrids_.size() ? &fogGrids_[size_t(player)] : nullptr;
    }
    int fogWidth() const { return fogW_; }
    int fogHeight() const { return fogH_; }
    float fogCell() const { return fogCell_; }
    float revealRange(const GameObjectType& t) const;
    // Transports.
    const GarrisonSpec* garrisonOf(const GameObjectType& t) const; // nullptr if it carries nobody
    // Redirect_Projectile_Ability (Novus field inverters in shield mode): the chance that a shot
    // hitting the unit is turned back at whoever fired it (0 = none); only while shields are up
    // when Redirect_If_Shield_Hit_Only.
    float redirectChance(const Unit& u) const;
    bool canBoard(const Unit& rider, const Unit& transport) const;
    // Riders walk to the transport and get in (those that can; the rest ignore the order).
    OrderResult commandBoard(const std::vector<uint32_t>& ids, uint32_t transport);
    // Everyone out, around the transport.
    OrderResult unload(uint32_t transport);
    // Structures (and other TACTICAL_SELL objects): taken down over Tactical_Sell_Time_Seconds, then
    // part of the price comes back (1 + Tactical_Sell_Percent_Modifier, 50% by default).
    OrderResult sell(uint32_t id);
    bool canSell(const Unit& u) const;
    float sellRefund(const Unit& u) const;
    // Order recording: while on, every order method appends a Command (orders given from inside
    // another order are not recorded twice). apply() gives a recorded order again.
    void setRecording(bool on) { recording_ = on; }
    // Capture (network games): orders are recorded like setRecording but not carried out; they
    // run when the lockstep turn they were sent for comes (apply()). takeCaptured() empties the log.
    void setCapture(bool on) { capture_ = on; }
    std::vector<Command> takeCaptured() {
        std::vector<Command> out;
        out.swap(log_);
        return out;
    }
    const std::vector<Command>& commandLog() const { return log_; }
    void apply(const Command& c);
    // A checksum of the game state (units, health, positions, credits): equal in two runs that
    // played out the same.
    uint64_t stateHash() const;
    void setTeam(int owner, int team) {
        if (owner >= 0 && owner < int(players_.size())) players_[size_t(owner)].team = team;
    }
    // Skirmish options: a player's income multiplier, and starting credits scaled.
    void setHandicap(int owner, float income, float creditScale = 1) {
        if (owner < 0 || owner >= int(players_.size())) return;
        players_[size_t(owner)].incomeBonus = income;
        players_[size_t(owner)].credits *= creditScale;
    }

    void tick();
    uint64_t tickCount() const { return ticks_; }
    float seconds() const { return float(ticks_) / kTicksPerSecond; }

    const NavGrid& nav() const { return nav_; }
    const GameDatabase& db() const { return db_; }
    const TechTree* tech() const { return tech_; }
    const Map& map() const { return map_; }

    // Unit ids within `radius` of a ground point, nearest first.
    std::vector<uint32_t> unitsNear(Vec3 p, float radius) const;

    // Economy. Resource piles come from the map's Multiplayer objects at construction.
    const std::vector<ResourceNode>& resources() const { return resources_; }
    const ResourceNode* resource(uint32_t id) const;
    uint32_t addResource(const std::string& typeName, Vec3 position); // tests / scripted piles
    const EconomySpec& economy(const GameObjectType& t) const;
    // Effect library for data-driven rates (Reaper harvest); optional.
    void setEffects(const GameDatabase* effects) { effects_ = effects; }
    void setBalance(const Balance& b) { balance_ = b; }
    const Balance& balance() const { return balance_; }
    float bankCap(int owner) const;    // 0 = unlimited
    float incomePerSecond(int owner) const; // passive income only (drones/reapers vary)

    // Effects and abilities. Without a library, abilities and generators do nothing.
    void setEffectLibrary(const EffectLibrary* lib) { effectLib_ = lib; }
    void setTargetPriorities(const TargetPriorities* p) { priorities_ = p; }
    // How much u wants to pick t on its own (Targeting_Priority_Set): lower first; 1000 for an
    // enemy its set doesn't list; -1 when its set excludes t (only attacked when ordered).
    int targetPriority(const Unit& u, const Unit& t) const;
    const EffectLibrary* effectLibrary() const { return effectLib_; }
    // Research trees (loadResearchTrees). Each player starts with its faction's locks; without
    // trees nothing is locked and research is unavailable.
    void setResearchTrees(const std::vector<ResearchTree>* trees);
    const ResearchTree* researchTree(int owner) const;
    // Skirmish research needs no facility (the Enables_Research structures are strategic-map
    // buildings): one suite at a time, one more per facility owned, at most kMaxResearchSuites.
    static constexpr int kMaxResearchSuites = 6;
    int researchFacilities(int owner) const; // finished structures with Enables_Research
    int researchSlots(int owner) const;
    // Suites the player could start now (prerequisite done, a slot free, under the cap).
    std::vector<const ResearchSuite*> availableResearch(int owner) const;

    // Superweapons. superweaponOf: the weapon a structure type fires (null if none).
    const SuperweaponSpec* superweaponOf(const GameObjectType& enabler) const;
    // Seconds until `enabler` can fire (0: ready, <0: not a finished superweapon enabler).
    float superweaponCountdown(uint32_t enabler) const;
    OrderResult fireSuperweapon(uint32_t enabler, Vec3 target);
    OrderResult startResearch(int owner, std::string_view path, int index);
    struct TypeAbilities {
        std::vector<GeneratorDef> generators;
        std::vector<AbilitySpec> abilities;
        std::vector<std::string> categories;
        std::string impactGenerator;   // projectiles: Impact_Effect_Generator_Name
    };
    const TypeAbilities& abilitiesOf(const GameObjectType& t) const;
    bool abilityLocked(const Unit& u, const AbilitySpec& s) const;
    bool generatorLocked(int owner, std::string_view name) const;
    bool effectLocked(int owner, std::string_view name) const;
    bool generatorLockedUpper(int owner, const std::string& n) const {
        return owner >= 0 && owner < int(players_.size()) && players_[size_t(owner)].locks.generatorLockedUpper(n);
    }
    bool effectLockedUpper(int owner, const std::string& n) const {
        return owner >= 0 && owner < int(players_.size()) && players_[size_t(owner)].locks.effectLockedUpper(n);
    }
    bool typeLocked(int owner, const GameObjectType& t) const;
    // Uses an ordered ability (index into unit->abilities). `target` is an object id or 0.
    OrderResult useAbility(uint32_t unitId, size_t ability, uint32_t target = 0, Vec3 point = {});
    // A sensible target for the ability right now (0 when it needs none); false when using it
    // would do nothing (no valid target in range, nothing it affects nearby).
    bool abilityTarget(const Unit& u, size_t ability, uint32_t& target) const;
    // Applies an effect to a unit, as if from `source` (null: no source unit).
    void applyEffect(Unit& target, const EffectDef& def, const Unit* source, int sourceOwner, int durationTicks,
                     uint64_t genKey = 0);
    bool hasEffectClass(const Unit& u, std::string_view effectClass) const;
    // Turns a unit into another type in place (mode switches): keeps id, owner, position,
    // orders and effects, scales health to the same fraction; weapons, abilities and
    // generators become the new type's.
    void switchType(Unit& u, const GameObjectType& to);
    float dmaMax(const Unit& u) const;
    // Invisible objects effects create (radiation fields, mines...): they run generators but
    // are not units for targeting, population or victory.
    const std::vector<Unit>& effectObjects() const { return effectObjects_; }

private:
    void moveUnit(Unit& u);
    void separate();
    void updateTargeting(Unit& u);
    void fireWeapons(Unit& u);
    bool fooHealing(Unit& u, Unit::WeaponState& w); // Foo Core in heal mode: fighters mend a friend
public:
    // Whether weapon w may fire at t (Attack_Category_Restrictions: no anti-air guns at ground units...).
    bool canHit(const Unit::WeaponState& w, const Unit& t) const {
        return !w.spec.restrictions || !categoriesAny(*t.type, *w.spec.restrictions);
    }
    bool canHitAny(const Unit& u, const Unit& t) const {
        for (const Unit::WeaponState& w : u.weapons)
            if (canHit(w, t)) return true;
        return false;
    }
private:
    void updateProjectiles();
    void applyDamage(Unit& victim, float amount, const std::string& damageType);
    void removeDead();
    float random01();
    void updateProduction(Unit& u);
    void updateConstruction(Unit& u);
    void rebuildNav();
    Vec3 exitPoint(const Unit& structure, const Unit& unit);
    std::vector<Vec3> planPath(const Unit& u, Vec3 to) const;
    bool isBaseBuilder(const GameObjectType& t) const;
    bool anyBaseBuilder() const;
    void loadMapResources();
    void updateEconomy();
    void updateDepot(Unit& depot);
    void updateCollector(Unit& u, const EconomySpec& e);
    void updateReaper(Unit& u, const EconomySpec& e);
    void deposit(int owner, float amount);
    void updatePower();
    ResourceNode* resourceMut(uint32_t id);
    Unit makeUnit(const GameObjectType* t, int owner, Vec3 position, float facing); // no id, not added
    void initDefenses(Unit& u);        // shields and dark-matter armor
    void initAbilities(Unit& u);
    void updateResearch();
    void updateAbilities();
    void updateEffects(Unit& u);
    void updateGenerators(Unit& u);
    void runGeneratorEntries(Unit& u, Unit::GeneratorState& g, bool force);
    void deactivateGenerator(Unit& u, Unit::GeneratorState& g);
    void purgeGenerator(Unit& u, Unit::GeneratorState& g); // drop the persistent effects it applied
    void activateGenerator(Unit& u, size_t index, uint32_t target, Vec3 point, int life);
    void autonomousAbilities(Unit& u);
    void executeAbility(Unit& u, Unit::AbilityState& a, uint32_t target, Vec3 point);
    void removeEffect(Unit& u, size_t i, bool expired);
    void recomputeModifiers(Unit& u);
    bool filterPasses(std::string_view filter, const Unit& source, int sourceOwner, const Unit& target) const;
    std::vector<Unit*> gather(Unit& source, const GeneratorEntry& e, const Unit::GeneratorState& g);
    Unit* anyUnit(uint32_t id); // units and effect objects
    void projectileImpact(const Projectile& p, const Unit* hit); // Impact_Effect_Generator_Name
    void updateLeap(Unit& u);
    void selfDestruct(Unit& u, const AbilitySpec& s, uint32_t target);
    void deathSpawn(const Unit& u);    // On_Death_Spawn_Object_Type
    // Creates an object an effect or a death spawns: a real unit if it is armed, else an invisible
    // effect object when it has gameplay generators (radiation fields, explosions); visuals are
    // skipped. Returns the effect object's id (0 for units and skipped objects).
    uint32_t createObject(const GameObjectType& t, int owner, Vec3 position, float facing, int lifeTicks);

    const Map& map_;
    bool fog_ = false;
    int64_t fogTick_ = -1;
    float fogCell_ = 32;
    int fogW_ = 0, fogH_ = 0;
    std::vector<std::vector<uint8_t>> fogGrids_;
    mutable std::unordered_map<const GameObjectType*, float> revealRanges_;
    void updateFog();
    const GameDatabase& db_;
    NavGrid nav_;
    const DamageTable* damage_;
    const TargetPriorities* priorities_ = nullptr;
    mutable std::map<std::pair<const void*, const void*>, int> priorityCache_; // (set, target type) -> priority
    mutable std::unordered_map<const GameObjectType*, const TargetPrioritySet*> prioritySetOf_;
    const TechTree* tech_;
    NavGrid baseNav_;                  // terrain only; nav_ adds structure footprints
    bool navDirty_ = false;
    std::vector<Player> players_;
    std::vector<Unit> units_;
    std::vector<Projectile> projectiles_;
    std::vector<ImpactEvent> impacts_;
    uint32_t nextProjectileId_ = 1;
    std::vector<DeathEvent> deaths_;
    std::vector<ResourceNode> resources_;
    const GameDatabase* effects_ = nullptr;
    const EffectLibrary* effectLib_ = nullptr;
    const std::vector<ResearchTree>* trees_ = nullptr;
    std::vector<Unit> effectObjects_;
    struct SuperweaponEvent {
        int64_t tick;
        int owner;
        const GameObjectType* type; // projectile to drop, or object to create
        bool projectile;
        Vec3 point;
    };
    std::vector<SuperweaponEvent> superweaponEvents_;
    mutable std::unordered_map<const GameObjectType*, std::unique_ptr<SuperweaponSpec>> superweapons_;
    void updateSuperweapons();
    // Burst abilities in progress (AbilitySpec::Kind::Burst): one entry per pulse still to come.
    struct BurstPulse {
        int64_t tick;
        uint32_t unit;
        const AbilitySpec* spec;
    };
    std::vector<BurstPulse> bursts_;
    struct PendingTeleport { int64_t tick; uint32_t unit; Vec3 point; };
    std::vector<PendingTeleport> teleports_;
    void updateBursts();
    void updateGarrisons();
    bool recording_ = false, capture_ = false;
    int recordDepth_ = 0;
    std::vector<Command> log_;
    friend struct RecordScope;
    mutable std::unordered_map<const GameObjectType*, std::unique_ptr<GarrisonSpec>> garrisons_;
    mutable std::unordered_map<const GameObjectType*, std::pair<float, bool>> redirects_;
    uint32_t createEffectObject(const GameObjectType& t, int owner, Vec3 position, int lifeTicks);
    std::vector<Unit> pendingObjects_;   // created during a tick, added after it
    struct PendingSpawn { std::string type; int owner; Vec3 position; int lifeTicks = -1; };
    std::vector<PendingSpawn> pendingSpawns_;
    mutable std::unordered_map<const GameObjectType*, TypeAbilities> abilityCache_;
    // Category filter results per (type, filter string, kind); filters live in cached specs.
    struct CatKey {
        const void* type; const void* filter; int kind;
        bool operator==(const CatKey& o) const { return type == o.type && filter == o.filter && kind == o.kind; }
    };
    struct CatKeyHash {
        size_t operator()(const CatKey& k) const {
            return std::hash<const void*>()(k.type) * 31 ^ std::hash<const void*>()(k.filter) * 7 ^ size_t(k.kind);
        }
    };
    mutable std::unordered_map<CatKey, bool, CatKeyHash> catCache_;
    bool categoriesMatch(const GameObjectType& t, const std::string& filter) const; // matchesCategories
    bool categoriesAny(const GameObjectType& t, const std::string& list) const;     // hasAnyCategory
    Balance balance_ = Balance::defaults();
    mutable std::unordered_map<const GameObjectType*, EconomySpec> economyCache_;
    mutable std::unordered_map<const GameObjectType*, BuildInfo> buildCache_;
    const BuildInfo& cachedBuildInfo(const GameObjectType& t) const;
    mutable int anyBaseBuilder_ = -1;
    // Objects placed by any base builder of their faction: no Tactical_Builder and no builder
    // among their requirements (Hierarchy walkers, carved by Glyph Carvers). Built on first use.
    mutable std::vector<std::pair<const GameObjectType*, std::vector<std::string>>> fieldBuilt_;
    mutable bool fieldBuiltReady_ = false;
    // Hardpoint sockets of a type (Hierarchy walkers buy hardpoints into them), with repeats.
    const std::vector<const GameObjectType*>& socketsOf(const GameObjectType& t) const;
    const GameObjectType* socketFor(const Unit& u, const GameObjectType& upgrade) const;
    mutable std::unordered_map<const GameObjectType*, std::vector<const GameObjectType*>> sockets_;
    uint32_t nextId_ = 1;
    std::vector<uint32_t> lastSpawned_;
    uint64_t ticks_ = 0;
    uint64_t rng_;
};

// Everything besides the object database a World needs from the game data: the effect
// library (twice: flattened for the economy's rates, and with components for abilities),
// research trees and balance. Load once, configure every World with it.
struct GameRules {
    GameDatabase effects;
    EffectLibrary effectLibrary;
    std::vector<ResearchTree> research;
    Balance balance = Balance::defaults();
    TargetPriorities priorities;
    void load(MegVfs& vfs);                 // missing parts stay empty
    void configure(World& w) const;
};

// Buildings: MovementClass Building or a *Structure* category.
bool isStructureType(const GameObjectType& t);

// A squad type's members (Company_Units), empty for anything else.
std::vector<const GameObjectType*> squadMembers(const GameObjectType& t, const GameDatabase& db);

float angleDiff(float from, float to); // shortest signed difference, radians

// Skirmish starting forces per faction (upper-case faction name -> object types;
// the first is the command structure placed on the start marker).
using StartingForces = std::map<std::string, std::vector<std::string>>;

// The original game's table from DefaultLandScript.lua:
//   ALIEN:  Alien_Arrival_Site    + Alien_Glyph_Carver
//   NOVUS:  Novus_Remote_Terminal + Novus_Constructor
//   MASARI: Masari_Foundation     + Masari_Architect
StartingForces defaultStartingForces();

// Reads the StartingForces table from Lua source text; returns empty when absent.
StartingForces parseStartingForces(std::string_view lua);

// Defaults, overridden by Data\Scripts\Story\DefaultLandScript.lua when it is plain
// source (mods such as Human Resurgence ship it uncompiled; the base game's is bytecode).
StartingForces loadStartingForces(MegVfs& vfs);

// Adds a player with the faction's Multiplayer_Starting_Credits and pop cap and spawns
// its starting forces at `start`. Returns the player id.
// Records an order (only the outermost one) while World recording is on; used by the order methods.
struct RecordScope {
    World& w;
    bool captured = false; // capture mode: recorded, not to be carried out now
    RecordScope(World& world, Command c) : w(world) {
        if ((w.recording_ || w.capture_) && w.recordDepth_ == 0) {
            c.tick = w.tickCount();
            w.log_.push_back(std::move(c));
            captured = w.capture_;
        }
        ++w.recordDepth_;
    }
    ~RecordScope() { --w.recordDepth_; }
    RecordScope(const RecordScope&) = delete;
    RecordScope& operator=(const RecordScope&) = delete;
};

int addSkirmishPlayer(World& world, const GameDatabase& db, const std::string& faction, Vec3 start,
                      const StartingForces* forces = nullptr);

} // namespace alamo
