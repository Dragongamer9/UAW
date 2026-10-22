# uaw-native

A clean-room, native 64-bit reimplementation of the Alamo engine as used by
**Universe at War: Earth Assault** (Petroglyph, 2007). It loads the original
game data from your own install. No Petroglyph assets are included or
redistributed.

## Status

| Stage | Piece | State |
|---|---|---|
| 1 | MEG v1 archives (+ virtual file system, loose-file overrides) | done, verified on retail Config/Maps/Shaders/Cinematics/Retailers.meg (1,668 files, all CRCs match) |
| 1 | XML game data (`Data/XML`), `MegaFiles.xml` load order | done: all 168 object files, 3,128 objects (1,099 variants), 0 warnings on retail data |
| 1 | String tables (`MasterTextFile_*.dat`) | done: 7,945 English strings |
| 2 | ALO models + DDS/TGA textures, glTF export | done: all 106 mesh models in retail Patch.meg load with 0 warnings (575k triangles); exports checked by rendering |
| 2 | ALA animations, posed export | done: all 107 retail animations (13,348 frames) load with 0 warnings; rigid and skinned poses checked by rendering |
| 2 | TED maps: terrain, texture layers, placed objects, editor groups, start positions | done: all 45 retail maps load (75,186 objects, all but 9 resolved to game object types); overviews checked against the real Giza layout |
| 2 | Real-time 3D viewer (SDL3 + OpenGL 3.3): terrain, placed objects, RTS camera, keyboard/mouse/gamepad | done: textured models from Models.meg/Textures.meg and the mods' loose art; every buildable unit and structure of all four factions has its model (`--lineup`) |
| 3 | Navigation: walkable areas from terrain flags, A* with wall clearance and path smoothing | done: start-to-start paths found on every skirmish map checked |
| 3 | Simulation core: fixed 30 Hz ticks, units with XML speed/turn/acceleration/size, group moves in formation, separation, deterministic | done (movement only) |
| 3 | Viewer: test squads per start, click/box select, right-click move, gamepad reticle controls | done |
| 3 | Combat: weapons from units and hardpoints, bursts/spread/recharge, projectiles with blast areas, `DamageToArmor.xml` (4,664 modifiers), auto-targeting, attack and attack-move orders, death | done (first version) |
| 3 | Economy: tech tree (`ProductionDependencies.xml`), players with credits and pop cap, structures that block ground, construction by builders, production queues, upgrades, skirmish start forces | done (first version) |
| 3 | HUD: game fonts and English strings (`MasterTextFile_ENGLISH.dat`), credits/population/clock, selection info, build menus, production queue, placement ghost, rally points, minimap (click to jump), research panel, ability keys, superweapon countdown, victory/defeat | done |
| 1 | Mods: every mod in `Game\Mods` is layered on top of the base game by default and merged entry by entry (objects, tech tree, factions, damage table, strings, skirmish starts) | done: Community Patch 4 + Human Resurgence 1.02 + Universe at Remix 2.8 load together (3,746 objects, 235 buildable entries) |
| 3 | Military (human) faction in skirmish via the mods: Bunker + Build Bot start, base building | done |
| 3 | Resources: scrap piles from the map, Novus Input Station + collector drones, Novus power network (routers/signal towers), Hierarchy Reapers, Masari Matter Engines with the bank cap, Military Supply / refineries, build limits | done (first version) |
| 3 | Skirmish AI: data-driven build orders for any faction/mod, production, rally, defence, attack waves; deterministic | done (first version) |
| 3 | Hardpoints with health: Hierarchy walkers are killed through their legs and core, weapon hardpoints are lost with them | done (first version) |
| 3 | Effects and abilities: the effect library (773 effects, typed components), effect generators (auras, passives, ordered and auto-cast abilities, projectile impacts, created objects such as radiation fields), melee and special attacks, leaps, self-destruct, Orlok's crush, mode switches, beam weapons, clones, point-blank bursts (Masari elemental modes, Orlok's siege/endure modes, shield modes), death explosions; AI uses abilities and Masari modes | done (first version): 898 of 936 ability entries simulated or handled elsewhere (construction; campaign-only retreat and hostile territory) |
| 3 | Research trees: read by running the compiled tree scripts (or reading a mod's source); per-player locks on units, generators, effects and abilities; research facilities, costs and times; AI researches | done (first version): all four factions' trees, 48 suites |
| 3 | Lua 5.1 bytecode interpreter for the game's compiled scripts (custom header: format byte 0x70, 4-byte float numbers, one extra int per function) | done: runs the research trees; no metatables/coroutines yet |
| 3 | Shields (`ShieldBehaviorType`) and Masari dark-matter armor (`DMA_Max`, driven by the DMA effects) | done (first version); the game has no veterancy |
| 3 | Superweapons (`TacticalSuperweaponBehaviorType`): warm-up and recharge, EMP, gravity bomb, mass drop, magma channel, thermal vacuum, Remix nuke; the AI fires them | done (first version) |
| 2 | Animated units in the viewer: GPU skinning (rigid, skinned and 4-bone weighted meshes), idle/move/attack `.ALA` per unit | done (first version) |
| 2 | Terrain texture splatting: the map's layer textures (tiling, rotation, tint) in a texture array, blended per vertex across each triangle | done |
| 2 | Particle systems (`0x1500` .ALO): emitters with rates, lifetimes, spawn shapes, acceleration, size/colour tables, atlas rectangles, additive/alpha/flat/stretched quads; explosions on impacts and deaths, projectile trails, effects attached to model bones (engine glows, flames) | done (first version): all 1,253 files parse; property meanings inferred from the data, heat-haze emitters skipped |
| 2 | Foliage: alpha-tested grass, leaves and bushes | done |
| 3 | Lua scripting host for behaviours, AI and missions (engine API bindings) | planned |
| 4 | Front end: skirmish setup screen (map, up to 8 players, factions, teams, fog of war), pause menu, game speed, saved games (orders replayed deterministically) | done (first version) |
| 3 | Fog of war: per-player sight from `Land_FOW_Reveal_Range`, shared by allies, explored ground remembered; units only target what their side sees; the viewer darkens unseen ground and hides enemies there (structures once seen stay) | done (first version); the AI still sees everything |
| 4 | Audio | planned: the staged game folder has no audio archives yet |
| 4 | LAN multiplayer: deterministic lockstep over TCP (orders only; 0.1 s turns, 0.2 s delay), host/join lobby on the setup screen, checksums to catch desyncs | done (first version): tested with two games on one machine |

## Mods

The original game loads one mod at a time (`MODPATH`). This engine stacks them: the base game is
the bottom layer and each folder in `Game\Mods` (found next to the game folder or inside it) is a
layer on top, in name order, so the last one wins where two mods change the same thing.

* Loose files and archives are looked up top layer first. A mod's own archives (listed in its
  `Data\MegaFiles.xml` and found in its folder, e.g. `Mod.meg`, `GFWL_Patch.meg`) belong to its layer.
* List files are **merged** instead of replaced: every layer's `GameObjectFiles.xml` contributes its
  files; each object file is loaded layer by layer, so a mod's definition of an object replaces the one
  below it while everything it doesn't touch survives. `ProductionDependencies.xml` is merged per built
  object, `Factions.xml` per faction, `DamageToArmor.xml` per damage/armor pair, string tables per key,
  and the `StartingForces` table of a plain-text `DefaultLandScript.lua` per faction.
* Objects a mod defines twice in its *own* files are reported as warnings (the original keeps the later one too).

All tools and the viewer take `--no-mods` (original game only) or `--mods A,B` (just these, bottom to top;
names or name prefixes under `Game\Mods`, or folders).

With the three mods in the user's install (Community Patch 4, Human Resurgence 1.02, Universe at Remix 2.8)
all four factions are playable: Remix already contains Human Resurgence's Military plus its own additions,
and Community Patch's new units (e.g. `Rosie_Shade`) sit alongside them. Where Community Patch and Remix both
rebalance the same unit, Remix (loaded later) wins.

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

SDL3 (and zlib, where the system has none) are downloaded and built
automatically. Every push builds Windows and Linux binaries on GitHub Actions;
download them from the run's **Artifacts**.

## uaw-viewer

```sh
uaw-viewer "<game>"                          # the skirmish setup screen
uaw-viewer "<game>" M16_Sahara               # straight in: you (Novus) vs a Hierarchy base
uaw-viewer "<game>" M16_Sahara --faction Alien --enemy Masari
uaw-viewer "<game>" M16_Sahara --squads      # mixed test squads at every start instead
uaw-viewer "<game>" M25_Grand_Canyon --player 5
uaw-viewer "<game>" M16_Sahara --screenshot shot.png --size 1920x1080 --camera 2700,1700,2400,15,38
uaw-viewer "<game>" M25_Grand_Canyon --faction Military --enemy Novus   # Military needs a mod (loaded by default)
uaw-viewer "<game>" M25_Grand_Canyon --spawn Novus_Power_Router --spawn Novus_Input_Station  # start with an economy
uaw-viewer "<game>" M16_Sahara --no-mods     # the original game only
uaw-viewer "<game>" M25_Grand_Canyon --slots you:Novus:1,ai:Masari:1,ai:Alien:2,ai:Military:2   # 2 v 2
```

**Setup screen.** Without a map (or double-clicked from the game folder) the viewer opens a skirmish
setup screen: every map with two or more start positions (22 with the base game and mods), a top-down
preview with the numbered starts, and one row per start position: open, you or computer, the faction, and
a team (players on the same team are allies; no team means everyone is an enemy). Click to change a
setting, right click to step back; Enter or *Start game* plays it. In a game, Esc (with nothing to cancel)
opens the pause menu: resume, restart, back to the setup screen, or exit. Fog of war is on unless switched
off on the setup screen (or `--no-fog`): unexplored ground is dark, explored ground you can't see right now is
dimmed, and enemies there are hidden (structures you have seen stay on the map).

**LAN games.** On the setup screen, *Host LAN game* opens a game on TCP port 7470 (allow it through the
firewall); the others click *Join LAN game* and type the host's address (or start with `--join ADDRESS`,
`--name NAME`; `--host` for the host). Joining players take the open seats; the host picks the map, factions,
teams and computer players and starts the game. Every machine runs the whole simulation and only the players'
orders travel: an order runs 0.2-0.3 s after it is given, on every machine at the same tick. Each machine
reports a checksum of the game state every turn, and the host announces it if the games ever differ. All
players need the same game files and mods. Network games run at normal speed and can't be paused or saved.

**Saved games.** F5 (or *Save game* in the Esc menu) writes a quick save to the user's preferences folder
(`%APPDATA%\Dragongamer9\uaw-native\quicksave.uawsave` on Windows); F9, the Esc menu or *Load saved game* on
the setup screen loads it, and `--load FILE` starts from any save. The simulation is deterministic, so a save
is just the setup and the orders you gave with their times: loading plays the game again up to that moment
(a 30-minute game takes a few seconds). A save made with other mods loaded may play out differently. `[` and `]` change the game
speed (x0.5 to x4), Pause stops the clock.

The opponent is played by the skirmish AI (`--no-ai` to switch it off, `--spectate` to let an AI play your
side too). Scrap piles are drawn as gold heaps that shrink as they are mined; collectors show their load as a crate.
The top bar shows credits, the Masari bank cap and passive income per second.

On Windows you can also put `uaw-viewer.exe` in the game folder (the one with
`Data\MegaFiles.xml`) or next to it and double-click it to open the setup screen.

Each start position gets a test squad (player 1 Novus, player 2 Hierarchy,
player 3 Masari: infantry, vehicles and a hero). You command player 1's
squad, or another with `--player N`.

| | Keyboard / mouse | Gamepad (reticle at screen centre) |
|---|---|---|
| Pan | WASD / arrows | left stick |
| Rotate / tilt | Q/E, R/F, right- or middle-drag | right stick |
| Zoom | mouse wheel, +/- | triggers |
| Select | left click, left-drag box, Shift adds, Tab = all yours, double click = that type on screen | A (hold LB to add), X = all on screen, B = clear |
| Control groups | Ctrl+0-9 stores, 0-9 selects (twice: camera to them) | |
| Move / attack | right click (on an enemy = attack) | RB |
| Attack-move | G then right click, or hold A + right click | D-pad up |
| Fire superweapon (enabler selected, ready) | V then right click | |
| Abilities of the selection | F1-F8 (targeted ones: then right click the target) | |
| Research panel | T (click a suite to start it) | |
| Jump the camera | click the minimap | |
| Stop / jump to selection | X / Space | |
| Load into / unload a transport | right click your transport / U | |
| Sell selected structures | Delete twice | |
| Jump to the last attack | Home (an alert also pings the minimap) | |
| Build menu (structure or builder selected) | 1-9 or click | D-pad left/right, D-pad down |
| Place structure / cancel | left click / right click | A / B |
| Cancel last queued item | Backspace | left stick click |
| Rally point (structure selected) | right click | RB |
| Toggle stand-in boxes | P | Y |
| Game speed / pause | `[` `]` / Pause | |
| Quick save / load | F5 / F9 (also in the Esc menu) | |
| Menu (resume, restart, setup, exit) | Esc | Start or Back |

Particle effects can be checked one at a time: `uaw-viewer "<game>" M16_Sahara --no-units --camera 2500,2500,200,0,35
--effect Novus_Large_Explosion_Vent@0.4 --screenshot fx.png` (a Particles.xml name or an .alo file, pictured 0.4 s
after it starts); `alotool "<game>" info pp_novus_explosion_vent.alo` lists a system's emitters.

Headless check of a move order: `uaw-viewer "<game>" M16_Sahara --order 1900,3000 --run 12 --screenshot out.png`.
Scripted battle (prints both sides' units and health every 5 seconds):
`uaw-viewer "<game>" M24_Midwest --battle --run 90 --screenshot out.png`.
Model gallery of a faction's buildable units, or its structures, on a flat floor (largest at the
back; an optional third field keeps only names containing it):
`uaw-viewer "<game>" M25_Grand_Canyon --lineup Masari --screenshot units.png`,
`--lineup Novus:structures`, `--lineup Alien:units:Walker`.
`UAW_MODEL_ERRORS=1` prints why a model could not be loaded.

🚩 Missing art is tracked in [docs/TODO.md](docs/TODO.md). A few textures named by mod models ship with no mod (`SeI_e5`, `Nb_shield*`, `NB_Girder2`,
`zb_foundationv_v2`, `redflash`, `Ev_tiecrawlertreads*`, `W_cannon_flash`); those meshes are drawn
untextured grey, and translucent layers without their texture are skipped.

`objdump "<game>" weapons Alien_Hero_Orlok Novus_Robotic_Infantry` lists a unit's
weapons (including hardpoints), damage per second and armor modifiers against the
named victims.

Objects whose model isn't available are drawn as boxes: grey props, structures
and units in their owner's colour, cyan scrap piles, and a tall pole at each start position.

## megtool

```sh
megtool list    Config.meg
megtool verify  Config.meg               # checks stored filename CRCs
megtool cat     Config.meg Data/XML/GameObjectFiles.xml
megtool extract Config.meg ./extracted
```

Lookups ignore case and slash direction. Extraction refuses paths that would
escape the output folder.

## objdump

Point it at the folder that contains `Data/MegaFiles.xml`:

```sh
objdump "<game>" summary              # archives, object counts, factions
objdump "<game>" units Novus          # buildable units: health, cost, build time, speed, armor
objdump "<game>" show Alien_Grunt     # every field of one object, variants resolved
objdump "<game>" weapons Alien_Grunt  # weapons, ranges, damage per second
objdump "<game>" effect HexEffect     # one effect from the effect library
objdump "<game>" abilities            # tally: abilities per type, how many are simulated
objdump "<game>" abilities Alien_Defiler   # its abilities, generators, effects and components
objdump "<game>" research Alien       # the research tree: suites, costs, times, what they unlock
objdump "<game>" warnings
```

Object fields are flattened to paths, e.g.
`BehaviorType[ProductionBehaviorType].Build_Cost_Credits`. Variants copy
their base type and override the paths they define. `<!--INCLUDE "path" -->`
comments are include directives and are expanded in place.

## alotool

```sh
alotool "<game>" check                          # load every .ALO in the mounted archives
alotool "<game>" info    NB_AIRCRAFT_ASSEMBLY.ALO  # bones, meshes, materials, textures
alotool "<game>" export  AH_ORLOK.ALO orlok.glb    # glTF for Blender / any viewer (--all adds hidden meshes)
alotool "<game>" export  MB_VTOL_FACTORY.ALO open.glb --anim MB_VTOL_FACTORY_OPEN_00.ALA --frame last
alotool "<game>" anim    MB_VTOL_FACTORY_OPEN_00.ALA   # frames, fps, which bones move
alotool "<game>" unit    Alien_Grunt grunt.glb     # export a game object's Land_Model_Name
alotool "<game>" texture NV_Corruptor.tga out.png
```

Exports bake bone transforms and convert Alamo's Z-up to glTF's Y-up.
Hidden meshes (LODs, damage states, shadow and collision meshes) are skipped
unless `--all` is given. Materials name textures as `.tga`; the shipped files
are usually `.dds` with the same name in `Data/Art/Textures`.

## maptool

```sh
maptool "<game>" list                               # every map: size, objects, start positions
maptool "<game>" info M16_Sahara                    # layers, groups, start positions, object types
maptool "<game>" overview M16_Sahara sahara.png 1024 --group Multiplayer
```

Overviews shade the terrain, colour it by texture layer (from the real textures
when `Textures.meg` is available, otherwise a guess from the texture name), and
mark structures, units, resources, the playable area and numbered start positions.

## simtool

```sh
simtool "<game>" M25_Grand_Canyon start                  # every playable faction's starting forces and menus
simtool "<game>" M25_Grand_Canyon opening Novus          # one faction builds everything for 8 minutes
simtool "<game>" M25_Grand_Canyon match Novus Alien 25   # AI vs AI until someone is defeated
simtool "<game>" M25_Grand_Canyon game Novus:1,Masari:1,Alien:2,Military:2 30   # 2 v 2 (or FFA without teams)
simtool "<game>" M25_Grand_Canyon,M16_Sahara tournament Novus,Alien,Masari,Military 20 2   # round robin
```

A 25-minute match takes 30-50 seconds to simulate. `UAW_AI_LOG=1` prints every AI decision; `UAW_FOG=1` plays
with fog of war (units only shoot what their side can see).

### Balance

> ⚑ **Provisional.** These numbers get re-tuned when the gameplay is close to finished; see
> [docs/TODO.md](docs/TODO.md).

The original numbers are not balanced for this engine's AI: in an untouched 8-map round robin (all mods,
20 minutes, both seats, 2 seeds) Novus won 86 of 96 games and the Masari none. Each faction now has an income
multiplier, and the Hierarchy's scrap is worth **10 credits per unit** to it (it harvests at the same rate, but
a pile lasts ten times longer, so it doesn't strip the map and starve):

| Faction   | Income | Resource value |
|-----------|-------:|---------------:|
| Novus     | 0.84   | 1              |
| Masari    | 2.3    | 1              |
| Military  | 1.5    | 1              |
| Hierarchy | 2.2    | 10             |

Checked with a 36-game round robin on Grand Canyon, Sahara and Appalachia (every matchup from both sides,
2 seeds, fog of war on) once weapons honoured their target restrictions and the saucers fought: Hierarchy 20
wins, Novus 17, Masari 16, Military 16, 6 draws. The mods set most units' population cost to 0, so armies
are limited by money and factory speed, and Human Resurgence's 75-credit Marine squads make the
Military's income the most sensitive knob. Novus Variants come back three times (Community Patch /
Remix), hence Novus' low multiplier. Override any of it with `Data\XML\UAW_Balance.xml` in the game or a mod:

```xml
<UAW_Balance>
  <Faction Name="Alien"><Income_Multiplier>2.2</Income_Multiplier><Resource_Unit_Value>10</Resource_Unit_Value></Faction>
</UAW_Balance>
```

### AI

`SkirmishAI` (engine/src/ai.cpp) knows nothing faction-specific. It classifies everything by data: economy
(depots, Matter Engines, refineries, Reapers), power (Novus generators/transmitters), production (anything,
structure or walker, that is a `Tactical_Builder` of a unit with weapons), defence (structures with weapons)
and tech. It aims to use **the whole roster**:

- **Build order**: a generator first (Novus), a little economy, one of each production building, a defence,
  then every tech structure once (labs, advanced factories, detectors, superweapons, field-built units such as
  the Hierarchy scan drone), more economy and defences as the game goes on, and extra line factories (not HQs)
  when credits pile up. The next item is built as soon as it is affordable; nothing cheaper jumps the queue.
- **Bank reserve**: production keeps the price of the next structure, the next research suite, or an expensive
  unit another factory is saving for in the bank (never more than a capped bank, Masari, can hold).
- **Units in rotation**: every unit type gets turns in proportion to its combat value per credit (never less
  than a quarter of the best), so cheap efficient units come most often but heroes, capital ships and the rest
  get built too; unarmed specialists (hackers, scouts, science teams) one or two each. Upgrades that unlock a
  unit are bought first; other upgrades and hardpoints once the bank is comfortable. Hierarchy walkers buy
  hardpoints into their sockets. At most 160 combat units (the mods' population costs are 0).
- **Novus power**: transmitters are placed within reach of the powered network, receivers inside a powered
  node's field, one node at a time.

The army gathers at a rally point, defends structures under attack, and attacks the nearest enemy structure
when its strength (sqrt of health x damage per second, summed) is 1.3x the enemy's after minute 4 with six
units, 3x after minute 3, at the population cap, or after minute 12 with ten units; it regroups when a wave
drops below a third. Only units with `Is_Tactical_Base_Builder` construct; objects with no `Tactical_Builder`
and no builder among their requirements (Hierarchy walkers, the Remix missile silo) are placed by any base
builder of their faction. `UAW_AI_DEBUG=1` prints production turns and construction attempts.

Roster reached in 25 minutes against an idle opponent (Grand Canyon): Novus 15/28 units, 15/18 structures;
Hierarchy 23/27, 10/10; Masari 14/17, 16/16; Military 18/20, 10/10. What is left is mostly research-locked
heroes and Novus' top tier (Providence, Flying Fortress) that its income doesn't reach in time.

```sh
simtool "<game>" M25_Grand_Canyon start                  # every playable faction's starting forces and menus
simtool "<game>" M25_Grand_Canyon opening Novus          # one faction builds everything for 8 minutes
simtool "<game>" M25_Grand_Canyon match Novus Alien 25   # AI vs AI until someone is defeated
simtool "<game>" M25_Grand_Canyon,M16_Sahara tournament Novus,Alien,Masari,Military 20 2   # round robin
```

A match takes about a second to simulate. `UAW_AI_LOG=1` prints every AI decision.

## Superweapons

A superweapon structure names its weapon in its Lua data (`SWOwnerList[Find_Object_Type("Novus_Superweapon_EMP")]
="NOVUS_SUPERWEAPON_EMP_WEAPON"`). It arms `TSW_Warm_Up_Seconds` after it is finished and recharges for
`TSW_Cooldown_Countdown_Seconds` after each shot. Firing creates the weapon object at the target, and the object
does the work through the data: effect generators (Novus EMP stun, Masari thermal vacuum and magma channel, which
drifts toward the nearest enemy), an orbital bombardment of projectiles in salvos (Hierarchy mass drop, the Remix
Military nuke), or an object spawned when it collapses (the gravity bomb's singularity). The AI fires each one
where its radius catches the most enemy build value, less twice the value of its own units there; in the viewer,
select the structure, press V and right-click the target.

## Effects and abilities

Almost every special ability in the game is data: an ability switches on an **effect generator**,
which gathers targets and applies **effects** made of typed components. `engine/src/effects.cpp`
reads them, `engine/src/abilities.cpp` runs them every tick before targeting.

* **Effects** (`Data/XML/Effects`, 773 with the mods): `Effect_Class`, `Duration` (-1 = until
  removed) and components. Simulated: `AttributeModifier` (coefficients add up: `Max_Speed_Mult_Coeff`,
  `Damage_Cast_Mult_Coeff`, `Damage_Taken_Mult_Coeff`, `Weapon_Rate_Multiplier`, `Weapon_Delay_Multiplier`,
  `Weapon_Range_Mult/Add_Coeff`, `Unit_Ability_Countdown_Rate/Delay_Multiplier`, plus any attribute
  the filters read), `Damage`, `DamageOverTime`, `Heal`, `HealOverTime`, `DisableBehavior`
  (locomotors, weapons, abilities), `DisableAbility`, `RemoveEffect`, `BlockEffect`, `SwitchFaction`
  (mind control, reverted when it ends), `ModifyArmor`, `MaxHealth`, `Destroy`, `ApplyEffectOnRemove`,
  `ApplyGenerator`/`ActivateGenerator`, `CreateObject`. Visual and sound components are ignored. A
  target carries one instance of an effect, refreshed by whoever re-applies it, unless the effect says
  `Allow_Same_Source_Stacking` or `Extends_Same_Source_Duration_Only`.
* **Generators** (`Effect_Generator_Data` on a unit or projectile): entries with `Effect_Name`,
  `Duration`, `Base_Delay`, `Target_Gathering_Mask` (Self, Target, SelfAE, TargetAE),
  `Area_Effect_Radius`, `Target_Filter_C` and `Target_Filter_Categories` (`"Small + Organic + ~Flying"`:
  `+` and, `|` or, `~` not). `Starts_Active` generators are auras and passives; they re-apply every
  `Modal_Refresh_Frequency` seconds, and effects with duration -1 drop as soon as a target leaves.
  `Application_Signals` (OBJECT_DAMAGED, OBJECT_FIRED_WEAPON) trigger on events. `Disable_*_Attribute`
  pauses them (e.g. while mind-controlled). Target filters are named C++ functions in the original;
  the common ones (`Is_Enemy`, `Is_Ally`, `Is_Friendly_Unit_Damaged`, `In_Ice_Mode`...) are implemented
  and any other name rejects the target, so an unknown effect stays off rather than misfiring.
* **Abilities**: `Unit_Abilities_Data.Unit_Ability` (type, `Recharge_Seconds`, generator, AI hints)
  linked to an `Abilities` entry (`Generic_Attack_Ability` for melee swings and special shots,
  `Effect_Generator_Ability`). `Activation_Style SPECIAL_ATTACK` and button-less attacks fire on their
  own; the rest are ordered (`World::useAbility`), and the skirmish AI orders them when
  `World::abilityTarget` finds a worthwhile target. It never uses abilities that destroy or cripple
  their user, and leaves alternate-fire toggles alone.
* **Projectiles** with `Impact_Effect_Generator_Name` run that generator where they land (radiated
  shots, Masari fire, crippling shots), on behalf of the shooter.
* **Created objects** (`CreateObject`, `On_Death_Spawn_Object_Type`): units become real units;
  invisible effect objects with generators (the Defiler's radiation field, volatile-reactor explosions)
  run their generators for `Particle_Lifetime_Frames`.
* **Mode switches**: `SWITCH_TYPE` abilities toggle `Inactive_Type_Class` and `Active_Type_Class`;
  Masari units switch between their `Elemental_Object_Types` (fire/ice). The unit becomes the other type
  in place: same id, orders and effects, health scaled to the same fraction, the new type's weapons,
  abilities and generators, then the `Switch_Effect_Generator_Name` generator runs. The AI puts Masari
  units into ice (dark) mode when they are hurt in a fight and back to fire when they have recovered.
* **Leaps** (`LEAP`, `LeapBehaviorType`): an arc to a target or location within range, then
  `On_Landing_Effect_Generator_Name`, and for Novus robotic infantry `Self_Destruct_On_Landing`
  (`Damage_Applied_On_Self_Destruct` within `Radius_Affected_By_Self_Destruction`). The AI only blows
  units up when the data's `Unit_Ability_AI_Only_At_Health_Lower` hint says so.
* **Defences**: shields soak damage first (with `Shield_Armor_Type` modifiers) and regenerate
  `Shield_Refresh_Rate` per second; Masari dark-matter armor (`Current_DMA_Level`, a "memoryless"
  attribute the DMA effects add to, reset or fill) soaks damage after armor modifiers. Ice-mode units
  start full and regenerate through their `DMARegenGenerator`; fire mode drops it.
* `Transfer` components drain health (`Target_Effect HealthDamage`). When the source effect is
  `AmmoUnits` the drain fills the user's `Energy_Ammo_Capacity`, and abilities with `Check_Ammo_Full`
  stop when it is full: Orlok's crush takes about five cartridges' worth, which his siege mode spends.
* **Beam weapons** (`RadiationManagerBehaviorType`, the Corruptor and Defiler sweeps): a weapon with
  no projectile; damage is `Radiation_Direct_Damage` per tick over the sweep, range is the beam
  ability's `Activation_Max_Range`.
* **Clones** (`SPAWN_CLONES`): `Spawn_Clones_Count` copies of `Spawn_Clones_Type` appear next to the
  unit and vanish after `Spawn_Clones_Lifetime_Seconds`.
* **Cloaking** (`Cloak` and `Mirage` components, e.g. Vertigo's and the field inverter's cloaks): the unit
  can't be seen, targeted or aimed at by enemies until something in its `Break_Mask` happens (attacking,
  taking damage, moving); reveal effects remove it as data says (`RemoveEffect Cloaking`).
  `Max_Land_FOW_Reveal_Mult/Add_Coeff` modifiers change how far a unit sees.
* **Bursts**: `Spray_Projectiles_Ability` (the Antimatter tank's vent, `POINT_BLANK_AE`) fires
  `Projectile_Count` shots `Projectile_Fire_Interval` apart at enemies within `Projectile_Radius`;
  `Dervish_Spin_Attack_Ability` damages everything within `Attack_Radius` after `Spin_Up_Seconds`,
  `AOE_Damage_Per_Second` plus up to `AOE_Additional_Random_Damage` twice a second. The AI uses both
  with three or more enemies close by.
* **Transports** (`GarrisonableBehaviorType`: the Military APC, Hierarchy and Masari gunships, the Novus
  Flying Fortress): riders with one of the `Garrisonable_Attributes` (and none of the anti-attributes) walk to
  it and get in, up to `Max_Garrison_Capacity`; inside they are off the map and travel with it.
  `EJECT_GARRISONED` (or U in the viewer) drops them around it; a transport that is destroyed drops them
  too, minus `Damage_Percent_On_Destruction` of their health. In the viewer, right click your transport
  with units selected to load them.
* **Teleports** (`Teleport_Move_Ability`, the Hierarchy Cylinder): after `Teleport_Pre_Delay_Time` the unit
  appears at a point between `Minimum_` and `Maximum_Teleport_Range` away.
* **Lightning arcs** (`Lightning_Attack_Ability`, walker arc triggers and the Hierarchy gunship): a share of
  the target's health every `Damage_Interval_In_Secs` for the ability's duration, simulated as one hit per
  cycle of duration plus cooldown.
* **Air raids** (`Air_Raid_Activation_Ability`, Military jets): `Firing_Pulse_Count` shots of the raid's
  `Projectile` every `Firing_Recharge_Seconds` at targets in range, on top of the jet's guns.
* **Redirecting shields** (`Redirect_Projectile_Ability`, Novus field inverters in shield mode): a shot that
  hits the unit is sent back at its shooter with `Block_Chance` (while the shield holds, when
  `Redirect_If_Shield_Hit_Only`).
* **Selling** (`TacticalSellBehaviorType`): a structure is taken down over `Tactical_Sell_Time_Seconds`
  (doing nothing meanwhile) and returns its price times `1 + Tactical_Sell_Percent_Modifier` (half by
  default), scaled by its remaining health.
* **Charges** (`Tackle_Ability`, the Brute): a low dash of up to `Tackle_Charge_Distance` at the target,
  which takes `Damage_Amount` when the Brute arrives.

## Research

The research trees are compiled Lua scripts (`Data/Scripts/Miscellaneous/*_PACKAGED_RESEARCH_TREE.LUA`).
`LuaVM` (`engine/src/luavm.cpp`) runs them: it reads the game's Lua 5.1 bytecode variant and runs it
forgivingly (calling an engine function that isn't there just returns nil). `Init_Research_Tree` leaves one
table per branch: suites with a path (A/B/C) and tier, cost, time, and lists of types, generators,
effects and abilities they unlock or lock. Mods may ship a tree as plain source (Human Resurgence's
Military tree); its literal `Branch_* = { ... }` tables are read directly. As `Research_Common` does at game start, everything a suite
unlocks starts locked, so heroes, walker hardpoints, Novus patches, gamma radiation and many abilities
need research. A suite needs the tier before it in its path; a player researches one suite at a time
and at most six per game (as in the original's multiplayer). The `Enables_Research` structures (Novus
Research Center, Masari Will Processor...) are strategic-map buildings; where a mod makes one buildable in
skirmish, each adds a parallel research slot.

## Format notes

### TED maps

Nothing about this format is publicly documented; the layout below was worked
out from the retail maps and checked against all of them.

- `0x0000` header: `0x08` UTF-16 name text ID, `0x0D`/`0x0E` world extent.
- `0x0001/0x0101` terrain: `0x00` header (`0x00` width, `0x01` height in
  vertices), `0x0D` vertices as `{ int16 height; uint8 layer; uint8 layer }`,
  `0x02` texture layers (`0x03` each: `0x0C` diffuse, `0x13` bump, `0x07`
  tiling, `0x08` rotation, `0x0A` tint).
- **Height = int16 × 25/512 world units** on a 20-unit grid, rows along +Y.
  On all 41 maps checked, every placed object sits exactly on this surface
  (median error 0).
- `0x0001/0x0102` objects: `0x17` editor groups (`0x100`: `0x00` name, `0x01`
  object ids) and `0x01` placed objects, each `0x44C { 0x454, 0x459 { 0x4B0 } }`
  with `0x4B0` mini-chunks `0x00` id, `0x01` type, `0x02` owner slot, `0x03`
  scale, `0x04` position, `0x05` rotation in degrees.
- **Object type = CRC-32 of the uppercase type name**, e.g.
  `crc32("NOVUS_ROBOTIC_INFANTRY")`.
- Skirmish start positions are `Marker_Player_<n>_Start` objects. Campaign
  objects live in mission groups (`ZM01` …) and skirmish ones in `Multiplayer`.
- `0x0001/0x0103` playable area (`0x05` min, `0x06` max). `0x010D` asset banks.

### ALO models

Chunks are `{ uint32 id; uint32 size; }`, with the high bit of `size` marking a
container. Material parameters and connections use mini-chunks
`{ uint8 id; uint8 size; }`.

| Chunk | Contents |
|---|---|
| `0x200` | skeleton: `0x201` count, `0x202` bone { `0x203` name, `0x206` parent, visible, billboard, 3×4 row-major matrix relative to parent } |
| `0x400` | mesh: `0x401` name, `0x402` info (material count, bounds, hidden, collision), then per material `0x10100` material + `0x10000` geometry |
| `0x10100` | `0x10101` shader name; params `0x10102` int, `0x10103` float, `0x10104` float3, `0x10105` texture, `0x10106` float4 |
| `0x10000` | `0x10001` vertex/face counts, `0x10002` vertex format, `0x10007` vertices, `0x10004` uint16 triangle list, `0x10006` skin bone map, `0x1200` collision tree |
| `0x600` | connections: `0x602` object→bone, `0x603` proxy (particle effect on a bone), `0x604` dazzle (UaW addition) |
| `0x1300` | light |
| `0x1500` | particle system; UaW stores particle effects as `.ALO` too |

- Every vertex is 144 bytes: position, normal, 4 UV sets, tangent,
  binormal, RGBA colour, 16 unused bytes, 4 bone indices, 4 bone weights.
- Connection object indices count `0x400` meshes and `0x1300` lights
  together, in file order (lights often come first).
- Rigid meshes are stored relative to the bone they're connected to.
  Skinned meshes (`…Skin…` vertex formats) are stored in model space in bind
  pose. Placing them per bone scatters the parts, which was checked on Orlok and
  the Novus Reflex.

### ALA animations

`0x1000` wraps everything. `0x1001` header mini-chunks: `1` frame count, `2`
fps, `3` bone count, `0x0B` rotation components per frame, `0x0C`
translation components per frame. Each `0x1002` bone has `0x1003` info
(`4` name, `5` bone index, `6`/`7` translation offset/scale, `0x0E`
translation index, `0x10` rotation index, `0x11` default rotation) and an
optional `0x1007` visibility bitfield. `0x1009` holds int16 quaternions
(x, y, z, w / 32767) and `0x100A` holds uint16 translations
(offset + value × scale). An index of -1 means the channel is constant.

A posed bone's local transform is T × R relative to its parent, replacing the
rest transform. Skinned vertices are posed with posed(bone) × inverse(rest(bone)).

### Terrain flags (chunk `0x010A`)

One uint32 per vertex. Meanings below come from drawing the flags on several maps
and comparing them with terrain and objects:

| Flag | Seen on | Ground units |
|---|---|---|
| `0x01` | open ground | walk |
| `0x02` | building footprints, river channels, canyon walls | blocked |
| `0x08` | ocean | blocked |
| `0x10` | coastal water | blocked |
| `0x20` | pyramid faces, map border walls, lakes | blocked |
| `0x40` | raised walkways (rare) | walk |
| `0x80` | fords and bridges over rivers | walk |

### Unit movement fields (to be confirmed against the original)

`Max_Speed` is read as world units per logic frame, and `Max_Rate_Of_Turn` /
`Stopped_Rate_Of_Turn` as degrees per frame. `OverrideAcceleration` is the
fraction of max speed gained per frame, and `Custom_Hard_XExtent` is the
collision radius. GameConstants.xml says the game is tuned for 30 frames per
second. At that rate a Hierarchy Grunt (1.0) crosses a 5,000-unit map in
about 2¾ minutes, and a Habitat Walker (0.6°/frame) turns 18°/s.

### Combat fields

A unit's weapons come from its own `WeaponBehaviorType`, plus the weapon of every
hardpoint in `Attached_Hard_Points` (`HP_Type, bone` pairs). Masari units carry
their guns on Fire/Ice variants, e.g. `Masari_Peacebringer_Fire`. Each weapon has:

- `Projectile_Types`, `Targeting_Min/Max_Attack_Distance`
- `Projectile_Fire_Recharge_Seconds`, `Projectile_Fire_Pulse_Count` / `_Delay_Seconds`
- `Projectile_Fire_Spread_Amount` (pellets) and `_Angle`

Each projectile has `Max_Speed` (units/frame), `Damage_Type`, `Projectile_Damage`,
`Projectile_Blast_Area_Damage` / `_Range` and `Projectile_Max_Flight_Distance`.
Damage is multiplied by the `Damage_To_Armor_Mod` entry for the damage type and
the victim's `Armor_Type` (1.0 if absent).

### Economy

- **Skirmish start** (from `DefaultLandScript.lua`): Hierarchy gets
  `Alien_Arrival_Site` + `Alien_Glyph_Carver`, Novus gets `Novus_Remote_Terminal`
  + `Novus_Constructor`, Masari gets `Masari_Foundation` + `Masari_Architect`.
  Credits and pop cap come from `Multiplayer_Starting_Credits` (10,000 / 10,000 /
  1,000) and `Pop_Cap_For_Multiplayer_Games` (90) in Factions.xml.
- **Tech tree**: `ProductionDependencies.xml`. Entries for one object are OR'd,
  and the types inside an entry are AND'd. Units name their factory in
  `Tactical_Builder`; structures name their builder unit in
  `Tactical_Required_Objects`.
- **Cost and build time** of structures and walkers live on their
  `Tactical_Under_Construction_Object_Type` (e.g.
  `Novus_Power_Router_Construction`: 800 credits, 20 s). Units keep them on
  themselves. Hierarchy glyphs add `Required_Pre_Build_Time_In_Secs` of
  carving that needs the Glyph Carver; after that the walker finishes on its own.
- **Upgrades and patches** (hardpoints and `X12_Novus_Patches`) are researched
  once per structure rather than spawned, and can unlock further units.

### Resources

Everything comes from the XML (and the effect library, `Data/XML/Effects/EffectSystemFiles.xml`):

| Source | Fields | Retail values |
|---|---|---|
| Piles (any object with `ResourceBehaviorType`, placed in the map's Multiplayer group) | `Resource_Units`, `Resource_For_Faction` | car pile 7,000; Grand Canyon has 335 piles; Masari/Military excluded |
| Novus Input Station (`ResourceDepotBehaviorType`) | `Resource_Depot_Active_Drones`, `..._Seconds_To_Build_Drone`, `..._Collection_Radius` | 3 drones, one per 3 s, whole map |
| Novus Collector (`AIBehaviorType`) | `Drone_Capacity`, `Resources_Per_Grab` / `Resources_Seconds_Per_Grab`, `Resource_Gather_Range` | 220, 110 per 1 s, 80 |
| Hierarchy Reaper (Lua `AlienReaperTurret`) | `resource_drone_search_radius`, `resource_drone_harvest_distance`; rate from `AlienHarvestResourcesEffect` Transfer `Amount_Per_Second` | 200, 100, 18/s |
| Masari Matter Engine (Lua `MasariMatterEngine`) | `MatterEngine.ResourcesPerTimeBlock / ResourcesTimeSeconds`, `ResourcesStorage`, `MustBeOfFaction`; bank cap = `Baseline_Bank_Capacity` (Factions.xml) + storage of every engine | 4 per 3 s, +80; base cap 400 |
| Refineries / Military Supply (Lua `NeutralRefinery`) | `ResourcesPerSecond` | Neutral Refinery 12/s, Military Supply 16/s (Remix) |

Novus power (`PoweredBehaviorType`): Generators (Power Router) and Transmitters (Signal Tower) chain within the
`Transmit_Radius_Attribute` radius (550); Receivers are powered within a powered node's `Radius_Attribute`
(260 router, 165 tower); `Self_Powered` (Remote Terminal) is always on. Unpowered Novus structures don't produce
and Input Stations don't launch drones (as `NovusInputStation.lua` checks `Is_Powered`).

To confirm against the original: collector round trips are fast at 30 ticks/s (about 2,700 credits a minute per
station on Grand Canyon, where scrap lies close to the start); the Input Station can also build extra collectors
for 10 credits each.

### String table (`Data/Text/MasterTextFile_<LANGUAGE>.dat`)

`uint32 count`, then `count` × `{ uint32 crc32(key); uint32 valueLength; uint32
keyLength }`, then all values as UTF-16LE, then all keys as ASCII. The retail
English file has 7,945 strings, and every key matches its CRC. Objects name
their string with `Text_ID`.

### MEG archives

MEG v1 (shared with Empire at War / Forces of Corruption), little-endian:

```
uint32 numFilenames
uint32 numFiles
numFilenames × { uint16 len; char name[len]; }
numFiles     × { uint32 crc32; uint32 index; uint32 size; uint32 start; uint32 nameIndex; }
file data
```

The file table is sorted by CRC-32 of the uppercase, backslash-separated
name. The engine merges every archive in `Data/MegaFiles.xml`; later archives
win, and loose files on disk override archives.

Format knowledge comes from the Empire at War community (Mike Lankamp's
Petrolution docs, AlamoEngine-Tools/PetroglyphTools, the Blender ALAMO plugin).
