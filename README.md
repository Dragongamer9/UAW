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
| 2 | Real-time 3D viewer (SDL3 + OpenGL 3.3): terrain, placed objects, RTS camera, keyboard/mouse/gamepad | done: first version; untextured until Textures.meg is available |
| 3 | Navigation: walkable areas from terrain flags, A* with wall clearance and path smoothing | done: start-to-start paths found on every skirmish map checked |
| 3 | Simulation core: fixed 30 Hz ticks, units with XML speed/turn/acceleration/size, group moves in formation, separation, deterministic | done (movement only) |
| 3 | Viewer: test squads per start, click/box select, right-click move, gamepad reticle controls | done |
| 3 | Combat: weapons from units and hardpoints, bursts/spread/recharge, projectiles with blast areas, `DamageToArmor.xml` (4,664 modifiers), auto-targeting, attack and attack-move orders, death | done (first version) |
| 3 | Economy: tech tree (`ProductionDependencies.xml`), players with credits and pop cap, structures that block ground, construction by builders, production queues, upgrades, skirmish start forces | done (first version) |
| 3 | HUD: game fonts and English strings (`MasterTextFile_ENGLISH.dat`), credits/population/clock, selection info, build menus, production queue, placement ghost, rally points | done |
| 1 | Mods: every mod in `Game\Mods` is layered on top of the base game by default and merged entry by entry (objects, tech tree, factions, damage table, strings, skirmish starts) | done: Community Patch 4 + Human Resurgence 1.02 + Universe at Remix 2.8 load together (3,746 objects, 235 buildable entries) |
| 3 | Military (human) faction in skirmish via the mods: Bunker + Build Bot start, base building | done |
| 3 | Resource collection (scrap, collectors, Military supply), enemy AI | next |
| 3 | Abilities, melee, hardpoint destruction, shields, veterancy | planned |
| 2 | Particle systems (`0x1500` .ALO), terrain texture splatting, animated units in the viewer | planned |
| 3 | Lua scripting host (AI, missions); scripts ship as precompiled Lua 5.1 bytecode with a custom header (format byte 0x70, 4-byte float numbers) | planned |
| 3 | Deterministic RTS simulation (units, pathing, combat) | planned |
| 4 | UI, audio, LAN multiplayer | planned |

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
uaw-viewer "<game>" M16_Sahara               # skirmish start: you (Novus) vs a Hierarchy base
uaw-viewer "<game>" M16_Sahara --faction Alien --enemy Masari
uaw-viewer "<game>" M16_Sahara --squads      # mixed test squads at every start instead
uaw-viewer "<game>" M25_Grand_Canyon --player 5
uaw-viewer "<game>" M16_Sahara --screenshot shot.png --size 1920x1080 --camera 2700,1700,2400,15,38
```

On Windows you can also put `uaw-viewer.exe` in the game folder (the one with
`Data\MegaFiles.xml`) and double-click it to open the Sahara map.

Each start position gets a test squad (player 1 Novus, player 2 Hierarchy,
player 3 Masari: infantry, vehicles and a hero). You command player 1's
squad, or another with `--player N`.

| | Keyboard / mouse | Gamepad (reticle at screen centre) |
|---|---|---|
| Pan | WASD / arrows | left stick |
| Rotate / tilt | Q/E, R/F, right- or middle-drag | right stick |
| Zoom | mouse wheel, +/- | triggers |
| Select | left click, left-drag box, Shift adds, Tab = all yours | A (hold LB to add), X = all on screen, B = clear |
| Move / attack | right click (on an enemy = attack) | RB |
| Attack-move | G then right click, or hold A + right click | D-pad up |
| Stop / jump to selection | X / Space | |
| Build menu (structure or builder selected) | 1-9 or click | D-pad left/right, D-pad down |
| Place structure / cancel | left click / right click | A / B |
| Cancel last queued item | Backspace | left stick click |
| Rally point (structure selected) | right click | RB |
| Toggle stand-in boxes | P | Y |
| Quit | Esc | Back |

Headless check of a move order: `uaw-viewer "<game>" M16_Sahara --order 1900,3000 --run 12 --screenshot out.png`.
Scripted battle (prints both sides' units and health every 5 seconds):
`uaw-viewer "<game>" M24_Midwest --battle --run 90 --screenshot out.png`.

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
simtool "<game>" M25_Grand_Canyon start            # skirmish start for each faction and what it can build
simtool "<game>" M25_Grand_Canyon opening Novus    # scripted build order with a timeline
```

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
