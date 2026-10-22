#include "alamo/sfx.hpp"
#include "alamo/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace alamo {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string trimmed(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!w.empty()) out.push_back(w), w.clear();
        } else {
            w += c;
        }
    }
    if (!w.empty()) out.push_back(w);
    return out;
}

bool yes(const std::string& s) {
    const std::string u = upper(trimmed(s));
    return u == "YES" || u == "TRUE" || u == "1";
}

} // namespace

void SfxLibrary::parse(std::string_view xml) {
    XmlDocument doc = parseXml(xml);
    std::vector<const XmlNode*> found;
    for (const auto& top : doc.root.children) {
        if (top->tag == "SFXEvent") found.push_back(top.get());
        for (const XmlNode* c : top->childrenNamed("SFXEvent")) found.push_back(c);
    }
    for (const XmlNode* n : found) {
        const std::string* name = n->attr("Name");
        if (!name) continue;
        SfxEvent e;
        if (const XmlNode* p = n->child("Use_Preset"))
            if (const SfxEvent* preset = find(trimmed(p->text))) e = *preset; // presets come first
        e.name = trimmed(*name);
        for (const auto& c : n->children) {
            const std::string tag = upper(c->tag);
            const std::string& v = c->text;
            const float f = float(std::atof(v.c_str()));
            if (tag == "SAMPLES") e.samples = words(v);
            else if (tag == "PRE_SAMPLES") e.preSamples = words(v);
            else if (tag == "POST_SAMPLES") e.postSamples = words(v);
            else if (tag == "MIN_VOLUME") e.minVolume = f;
            else if (tag == "MAX_VOLUME") e.maxVolume = f;
            else if (tag == "MIN_PITCH") e.minPitch = f;
            else if (tag == "MAX_PITCH") e.maxPitch = f;
            else if (tag == "PROBABILITY") e.probability = f;
            else if (tag == "MAX_INSTANCES") e.maxInstances = int(f);
            else if (tag == "PRIORITY") e.priority = int(f);
            else if (tag == "IS_3D") e.is3D = yes(v);
            else if (tag == "IS_2D") e.is3D = e.is3D && !yes(v);
            else if (tag == "IS_GUI") e.isGUI = yes(v);
            else if (tag == "IS_UNIT_RESPONSE_VO") e.unitResponse = yes(v);
            else if (tag == "PLAY_SEQUENTIALLY") e.playSequentially = yes(v);
            else if (tag == "LOCALIZE") e.localize = yes(v);
            else if (tag == "VOLUME_SATURATION_DISTANCE") e.saturationDistance = f;
            else if (tag == "MIN_PREDELAY") e.minPredelay = f / 1000.f;
            else if (tag == "MAX_PREDELAY") e.maxPredelay = f / 1000.f;
        }
        events_[upper(e.name)] = std::move(e);
    }
}

void SfxLibrary::load(MegVfs& vfs) {
    std::vector<std::string> files;
    try {
        for (const auto& v : vfs.readAllLayers("Data\\XML\\SFXEventFiles.xml")) {
            XmlDocument doc = parseXml(std::string(v.data.begin(), v.data.end()));
            for (const auto& top : doc.root.children)
                for (const XmlNode* f : top->childrenNamed("File")) {
                    const std::string name = trimmed(f->text);
                    if (!name.empty() &&
                        std::find_if(files.begin(), files.end(), [&](const std::string& x) { return upper(x) == upper(name); }) == files.end())
                        files.push_back(name);
                }
        }
    } catch (const std::exception&) {
    }
    for (const std::string& f : files) {
        try {
            for (const auto& v : vfs.readAllLayers("Data\\XML\\" + f)) parse(std::string_view(reinterpret_cast<const char*>(v.data.data()), v.data.size()));
        } catch (const std::exception&) {
        }
    }
}

const SfxEvent* SfxLibrary::find(std::string_view name) const {
    auto it = events_.find(upper(trimmed(name)));
    return it == events_.end() ? nullptr : &it->second;
}

} // namespace alamo
