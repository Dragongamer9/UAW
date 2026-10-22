// Effects, effect generators and unit abilities.
//
// The original game builds nearly every special ability out of three pieces of data:
//
//  * Effects (Data/XML/Effects/*.xml, listed in EffectSystemFiles.xml): a class name, a
//    duration and a list of typed components, e.g.
//      <Effect Name="SomeSlow"><Effect_Class>Burden</Effect_Class><Duration>5</Duration>
//        <Effect_Component Type="AttributeModifier"><Attribute>Max_Speed_Mult_Coeff</Attribute>
//          <Modifier_Value>-0.5</Modifier_Value></Effect_Component></Effect>
//  * Effect generators, defined on a unit (Effect_Generator_Data): entries that gather
//    targets (Self, Target, an area around either...), filter them and apply an effect.
//    Some start active (auras, passives), others are switched on by an ability.
//  * Abilities: Unit_Abilities_Data.Unit_Ability (the button: type, recharge, generator)
//    and the Abilities block (Generic_Attack_Ability, Effect_Generator_Ability...) that
//    does the work. Abilities with Activation_Style SPECIAL_ATTACK fire on their own.
//
// The component types and attribute names are the game's own; their semantics here are
// a reading of the data (coefficients add up, so Max_Speed_Mult_Coeff -0.5 halves speed).
#pragma once

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace alamo {

// Component types the simulation acts on (the rest are visuals and sound: Other).
enum class EffectOp : uint8_t {
    Other, AttributeModifier, Damage, DamageOverTime, Heal, HealOverTime, DisableBehavior, DisableAbility,
    RemoveEffect, BlockEffect, SwitchFaction, ModifyArmor, Destroy, MaxHealth, ApplyEffectOnRemove,
    ApplyGenerator, ActivateGenerator, CreateObject, Transfer,
    Cloak, // Cloak / Mirage: hidden from enemies until Break_Mask events (Attack, Damage, Movement, ...)
};
EffectOp effectOpFromName(std::string_view type);

struct EffectComponent {
    std::string type;                                         // AttributeModifier, Damage, ...
    EffectOp op = EffectOp::Other;
    std::string attribute;                                    // AttributeModifier: upper-case attribute
    float value = 0;                                          // AttributeModifier: Modifier_Value (0 if not additive)
    std::string modifierAttribute;                            // AttributeModifier: Modifier_Attribute (value read from the unit)
    bool reset = false;                                       // AttributeModifier: Reset_Memoryless_Attribute
    std::vector<std::pair<std::string, std::string>> fields;  // child tag -> trimmed text
    const std::string* get(std::string_view key) const;
    float number(std::string_view key, float fallback = 0) const;
    bool flag(std::string_view key, bool fallback = false) const;
};

struct EffectDef {
    std::string name;
    std::string effectClass;               // Effect_Class (what RemoveEffect/BlockEffect refer to)
    float duration = -1;                   // seconds, -1 = until removed
    bool extendsDuration = true;           // Extends_Existing_Duration: re-applying refreshes
    bool sameSourceStacking = false;       // Allow_Same_Source_Stacking: a source may add copies
    bool sameSourceOnly = false;           // Extends_Same_Source_Duration_Only: other sources add copies
    // Otherwise a target carries one instance, which any source's re-application refreshes.
    std::vector<EffectComponent> components;
    // Components that change the simulation (the rest are visuals and sound).
    bool hasGameplay() const;
};

class EffectLibrary {
public:
    // Every layer's EffectSystemFiles.xml; each listed file is read layer by layer and
    // later definitions replace earlier ones by name.
    void load(MegVfs& vfs);
    void add(EffectDef def);
    // Reads <Effect> elements from one file's text.
    void parse(std::string_view xml);
    const EffectDef* find(std::string_view name) const;
    size_t size() const { return effects_.size(); }
    template <class F> void forEach(F&& f) const {
        for (const auto& [k, v] : effects_) f(*v);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<EffectDef>> effects_; // upper-case name
};

// Target_Gathering_Mask bits.
enum GatherMask : uint32_t {
    GatherSelf = 1,          // Self
    GatherTarget = 2,        // Target (the ability's target)
    GatherSelfArea = 4,      // SelfAE: everything within Area_Effect_Radius of the source
    GatherTargetArea = 8,    // TargetAE: ... of the target
    GatherParent = 16,       // SelfParent / SelfHPParent / TargetHPParent
    GatherUnsupported = 32,  // GlobalRegion, Rectangle, RadarFilters, ...
};

struct GeneratorEntry {
    std::string effect;                 // Effect_Name
    std::string effectUpper;
    float duration = -2;                // seconds; -1 = while the generator runs; -2 = the effect's own
    std::string durationAttribute;      // Duration_Attribute (looked up on the source type)
    float baseDelay = 0;                // Base_Delay, seconds after activation
    uint32_t mask = GatherSelf;
    float radius = 0;                   // Area_Effect_Radius
    std::vector<std::string> filters;   // Target_Filter_C names (all must pass)
    std::string categories;             // Target_Filter_Categories, e.g. "~Stationary"
    int maxTargets = 0;                 // 0 = no limit
    std::string disableAttribute;       // Disable_Entry_Attribute: skipped while non-zero on the source (upper case)
    float probability = 1;              // Target_Application_Probability
};

struct GeneratorDef {
    std::string name;
    std::string nameUpper;
    bool startsActive = false;          // passive / aura
    bool manualDeactivate = false;      // Should_Deactivate_Manually: runs until switched off
    float refreshSeconds = 0;           // Modal_Refresh_Frequency: re-gather this often (0 = once)
    float startDelay = 0;               // Self_Start_Delay
    std::vector<std::string> signals;   // Application_Signals: apply on events instead of continuously
    std::string disableAttribute;       // Disable_Generator_Attribute (upper case)
    std::vector<GeneratorEntry> entries;
};

// What an ability does when used.
struct AbilitySpec {
    enum class Kind : uint8_t {
        Generator,      // switch on an effect generator (on a target, or around the unit)
        Attack,         // Generic_Attack_Ability: direct damage (melee) or a projectile
        Unsupported,    // listed, not simulated yet (garrisons, retreat, clones, ...)
        Switch,         // SWITCH_TYPE / Masari elemental mode: become another type
        Leap,           // LEAP: jump to a target or location (LeapBehaviorType)
        SelfDestruct,   // SELF_DESTRUCT: blow up, damaging what is around
        Weapon,         // Radiation_Weapon_Ability: a beam, simulated as the unit's weapon (combat.cpp)
        SpawnClones,    // SPAWN_CLONES: temporary copies appear around the unit (SpawnClonesBehaviorType)
        Burst,          // point-blank area attack over time: projectile sprays, the Dervish spin
    };
    Kind kind = Kind::Unsupported;
    std::string name;                   // Unit_Ability name, or the Abilities entry name
    std::string specialName;            // the Abilities entry doing the work (research locks use both)
    std::string abilityType;            // Unit_Ability Type or Abilities element (for listings)
    bool autonomous = false;            // fires by itself (SPECIAL_ATTACK); otherwise ordered
    bool needsTarget = false;           // aimed at an object (Targeting_Type ..._OBJECT / Target mask)
    bool targetsAllies = false;         // heals, buffs (Can_Target_Allies / Is_Ally filters)
    float minRange = 0, maxRange = 0;   // Activation_Min/Max_Range; 0 max = no limit (self/area)
    int rechargeTicks = 0;
    int durationTicks = 0;              // Expiration_Seconds: how long a toggled generator runs
    std::string generator;              // Effect_Generator_Name
    // Attack.
    float damage = 0, damagePercent = 0;
    std::string damageType;
    const GameObjectType* projectile = nullptr;
    int projectileCount = 1;
    std::string applicable, excluded;   // Applicable_/Excluded_Unit_Categories
    // Switch: the two types toggled between (SWITCH_TYPE Active/Inactive_Type_Class), or the
    // elemental modes (Elemental_Object_Types), cycled in order.
    std::vector<std::string> switchTypes;
    bool elemental = false;
    // Leap / self-destruct (the unit's LeapBehaviorType).
    std::string landingGenerator;       // On_Landing_Effect_Generator_Name
    bool selfDestructOnLanding = false;
    float leapHeight = 60;              // Leap_Max_Height
    float selfDestructDamage = 0;       // Damage_Applied_On_Self_Destruct
    float selfDestructRadius = 0;       // Radius_Affected_By_Self_Destruction
    std::string selfDestructDamageType;
    // AI hints (Unit_Ability_AI_*).
    float aiRange = 0;
    float aiHealthBelow = 0;            // only when the user is below this health fraction
    std::string aiCategories;
    bool aiNoAutofire = false;
    bool needsAmmoRoom = false;         // Check_Ammo_Full: only while the unit's ammo isn't full
    // SpawnClones.
    std::string cloneType;
    int cloneCount = 0;
    float cloneLifetime = 15;           // seconds
    // Burst: pulses around the unit after a delay. Each pulse either fires the projectile at an
    // enemy within the radius (Spray_Projectiles_Ability) or damages every enemy in it
    // (Dervish_Spin_Attack_Ability: burstDamage plus up to burstRandom more).
    float burstRadius = 0;
    int burstPulses = 0, burstInterval = 0, burstDelay = 0; // ticks
    float burstDamage = 0, burstRandom = 0;
};

// Effect generators defined on an object type.
std::vector<GeneratorDef> generatorsFor(const GameObjectType& type);

// Abilities of an object type, autonomous ones first.
std::vector<AbilitySpec> abilitiesFor(const GameObjectType& type, const GameDatabase& db,
                                      const std::vector<GeneratorDef>& generators);

// CategoryMask helpers. Filters combine categories with "|" (or ","), "+" (and) and "~" (not):
// "Small | Piloted", "Small + Organic + ~RadiationHelps". An empty filter passes everything.
std::vector<std::string> categoriesOf(const GameObjectType& type);
bool matchesCategories(const std::vector<std::string>& unitCategories, std::string_view filter);
bool hasAnyCategory(const std::vector<std::string>& unitCategories, std::string_view list);

} // namespace alamo
