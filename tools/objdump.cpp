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
//   objdump <game-root> scripts run NAME... run scripts (and what they require) with no engine API
//   objdump <game-root> scripts list NAME [FUNCTION]  disassemble a script (or one global function)
//   objdump <game-root> scripts trace NAME  run a behaviour script against stand-ins and list every
//                                           engine function and method it reaches, in order
//   objdump <game-root> warnings
// <game-root> is the folder containing Data/MegaFiles.xml.
#include "common.hpp"
#include "alamo/combat.hpp"
#include "alamo/effects.hpp"
#include "alamo/research.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/luavm.hpp"
#include "alamo/scripts.hpp"

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
static int scriptApi(MegVfs& vfs, bool all, std::set<std::string>* engineOut = nullptr) {
    std::map<std::string, std::set<std::string>> globals, methods; // name -> scripts using it
    std::set<std::string> defined;
    size_t compiled = 0, source = 0, failed = 0;
    for (const std::string& path : vfs.listDirectory("Data\\Scripts")) {
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
            if (!engineOut) std::printf("  failed: %s (%s)\n", path.c_str(), err.c_str());
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
    if (engineOut) { // just the engine's globals, for the tracer
        for (const auto& [n, users] : globals)
            if (!defined.count(n)) engineOut->insert(n);
        return 0;
    }
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

// A behaviour script against stand-ins: every engine global and every method on what they return
// is recorded and answers with another stand-in, so the script runs as far as it can.
static int scriptTrace(MegVfs& vfs, const std::string& name) {
    ScriptLibrary lib(vfs);
    LuaVM vm;
    vm.stepBudget = 2'000'000;
    lib.install(vm);
    std::vector<std::string> order;
    std::map<std::string, int> seen;
    auto note = [&](const std::string& what) {
        if (!seen[what]++) order.push_back(what);
    };
    // A stand-in: indexing it gives a function that notes "label.key" and returns a stand-in.
    std::function<LuaValue(const std::string&)> probe = [&](const std::string& label) {
        LuaValue t = LuaValue::newTable();
        t.t->meta = std::make_shared<LuaTable>();
        t.t->meta->set(LuaValue::string("__index"), LuaValue::function([&, label](const std::vector<LuaValue>& a) {
            const std::string key = a.size() > 1 ? a[1].str() : "?";
            return std::vector<LuaValue>{LuaValue::function([&, label, key](const std::vector<LuaValue>&) {
                note(label + "." + key + "()");
                return std::vector<LuaValue>{probe(key + "()")};
            })};
        }));
        t.t->meta->set(LuaValue::string("__call"), LuaValue::function([&, label](const std::vector<LuaValue>&) {
            note(label + "()");
            return std::vector<LuaValue>{probe(label + "()")};
        }));
        return t;
    };
    std::set<std::string> engine; // globals no script defines: the engine's
    scriptApi(vfs, false, &engine);
    vm.onMissingGlobal = [&](const std::string& g) {
        if (!engine.count(g)) return LuaValue(); // a script's own global, not set yet
        note(g);
        return probe(g);
    };
    vm.setGlobal("Object", probe("Object"));
    { // the engine names the object's behaviours (Lua_Land_Behaviors) for Init_Behaviors
        LuaValue list = LuaValue::newTable();
        list.t->set(LuaValue::number(1), LuaValue::string(name));
        vm.setGlobal("BehaviorList", list);
    }
    std::string err;
    if (!lib.load(vm, name, &err)) std::printf("load: %s\n", err.empty() ? "failed" : err.c_str());
    std::string fns;
    for (const auto& [k, v] : vm.globals().fields)
        if (v.type == LuaValue::Type::Function) fns += " " + k.str();
    std::printf("script functions:%s\n", fns.c_str());
    for (const char* fn : {"Base_Definitions", "Init_Behaviors", "main", "First_Service", "Does_Object_Need_Behavior_Service", "Service", "Service"}) {
        const LuaValue f = vm.globals().get(LuaValue::string(fn));
        if (f.isNil()) continue;
        note(std::string("--- engine calls ") + fn + "()");
        vm.call(f, {});
        if (vm.failed()) note(std::string("--- ") + fn + ": " + vm.lastError());
    }
    std::printf("%s: %zu things reached\n", name.c_str(), order.size());
    for (const std::string& o : order) std::printf("  %4d  %s\n", seen[o], o.c_str());
    return 0;
}

// luac -l style listing of a compiled (or compiled-from-source) script; functions are named after
// the global they are stored in when they are.
static void listProto(const LuaProto& p, const std::string& name, int depth) {
    static const char* ops[] = {"MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL",
                                "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "MOD", "POW", "UNM",
                                "NOT", "LEN", "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "TESTSET", "CALL", "TAILCALL",
                                "RETURN", "FORLOOP", "FORPREP", "TFORLOOP", "SETLIST", "CLOSE", "CLOSURE", "VARARG"};
    auto k = [&](uint32_t i) -> std::string {
        if (i >= p.constants.size()) return "?";
        const LuaValue& v = p.constants[i];
        return v.isString() ? "\"" + v.s + "\"" : v.type == LuaValue::Type::Nil ? "nil" : v.str();
    };
    auto rk = [&](uint32_t x) { return (x & 0x100) ? k(x & 0xFF) : "R" + std::to_string(x); };
    const std::string pad(size_t(depth) * 2, ' ');
    std::printf("%sfunction %s (%u params%s, %u upvalues, %zu instructions)\n", pad.c_str(), name.c_str(), p.params,
                p.varargFlags ? ", vararg" : "", p.upvalues, p.code.size());
    std::vector<std::string> childNames(p.protos.size());
    for (size_t pc = 0; pc < p.code.size(); ++pc) {
        const uint32_t i = p.code[pc], op = i & 0x3F, A = (i >> 6) & 0xFF, C = (i >> 14) & 0x1FF, B = (i >> 23) & 0x1FF, Bx = i >> 14;
        const int sBx = int(Bx) - 131071;
        std::string args;
        switch (op) {
        case 1: case 5: case 7: args = "R" + std::to_string(A) + " " + k(Bx); break;
        case 6: args = "R" + std::to_string(A) + " = R" + std::to_string(B) + "[" + rk(C) + "]"; break;
        case 9: args = "R" + std::to_string(A) + "[" + rk(B) + "] = " + rk(C); break;
        case 11: args = "R" + std::to_string(A) + " = R" + std::to_string(B) + ":" + rk(C); break;
        case 12: case 13: case 14: case 15: case 16: case 17: case 23: case 24: case 25:
            args = std::to_string(A) + " " + rk(B) + " " + rk(C); break;
        case 22: case 31: case 32: args = "R" + std::to_string(A) + " -> " + std::to_string(int(pc) + 1 + sBx); break;
        case 36: {
            args = "R" + std::to_string(A) + " function #" + std::to_string(Bx);
            // CLOSURE ... then SETGLOBAL of the same register: a named global function
            size_t q = pc + 1 + (Bx < p.protos.size() ? p.protos[Bx]->upvalues : 0);
            if (q < p.code.size() && (p.code[q] & 0x3F) == 7 && ((p.code[q] >> 6) & 0xFF) == A && Bx < childNames.size())
                if (const uint32_t kk = p.code[q] >> 14; kk < p.constants.size()) childNames[Bx] = p.constants[kk].str();
            break;
        }
        default: args = std::to_string(A) + " " + std::to_string(B) + " " + std::to_string(C);
        }
        std::printf("%s  %4zu  %-9s %s\n", pad.c_str(), pc, op < 38 ? ops[op] : "?", args.c_str());
    }
    for (size_t c = 0; c < p.protos.size(); ++c)
        listProto(*p.protos[c], childNames[c].empty() ? name + "/#" + std::to_string(c) : childNames[c], depth + 1);
}

static int scriptList(MegVfs& vfs, const std::string& name, const std::string& only) {
    ScriptLibrary lib(vfs);
    std::string key = name;
    for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
    auto it = lib.scripts().find(key);
    if (it == lib.scripts().end()) { std::printf("no script %s\n", name.c_str()); return 1; }
    const std::vector<uint8_t> bytes = vfs.read(it->second);
    std::string err;
    auto main = !bytes.empty() && bytes[0] == 0x1b ? loadLuaChunk(bytes, &err)
                                                   : compileLua(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &err);
    if (!main) { std::printf("%s: %s\n", it->second.c_str(), err.c_str()); return 1; }
    if (only.empty()) { listProto(*main, it->second, 0); return 0; }
    // Only the function stored in global `only` (searched through the main function's closures).
    for (size_t pc = 0; pc < main->code.size(); ++pc) {
        const uint32_t i = main->code[pc];
        if ((i & 0x3F) != 36) continue;
        const uint32_t A = (i >> 6) & 0xFF, Bx = i >> 14;
        if (Bx >= main->protos.size()) continue;
        const size_t q = pc + 1 + main->protos[Bx]->upvalues;
        if (q < main->code.size() && (main->code[q] & 0x3F) == 7 && ((main->code[q] >> 6) & 0xFF) == A) {
            const uint32_t kk = main->code[q] >> 14;
            if (kk < main->constants.size() && main->constants[kk].str() == only) {
                listProto(*main->protos[Bx], only, 0);
                return 0;
            }
        }
    }
    std::printf("no global function %s in %s\n", only.c_str(), name.c_str());
    return 1;
}

int main(int argc, char** argv) {
    const tools::ModChoice modArg = tools::takeModOptions(argc, argv);
    if (argc < 3) return usage();
    const std::string cmd = argv[2];
    try {
        MegVfs vfs;
        auto missing = tools::mountWithMods(vfs, argv[1], modArg);
        if (cmd == "scripts" && argc > 4 && std::string(argv[3]) == "trace") return scriptTrace(vfs, argv[4]);
        if (cmd == "scripts" && argc > 4 && std::string(argv[3]) == "list") return scriptList(vfs, argv[4], argc > 5 ? argv[5] : "");
        if (cmd == "scripts" && argc > 4 && std::string(argv[3]) == "run") {
            ScriptLibrary lib(vfs);
            int bad = 0;
            std::vector<std::string> names(argv + 4, argv + argc);
            if (names.size() == 1 && names[0] == "Library") { // every library script
                names.clear();
                for (const auto& [n, path] : lib.scripts())
                    if (path.find("LIBRARY") != std::string::npos || path.find("Library") != std::string::npos) names.push_back(n);
            }
            for (const std::string& n : names) {
                LuaVM vm;
                lib.install(vm);
                std::string err;
                const bool ok = lib.load(vm, n, &err);
                size_t functions = 0;
                for (const auto& [k, v] : vm.globals().fields) functions += v.type == LuaValue::Type::Function;
                std::printf("%-36s %s, %zu global functions%s%s\n", n.c_str(), ok ? "ran" : "FAILED", functions,
                            err.empty() ? "" : ": ", err.c_str());
                bad += !ok;
            }
            return bad ? 1 : 0;
        }
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
