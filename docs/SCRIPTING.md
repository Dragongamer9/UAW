# The game's Lua scripts: what a scripting host has to provide

Notes for the scripting host (not written yet). Everything here was found by running the game's own
scripts against stand-ins with `objdump <game> scripts trace NAME`. Each engine global, and each
method called on what it returns, is recorded and answers with another stand-in.

## Inventory

`objdump <game> scripts` loads all 428 scripts under `Data/Scripts` (421 compiled, 7 mod sources) and
lists the globals they read that no script defines: **623 engine globals and 1,743 method names**. The
most used are `Find_Player`, `Get_Game_Text`, `Play_SFX_Event`, `Find_Object_Type`, `Create_Thread` and
`GetCurrentTime`, and the methods `Set_Hidden`, `Register_Event_Handler` and `Get_Owner`. The GUI
scripts make up most of the method list.

Lua's own library is complete in the VM (`engine/src/lualib.cpp`). `require` comes from
`ScriptLibrary` (`alamo/scripts.hpp`), which looks a script up by name in every `Data/Scripts` folder.
The scripts also read `Math` (with `Math.mod`, Lua 5.0's name for `math.fmod`) and check `jit`.

## How an object's behaviour scripts run

Objects name their behaviours in XML: `Lua_Land_Behaviors` lists them, `Lua_Script_Data` holds the
data, and `Lua_Script` names a single script for `LuaScriptBehaviorType` objects. Each behaviour script
looks like this:

```lua
require("PGBehaviors")            -- the framework (Data/Scripts/Library)
local my_behavior = { Name = _REQUIREDNAME, Init = ..., Service = ..., First_Service = ... }
Register_Behavior(my_behavior)    -- stored in BehaviorNameTable by name
```

Each object gets its own Lua state. The globals are per object, and `Object` is the object itself.

The traces show this call order from the engine:

1. **Load the scripts.** The engine sets `_REQUIREDNAME` to each behaviour's name and runs it.
2. **Set the globals.**
   - `Object`: the object itself.
   - `BehaviorList`: the object's behaviour names, as a Lua array.
   - The data from `Lua_Script_Data`, such as `MatterEngine = {...}`.
3. **`Base_Definitions()`** resets the event and thread tables (`GetEvent.Reset()`, `ThreadValue.Reset()`).
4. **`Init_Behaviors()`** marks every behaviour named in `BehaviorList` active and calls its `Init`. Init:
   - registers event callbacks such as `Object.Event_Object_In_Range(fn, range)` and `Object.Register_Signal_Handler(fn, "OBJECT_OWNER_CHANGED")`;
   - reads its data;
   - spreads service times with `GameRandom.Get_Float()` and `Math.mod()`;
   - stores async data with `Script.Set_Async_Data()`.
5. **`main()`** is the script's thread. It loops pumping events:
   - `GetEvent()` returns the next event, `GetEvent.Params()` its arguments;
   - `Thread.Is_Pumping_Thread()` and `Object.Service_Wrapper()` are also involved;
   - between events it sleeps (`Sleep`, which yields).
6. **`First_Service()`, then `Service()`, run on the service rate.** The engine asks first through
   `Does_Object_Need_Behavior_Service()`. Each call reaches every active behaviour's function of the
   same name through `Call_Behavior_Function`.
7. **Other entry points**, called by the engine on these events:
   - `Health_At_Zero` when the object dies;
   - `Delete_Pending` when it is about to be removed;
   - `Refresh_After_Mode_Switch` after a mode switch;
   - `Post_Load_Game` after a saved game is loaded.

Threads (`Create_Thread("Function_Name")`, `Sleep`, `BlockOnCommand`) map onto the VM's coroutines.

## What some behaviours reach

| Behaviour | Engine API reached on start-up |
|---|---|
| CreditPowerUp | `Object.Event_Object_In_Range`; in range: `Get_Owner().Is_Human()`, `Give_Money`, `Despawn` |
| AlienReaperTurret | `Object.Get_Owner`, `Is_Valid`, `Can_Move`, `Get_Attribute_Value`, `Is_AI_Player`, `Is_AI_Recruited`, `GetCurrentTime` |
| NovusInputStation | `Object.Get_Type().Get_Type_Value`, `Register_Signal_Handler`, `Find_Object_Type`, `Create_Thread` |
| PatchBehavior | `Object.Is_Valid`, `Get_Owner().Is_Valid`, `Register_Signal_Handler` (calls the player script: `Get_Script().Call_Function("Register_Patch_Enabler")`) |

Several behaviours call into the owning player's script (`Get_Owner().Get_Script().Call_Function(...)`),
for example `Novus_Player.lua` and `Military_Player.lua`. So player scripts have to run too.

The engine re-implements the economy behaviours natively (Reapers, Matter Engines, Input Stations,
collectors and supply depots; see `engine/src/economy.cpp`). Those are the first candidates to switch
over to their scripts once the host exists, because the native versions give a result to compare
against.
