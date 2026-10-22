// simtool: exercise the simulation against real game data without a window.
//   simtool <game-root> <MAP> start            skirmish start for Novus/Hierarchy/Masari: what each can build
//   simtool <game-root> <MAP> opening FACTION  scripted build order (construct, produce) with a timeline
#include "common.hpp"
#include "alamo/combat.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/sim.hpp"
#include "alamo/techtree.hpp"
#include "alamo/ted.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

using namespace alamo;

static std::string mapPath(std::string name) {
    if (name.find('\\') == std::string::npos && name.find('/') == std::string::npos) name = "Data\\Art\\Maps\\" + name;
    if (normalizePath(name).find(".TED") == std::string::npos) name += ".ted";
    return name;
}

static std::vector<Vec3> starts(const Map& m, const GameDatabase& db) {
    std::vector<std::pair<int, Vec3>> s;
    for (const auto& o : m.objects) {
        const GameObjectType* t = db.findByCrc(o.typeCrc);
        int slot;
        if (t && std::sscanf(t->name.c_str(), "Marker_Player_%d_Start", &slot) == 1) s.push_back({slot, o.position});
    }
    std::sort(s.begin(), s.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<Vec3> out;
    for (auto& p : s) out.push_back(p.second);
    return out;
}

static void listOptions(World& w) {
    for (const Unit& u : w.units()) {
        const Player* p = w.player(u.owner);
        auto prod = w.producible(u.id);
        auto cons = w.constructible(u.id);
        std::printf("  [%s] %-26s", p ? p->faction.c_str() : "?", u.type->name.c_str());
        if (u.structure) std::printf(" structure %gx%g", u.footprintX * 2, u.footprintY * 2);
        std::printf("\n");
        if (!prod.empty()) {
            std::printf("      produces:");
            for (auto& n : prod) std::printf(" %s", n.c_str());
            std::printf("\n");
        }
        if (!cons.empty()) {
            std::printf("      constructs:");
            for (auto& n : cons) std::printf(" %s", n.c_str());
            std::printf("\n");
        }
    }
}

static std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

int main(int argc, char** argv) {
    const tools::ModChoice modArg = tools::takeModOptions(argc, argv);
    if (argc < 4) {
        std::cerr << "usage: simtool <game-root> <MAP> start | opening <Novus|Alien|Masari|Military> [--mods A,B | --no-mods]\n";
        return 2;
    }
    try {
        MegVfs vfs;
        tools::mountWithMods(vfs, argv[1], modArg);
        GameDatabase db;
        db.load(vfs);
        const TechTree tech = loadTechTree(vfs);
        const DamageTable dmg = loadDamageTable(vfs);
        GameDatabase effects;
        try { effects.loadEffects(vfs); } catch (const std::exception&) {}
        Map map = loadTed(vfs.read(mapPath(argv[2])));
        const auto st = starts(map, db);
        if (st.size() < 2) { std::cerr << "map needs 2 start positions\n"; return 1; }
        const std::string cmd = argv[3];
        std::printf("tech tree: %zu buildable objects\n", tech.size());

        const StartingForces forces = loadStartingForces(vfs);
        std::printf("resource piles on the map: %zu\n", World(map, db).resources().size());
        if (cmd == "start") {
            World w(map, db, &dmg, 1, &tech);
            w.setEffects(&effects);
            std::vector<std::string> factions;
            for (const Faction& f : db.factions())
                if (f.humanPlayable && forces.count(upper(f.name))) factions.push_back(f.name);
            if (factions.empty()) factions = {"Novus", "Alien", "Masari"};
            for (size_t i = 0; i < factions.size() && i < st.size(); ++i) addSkirmishPlayer(w, db, factions[i], st[i], &forces);
            for (const Player& p : w.players()) std::printf("player %d %s: %.0f credits, pop cap %d\n", p.id, p.faction.c_str(), p.credits, p.popCap);
            listOptions(w);
            return 0;
        }
        if (cmd == "opening" && argc > 4) {
            const std::string faction = argv[4];
            World w(map, db, &dmg, 1, &tech);
            w.setEffects(&effects);
            const int me = addSkirmishPlayer(w, db, faction, st[0], &forces);
            uint32_t builder = 0, hq = 0;
            for (const Unit& u : w.units()) (u.structure ? hq : builder) = u.id;
            const Vec3 base = w.unit(hq)->position;
            const float toCentre = std::atan2(map.sizeY() / 2 - base[1], map.sizeX() / 2 - base[0]);

            // Build every structure the builder can make, one after another, at sites around the HQ.
            float ring = 220, angle = toCentre - 1.2f;
            auto findSite = [&](const std::string& type) -> std::optional<Vec3> {
                const GameObjectType* t = db.find(type);
                for (int tries = 0; tries < 48; ++tries) {
                    const Vec3 p{base[0] + std::cos(angle) * ring, base[1] + std::sin(angle) * ring, 0};
                    angle += 0.45f;
                    if (tries % 12 == 11) ring += 90;
                    if (t && w.siteClear(*t, p)) return p;
                }
                return std::nullopt;
            };
            std::vector<std::string> built;
            int lastReport = -1;
            for (int t = 0; t < 8 * 60 * kTicksPerSecond; ++t) {
                const Unit* b = w.unit(builder);
                if (!b) break;
                if (!b->constructing) {
                    for (const auto& type : w.constructible(builder)) {
                        if (std::find(built.begin(), built.end(), type) != built.end()) continue;
                        auto site = findSite(type);
                        if (!site) continue;
                        const OrderResult r = w.commandConstruct(builder, type, *site);
                        std::printf("%6.1fs  construct %-28s -> %s\n", w.seconds(), type.c_str(), toString(r));
                        built.push_back(type);
                        if (r == OrderResult::Ok) break;
                    }
                }
                // Every finished structure keeps one unit queued.
                for (const Unit& u : w.units()) {
                    if (!u.structure || u.underConstruction || !u.queue.empty()) continue;
                    for (const auto& type : w.producible(u.id)) {
                        if (iequals(type, b->type->name)) continue; // no extra builders
                        const GameObjectType* pt = db.find(type);
                        if (!pt || pt->category.find("Patch") != std::string::npos || pt->category.find("Hero") != std::string::npos) continue;
                        if (w.economy(*pt).collector) continue; // depots launch their own drones
                        const OrderResult r = w.queueProduction(u.id, type);
                        if (r == OrderResult::Ok) {
                            std::printf("%6.1fs  %-26s queues %s\n", w.seconds(), u.type->name.c_str(), type.c_str());
                            break;
                        }
                    }
                }
                const size_t before = w.units().size();
                w.tick();
                if (w.units().size() > before)
                    for (size_t i = before; i < w.units().size(); ++i)
                        std::printf("%6.1fs  + %s\n", w.seconds(), w.units()[i].type->name.c_str());
                for (const Unit& u : w.units())
                    if (u.structure && !u.underConstruction && u.buildTicks > 0 && u.buildProgress >= 1) {
                        // first tick after completion
                    }
                const int sec = int(w.seconds());
                if (sec % 60 == 0 && sec != lastReport) {
                    lastReport = sec;
                    size_t structures = 0, units = 0;
                    size_t drones = 0, depots = 0;
                    for (const Unit& u : w.units()) {
                        (u.structure ? structures : units)++;
                        if (u.depot) ++drones;
                        if (w.economy(*u.type).drones) ++depots;
                    }
                    if (drones || depots) std::printf("    depots %zu drones %zu\n", depots, drones);
                    std::printf("--- %d:00  credits %.0f  earned %.0f  pop %d/%d  structures %zu  units %zu\n", sec / 60,
                                w.player(me)->credits, w.player(me)->earned, w.popUsed(me), w.player(me)->popCap, structures, units);
                    if (const Unit* b = w.unit(builder); b && b->constructing)
                        if (const Unit* s = w.unit(b->constructing))
                            std::printf("    builder at (%.0f,%.0f) site (%.0f,%.0f) %s, path %zu, progress %.0f%%\n", b->position[0],
                                        b->position[1], s->position[0], s->position[1], s->type->name.c_str(), b->path.size(),
                                        s->buildProgress * 100);
                }
            }
            return 0;
        }
        std::cerr << "unknown command\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
