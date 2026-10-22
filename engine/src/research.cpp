// Research trees from the game's compiled tree scripts (see research.hpp).
#include <cstdlib>
#include "alamo/research.hpp"

#include "alamo/luavm.hpp"
#include "alamo/xml.hpp"

#include <algorithm>
#include <cctype>

namespace alamo {
namespace {

std::string up(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// Items of a list table {1 = {...}, 2 = {...}} in index order.
std::vector<LuaValue> items(const LuaValue& list) {
    std::vector<LuaValue> out;
    if (!list.isTable()) return out;
    for (size_t i = 1, n = list.t->length(); i <= n; ++i) out.push_back(list.get(double(i)));
    return out;
}

ResearchSuite readSuite(const std::string& branch, const LuaValue& s) {
    ResearchSuite r;
    r.branch = branch;
    r.path = s.get("Path").str();
    r.index = int(s.get("Index").n);
    r.cost = float(s.get("ResearchCost").n);
    r.seconds = float(s.get("TotalResearchTime").n);
    auto add = [&](ResearchChange::Kind kind, const char* list, const char* nameKey, const char* typeKey, bool locks) {
        for (const LuaValue& it : items(s.get(list))) {
            ResearchChange c;
            c.kind = kind;
            c.name = it.get(nameKey).str();
            if (typeKey) c.objectType = it.get(typeKey).str();
            c.locks = locks;
            if (r.title.empty() && !locks) r.title = it.get("TitleTextID").str();
            if (!c.name.empty() && c.name != "nil") r.changes.push_back(c);
            // UnlocksEffects entries may switch an older effect off (radiation -> gamma radiation).
            const LuaValue lockThis = it.get("LockThisEffect");
            if (lockThis.isString()) r.changes.push_back({ResearchChange::Kind::Effect, lockThis.s, "", true});
        }
    };
    using K = ResearchChange::Kind;
    add(K::Type, "UnlocksTypes", "ObjectType", nullptr, false);
    add(K::Type, "UnlocksPatches", "ObjectType", nullptr, false);
    add(K::Generator, "UnlocksGenerators", "EffectName", nullptr, false);
    add(K::Generator, "LocksGenerators", "EffectName", nullptr, true);
    add(K::Effect, "UnlocksEffects", "EffectName", nullptr, false);
    add(K::Effect, "LocksEffects", "EffectName", nullptr, true);
    add(K::UnitAbility, "UnlocksUnitAbilities", "AbilityName", "TypeName", false);
    add(K::UnitAbility, "LocksUnitAbilities", "AbilityName", "TypeName", true);
    add(K::SpecialAbility, "UnlocksSpecialAbilities", "AbilityName", "ObjectType", false);
    add(K::SpecialAbility, "LocksSpecialAbilities", "AbilityName", "ObjectType", true);
    return r;
}

std::string key(const ResearchChange& c) {
    if (c.kind == ResearchChange::Kind::UnitAbility || c.kind == ResearchChange::Kind::SpecialAbility)
        return up(c.objectType) + "|" + up(c.name);
    return up(c.name);
}

std::set<std::string>& listFor(ResearchLocks& l, ResearchChange::Kind k) {
    switch (k) {
    case ResearchChange::Kind::Type: return l.types;
    case ResearchChange::Kind::Generator: return l.generators;
    case ResearchChange::Kind::Effect: return l.effects;
    default: return l.abilities;
    }
}

} // namespace

const ResearchSuite* ResearchTree::find(std::string_view path, int index) const {
    for (const ResearchSuite& s : suites)
        if (iequals(s.path, path) && s.index == index) return &s;
    return nullptr;
}

std::vector<ResearchTree> loadResearchTrees(MegVfs& vfs) {
    std::vector<ResearchTree> out;
    for (const std::string& name : vfs.listDirectory("Data\\Scripts\\Miscellaneous")) {
        const std::string u = up(name);
        static const std::string kDir = "DATA\\SCRIPTS\\MISCELLANEOUS\\", kSuffix = "_PACKAGED_RESEARCH_TREE.LUA";
        if (u.rfind(kDir, 0) != 0 || u.size() <= kDir.size() + kSuffix.size() ||
            u.compare(u.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        ResearchTree tree;
        tree.faction = u.substr(kDir.size(), u.size() - kDir.size() - kSuffix.size());
        LuaVM vm;
        vm.setFunction("Find_Object_Type", [](const std::vector<LuaValue>& a) { return std::vector<LuaValue>{a.empty() ? LuaValue() : a[0]}; });
        LuaValue script = LuaValue::newTable();
        script.t->set(LuaValue::string("Set_Async_Data"), LuaValue::function([](const std::vector<LuaValue>&) { return std::vector<LuaValue>{}; }));
        vm.setGlobal("Script", script);
        const std::vector<uint8_t> chunk = vfs.read(name);
        if (chunk.size() > 4 && chunk[0] == 0x1b) { // compiled: run it
            if (!vm.run(chunk)) continue;
            vm.call(vm.global("Init_Research_Tree"), {});
        } else { // source (mods): compile and run it; if it doesn't compile, read the tables it assigns
            const std::string_view src(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            if (vm.runSource(src) && !std::getenv("UAW_LUA_DATA_ONLY")) vm.call(vm.global("Init_Research_Tree"), {});
            else vm.loadDataAssignments(src);
        }
        for (const auto& [k, v] : vm.globals().fields) {
            const std::string g = k.str();
            if (g.rfind("Branch_", 0) != 0 || !v.isTable()) continue;
            for (const auto& [sk, sv] : v.t->fields)
                if (sv.isTable() && sk.str().rfind("Suite", 0) == 0) tree.suites.push_back(readSuite(g.substr(7), sv));
        }
        if (tree.suites.empty()) continue;
        std::sort(tree.suites.begin(), tree.suites.end(), [](const ResearchSuite& a, const ResearchSuite& b) {
            return a.path != b.path ? a.path < b.path : a.index < b.index;
        });
        out.push_back(std::move(tree));
    }
    std::sort(out.begin(), out.end(), [](const ResearchTree& a, const ResearchTree& b) { return a.faction < b.faction; });
    return out;
}

ResearchLocks ResearchLocks::initial(const ResearchTree& tree) {
    ResearchLocks l;
    for (const ResearchSuite& s : tree.suites)
        for (const ResearchChange& c : s.changes)
            if (!c.locks) listFor(l, c.kind).insert(key(c));
    return l;
}

void ResearchLocks::apply(const ResearchSuite& s) {
    for (const ResearchChange& c : s.changes) {
        if (c.locks) listFor(*this, c.kind).insert(key(c));
        else listFor(*this, c.kind).erase(key(c));
    }
}

bool ResearchLocks::typeLocked(const GameObjectType& t, const GameDatabase& db) const {
    if (types.empty()) return false;
    const GameObjectType* cur = &t;
    for (int depth = 0; cur && depth < 8; ++depth) {
        if (types.count(up(cur->name))) return true;
        cur = cur->variantOf.empty() ? nullptr : db.find(cur->variantOf);
    }
    return false;
}

bool ResearchLocks::generatorLocked(std::string_view name) const { return !generators.empty() && generators.count(up(name)); }
bool ResearchLocks::effectLocked(std::string_view name) const { return !effects.empty() && effects.count(up(name)); }

bool ResearchLocks::abilityLocked(const GameObjectType& t, std::string_view ability, const GameDatabase& db) const {
    if (abilities.empty() || ability.empty()) return false;
    const GameObjectType* cur = &t;
    for (int depth = 0; cur && depth < 8; ++depth) {
        if (abilities.count(up(cur->name) + "|" + up(ability))) return true;
        cur = cur->variantOf.empty() ? nullptr : db.find(cur->variantOf);
    }
    return false;
}

} // namespace alamo
