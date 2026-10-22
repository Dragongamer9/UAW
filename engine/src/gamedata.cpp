#include "alamo/gamedata.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <functional>
#include <unordered_set>

namespace alamo {

namespace {

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = char(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

std::string lastSegment(std::string_view path) {
    size_t dot = path.rfind('.');
    // Ignore dots inside a [Name] bracket.
    size_t br = path.rfind(']');
    if (dot != std::string_view::npos && br != std::string_view::npos && dot < br) dot = std::string_view::npos;
    return std::string(dot == std::string_view::npos ? path : path.substr(dot + 1));
}

// Strip "./" or ".\" prefixes the INCLUDE directives use.
std::string cleanIncludePath(std::string_view p) {
    std::string s = trim(p);
    while (s.size() >= 2 && s[0] == '.' && (s[1] == '\\' || s[1] == '/')) s.erase(0, 2);
    return s;
}

void flatten(const XmlNode& node, const std::string& prefix, GameObjectType& obj) {
    for (const auto& c : node.children) {
        const std::string* subName = c->attr("Name");
        if (c->children.empty() && !subName) {
            obj.addValue(prefix + c->tag, c->text);
            continue;
        }
        std::string p = prefix + c->tag;
        if (subName) p += "[" + trim(*subName) + "]";
        if (!c->text.empty()) obj.addValue(p, c->text);
        flatten(*c, p + ".", obj);
    }
}

} // namespace

// ---------------------------------------------------------------- GameObjectType

void GameObjectType::addValue(const std::string& path, std::string value) {
    findCache_.clear();
    std::string key = upper(path);
    auto it = index_.find(key);
    if (it == index_.end()) {
        index_.emplace(key, fields_.size());
        fields_.push_back({path, {std::move(value)}});
    } else {
        fields_[it->second].values.push_back(std::move(value));
    }
}

void GameObjectType::overlay(const GameObjectType& other) {
    findCache_.clear();
    for (const auto& f : other.fields_) {
        std::string key = upper(f.path);
        auto it = index_.find(key);
        if (it == index_.end()) {
            index_.emplace(key, fields_.size());
            fields_.push_back(f);
        } else {
            fields_[it->second].values = f.values;
        }
    }
}

const std::string* GameObjectType::get(std::string_view path) const {
    auto it = index_.find(upper(path));
    return it == index_.end() ? nullptr : &fields_[it->second].values.back();
}

const std::string* GameObjectType::find(std::string_view leaf) const {
    std::string key = upper(leaf);
    if (auto it = findCache_.find(key); it != findCache_.end())
        return it->second < 0 ? nullptr : &fields_[size_t(it->second)].values.back();
    long found = -1;
    if (auto it = index_.find(key); it != index_.end()) found = long(it->second);
    for (size_t i = 0; found < 0 && i < fields_.size(); ++i)
        if (iequals(lastSegment(fields_[i].path), leaf)) found = long(i);
    findCache_.emplace(std::move(key), found);
    return found < 0 ? nullptr : &fields_[size_t(found)].values.back();
}

std::optional<double> GameObjectType::number(std::string_view leaf) const {
    const std::string* v = find(leaf);
    if (!v) return std::nullopt;
    std::string s = trim(*v);
    if (!s.empty() && (s.back() == 'f' || s.back() == 'F')) s.pop_back(); // "3.0f" appears in the data
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return std::nullopt;
    return d;
}

// ---------------------------------------------------------------- loading

std::string readText(MegVfs& vfs, std::string_view path) {
    auto bytes = vfs.read(path);
    return std::string(bytes.begin(), bytes.end());
}

namespace {

// Case-insensitive lookup of a backslash path under `root`; empty when absent.
std::filesystem::path resolveOnDisk(const std::filesystem::path& root, const std::string& rel) {
    std::filesystem::path cur = root;
    std::string part;
    bool ok = true;
    auto step = [&](const std::string& name) {
        if (name.empty() || !ok) return;
        std::error_code ec;
        for (const auto& d : std::filesystem::directory_iterator(cur, ec))
            if (iequals(d.path().filename().string(), name)) { cur = d.path(); return; }
        ok = false;
    };
    for (char c : rel) {
        if (c == '\\' || c == '/') { step(part); part.clear(); }
        else part += c;
    }
    step(part);
    std::error_code ec;
    return ok && std::filesystem::is_regular_file(cur, ec) ? cur : std::filesystem::path{};
}

std::vector<std::string> megaFileList(const std::vector<uint8_t>& bytes) {
    std::vector<std::string> out;
    XmlDocument doc = parseXml(std::string(bytes.begin(), bytes.end()));
    const XmlNode* list = doc.root.child("Mega_Files");
    if (!list) throw MegError("MegaFiles.xml has no <Mega_Files> element");
    for (const XmlNode* f : list->childrenNamed("File")) out.push_back(cleanIncludePath(f->text));
    return out;
}

} // namespace

std::vector<std::string> mountGame(MegVfs& vfs, const std::filesystem::path& gameRoot,
                                   const std::vector<std::filesystem::path>& mods) {
    // Base layer: the game folder and its MegaFiles.xml archives.
    vfs.setLooseRoot(gameRoot);
    std::vector<std::string> missing;
    std::vector<std::string> mounted; // archives the base list covers (normalized)
    const auto baseList = resolveOnDisk(gameRoot, "Data\\MegaFiles.xml");
    if (baseList.empty()) throw MegError("Data\\MegaFiles.xml not found in " + gameRoot.string());
    std::ifstream in(baseList, std::ios::binary);
    for (const std::string& rel : megaFileList({std::istreambuf_iterator<char>(in), {}})) {
        mounted.push_back(normalizePath(rel)); // handled (or reported) by the base layer
        if (auto p = resolveOnDisk(gameRoot, rel); !p.empty()) {
            vfs.mount(p);
        } else {
            missing.push_back(rel);
        }
    }
    // One layer per mod, bottom to top. Each mod's MegaFiles.xml repeats the base list
    // plus its own archives; the ones inside the mod folder go into its layer, any base
    // archive the game list didn't have goes into the base layer's set.
    for (const auto& mod : mods) {
        vfs.addLayer(mod, mod.filename().string());
        const auto listPath = resolveOnDisk(mod, "Data\\MegaFiles.xml");
        if (listPath.empty()) continue; // loose-file-only mod
        std::ifstream ml(listPath, std::ios::binary);
        for (const std::string& rel : megaFileList({std::istreambuf_iterator<char>(ml), {}})) {
            if (auto p = resolveOnDisk(mod, rel); !p.empty()) {
                vfs.mount(p);
            } else if (std::find(mounted.begin(), mounted.end(), normalizePath(rel)) == mounted.end()) {
                missing.push_back(mod.filename().string() + ": " + rel);
            }
        }
    }
    return missing;
}

std::vector<std::string> mountGame(MegVfs& vfs, const std::filesystem::path& gameRoot,
                                   const std::filesystem::path& modRoot) {
    std::vector<std::filesystem::path> mods;
    if (!modRoot.empty()) mods.push_back(modRoot);
    return mountGame(vfs, gameRoot, mods);
}

std::vector<std::filesystem::path> installedMods(const std::filesystem::path& gameRoot) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& dir : {gameRoot / "Mods", gameRoot.parent_path() / "Mods"}) {
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& d : std::filesystem::directory_iterator(dir, ec))
            if (d.is_directory() && !resolveOnDisk(d.path(), "Data\\MegaFiles.xml").empty()) out.push_back(d.path());
        if (!out.empty()) break;
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return normalizePath(a.filename().string()) < normalizePath(b.filename().string());
    });
    return out;
}

std::filesystem::path findMod(const std::filesystem::path& gameRoot, const std::string& mod) {
    std::filesystem::path p(mod);
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) return p;
    // A bare name: look in <game>\Mods and <game>\..\Mods, case-insensitively and by prefix.
    for (const auto& dir : {gameRoot / "Mods", gameRoot.parent_path() / "Mods"}) {
        if (!std::filesystem::is_directory(dir, ec)) continue;
        std::filesystem::path best;
        for (const auto& d : std::filesystem::directory_iterator(dir, ec)) {
            if (!d.is_directory()) continue;
            const std::string n = d.path().filename().string();
            if (iequals(n, mod)) return d.path();
            if (n.size() > mod.size() && iequals(n.substr(0, mod.size()), mod) && best.empty()) best = d.path();
        }
        if (!best.empty()) return best;
    }
    return {};
}

void GameDatabase::loadFileList(MegVfs& vfs, const std::string& listPath, const char* rootTag, const std::string& dir) {
    // Every layer's list contributes its files (union, first-seen order).
    std::vector<std::string> files;
    std::unordered_set<std::string> seen;
    for (const auto& v : vfs.readAllLayers(listPath)) {
        XmlDocument list = parseXml(std::string(v.data.begin(), v.data.end()));
        const XmlNode* root = list.root.child(rootTag);
        if (!root) {
            warnings_.push_back(v.layer + ": " + listPath + " has no <" + rootTag + "> element");
            continue;
        }
        for (const XmlNode* f : root->childrenNamed("File")) {
            std::string path = dir + trim(f->text);
            if (seen.insert(normalizePath(path)).second) files.push_back(path);
        }
    }
    if (files.empty()) throw MegError("no " + listPath);
    // Load layer by layer, so a mod's definition of an object replaces the one below it.
    for (size_t layer = 0; layer < vfs.layerCount(); ++layer) {
        for (const std::string& path : files) {
            try {
                auto data = vfs.readInLayer(layer, path);
                if (data) loadObjectFile(vfs, path, layer, std::string(data->begin(), data->end()));
            } catch (const std::exception& e) {
                warnings_.push_back(path + ": " + e.what());
            }
        }
    }
    for (const std::string& path : files)
        if (!vfs.exists(path)) warnings_.push_back(path + ": file not found");
}

void GameDatabase::loadEffects(MegVfs& vfs) {
    loadFileList(vfs, "Data\\XML\\Effects\\EffectSystemFiles.xml", "EffectSystemFiles", "Data\\XML\\Effects\\");
    resolveVariants();
}

void GameDatabase::load(MegVfs& vfs) {
    loadFileList(vfs, "Data\\XML\\GameObjectFiles.xml", "Game_Object_Files", "Data\\XML\\");
    resolveVariants();
    for (size_t i = 0; i < objects_.size(); ++i) byCrc_[crc32(upper(objects_[i].name))] = i;

    // Factions: each layer's Factions.xml, later layers replacing factions by name.
    for (const auto& v : vfs.readAllLayers("Data\\XML\\Factions.xml")) {
        XmlDocument fd = parseXml(std::string(v.data.begin(), v.data.end()));
        const XmlNode* fr = fd.root.child("Factions");
        if (!fr) continue;
        for (const XmlNode* f : fr->childrenNamed("Faction")) {
            Faction fac;
            if (const std::string* n = f->attr("Name")) fac.name = trim(*n);
            auto yes = [&](const char* tag) {
                const XmlNode* c = f->child(tag);
                return c && (iequals(trim(c->text), "yes") || iequals(trim(c->text), "true") || trim(c->text) == "1");
            };
            fac.humanPlayable = yes("Is_Human_Playable");
            fac.aiPlayable = yes("Is_AI_Playable");
            if (const XmlNode* c = f->child("Multiplayer_Starting_Credits")) fac.multiplayerCredits = float(std::atof(c->text.c_str()));
            if (const XmlNode* c = f->child("Pop_Cap_For_Multiplayer_Games")) fac.multiplayerPopCap = std::atoi(c->text.c_str());
            if (const XmlNode* c = f->child("Baseline_Bank_Capacity")) fac.baselineBankCapacity = float(std::atof(c->text.c_str()));
            auto it = std::find_if(factions_.begin(), factions_.end(), [&](const Faction& x) { return iequals(x.name, fac.name); });
            if (it != factions_.end()) *it = std::move(fac);
            else factions_.push_back(std::move(fac));
        }
    }
}

void GameDatabase::loadObjectFile(MegVfs& vfs, const std::string& path) {
    loadObjectFile(vfs, path, vfs.layerCount() ? vfs.layerCount() - 1 : 0, readText(vfs, path));
}

void GameDatabase::loadObjectFile(MegVfs& vfs, const std::string& path, size_t layer, const std::string& text) {
    // Includes resolve from this layer down, as that mod would have seen them.
    auto resolver = [&](std::string_view inc) -> std::optional<std::string> {
        auto d = vfs.readUpTo(layer, cleanIncludePath(inc));
        if (!d) return std::nullopt;
        return std::string(d->begin(), d->end());
    };
    XmlDocument doc = parseXml(text, resolver);
    for (const auto& w : doc.warnings) warnings_.push_back(path + ": " + w);
    ++fileCount_;

    for (const auto& top : doc.root.children) {        // the file's root element
        for (const auto& el : top->children) {          // object definitions
            const std::string* name = el->attr("Name");
            if (!name) continue;
            GameObjectType obj;
            obj.name = trim(*name);
            obj.category = el->tag;
            obj.sourceFile = normalizePath(path);
            obj.layer = layer;
            if (const XmlNode* v = el->child("Variant_Of_Existing_Type")) obj.variantOf = trim(v->text);
            flatten(*el, "", obj);

            std::string key = upper(obj.name);
            auto it = byName_.find(key);
            if (it != byName_.end()) {
                // Replacing a lower layer's definition is what mods do; only flag clashes within one layer.
                if (objects_[it->second].layer == layer)
                    warnings_.push_back("duplicate object " + obj.name + " in " + path + " (first in " +
                                        objects_[it->second].sourceFile + "); later definition wins");
                else
                    ++overridden_;
                objects_[it->second] = std::move(obj);
            } else {
                byName_.emplace(key, objects_.size());
                objects_.push_back(std::move(obj));
            }
        }
    }
}

void GameDatabase::resolveVariants() {
    // 0 = unresolved, 1 = in progress, 2 = done
    std::vector<int> state(objects_.size(), 0);
    std::function<void(size_t)> resolve = [&](size_t i) {
        if (state[i] == 2) return;
        if (state[i] == 1) {
            warnings_.push_back("variant cycle through " + objects_[i].name);
            return;
        }
        GameObjectType& obj = objects_[i];
        if (obj.variantOf.empty()) { state[i] = 2; return; }
        state[i] = 1;
        auto it = byName_.find(upper(obj.variantOf));
        if (it == byName_.end()) {
            warnings_.push_back(obj.name + ": base type " + obj.variantOf + " not found");
        } else {
            resolve(it->second);
            const GameObjectType& base = objects_[it->second];
            GameObjectType merged = base;
            merged.name = obj.name;
            merged.category = obj.category;
            merged.sourceFile = obj.sourceFile;
            merged.variantOf = obj.variantOf;
            merged.overlay(obj);
            obj = std::move(merged);
        }
        state[i] = 2;
    };
    for (size_t i = 0; i < objects_.size(); ++i) resolve(i);
}

const Faction* GameDatabase::faction(std::string_view name) const {
    for (const auto& f : factions_)
        if (iequals(f.name, name)) return &f;
    return nullptr;
}

const GameObjectType* GameDatabase::findByCrc(uint32_t crc) const {
    auto it = byCrc_.find(crc);
    return it == byCrc_.end() ? nullptr : &objects_[it->second];
}

const GameObjectType* GameDatabase::find(std::string_view name) const {
    auto it = byName_.find(upper(trim(name)));
    return it == byName_.end() ? nullptr : &objects_[it->second];
}

} // namespace alamo
