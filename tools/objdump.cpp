// objdump: load Universe at War's game object database from an install.
//   objdump <game-root> summary
//   objdump <game-root> units [faction]     buildable units/structures with health, cost, speed
//   objdump <game-root> show <ObjectName>   every field of one object (variants resolved)
//   objdump <game-root> field <Name>        every object that has the field, with its value
//   objdump <game-root> weapons <ObjectName> [VictimName...]  weapons, dps, armor modifiers
//   objdump <game-root> abilities [ObjectName]  abilities and effect generators (all units: a tally)
//   objdump <game-root> research [faction]    research trees (run from the compiled tree scripts)
//   objdump <game-root> scripts [all]      the game's Lua scripts: how many load, and the engine API
//                                           they call (globals and methods no script defines)
//   objdump <game-root> warnings
// <game-root> is the folder containing Data/MegaFiles.xml.
#include "common.hpp"
#include "alamo/combat.hpp"
#include "alamo/effects.hpp"
#include "alamo/research.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/luavm.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>

using namespace alamo;

static int usage() {
    std::cerr << "usage: objdump <game-root> summary | units [faction] | field <Name> | show <name> | effect <name> | weapons <name> | abilities [name] | research [faction] | scripts [all] | warnings\n";
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


// Engine API inventory of the game's scripts: every global a script reads that no script assigns
// (and the VM doesn't provide), and every name called as a method (obj:Name() or obj.Name()).
static int scriptApi(MegVfs& vfs, bool all) {
    std::map<std::string, std::set<std::string>> globals, methods; // name -> scripts using it
    std::set<std::string> defined;
    size_t compiled = 0, source = 0, failed = 0;
    for (const std::string& path : vfs.list()) {
        std::string up = path;
        for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (up.rfind("DATA\\SCRIPTS\\", 0) != 0 || up.size() < 4 || up.compare(up.size() - 4, 4, ".LUA") != 0) continue;
        const std::vector<uint8_t> bytes = vfs.read(path);
        std::string err;
        std::shared_ptr<LuaProto> main;
        if (!bytes.empty() && bytes[0] == 0x1b) main = loadLuaChunk(bytes, &err), ++compiled;
        else main = compileLua(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &err), ++source;
        if (!main) {
            ++failed;
            std::printf("  failed: %s (%s)\n", path.c_str(), err.c_str());
            continue;
        }
        const std::string script = up.substr(13);
        std::vector<const LuaProto*> todo{main.get()};
        while (!todo.empty()) {
            const LuaProto& p = *todo.back();
            todo.pop_back();
            for (const auto& c : p.protos) todo.push_back(c.get());
            auto name = [&](uint32_t k) -> std::string {
                return k < p.constants.size() && p.constants[k].isString() ? p.constants[k].s : std::string();
            };
            for (size_t pc = 0; pc < p.code.size(); ++pc) {
                const uint32_t i = p.code[pc];
                const uint32_t op = i & 0x3F, A = (i >> 6) & 0xFF, C = (i >> 14) & 0x1FF, Bx = i >> 14;
                if (op == 5) { if (std::string n = name(Bx); !n.empty()) globals[n].insert(script); }   // GETGLOBAL
                else if (op == 7) { if (std::string n = name(Bx); !n.empty()) defined.insert(n); }       // SETGLOBAL
                else if (op == 11 && (C & 0x100)) { if (std::string n = name(C & 0xFF); !n.empty()) methods[n].insert(script); } // SELF
                else if (op == 6 && (C & 0x100)) {                                                        // GETTABLE then CALL
                    for (size_t q = pc + 1; q < p.code.size() && q < pc + 12; ++q)
                        if ((p.code[q] & 0x3F) == 28 && ((p.code[q] >> 6) & 0xFF) == A) {
                            if (std::string n = name(C & 0xFF); !n.empty()) methods[n].insert(script);
                            break;
                        }
                }
            }
        }
    }
    LuaVM vm; // what the VM provides itself
    std::set<std::string> provided;
    for (const auto& [k, v] : vm.globals().fields) {
        provided.insert(k.str());
        if (v.isTable())
            for (const auto& [fk, fv] : v.t->fields) provided.insert(fk.str());
    }
    // Lua 5.1's own library: reported apart from the engine's API (what the VM still lacks of it).
    static const char* kLuaLib[] = {
        "assert", "collectgarbage", "dofile", "error", "gcinfo", "getfenv", "getmetatable", "ipairs", "loadstring",
        "module", "next", "pairs", "pcall", "print", "rawequal", "rawget", "rawset", "require", "select", "setfenv",
        "setmetatable", "tonumber", "tostring", "type", "unpack", "xpcall", "string", "table", "math", "coroutine", "os", "io", "debug",
        "byte", "char", "find", "format", "gmatch", "gsub", "len", "lower", "match", "rep", "reverse", "sub", "upper",
        "concat", "insert", "maxn", "remove", "sort", "getn", "setn", "foreach", "foreachi",
        "abs", "ceil", "cos", "deg", "exp", "floor", "fmod", "frexp", "huge", "ldexp", "log", "log10", "max", "min",
        "modf", "pi", "pow", "rad", "random", "randomseed", "sin", "sqrt", "tan", "clock", "date", "time"};
    std::map<std::string, std::set<std::string>> lib;
    for (const char* n : kLuaLib) {
        for (auto* m : {&globals, &methods})
            if (auto it = m->find(n); it != m->end()) {
                if (!provided.count(n)) lib[n].insert(it->second.begin(), it->second.end());
                m->erase(it);
            }
    }
    for (const std::string& n : provided) defined.insert(n);
    // Functions scripts define and call on their own tables (Behavior_Init, ...) are theirs, not the engine's.
    auto print = [&](const char* what, const std::map<std::string, std::set<std::string>>& m) {
        std::vector<std::pair<size_t, std::string>> v;
        for (const auto& [n, users] : m)
            if (!defined.count(n)) v.emplace_back(users.size(), n);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        std::printf("%s: %zu\n", what, v.size());
        const size_t shown = all ? v.size() : std::min<size_t>(v.size(), 40);
        for (size_t i = 0; i < shown; ++i) std::printf("  %4zu  %s\n", v[i].first, v[i].second.c_str());
        if (shown < v.size()) std::printf("  ... (objdump <game-root> scripts all)\n");
    };
    std::printf("scripts: %zu compiled, %zu source, %zu failed\n", compiled, source, failed);
    print("engine globals (by number of scripts using them)", globals);
    print("methods called", methods);
    print("Lua library functions the VM lacks", lib);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    const tools::ModChoice modArg = tools::takeModOptions(argc, argv);
    if (argc < 3) return usage();
    const std::string cmd = argv[2];
    try {
        MegVfs vfs;
        auto missing = tools::mountWithMods(vfs, argv[1], modArg);
        if (cmd == "scripts") return scriptApi(vfs, argc > 3 && std::string(argv[3]) == "all");
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
        if (cmd == "field" && argc > 3) { // every object with this field (leaf name), and its value
            size_t n = 0;
            for (const auto& o : db.objects())
                if (const std::string* v = o.find(argv[3])) {
                    std::printf("  %-44s %s\n", o.name.c_str(), trim(*v).substr(0, 80).c_str());
                    ++n;
                }
            std::printf("%zu objects\n", n);
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
            static const char* kKind[] = {"generator", "attack", "unsupported", "switch", "leap", "self-destruct", "beam weapon", "spawn clones", "burst", "elsewhere", "unload", "teleport", "place", "foo mode", "deploy"};
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
