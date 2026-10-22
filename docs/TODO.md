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

Last run (2026-10-22): Novus 3, Masari 3, Hierarchy 3, Military 1 wins of 6; 2 draws.
