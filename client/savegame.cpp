#include "savegame.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace uaw {

namespace {

const char* kindName(alamo::Command::Kind k) {
    static const char* names[] = {"move", "stop", "attackmove", "attack", "queue", "cancelqueue", "rally",
                                  "construct", "superweapon", "research", "ability", "board", "unload", "sell", "patrol"};
    return names[int(k)];
}

bool kindFrom(const std::string& s, alamo::Command::Kind& k) {
    for (int i = 0; i <= int(alamo::Command::Kind::Patrol); ++i)
        if (s == kindName(alamo::Command::Kind(i))) {
            k = alamo::Command::Kind(i);
            return true;
        }
    return false;
}

const char* slotKind(PlayerSlot::Kind k) {
    return k == PlayerSlot::Kind::Human ? "human" : k == PlayerSlot::Kind::Computer ? "computer" : "open";
}

} // namespace

std::string saveFolder() {
    if (char* p = SDL_GetPrefPath("Dragongamer9", "uaw-native")) {
        std::string s = p;
        SDL_free(p);
        return s;
    }
    return "./";
}

Settings loadSettings() {
    Settings s;
    std::ifstream in(saveFolder() + "settings.txt");
    std::string key;
    float v = 0;
    while (in >> key >> v) {
        if (key == "sound") s.sound = std::clamp(v, 0.f, 1.f);
        else if (key == "music") s.music = std::clamp(v, 0.f, 1.f);
        else if (key == "graphics") s.fullGraphics = v != 0;
    }
    return s;
}

void saveSettings(const Settings& s) {
    std::ofstream out(saveFolder() + "settings.txt");
    out << "sound " << s.sound << "\nmusic " << s.music << "\ngraphics " << (s.fullGraphics ? 1 : 0) << "\n";
}

std::string encodeCommand(const alamo::Command& c) {
    std::ostringstream out;
    out << std::setprecision(9) << c.tick << " " << kindName(c.kind) << " " << c.target << " " << c.owner << " " << c.index << " "
        << c.point[0] << " " << c.point[1] << " " << c.point[2] << " " << (c.name.empty() ? "-" : c.name) << " " << c.ids.size();
    for (uint32_t id : c.ids) out << " " << id;
    return out.str();
}

bool decodeCommand(const std::string& text, alamo::Command& c) {
    std::istringstream ls(text);
    std::string kind, name;
    size_t n = 0;
    c = alamo::Command{};
    ls >> c.tick >> kind >> c.target >> c.owner >> c.index >> c.point[0] >> c.point[1] >> c.point[2] >> name >> n;
    if (!ls || !kindFrom(kind, c.kind) || n > 100000) return false;
    if (name != "-") c.name = name;
    for (size_t k = 0; k < n; ++k) {
        uint32_t id = 0;
        if (!(ls >> id)) return false;
        c.ids.push_back(id);
    }
    return true;
}

std::string encodeSetup(const SkirmishSetup& s) {
    std::ostringstream out;
    out << s.map << " " << (s.fog ? 1 : 0) << " " << s.slots.size();
    for (const PlayerSlot& p : s.slots) out << " " << slotKind(p.kind) << ":" << p.faction << ":" << p.team << ":" << p.netId;
    out << " ai:" << s.aiLevel << " credits:" << s.credits; // optional (older saves end before these)
    return out.str();
}

bool decodeSetup(const std::string& text, SkirmishSetup& s) {
    std::istringstream ls(text);
    int fog = 1;
    size_t n = 0;
    s = SkirmishSetup{};
    ls >> s.map >> fog >> n;
    if (!ls || n > 8) return false;
    s.fog = fog != 0;
    for (size_t k = 0; k < n; ++k) {
        std::string item;
        if (!(ls >> item)) return false;
        PlayerSlot p;
        std::istringstream is(item);
        std::string kind, faction, team, net;
        std::getline(is, kind, ':');
        std::getline(is, faction, ':');
        std::getline(is, team, ':');
        std::getline(is, net, ':');
        p.kind = kind == "human" ? PlayerSlot::Kind::Human : kind == "computer" ? PlayerSlot::Kind::Computer : PlayerSlot::Kind::Open;
        p.faction = faction;
        p.team = team.empty() ? -1 : std::atoi(team.c_str());
        p.netId = net.empty() ? -1 : std::atoi(net.c_str());
        s.slots.push_back(p);
    }
    for (std::string extra; ls >> extra;) {
        if (extra.rfind("ai:", 0) == 0) s.aiLevel = std::clamp(std::atoi(extra.c_str() + 3), 0, 2);
        else if (extra.rfind("credits:", 0) == 0) s.credits = std::clamp(std::atoi(extra.c_str() + 8), 0, 3);
    }
    return true;
}

bool writeSave(const std::string& path, const SaveGame& s) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << std::setprecision(9); // floats round-trip exactly (replays must match)
    out << "UAWSAVE 1\n";
    out << "map " << s.setup.map << "\n";
    out << "fog " << (s.setup.fog ? 1 : 0) << "\n";
    out << "options " << s.setup.aiLevel << " " << s.setup.credits << "\n";
    for (const PlayerSlot& p : s.setup.slots) out << "slot " << slotKind(p.kind) << " " << p.faction << " " << p.team << "\n";
    for (const std::string& m : s.mods) out << "mod " << m << "\n";
    out << "player " << s.player << " " << (s.spectate ? 1 : 0) << "\n";
    out << "camera " << s.camera[0] << " " << s.camera[1] << " " << s.camera[2] << " " << s.camera[3] << " " << s.camera[4] << "\n";
    out << "tick " << s.tick << "\n";
    for (const alamo::Command& c : s.commands) {
        out << "cmd " << c.tick << " " << kindName(c.kind) << " " << c.target << " " << c.owner << " " << c.index << " " << c.point[0] << " "
            << c.point[1] << " " << c.point[2] << " " << (c.name.empty() ? "-" : c.name) << " " << c.ids.size();
        for (uint32_t id : c.ids) out << " " << id;
        out << "\n";
    }
    out << "end\n";
    return bool(out);
}

bool readSave(const std::string& path, SaveGame& s, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("can't open " + path);
    std::string line;
    if (!std::getline(in, line) || line.rfind("UAWSAVE 1", 0) != 0) return fail("not a saved game");
    s = SaveGame{};
    bool ended = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "map") ls >> s.setup.map;
        else if (key == "fog") { int f = 1; ls >> f; s.setup.fog = f != 0; }
        else if (key == "options") {
            int ai = 1, cr = 1;
            ls >> ai >> cr;
            s.setup.aiLevel = std::clamp(ai, 0, 2);
            s.setup.credits = std::clamp(cr, 0, 3);
        }
        else if (key == "slot") {
            std::string kind;
            PlayerSlot p;
            ls >> kind >> p.faction >> p.team;
            p.kind = kind == "human" ? PlayerSlot::Kind::Human : kind == "computer" ? PlayerSlot::Kind::Computer : PlayerSlot::Kind::Open;
            s.setup.slots.push_back(p);
        } else if (key == "mod") {
            std::string m;
            std::getline(ls >> std::ws, m);
            s.mods.push_back(m);
        } else if (key == "player") {
            int spec = 0;
            ls >> s.player >> spec;
            s.spectate = spec != 0;
        } else if (key == "camera") {
            ls >> s.camera[0] >> s.camera[1] >> s.camera[2] >> s.camera[3] >> s.camera[4];
        } else if (key == "tick") {
            ls >> s.tick;
        } else if (key == "cmd") {
            alamo::Command c;
            std::string kind, name;
            size_t n = 0;
            ls >> c.tick >> kind >> c.target >> c.owner >> c.index >> c.point[0] >> c.point[1] >> c.point[2] >> name >> n;
            if (!ls || !kindFrom(kind, c.kind)) return fail("bad order: " + line);
            if (name != "-") c.name = name;
            for (size_t k = 0; k < n; ++k) {
                uint32_t id = 0;
                ls >> id;
                c.ids.push_back(id);
            }
            s.commands.push_back(std::move(c));
        } else if (key == "end") {
            ended = true;
            break;
        }
    }
    if (!ended) return fail("the file is cut short");
    if (s.setup.map.empty() || s.setup.slots.size() < 2) return fail("no map or players");
    return true;
}

} // namespace uaw
