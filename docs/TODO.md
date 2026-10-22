# Open items

## Before release: rerun the balance games  ⚑

The faction income multipliers in `Balance::defaults()` (engine/src/economy.cpp) are **provisional**.
They were tuned while abilities, research, squads and mode switches were still landing, and every one
of those shifted the results a lot. Rerun the games once the gameplay features are close to finished:

```sh
# 12 games: every matchup played from both sides on one map (about 1.5 minutes)
simtool "<game>" M25_Grand_Canyon tournament Novus,Alien,Masari,Military 20 1
```

Try candidate multipliers with `Data\XML\UAW_Balance.xml` in the game folder (no rebuild needed), then
copy the final numbers into `Balance::defaults()` and the README's balance table. Ideally by then the
AI uses every unit and building of each faction, so the games exercise the whole roster.

Last run (2026-10-22 evening, fog of war on with the AI playing by it, targeting priority sets, the AI
building quick-payback income in numbers, countering the armor it has seen, growing careful after a
lost wave and raising unit-unlocking tech structures first; 72 games on M25_Grand_Canyon, M16_Sahara and
M22_Appalachia, 2 seeds): Masari 20, Novus 16, Military 16, Hierarchy 15 wins of 36; 5 draws. Matchups are lopsided (Novus 12-0 over the Military, Masari
10-2 over Novus); the
Military's result swings most with its multiplier (1.1 gave 15 wins, 1.2 about 20, 1.3 gave 26). Results
swing a lot between seeds and maps, so use 2+ of each. On three other maps (M31_Amazon_Basin, M24_Midwest, M09_Tibetan_Plateau, 1 seed) the
same multipliers gave Military 11, Masari 8, Novus 5, Hierarchy 5 of 18 (7 draws): map layouts (how much scrap
lies near the starts) move the balance about as much as the multipliers.

## Check the sound against the real game files

Sound effects and music (client/audio.cpp) are written against the game's XML (1,405 SFX events, 110
music events) but the staged copy has no samples: only synthetic sounds and the two speech mp3s in
Patch.meg were played. With the full install: run `UAW_AUDIO_LOG=1 uaw-viewer` and look for "no sample"
/ "no music" lines (samples are looked up by file name in every archive and loose folder; localized
speech also as NAME_ENG.mp3), and listen for levels: SFX `Min/Max_Volume`, music `Volume_Percent`,
and the distance fade in `Audio::place`.

## Check the build menu icons against the real game files

The build menu, production queue and selection panel show each type's `Icon_Name` texture
(`i_icon_*.tga`), but the staged copy has none of them (only a few `I_ICON_*_SA_*` in Patch.meg), so the
layout was only checked with a stand-in texture. With the full install the icons should appear beside every
item; if they don't, look for the archive that holds them (a GUI .meg not listed in MegaFiles.xml?).

## 🚩 Missing art (not shipped by the game or any mod)

Checked 2026-10-22 against the base archives, Community Patch 4, Human Resurgence 1.02 and
Universe at Remix 2.8, plus the rest of the launcher folder. All 231 buildable units and structures
have their models; what is still missing:

**Models that matter in play**
- `HP_AAT_Turret`, `HP_AAT_00`, `HP_AAT_01` (Novus AAT tank, Remix) name
  `armored_assault_tank_turret/ll/rl.alo`, which Remix doesn't ship. The tank body draws; those
  three hardpoints don't.
- (fixed) `Alien_Walker_Habitat_HP_Armor_Leg_Under_Construction` names `ABHP_Habitat_Armor_Leg.alo.alo`
  (typo in the mod XML); `modelPath` now drops the doubled extension.

**Models never used in skirmish:** 8 cinematic heroes (`CIN_*`, `Cin_Orlok_Armgone`) and 4
developer test objects (`Test_Arrow`, `Test_Turret`, `Test_Hardopint_Shield`, `Test_Building`).

**Textures** named by mod models (most look like Star Wars: Empire at War art the mod authors
ported without its textures): `SeI_e5`, `redflash`, `Nb_shieldbase`, `Nb_shieldripple`,
`Nb_shieldwave`, `NB_Girder2`, `zb_foundationv_v2`, `Ev_tiecrawlertreads`,
`Ev_tiecrawlertreads_bump`, `W_cannon_flash`. Opaque meshes draw untextured grey; translucent
layers (the droideka shield) are skipped. If an Empire at War install turns up, copy them from it.

Recheck: every faction type's `Land_Model_Name` must exist (`uaw-viewer --lineup` also logs failures with `UAW_MODEL_ERRORS=1`),
and every texture its model names.

## AI roster: what is still never used

The AI now builds nearly everything (README, AI section). Left over: research-locked heroes that its
research order doesn't reach, Novus' top tier (Providence 30,000, Flying Fortress) and some Novus patches,
superweapons are built and fired (README, Superweapons). `UAW_ROSTER=1 simtool ... match` lists what a side
could have built and never did. Tech structures that unlock units are now raised before the other tech, but Novus games often end
before the Remix Material Center's units (Hailfire, Armored Assault Tank, Providence) are reached.

## Bigger pieces still to do

- A Lua scripting host: the engine API the game's own scripts call (unit behaviours, the skirmish AI
  scripts, story missions). The VM itself is complete enough (metatables, coroutines).
- A Lua source compiler, for mods that ship scripts uncompiled (today only their literal data is read).
- The campaign (story maps, Global Conquest).
- GitHub Actions stopped running on 2026-10-22 (the account's billing / spending limit); builds resume
  once that is sorted.
