#include "alamo/techtree.hpp"

#include "alamo/xml.hpp"

#include <cctype>

namespace alamo {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> names(const XmlNode* n) {
    std::vector<std::string> out;
    if (!n) return out;
    std::string cur;
    auto flush = [&]() {
        std::string t = trim(cur);
        if (!t.empty() && !iequals(t, "null")) out.push_back(t);
        cur.clear();
    };
    for (char c : n->text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    return out;
}

bool containsCI(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (iequals(x, s)) return true;
    return false;
}

} // namespace

void TechTree::add(const std::string& object, BuildOption option) {
    const std::string key = upper(object);
    auto it = byObject_.find(key);
    if (it == byObject_.end()) {
        order_.push_back(object);
        byObject_[key].push_back(std::move(option));
    } else {
        it->second.push_back(std::move(option));
    }
}

const std::vector<BuildOption>& TechTree::options(const std::string& object) const {
    static const std::vector<BuildOption> none;
    auto it = byObject_.find(upper(object));
    return it == byObject_.end() ? none : it->second;
}

std::vector<std::string> TechTree::producibleBy(const std::string& producer, const Has& has) const {
    std::vector<std::string> out;
    for (const std::string& obj : order_) {
        for (const BuildOption& o : options(obj)) {
            if (!containsCI(o.builders, producer)) continue;
            bool ok = true;
            for (const auto& b : o.builders) ok = ok && (iequals(b, producer) || has(b));
            for (const auto& r : o.required) ok = ok && has(r);
            if (ok) { out.push_back(obj); break; }
        }
    }
    return out;
}

std::vector<std::string> TechTree::constructibleBy(const std::string& builder, const Has& has) const {
    std::vector<std::string> out;
    for (const std::string& obj : order_) {
        for (const BuildOption& o : options(obj)) {
            if (!o.builders.empty() || !containsCI(o.required, builder)) continue;
            bool ok = true;
            for (const auto& r : o.required) ok = ok && (iequals(r, builder) || has(r));
            if (ok) { out.push_back(obj); break; }
        }
    }
    return out;
}

void TechTree::replace(const std::string& object, std::vector<BuildOption> options) {
    const std::string key = upper(object);
    if (!byObject_.count(key)) order_.push_back(object);
    byObject_[key] = std::move(options);
}

TechTree loadTechTree(MegVfs& vfs) {
    // Each layer's ProductionDependencies.xml, bottom to top. A layer that mentions an
    // object replaces every way of building it from the layers below; objects it doesn't
    // mention keep their lower-layer entries (so mods' additions add up).
    TechTree tree;
    for (const auto& v : vfs.readAllLayers("Data\\XML\\ProductionDependencies.xml")) {
        XmlDocument doc = parseXml(std::string(v.data.begin(), v.data.end()));
        const XmlNode* root = doc.root.child("ProductionDependencies");
        if (!root) continue;
        std::vector<std::string> order;
        std::unordered_map<std::string, std::vector<BuildOption>> layer;
        for (const XmlNode* d : root->childrenNamed("Dependency")) {
            const auto obj = names(d->child("Built_Object"));
            if (obj.empty()) continue;
            BuildOption o;
            o.builders = names(d->child("Tactical_Builder"));
            o.required = names(d->child("Tactical_Required_Objects"));
            if (o.builders.empty() && o.required.empty()) continue; // strategic-only entry
            const std::string key = upper(obj.front());
            if (!layer.count(key)) order.push_back(obj.front());
            layer[key].push_back(std::move(o));
        }
        for (const auto& name : order) tree.replace(name, std::move(layer[upper(name)]));
    }
    return tree;
}

} // namespace alamo
