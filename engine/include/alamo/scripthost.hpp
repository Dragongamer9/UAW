// Runs the game's own Lua behaviour scripts on simulated objects (docs/SCRIPTING.md): each object
// gets a Lua state with `Object` bound to it, its behaviours (`Lua_Land_Behaviors`, or the single
// `Lua_Script`) are loaded through PGBehaviors, and the engine side of the framework is provided:
// Base_Definitions / Init_Behaviors, the `main` thread resumed on a service rate, the event queue
// that PumpEvents drains (GetEvent), thread values, Create_Thread, and the first part of the object
// and player API. First version: enough for event-driven behaviours such as CreditPowerUp.
#pragma once

#include "alamo/luavm.hpp"
#include "alamo/scripts.hpp"
#include "alamo/sim.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alamo {

class BehaviorHost {
public:
    BehaviorHost(World& world, ScriptLibrary& library);
    ~BehaviorHost();
    // The behaviour names an object type runs (Lua_Land_Behaviors, else Lua_Script); empty if none.
    static std::vector<std::string> behaviorsOf(const GameObjectType& t);
    // Starts the behaviours on unit `id`. False (with a message) when none of them loads.
    bool attach(uint32_t id, const std::vector<std::string>& behaviors, std::string* error = nullptr);
    // Call after every simulation tick: events are checked and the scripts serviced (10 times a second).
    void update();
    size_t running() const;
    // Which players are people (Player.Is_Human); by default player 0.
    std::function<bool(int player)> isHuman = [](int p) { return p == 0; };
    // Script messages and unknown engine calls, for debugging (none by default).
    std::function<void(const std::string&)> log;

    struct Instance;

private:
    World& world_;
    ScriptLibrary& library_;
    std::vector<std::unique_ptr<Instance>> instances_;
    void service(Instance& in);
};

} // namespace alamo
