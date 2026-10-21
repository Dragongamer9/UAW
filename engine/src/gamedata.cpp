#include "alamo/gamedata.hpp"

#include <cctype>
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
    if (auto* v = get(leaf)) return v;
    for (const auto& f : fields_)
        if (iequals(lastSegment(f.path), leaf)) return &f.values.back();
    return nullptr;
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

std::vector<std::string> mountGame(MegVfs& vfs, const std::filesystem::path& gameRoot) {
    vfs.setLooseRoot(gameRoot);
    std::vector<std::string> missing;
    XmlDocument doc = parseXml(readText(vfs, "Data\\MegaFiles.xml"));
    const XmlNode* list = doc.root.child("Mega_Files");
    if (!list) throw MegError("MegaFiles.xml has no <Mega_Files> element");
    for (const XmlNode* f : list->childrenNamed("File")) {
        std::string rel = cleanIncludePath(f->text);
        // Resolve case-insensitively on disk.
        std::filesystem::path cur = gameRoot;
        bool ok = true;
        std::string part;
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
        if (ok && std::filesystem::is_regular_file(cur)) vfs.mount(cur);
        else missing.push_back(rel);
    }
    return missing;
}

void GameDatabase::load(MegVfs& vfs) {
    XmlDocument list = parseXml(readText(vfs, "Data\\XML\\GameObjectFiles.xml"));
    const XmlNode* root = list.root.child("Game_Object_Files");
    if (!root) throw MegError("GameObjectFiles.xml has no <Game_Object_Files> element");
    for (const XmlNode* f : root->childrenNamed("File")) {
        std::string path = "Data\\XML\\" + trim(f->text);
        try {
            loadObjectFile(vfs, path);
        } catch (const std::exception& e) {
            warnings_.push_back(path + ": " + e.what());
        }
    }
    resolveVariants();
    for (size_t i = 0; i < objects_.size(); ++i) byCrc_[crc32(upper(objects_[i].name))] = i;

    if (vfs.exists("Data\\XML\\Factions.xml")) {
        XmlDocument fd = parseXml(readText(vfs, "Data\\XML\\Factions.xml"));
        if (const XmlNode* fr = fd.root.child("Factions")) {
            for (const XmlNode* f : fr->childrenNamed("Faction")) {
                Faction fac;
                if (const std::string* n = f->attr("Name")) fac.name = trim(*n);
                auto yes = [&](const char* tag) {
                    const XmlNode* c = f->child(tag);
                    return c && (iequals(c->text, "yes") || iequals(c->text, "true") || c->text == "1");
                };
                fac.humanPlayable = yes("Is_Human_Playable");
                fac.aiPlayable = yes("Is_AI_Playable");
                factions_.push_back(std::move(fac));
            }
        }
    }
}

void GameDatabase::loadObjectFile(MegVfs& vfs, const std::string& path) {
    auto resolver = [&](std::string_view inc) -> std::optional<std::string> {
        std::string p = cleanIncludePath(inc);
        if (!vfs.exists(p)) return std::nullopt;
        return readText(vfs, p);
    };
    XmlDocument doc = parseXml(readText(vfs, path), resolver);
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
            if (const XmlNode* v = el->child("Variant_Of_Existing_Type")) obj.variantOf = trim(v->text);
            flatten(*el, "", obj);

            std::string key = upper(obj.name);
            auto it = byName_.find(key);
            if (it != byName_.end()) {
                warnings_.push_back("duplicate object " + obj.name + " in " + path + " (first in " +
                                    objects_[it->second].sourceFile + "); later definition wins");
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

const GameObjectType* GameDatabase::findByCrc(uint32_t crc) const {
    auto it = byCrc_.find(crc);
    return it == byCrc_.end() ? nullptr : &objects_[it->second];
}

const GameObjectType* GameDatabase::find(std::string_view name) const {
    auto it = byName_.find(upper(trim(name)));
    return it == byName_.end() ? nullptr : &objects_[it->second];
}

} // namespace alamo
