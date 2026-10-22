// objdump: load Universe at War's game object database from an install.
//   objdump <game-root> summary
//   objdump <game-root> units [faction]     buildable units/structures with health, cost, speed
//   objdump <game-root> show <ObjectName>   every field of one object (variants resolved)
//   objdump <game-root> weapons <ObjectName> [VictimName...]  weapons, dps, armor modifiers
//   objdump <game-root> abilities [ObjectName]  abilities and effect generators (all units: a tally)
//   objdump <game-root> research [faction]    research trees (run from the compiled tree scripts)
//   objdump <game-root> warnings
// <game-root> is the folder containing Data/MegaFiles.xml.
#include "common.hpp"
#include "alamo/combat.hpp"
#include "alamo/effects.hpp"
#include "alamo/research.hpp"
#include "alamo/gamedata.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>

using namespace alamo;

static int usage() {
    std::cerr << "usage: objdump <game-root> summary | units [faction] | show <name> | effect <name> | weapons <name> | abilities [name] | research [faction] | warnings\n";
    return 2;
}

static std::string firstWord(const std::string* s) {
    if (!s) return "-";
    std::string t = trim(*s);
    size_t c = t.find_first_of(", \t\r\n");
    return c == std::string::npos ? t : t.substr(0, c);
}

static std::string num(std::optional<double> v) {
    if (!v) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", *v);
    return buf;
}

int main(int argc, char** argv) {
    const tools::ModChoice modArg = tools::takeModOptions(argc, argv);
    if (argc < 3) return usage();
    const std::string cmd = argv[2];
    try {
        MegVfs vfs;
        auto missing = tools::mountWithMods(vfs, argv[1], modArg);
        GameDatabase db;
        db.load(vfs);

        if (cmd == "summary") {
            std::printf("archives mounted: %zu (%zu listed but missing)\n", vfs.archiveCount(), missing.size());
            for (const auto& m : missing) std::printf("  missing: %s\n", m.c_str());
            std::printf("object files:     %zu\n", db.fileCount());
            std::printf("game objects:     %zu\n", db.objects().size());
            size_t variants = std::count_if(db.objects().begin(), db.objects().end(),
                                            [](const auto& o) { return !o.variantOf.empty(); });
            std::printf("  of which variants: %zu\n", variants);
            std::printf("warnings:         %zu\n", db.warnings().size());
            std::map<std::string, size_t> cats;
            for (const auto& o : db.objects()) ++cats[o.category];
            std::vector<std::pair<size_t, std::string>> sorted;
            for (auto& [k, v] : cats) sorted.emplace_back(v, k);
            std::sort(sorted.rbegin(), sorted.rend());
            std::printf("top categories:\n");
            for (size_t i = 0; i < sorted.size() && i < 15; ++i)
                std::printf("  %5zu  %s\n", sorted[i].first, sorted[i].second.c_str());
            std::printf("factions:\n");
            for (const auto& f : db.factions())
                std::printf("  %-28s human:%s ai:%s\n", f.name.c_str(), f.humanPlayable ? "yes" : "no",
                            f.aiPlayable ? "yes" : "no");
            return 0;
        }
        if (cmd == "units") {
            std::string filter = argc > 3 ? argv[3] : "";
            std::printf("%-40s %-10s %7s %6s %6s %6s %s\n", "name", "faction", "health", "cost", "build", "speed", "armor");
            size_t n = 0;
            for (const auto& o : db.objects()) {
                auto cost = o.number("Tactical_Build_Cost_Multiplayer");
                if (!cost) continue; // only things a player can build
                std::string fac = firstWord(o.find("Affiliation"));
                if (!filter.empty() && !iequals(fac, filter)) continue;
                std::printf("%-40s %-10s %7s %6s %6s %6s %s\n", o.name.c_str(), fac.c_str(),
                            num(o.number("Tactical_Health")).c_str(), num(cost).c_str(),
                            num(o.number("Tactical_Build_Time_Seconds")).c_str(), num(o.number("Max_Speed")).c_str(),
                            firstWord(o.find("Armor_Type")).c_str());
                ++n;
            }
            std::printf("%zu buildable objects\n", n);
            return 0;
        }
        GameDatabase effects;
        if (cmd == "effect" && argc > 3) {
            effects.loadEffects(vfs);
            std::fprintf(stderr, "%zu effects\n", effects.objects().size());
        }
        if ((cmd == "show" || cmd == "effect") && argc > 3) {
            const GameObjectType* o = (cmd == "effect" ? effects : db).find(argv[3]);
            if (!o) { std::cerr << "no object named " << argv[3] << "\n"; return 1; }
            std::printf("%s  (%s, %s)\n", o->name.c_str(), o->category.c_str(), o->sourceFile.c_str());
            if (!o->variantOf.empty()) std::printf("variant of %s\n", o->variantOf.c_str());
            for (const auto& f : o->fields()) {
                for (const auto& v : f.values) {
                    std::string one = v;
                    std::replace_if(one.begin(), one.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
                    std::printf("  %s = %s\n", f.path.c_str(), one.c_str());
                }
            }
            return 0;
        }
        if (cmd == "weapons" && argc > 3) {
            const GameObjectType* o = db.find(argv[3]);
            if (!o) { std::cerr << "no object named " << argv[3] << "\n"; return 1; }
            const DamageTable table = loadDamageTable(vfs);
            const std::string* armor = o->find("Armor_Type");
            std::printf("%s: health %s, armor %s\n", o->name.c_str(), num(o->number("Tactical_Health")).c_str(),
                        armor ? trim(*armor).c_str() : "-");
            for (const WeaponSpec& w : weaponsFor(*o, db)) {
                std::printf("  %-34s via %-32s range %g-%g, %d x %d pellet(s), burst gap %d, recharge %d ticks\n",
                            w.projectile ? w.projectile->name.c_str() : "(beam)", w.source->name.c_str(), w.minRange, w.maxRange, w.pulseCount,
                            w.pellets, w.pulseDelayTicks, w.rechargeTicks);
                std::printf("      %s: %g damage + %g blast (radius %g), speed %g, %.1f dps\n", w.damageType.c_str(), w.damage,
                            w.blastDamage, w.blastRange, w.projectileSpeed, w.damagePerSecond());
                if (argc > 4)
                    for (int i = 4; i < argc; ++i)
                        if (const GameObjectType* v = db.find(argv[i]))
                            if (const std::string* va = v->find("Armor_Type"))
                                std::printf("      vs %-28s (%s): x%g\n", v->name.c_str(), trim(*va).c_str(),
                                            table.modifier(w.damageType, trim(*va)));
            }
            return 0;
        }
        if (cmd == "abilities") {
            EffectLibrary lib;
            lib.load(vfs);
            ResearchLocks locks; // everything any tree starts locked
            for (const ResearchTree& t : loadResearchTrees(vfs)) {
                const ResearchLocks l = ResearchLocks::initial(t);
                locks.generators.insert(l.generators.begin(), l.generators.end());
                locks.abilities.insert(l.abilities.begin(), l.abilities.end());
                locks.effects.insert(l.effects.begin(), l.effects.end());
            }
            static const char* kKind[] = {"generator", "attack", "unsupported", "switch", "leap", "self-destruct", "beam weapon", "spawn clones", "burst", "elsewhere", "unload", "teleport", "foo mode", "deploy"};
            if (argc > 3 && std::string_view(argv[3]).substr(0, 5) == "type:") { // who has abilities of a type
                const std::string want = std::string(argv[3]).substr(5);
                for (const GameObjectType& o : db.objects())
                    for (const AbilitySpec& a : abilitiesFor(o, db, generatorsFor(o)))
                        if (iequals(a.abilityType, want))
                            std::printf("  %-40s %-40s %s\n", o.name.c_str(), a.name.c_str(), kKind[int(a.kind)]);
                return 0;
            }
            if (argc > 3) {
                const GameObjectType* o = db.find(argv[3]);
                if (!o) { std::cerr << "no object named " << argv[3] << "\n"; return 1; }
                const auto gens = generatorsFor(*o);
                const auto abs = abilitiesFor(*o, db, gens);
                std::printf("%s: %zu abilities, %zu generators (%zu effects in the library)\n", o->name.c_str(), abs.size(),
                            gens.size(), lib.size());
                for (const AbilitySpec& a : abs) {
                    std::printf("  ability %-36s %-26s %-11s %s%s%s range %g-%g, recharge %.1fs", a.name.c_str(), a.abilityType.c_str(),
                                kKind[int(a.kind)], a.autonomous ? "auto " : "ordered ", a.needsTarget ? "targeted " : "",
                                a.targetsAllies ? "allies " : "", a.minRange, a.maxRange, a.rechargeTicks / 30.0);
                    if (!a.generator.empty()) std::printf(", generator %s", a.generator.c_str());
                    for (size_t k = 0; k < a.switchTypes.size(); ++k) std::printf("%s%s", k ? " / " : ", modes ", a.switchTypes[k].c_str());
                    if (a.kind == AbilitySpec::Kind::Attack)
                        std::printf(", %g dmg %s%s%s", a.damage, a.damageType.c_str(), a.projectile ? " via " : "",
                                    a.projectile ? a.projectile->name.c_str() : "");
                    if (locks.abilityLocked(*o, a.name, db) || locks.abilityLocked(*o, a.specialName, db) ||
                        locks.generatorLocked(a.generator))
                        std::printf("  [research]");
                    std::printf("\n");
                }
                for (const GeneratorDef& g : gens) {
                    std::printf("  generator %s%s%s%s\n", g.name.c_str(), g.startsActive ? " (starts active)" : "",
                                g.manualDeactivate ? " (toggled)" : "", locks.generatorLocked(g.name) ? " [research]" : "");
                    for (const GeneratorEntry& e : g.entries) {
                        const EffectDef* d = lib.find(e.effect);
                        std::printf("      %-44s %-22s %s mask %u r %g dur %g delay %g%s", e.effect.c_str(),
                                    d ? d->effectClass.c_str() : "(missing)", d && d->hasGameplay() ? "gameplay" : "visual  ",
                                    e.mask, e.radius, e.duration, e.baseDelay, locks.effectLocked(e.effect) ? " [research]" : "");
                        for (const auto& f : e.filters) std::printf(" [%s]", f.c_str());
                        if (!e.categories.empty()) std::printf(" {%s}", e.categories.c_str());
                        std::printf("\n");
                        if (d)
                            for (const EffectComponent& c : d->components) {
                                if (c.type == "ParticleSystem" || c.type == "Sound" || c.type == "Lightning") continue;
                                std::printf("          %s:", c.type.c_str());
                                for (const auto& [k, v] : c.fields) std::printf(" %s=%s", k.c_str(), v.c_str());
                                std::printf("\n");
                            }
                    }
                }
                return 0;
            }
            // Tally over everything with health.
            std::map<std::string, std::pair<int, int>> byType; // ability type -> (supported, total)
            size_t units = 0, withAbilities = 0;
            for (const GameObjectType& o : db.objects()) {
                if (!o.number("Tactical_Health")) continue;
                ++units;
                const auto abs = abilitiesFor(o, db, generatorsFor(o));
                if (!abs.empty()) ++withAbilities;
                for (const AbilitySpec& a : abs) {
                    auto& t = byType[a.abilityType];
                    ++t.second;
                    if (a.kind != AbilitySpec::Kind::Unsupported) ++t.first;
                }
            }
            std::printf("%zu effects; %zu objects with health, %zu with abilities\n", lib.size(), units, withAbilities);
            for (const auto& [t, c] : byType) std::printf("  %-32s %4d / %4d simulated\n", t.c_str(), c.first, c.second);
            return 0;
        }
        if (cmd == "research") {
            static const char* kKinds[] = {"type", "generator", "effect", "unit ability", "special ability"};
            for (const ResearchTree& t : loadResearchTrees(vfs)) {
                if (argc > 3 && !iequals(t.faction, argv[3])) continue;
                std::printf("%s: %zu suites\n", t.faction.c_str(), t.suites.size());
                for (const ResearchSuite& s : t.suites) {
                    std::printf("  %s%d %-16s %6.0f credits %4.0fs  %s\n", s.path.c_str(), s.index, s.branch.c_str(), s.cost,
                                s.seconds, s.title.c_str());
                    for (const ResearchChange& c : s.changes)
                        std::printf("      %s %-15s %s%s%s\n", c.locks ? "locks  " : "unlocks", kKinds[int(c.kind)], c.name.c_str(),
                                    c.objectType.empty() ? "" : " on ", c.objectType.c_str());
                }
            }
            return 0;
        }
        if (cmd == "warnings") {
            for (const auto& w : db.warnings()) std::printf("%s\n", w.c_str());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
