#include "alamo/scripts.hpp"

#include <cctype>

namespace alamo {

namespace {
std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

ScriptLibrary::ScriptLibrary(MegVfs& vfs) : vfs_(vfs) {
    for (const std::string& path : vfs.listDirectory("Data\\Scripts")) { // archives and loose files
        const std::string up = upper(path);
        if (up.rfind("DATA\\SCRIPTS\\", 0) != 0 || up.size() < 4 || up.compare(up.size() - 4, 4, ".LUA") != 0) continue;
        const size_t slash = up.find_last_of('\\');
        const std::string name = up.substr(slash + 1, up.size() - slash - 5);
        // Two folders with the same name: keep the one in Library (what require means), else the first.
        auto [it, fresh] = paths_.emplace(name, path);
        if (!fresh && up.find("\\LIBRARY\\") != std::string::npos) it->second = path;
    }
}

void ScriptLibrary::install(LuaVM& vm) {
    LuaVM* v = &vm;
    vm.setFunction("require", [this, v](const std::vector<LuaValue>& a) {
        if (!a.empty()) load(*v, a[0].str());
        return std::vector<LuaValue>{};
    });
}

bool ScriptLibrary::load(LuaVM& vm, const std::string& name, std::string* error) {
    std::string key = upper(name);
    if (key.size() > 4 && key.compare(key.size() - 4, 4, ".LUA") == 0) key.resize(key.size() - 4);
    if (const size_t slash = key.find_last_of("\\/"); slash != std::string::npos) key = key.substr(slash + 1);
    std::set<std::string>& done = loaded_[&vm];
    if (done.count(key)) return true;
    const auto it = paths_.find(key);
    if (it == paths_.end()) {
        if (error) *error = "no script " + name;
        return false;
    }
    done.insert(key); // before running: scripts that require each other load once
    const std::vector<uint8_t> bytes = vfs_.read(it->second);
    const LuaValue previous = vm.global("_REQUIREDNAME");
    vm.setGlobal("_REQUIREDNAME", LuaValue::string(name));
    // A nested require runs inside the caller's run: keep the caller's step count going.
    const bool ok = !bytes.empty() && bytes[0] == 0x1b
                        ? vm.run(bytes, error)
                        : vm.runSource(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), error);
    vm.setGlobal("_REQUIREDNAME", previous);
    return ok;
}

const std::set<std::string>& ScriptLibrary::engineGlobals() {
    if (engineDone_) return engine_;
    engineDone_ = true;
    std::set<std::string> read, assigned;
    for (const auto& [name, path] : paths_) {
        const std::vector<uint8_t> bytes = vfs_.read(path);
        const auto main = !bytes.empty() && bytes[0] == 0x1b
                              ? loadLuaChunk(bytes)
                              : compileLua(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!main) continue;
        std::vector<const LuaProto*> todo{main.get()};
        while (!todo.empty()) {
            const LuaProto& p = *todo.back();
            todo.pop_back();
            for (const auto& c : p.protos) todo.push_back(c.get());
            for (const uint32_t i : p.code) {
                const uint32_t op = i & 0x3F, bx = i >> 14;
                if ((op == 5 || op == 7) && bx < p.constants.size() && p.constants[bx].isString())
                    (op == 5 ? read : assigned).insert(p.constants[bx].s);
            }
        }
    }
    LuaVM vm;
    for (const std::string& n : read)
        if (!assigned.count(n) && vm.global(n).isNil()) engine_.insert(n);
    return engine_;
}

} // namespace alamo
