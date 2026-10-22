// The game's Lua scripts as a library for LuaVM: `require("PGBase")` loads Data\Scripts\...\PGBase.lua
// (from any folder under Data\Scripts, compiled or source, mods' copies first) once per VM.
#pragma once

#include "alamo/luavm.hpp"
#include "alamo/meg.hpp"

#include <map>
#include <set>
#include <string>

namespace alamo {

class ScriptLibrary {
public:
    explicit ScriptLibrary(MegVfs& vfs);
    // Defines `require` (and _REQUIREDNAME while a script loads) in the VM.
    void install(LuaVM& vm);
    // Loads a script by name (no folder or extension) unless this VM already has; false if it is
    // missing or fails (the message in `error`).
    bool load(LuaVM& vm, const std::string& name, std::string* error = nullptr);
    size_t size() const { return paths_.size(); }
    const std::map<std::string, std::string>& scripts() const { return paths_; } // NAME -> path

private:
    MegVfs& vfs_;
    std::map<std::string, std::string> paths_; // upper-case base name -> VFS path
    std::map<const LuaVM*, std::set<std::string>> loaded_;
};

} // namespace alamo
