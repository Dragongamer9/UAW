#include "alamo/scripthost.hpp"

#include "alamo/strings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>

namespace alamo {

using Values = std::vector<LuaValue>;

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
LuaValue table() { return LuaValue::newTable(); }
void def(LuaValue& t, const char* name, LuaNative fn) { t.t->set(LuaValue::string(name), LuaValue::function(std::move(fn))); }
// A table that can also be called (GetEvent(), ThreadValue("x"), GameRandom(1, 5)...).
void callable(LuaValue& t, LuaNative fn) {
    if (!t.t->meta) t.t->meta = std::make_shared<LuaTable>();
    // __call gets the table first: drop it.
    t.t->meta->set(LuaValue::string("__call"), LuaValue::function([fn](const Values& a) {
        return fn(Values(a.begin() + (a.empty() ? 0 : 1), a.end()));
    }));
}
double num(const Values& a, size_t i, double fallback = 0) {
    return i < a.size() && a[i].type == LuaValue::Type::Number ? a[i].n : fallback;
}

} // namespace

struct BehaviorHost::Instance {
    uint32_t id = 0;
    int owner = 0;
    std::unique_ptr<LuaVM> vm;
    LuaValue main;                              // the pumping thread (coroutine running main())
    struct Thread {
        int id;
        LuaValue co;
        LuaValue param;
        bool started = false;
    };
    std::vector<Thread> threads;               // Create_Thread
    int nextThread = 1;
    std::deque<std::pair<LuaValue, Values>> events; // callback and its arguments, for GetEvent
    LuaValue lastParams;                        // GetEvent.Params() of the event just taken
    struct RangeEvent {
        LuaValue fn;
        float range;
        std::set<uint32_t> inside;
    };
    std::vector<RangeEvent> ranges;
    std::multimap<std::string, LuaValue> signals; // OBJECT_OWNER_CHANGED, ...
    std::map<const void*, std::map<std::string, LuaValue>> threadValues; // per coroutine
    std::map<uint32_t, LuaValue> objects;       // handles, one per id (scripts compare them)
    std::map<int, LuaValue> players;
    std::map<const GameObjectType*, LuaValue> types;
    std::map<uint64_t, LuaValue> hardpoints;   // handles by (object id << 8 | slot)
    struct HardPointWatch {
        uint32_t id;
        size_t slot;
        LuaValue fn;
        bool fired = false;
    };
    std::vector<HardPointWatch> hardpointDeaths; // OBJECT_HEALTH_AT_ZERO on a hardpoint
    uint64_t random = 0;
    int playerId = -1;                         // a player's script (Player, no Object)
    bool dead = false, finished = false, firstService = true;
    std::set<std::string> said; // log lines already given
    // Handle factories (cached in the maps above).
    std::function<LuaValue(int)> player;
    std::function<LuaValue(uint32_t)> object;
    std::function<LuaValue(const GameObjectType*)> type;
};

BehaviorHost::BehaviorHost(World& world, ScriptLibrary& library) : world_(world), library_(library) {}
BehaviorHost::~BehaviorHost() = default;

size_t BehaviorHost::running() const {
    return size_t(std::count_if(instances_.begin(), instances_.end(), [](const auto& i) { return !i->finished; }));
}

std::vector<std::string> BehaviorHost::behaviorsOf(const GameObjectType& t) {
    std::vector<std::string> out;
    auto add = [&](const std::string* v) {
        if (!v) return;
        std::string cur;
        for (char c : *v + ",") {
            if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    };
    add(t.find("Lua_Land_Behaviors"));
    if (out.empty()) add(t.find("Lua_Script"));
    return out;
}

bool BehaviorHost::attach(uint32_t id, const std::vector<std::string>& behaviors, std::string* error) {
    return start(id, -1, behaviors, error);
}

bool BehaviorHost::attachPlayer(int playerId, std::string* error) {
    const Player* p = world_.player(playerId);
    if (!p) {
        if (error) *error = "no such player";
        return false;
    }
    std::string faction = p->faction;
    if (!faction.empty()) faction[0] = char(std::toupper(static_cast<unsigned char>(faction[0])));
    return start(0, playerId, {faction + "_Player"}, error);
}

BehaviorHost::Instance* BehaviorHost::playerScript(int playerId) {
    if (auto it = playerScripts_.find(playerId); it != playerScripts_.end()) return it->second;
    playerScripts_[playerId] = nullptr; // tried once
    std::string err;
    if (!attachPlayer(playerId, &err)) {
        if (log) log("player script " + std::to_string(playerId) + ": " + err);
        return nullptr;
    }
    return playerScripts_[playerId] = instances_.back().get();
}

bool BehaviorHost::start(uint32_t id, int playerId, const std::vector<std::string>& behaviors, std::string* error) {
    Unit* u = playerId >= 0 ? nullptr : world_.unit(id);
    if ((playerId < 0 && !u) || behaviors.empty()) {
        if (error) *error = "no such unit or no behaviours";
        return false;
    }
    auto in = std::make_unique<Instance>();
    Instance& I = *in;
    I.id = id;
    I.playerId = playerId;
    I.owner = u ? u->owner : playerId;
    I.random = 0x9E3779B97F4A7C15ull ^ id;
    I.vm = std::make_unique<LuaVM>();
    LuaVM& vm = *I.vm;
    library_.install(vm);
    World& w = world_;
    auto say = [this, &I](const std::string& m) {
        if (log && I.said.insert(m).second) log(m);
    };

    // ---- handles: objects, players, types, positions
    auto position = [](Vec3 p) {
        LuaValue t = table();
        t.t->set(LuaValue::string("x"), LuaValue::number(p[0]));
        t.t->set(LuaValue::string("y"), LuaValue::number(p[1]));
        t.t->set(LuaValue::string("z"), LuaValue::number(p[2]));
        def(t, "Get_Position", [t](const Values&) { return Values{t}; });
        def(t, "Is_Valid", [](const Values&) { return Values{LuaValue::boolean(true)}; });
        return t;
    };
    // Where a handle is: an object's position or a position's coordinates.
    auto whereOf = [&w](const LuaValue& v, Vec3& out) {
        if (!v.isTable()) return false;
        const LuaValue id = v.t->get(LuaValue::string("__id"));
        if (id.type == LuaValue::Type::Number) {
            const Unit* u = w.unit(uint32_t(id.n));
            if (!u) return false;
            out = u->position;
            return true;
        }
        const LuaValue x = v.t->get(LuaValue::string("x"));
        if (x.type != LuaValue::Type::Number) return false;
        out = {float(x.n), float(v.t->get(LuaValue::string("y")).n), float(v.t->get(LuaValue::string("z")).n)};
        return true;
    };
    I.player = [&I, &w, this](int pid) -> LuaValue {
        if (auto it = I.players.find(pid); it != I.players.end()) return it->second;
        LuaValue p = table();
        I.players[pid] = p;
        def(p, "Get_ID", [pid](const Values&) { return Values{LuaValue::number(pid)}; });
        def(p, "Is_Valid", [](const Values&) { return Values{LuaValue::boolean(true)}; });
        def(p, "Is_Human", [this, pid](const Values&) { return Values{LuaValue::boolean(isHuman(pid))}; });
        def(p, "Is_AI_Player", [this, pid](const Values&) { return Values{LuaValue::boolean(!isHuman(pid))}; });
        def(p, "Get_Allow_AI_Unit_Behavior", [](const Values&) { return Values{LuaValue::boolean(false)}; });
        def(p, "Give_Money", [&w, pid](const Values& a) {
            if (Player* pl = w.player(pid)) {
                pl->credits += float(num(a, 0));
                pl->earned += float(std::max(0.0, num(a, 0)));
            }
            return Values{};
        });
        def(p, "Get_Credits", [&w, pid](const Values&) {
            const Player* pl = w.player(pid);
            return Values{LuaValue::number(pl ? pl->credits : 0)};
        });
        def(p, "Get_Faction_Name", [&w, pid](const Values&) {
            const Player* pl = w.player(pid);
            return Values{LuaValue::string(pl ? upper(pl->faction) : "")};
        });
        def(p, "Is_Enemy", [&w, pid](const Values& a) {
            const LuaValue other = a.empty() ? LuaValue() : a[0].isTable() ? a[0].t->get(LuaValue::string("Get_ID")) : LuaValue();
            int oid = -1;
            if (other.type == LuaValue::Type::Native) {
                const Values r = (*other.native)({});
                oid = r.empty() ? -1 : int(r[0].n);
            }
            const Player* x = w.player(pid);
            const Player* y = w.player(oid);
            const bool allied = oid == pid || (x && y && x->team >= 0 && x->team == y->team);
            return Values{LuaValue::boolean(oid >= 0 && !allied)};
        });
        // The player's own script (<Faction>_Player.lua), started on first use.
        def(p, "Get_Script", [this, pid](const Values&) {
            Instance* ps = playerScript(pid);
            if (!ps) return Values{};
            LuaValue h = table();
            LuaVM* pvm = ps->vm.get();
            def(h, "Call_Function", [pvm](const Values& a) {
                if (a.empty()) return Values{};
                const LuaValue f = pvm->global(a[0].str());
                return f.isNil() ? Values{} : pvm->call(f, Values(a.begin() + 1, a.end()));
            });
            def(h, "Get_Variable", [pvm](const Values& a) { return a.empty() ? Values{} : Values{pvm->global(a[0].str())}; });
            return Values{h};
        });
        return p;
    };
    I.type = [&I](const GameObjectType* t) -> LuaValue {
        if (!t) return LuaValue();
        if (auto it = I.types.find(t); it != I.types.end()) return it->second;
        LuaValue h = table();
        I.types[t] = h;
        h.t->set(LuaValue::string("__type"), LuaValue::string(t->name));
        def(h, "Is_Valid", [](const Values&) { return Values{LuaValue::boolean(true)}; });
        def(h, "Get_Name", [t](const Values&) { return Values{LuaValue::string(upper(t->name))}; });
        def(h, "Get_Type_Value", [t](const Values& a) {
            const std::string* v = a.empty() ? nullptr : t->find(a[0].str());
            if (!v) return Values{};
            const std::string s = trim(*v);
            char* end = nullptr;
            const double d = std::strtod(s.c_str(), &end);
            if (end && end != s.c_str() && *end == '\0') return Values{LuaValue::number(d)};
            if (iequals(s, "yes") || iequals(s, "true")) return Values{LuaValue::boolean(true)};
            if (iequals(s, "no") || iequals(s, "false")) return Values{LuaValue::boolean(false)};
            return Values{LuaValue::string(s)};
        });
        return h;
    };
    I.object = [&I, &w, whereOf, position, say](uint32_t oid) -> LuaValue {
        if (auto it = I.objects.find(oid); it != I.objects.end()) return it->second;
        LuaValue o = table();
        I.objects[oid] = o;
        o.t->set(LuaValue::string("__id"), LuaValue::number(oid));
        auto alive = [&w, oid]() -> Unit* {
            Unit* u = w.unit(oid);
            return u && u->alive() ? u : nullptr;
        };
        def(o, "Get_ID", [oid](const Values&) { return Values{LuaValue::number(oid)}; });
        def(o, "Is_Valid", [alive](const Values&) { return Values{LuaValue::boolean(alive() != nullptr)}; });
        def(o, "Get_Owner", [alive, &I](const Values&) {
            Unit* u = alive();
            return u ? Values{I.player(u->owner)} : Values{};
        });
        def(o, "Get_Type", [alive, &I](const Values&) {
            Unit* u = alive();
            return u ? Values{I.type(u->type)} : Values{};
        });
        def(o, "Get_Position", [alive, position](const Values&) {
            Unit* u = alive();
            return u ? Values{position(u->position)} : Values{};
        });
        def(o, "Get_Distance", [alive, whereOf](const Values& a) {
            Unit* u = alive();
            Vec3 p;
            if (!u || a.empty() || !whereOf(a[0], p)) return Values{LuaValue::number(1e9)};
            return Values{LuaValue::number(std::hypot(u->position[0] - p[0], u->position[1] - p[1]))};
        });
        def(o, "Get_Health", [alive](const Values&) {
            Unit* u = alive();
            return Values{LuaValue::number(u ? u->health / std::max(u->maxHealth, 1.f) : 0)};
        });
        def(o, "Can_Move", [alive](const Values&) {
            Unit* u = alive();
            return Values{LuaValue::boolean(u && !u->structure && u->maxSpeed > 0)};
        });
        def(o, "Despawn", [alive](const Values&) {
            if (Unit* u = alive()) u->health = 0; // removed with the dead (first version: as a death)
            return Values{};
        });
        def(o, "Is_Category", [alive](const Values& a) {
            Unit* u = alive();
            const std::string* mask = u ? u->type->find("CategoryMask") : nullptr;
            return Values{LuaValue::boolean(mask && !a.empty() && upper(*mask).find(upper(a[0].str())) != std::string::npos)};
        });
        def(o, "Get_Attribute_Value", [](const Values&) { return Values{LuaValue::number(0)}; });
        def(o, "Is_Powered", [alive](const Values&) {
            Unit* u = alive();
            return Values{LuaValue::boolean(u && u->powered)};
        });
        def(o, "Is_AI_Recruited", [](const Values&) { return Values{LuaValue::boolean(false)}; });
        def(o, "Is_AI_Recruitable", [](const Values&) { return Values{LuaValue::boolean(false)}; });
        def(o, "Has_Behavior", [](const Values&) { return Values{LuaValue::boolean(false)}; });
        def(o, "Get_Script", [](const Values&) { return Values{}; });
        def(o, "Service_Wrapper", [](const Values&) { return Values{}; });
        // Hardpoints (walker legs, cores, weapons) as handles of their own.
        auto hardpoint = [&I, &w](uint32_t owner, size_t slot) -> LuaValue {
            const uint64_t key = uint64_t(owner) << 8 | slot;
            if (auto it = I.hardpoints.find(key); it != I.hardpoints.end()) return it->second;
            LuaValue h = table();
            I.hardpoints[key] = h;
            auto hp = [&w, owner, slot]() -> Unit::HardPoint* {
                Unit* u = w.unit(owner);
                return u && u->alive() && slot < u->hardpoints.size() && u->hardpoints[slot].alive() ? &u->hardpoints[slot] : nullptr;
            };
            def(h, "Is_Valid", [hp](const Values&) { return Values{LuaValue::boolean(hp() != nullptr)}; });
            def(h, "Get_Type", [hp, &I](const Values&) {
                Unit::HardPoint* p = hp();
                return p ? Values{I.type(p->type)} : Values{};
            });
            def(h, "Get_Health", [hp](const Values&) {
                Unit::HardPoint* p = hp();
                return Values{LuaValue::number(p ? p->health / std::max(p->maxHealth, 1.f) : 0)};
            });
            def(h, "Get_Parent_Object", [&I, owner](const Values&) { return Values{I.object(owner)}; });
            // The walker's damage rules (legs, then the core) are the engine's own: these are accepted.
            for (const char* quiet : {"Make_Invulnerable", "Set_Cannot_Be_Killed", "Enable_Behavior"})
                def(h, quiet, [](const Values&) { return Values{}; });
            def(h, "Register_Signal_Handler", [&I, owner, slot](const Values& a) {
                if (a.size() >= 2 && upper(a[1].str()) == "OBJECT_HEALTH_AT_ZERO") I.hardpointDeaths.push_back({owner, slot, a[0]});
                return Values{};
            });
            def(h, "Unregister_Signal_Handler", [&I, owner, slot](const Values& a) {
                if (a.empty()) return Values{};
                auto& v = I.hardpointDeaths;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& x) { return x.id == owner && x.slot == slot && rawEquals(x.fn, a[0]); }), v.end());
                return Values{};
            });
            return h;
        };
        def(o, "Find_All_Hard_Points_Of_Type", [alive, hardpoint](const Values& a) {
            Unit* u = alive();
            if (!u || a.empty()) return Values{};
            const std::string want = upper(a[0].isTable() ? a[0].t->get(LuaValue::string("__type")).str() : a[0].str());
            LuaValue list = table(); // a list, as its name says
            double n = 0;
            for (size_t k = 0; k < u->hardpoints.size(); ++k)
                if (u->hardpoints[k].type && upper(u->hardpoints[k].type->name) == want) list.t->set(LuaValue::number(++n), hardpoint(u->id, k));
            return n > 0 ? Values{list} : Values{};
        });
        def(o, "Get_All_Hard_Points", [alive, hardpoint](const Values&) {
            Unit* u = alive();
            if (!u) return Values{};
            LuaValue list = table();
            for (size_t k = 0; k < u->hardpoints.size(); ++k) list.t->set(LuaValue::number(double(k + 1)), hardpoint(u->id, k));
            return Values{list};
        });
        for (const char* quiet : {"Make_Invulnerable", "Set_Cannot_Be_Killed", "Enable_Behavior"})
            def(o, quiet, [](const Values&) { return Values{}; });
        if (oid == I.id) { // events and signals: only on the script's own object
            def(o, "Event_Object_In_Range", [&I](const Values& a) {
                if (a.size() >= 2) I.ranges.push_back({a[0], float(num(a, 1)), {}});
                return Values{};
            });
            def(o, "Cancel_Event_Object_In_Range", [&I](const Values& a) {
                if (!a.empty())
                    I.ranges.erase(std::remove_if(I.ranges.begin(), I.ranges.end(), [&](const auto& r) { return rawEquals(r.fn, a[0]); }),
                                   I.ranges.end());
                return Values{};
            });
            def(o, "Register_Signal_Handler", [&I](const Values& a) {
                if (a.size() >= 2) I.signals.emplace(upper(a[1].str()), a[0]);
                return Values{};
            });
            def(o, "Unregister_Signal_Handler", [&I](const Values& a) {
                if (a.size() < 2) return Values{};
                auto range = I.signals.equal_range(upper(a[1].str()));
                for (auto it = range.first; it != range.second;)
                    it = rawEquals(it->second, a[0]) ? I.signals.erase(it) : std::next(it);
                return Values{};
            });
        }
        // Anything else: nil, noted once.
        o.t->meta = std::make_shared<LuaTable>();
        o.t->meta->set(LuaValue::string("__index"), LuaValue::function([say](const Values& a) {
            if (a.size() > 1) say("unknown object method " + a[1].str());
            return Values{};
        }));
        return o;
    };

    // ---- the engine globals the framework needs
    auto running = [&vm]() {
        const Values r = vm.call(vm.global("coroutine").get("running"), {});
        return r.empty() ? LuaValue() : r[0];
    };
    if (playerId >= 0) vm.setGlobal("Player", I.player(playerId));
    else vm.setGlobal("Object", I.object(id));
    if (playerId < 0) {
        LuaValue list = table();
        double n = 0;
        for (const std::string& b : behaviors) list.t->set(LuaValue::number(++n), LuaValue::string(b));
        vm.setGlobal("BehaviorList", list);
    }
    vm.setFunction("GetCurrentTime", [&w](const Values&) { return Values{LuaValue::number(w.seconds())}; });
    LuaValue random = table();
    auto rnd = [&I](const Values& a) {
        I.random = I.random * 6364136223846793005ull + 1442695040888963407ull;
        const double r = double(I.random >> 11) / double(1ull << 53);
        const double lo = num(a, 0, 0), hi = num(a, 1, 1);
        return r * (hi - lo) + lo;
    };
    def(random, "Get_Float", [rnd](const Values& a) { return Values{LuaValue::number(rnd(a))}; });
    callable(random, [rnd](const Values& a) { return Values{LuaValue::number(std::floor(rnd({LuaValue::number(num(a, 0)), LuaValue::number(num(a, 1) + 1)})))}; });
    vm.setGlobal("GameRandom", random);
    {
        LuaValue math = table();
        for (const auto& [k, v] : vm.global("math").t->fields) math.t->set(k, v);
        def(math, "mod", [](const Values& a) { return Values{LuaValue::number(std::fmod(num(a, 0), num(a, 1, 1)))}; });
        vm.setGlobal("Math", math);
    }
    LuaValue script = table();
    def(script, "Set_Async_Data", [](const Values&) { return Values{}; });
    def(script, "Get_Async_Data", [](const Values&) { return Values{}; });
    def(script, "Debug_Should_Issue_Event_Alert", [](const Values&) { return Values{LuaValue::boolean(false)}; });
    vm.setGlobal("Script", script);
    // Script messages: kept quiet, but the last one is told when the script exits on an error.
    auto lastMessage = std::make_shared<std::string>();
    for (const char* quiet : {"_ScriptMessage", "_OuputDebug", "_CustomScriptMessage", "DumpCallStack"})
        vm.setFunction(quiet, [lastMessage, this](const Values& a) {
            if (!a.empty() && !a[0].isNil()) *lastMessage = a[0].str();
            static const bool verbose = std::getenv("UAW_SCRIPT_DEBUG") != nullptr;
            if (verbose && log && !a.empty()) log("message: " + a[0].str());
            return Values{};
        });
    vm.setFunction("_MessagePopup", [say](const Values& a) {
        say("script message: " + (a.empty() ? std::string() : a[0].str()));
        return Values{};
    });
    vm.setFunction("_ScriptExit", [&I, say, lastMessage](const Values&) {
        say("script exits after: " + *lastMessage);
        I.finished = true;
        return Values{};
    });
    LuaValue threadValue = table();
    callable(threadValue, [&I, running](const Values& a) {
        const LuaValue co = running();
        auto& m = I.threadValues[co.co.get()];
        auto it = m.find(a.empty() ? "" : a[0].str());
        return it == m.end() ? Values{} : Values{it->second};
    });
    def(threadValue, "Set", [&I, running](const Values& a) {
        if (!a.empty()) I.threadValues[running().co.get()][a[0].str()] = a.size() > 1 ? a[1] : LuaValue();
        return Values{};
    });
    def(threadValue, "Reset", [&I](const Values&) {
        I.threadValues.clear();
        return Values{};
    });
    vm.setGlobal("ThreadValue", threadValue);
    LuaValue thread = table();
    def(thread, "Is_Pumping_Thread", [&I, running](const Values&) {
        return Values{LuaValue::boolean(running().co && running().co == I.main.co)};
    });
    vm.setGlobal("Thread", thread);
    vm.setFunction("GetThreadID", [&I, running](const Values&) {
        const LuaValue co = running();
        for (const auto& t : I.threads)
            if (t.co.co == co.co) return Values{LuaValue::number(t.id)};
        return Values{LuaValue::number(0)};
    });
    LuaValue getEvent = table();
    callable(getEvent, [&I](const Values&) {
        if (I.events.empty()) {
            I.lastParams = LuaValue();
            return Values{};
        }
        auto [fn, args] = std::move(I.events.front());
        I.events.pop_front();
        LuaValue params = table();
        for (size_t k = 0; k < args.size(); ++k) params.t->set(LuaValue::number(double(k + 1)), args[k]);
        I.lastParams = params;
        return Values{fn};
    });
    def(getEvent, "Params", [&I](const Values&) { return Values{I.lastParams}; });
    def(getEvent, "Reset", [&I](const Values&) {
        I.events.clear();
        return Values{};
    });
    vm.setGlobal("GetEvent", getEvent);
    LuaValue createThread = table();
    callable(createThread, [&I, &vm, say](const Values& a) {
        const LuaValue fn = a.empty() ? LuaValue() : a[0].isString() ? vm.global(a[0].s) : a[0];
        if (fn.isNil()) {
            say("Create_Thread: no function " + (a.empty() ? std::string() : a[0].str()));
            return Values{};
        }
        const Values co = vm.call(vm.global("coroutine").get("create"), {fn});
        if (co.empty()) return Values{};
        const int tid = I.nextThread++;
        I.threads.push_back({tid, co[0], a.size() > 1 ? a[1] : LuaValue(), false});
        return Values{LuaValue::number(tid)};
    });
    def(createThread, "Kill", [&I](const Values& a) {
        const int tid = int(num(a, 0, -1));
        I.threads.erase(std::remove_if(I.threads.begin(), I.threads.end(), [tid](const auto& t) { return t.id == tid; }), I.threads.end());
        return Values{};
    });
    vm.setGlobal("Create_Thread", createThread);
    vm.setFunction("Find_Object_Type", [&w, &I](const Values& a) {
        return Values{a.empty() ? LuaValue() : I.type(w.db().find(a[0].str()))};
    });
    // ---- common engine globals
    vm.setFunction("Is_Campaign_Game", [](const Values&) { return Values{LuaValue::boolean(false)}; });
    vm.setFunction("Get_Game_Mode", [](const Values&) { return Values{LuaValue::string("Land")}; });
    vm.setFunction("Create_Wide_String", [](const Values& a) { return Values{LuaValue::string(a.empty() ? "" : a[0].str())}; });
    vm.setFunction("Get_Game_Text", [this](const Values& a) {
        const std::string key = a.empty() ? "" : a[0].str();
        const std::string* t = text ? text->get(key) : nullptr;
        return Values{LuaValue::string(t ? *t : key)};
    });
    vm.setFunction("Create_Position", [position](const Values& a) {
        return Values{position({float(num(a, 0)), float(num(a, 1)), float(num(a, 2))})};
    });
    vm.setFunction("Find_Player", [&w, &I, this](const Values& a) {
        const std::string want = a.empty() ? "" : upper(a[0].str());
        for (const Player& p : w.players())
            if ((want == "LOCAL" && isHuman(p.id)) || upper(p.faction) == want) return Values{I.player(p.id)};
        return Values{};
    });
    // Objects of a type (handle or name) or with a category, optionally only one player's.
    auto findAll = [&w, &I](const Values& a, bool first) {
        LuaValue list = table();
        if (a.empty()) return list;
        std::string want;
        if (a[0].isTable()) want = a[0].t->get(LuaValue::string("__type")).str();
        else want = a[0].str();
        int owner = -1;
        if (a.size() > 1 && a[1].isTable())
            if (const LuaValue gid = a[1].t->get(LuaValue::string("Get_ID")); gid.type == LuaValue::Type::Native) {
                const Values r = (*gid.native)({});
                owner = r.empty() ? -1 : int(r[0].n);
            }
        const std::string wantUp = upper(want);
        double n = 0;
        for (const Unit& u : w.units()) {
            if (!u.alive() || (owner >= 0 && u.owner != owner)) continue;
            bool match = upper(u.type->name) == wantUp;
            if (!match && !a[0].isTable())
                if (const std::string* mask = u.type->find("CategoryMask")) match = upper(*mask).find(wantUp) != std::string::npos;
            if (!match) continue;
            list.t->set(LuaValue::number(++n), I.object(u.id));
            if (first) break;
        }
        return list;
    };
    vm.setFunction("Find_All_Objects_Of_Type", [findAll](const Values& a) { return Values{findAll(a, false)}; });
    vm.setFunction("Find_First_Object", [findAll](const Values& a) {
        const LuaValue l = findAll(a, true);
        return Values{l.t->get(LuaValue::number(1))};
    });

    // Create_Generic_Object(type, position, player): a new object, as its handle.
    vm.setFunction("Create_Generic_Object", [&w, &I, whereOf, say](const Values& a) {
        if (a.size() < 3 || !a[0].isTable()) return Values{};
        const std::string typeName = a[0].t->get(LuaValue::string("__type")).str();
        Vec3 at;
        if (!whereOf(a[1], at)) return Values{};
        int owner = -1;
        if (a[2].isTable())
            if (const LuaValue gid = a[2].t->get(LuaValue::string("Get_ID")); gid.type == LuaValue::Type::Native) {
                const Values r = (*gid.native)({});
                owner = r.empty() ? -1 : int(r[0].n);
            }
        const uint32_t id = w.spawn(typeName, owner, at);
        if (!id) {
            say("Create_Generic_Object: cannot create " + typeName);
            return Values{};
        }
        say("created " + typeName);
        return Values{I.object(id)};
    });
    vm.onMissingGlobal = [say, this](const std::string& g) {
        if (log && library_.engineGlobals().count(g)) say("unknown engine global " + g); // not a script's own, unset yet
        return LuaValue();
    };

    // ---- load the behaviours and start
    std::string err;
    int loaded = 0;
    for (const std::string& b : behaviors) {
        std::string e;
        if (library_.load(vm, b, &e)) ++loaded;
        else err += (err.empty() ? "" : "; ") + b + ": " + e;
    }
    if (!loaded) {
        if (error) *error = err;
        return false;
    }
    // The object's script data (Lua_Script_Data: assignments such as MatterEngine = {...}).
    if (const GameObjectType* t = u ? u->type : nullptr)
        if (const std::string* data = t->find("Lua_Script_Data"); data && !trim(*data).empty()) {
            std::string e;
            if (!vm.runSource(*data, &e)) say("Lua_Script_Data: " + e);
        }
    // When main() has nothing to service it returns; events are then pumped by this loop instead.
    vm.runSource("function __Pump_Loop() while true do PumpEvents() end end");
    for (const char* fn : {"Base_Definitions", playerId >= 0 ? "Definitions" : "Init_Behaviors"})
        if (const LuaValue f = vm.global(fn); !f.isNil()) vm.call(f, {});
    if (const LuaValue m = vm.global("main"); !m.isNil()) {
        const Values co = vm.call(vm.global("coroutine").get("create"), {m});
        if (!co.empty()) I.main = co[0];
    }
    instances_.push_back(std::move(in));
    return true;
}

void BehaviorHost::service(Instance& I) {
    LuaVM& vm = *I.vm;
    if (I.playerId >= 0) { // a player's script: just its threads
        runThreads(I);
        return;
    }
    const Unit* self = world_.unit(I.id);
    if (!self || !self->alive()) {
        if (!I.dead) {
            I.dead = true;
            if (const LuaValue f = vm.global("Health_At_Zero"); !f.isNil()) vm.call(f, {});
        }
        I.finished = true;
        return;
    }
    // Objects coming into range: their callbacks wait in the event queue for PumpEvents.
    const LuaValue me = vm.global("Object");
    for (Instance::RangeEvent& r : I.ranges) {
        std::set<uint32_t> now;
        for (const Unit& u : world_.units()) {
            if (u.id == I.id || !u.alive()) continue;
            if (std::hypot(u.position[0] - self->position[0], u.position[1] - self->position[1]) > r.range + u.radius) continue;
            now.insert(u.id);
            if (!r.inside.count(u.id)) {
                I.events.push_back({r.fn, {me, I.object(u.id)}});
            }
        }
        r.inside = std::move(now);
    }
    for (Instance::HardPointWatch& h : I.hardpointDeaths) { // a hardpoint destroyed
        const Unit* u = world_.unit(h.id);
        const bool gone = !u || h.slot >= u->hardpoints.size() || !u->hardpoints[h.slot].alive();
        if (gone && !h.fired) {
            h.fired = true;
            const auto it = I.hardpoints.find(uint64_t(h.id) << 8 | h.slot);
            I.events.push_back({h.fn, {it != I.hardpoints.end() ? it->second : LuaValue()}});
        }
    }
    if (self->owner != I.owner) {
        I.owner = self->owner;
        auto range = I.signals.equal_range("OBJECT_OWNER_CHANGED");
        for (auto it = range.first; it != range.second; ++it) I.events.push_back({it->second, {me}});
    }
    runThreads(I);
}

void BehaviorHost::runThreads(Instance& I) {
    LuaVM& vm = *I.vm;
    const LuaValue resume = vm.global("coroutine").get("resume");
    const LuaValue status = vm.global("coroutine").get("status");
    auto say = [&](const std::string& m) {
        if (log && I.said.insert(m).second) log(m);
    };
    if (I.firstService) {
        I.firstService = false;
        if (const LuaValue f = vm.global("First_Service"); !f.isNil()) vm.call(f, {});
    }
    if (!I.main.isNil()) {
        const Values st = vm.call(status, {I.main});
        if (!st.empty() && st[0].str() == "dead") { // main() is over: keep pumping events
            const Values co = vm.call(vm.global("coroutine").get("create"), {vm.global("__Pump_Loop")});
            I.main = co.empty() ? LuaValue() : co[0];
        }
        const Values r = vm.call(resume, {I.main});
        if (!r.empty() && !r[0].truthy()) say("main: " + (r.size() > 1 ? r[1].str() : std::string("stopped")));
        if (vm.failed()) say("main: " + vm.lastError());
    }
    for (size_t k = 0; k < I.threads.size(); ++k) {
        const LuaValue co = I.threads[k].co; // (a thread may start others: the vector can grow)
        const bool started = I.threads[k].started;
        I.threads[k].started = true;
        const Values r = started ? vm.call(resume, {co}) : vm.call(resume, {co, I.threads[k].param});
        if (!r.empty() && !r[0].truthy()) say("thread: " + (r.size() > 1 ? r[1].str() : std::string("stopped")));
        if (k >= I.threads.size() || I.threads[k].co.co != co.co) continue; // killed meanwhile
        const Values st = vm.call(status, {co});
        if (!st.empty() && st[0].str() == "dead") {
            I.threads.erase(I.threads.begin() + long(k));
            --k;
        }
    }
}

void BehaviorHost::update() {
    if (world_.tickCount() % 3 != 0) return; // 10 services a second
    for (size_t i = 0; i < instances_.size(); ++i) // (player scripts may start meanwhile)
        if (!instances_[i]->finished) service(*instances_[i]);
}

} // namespace alamo
