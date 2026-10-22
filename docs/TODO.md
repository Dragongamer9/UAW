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

Last run (2026-10-22, after the AI learned the whole roster; 24 games, 2 seeds): Masari 7,
Hierarchy 6, Military 5, Novus 3 wins of 12; 3 draws. Results swing a lot between seeds, so use 2+.

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
superweapons are built and fired (README, Superweapons).
