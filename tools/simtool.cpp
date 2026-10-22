// simtool: exercise the simulation against real game data without a window.
//   simtool <game-root> <MAP> start            skirmish start for Novus/Hierarchy/Masari: what each can build
//   simtool <game-root> <MAP> opening FACTION  scripted build order (construct, produce) with a timeline
#include "common.hpp"
#include "alamo/ai.hpp"
#include "alamo/combat.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/sim.hpp"
#include "alamo/techtree.hpp"
#include "alamo/ted.hpp"

#include <cmath>
#include <map>
#include <cstdlib>
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
        std::cerr << "usage: simtool <game-root> <MAP> start | opening <Novus|Alien|Masari|Military> | match <F1> <F2> [minutes] [seed]  [--mods A,B | --no-mods]\n";
        return 2;
    }
    try {
        MegVfs vfs;
        tools::mountWithMods(vfs, argv[1], modArg);
        GameDatabase db;
        db.load(vfs);
        const TechTree tech = loadTechTree(vfs);
        const DamageTable dmg = loadDamageTable(vfs);
        GameRules rules;
        rules.load(vfs);
        const std::string mapList = argv[2];
        Map map = loadTed(vfs.read(mapPath(mapList.substr(0, mapList.find(',')))));
        const auto st = starts(map, db);
        if (st.size() < 2) { std::cerr << "map needs 2 start positions\n"; return 1; }
        const std::string cmd = argv[3];
        std::printf("tech tree: %zu buildable objects\n", tech.size());

        const StartingForces forces = loadStartingForces(vfs);
        std::printf("resource piles on the map: %zu\n", World(map, db).resources().size());
        if (cmd == "start") {
            World w(map, db, &dmg, 1, &tech);
            rules.configure(w);
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
            rules.configure(w);
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
        if (cmd == "match" && argc > 5) {
            // AI vs AI on the first two start positions until one is defeated or time runs out.
            const float minutes = argc > 6 ? float(std::atof(argv[6])) : 20.f;
            const uint64_t seed = argc > 7 ? std::strtoull(argv[7], nullptr, 10) : 1;
            World w(map, db, &dmg, seed, &tech);
            rules.configure(w);
            const int a = addSkirmishPlayer(w, db, argv[4], st[0], &forces);
            const int b = addSkirmishPlayer(w, db, argv[5], st[1], &forces);
            std::vector<SkirmishAI> ais{SkirmishAI(w, a, seed), SkirmishAI(w, b, seed + 7)};
            std::vector<std::string> last(2);
            const bool verbose = std::getenv("UAW_AI_LOG") != nullptr;
            const int end = int(minutes * 60 * kTicksPerSecond);
            for (int t = 0; t <= end; ++t) {
                for (auto& ai : ais) ai.update();
                w.tick();
                for (size_t i = 0; i < ais.size(); ++i)
                    if (ais[i].lastAction() != last[i] && (verbose || ais[i].lastAction().rfind("attack", 0) == 0 ||
                                                            ais[i].lastAction() == "retreat" || ais[i].lastAction() == "defend")) {
                        last[i] = ais[i].lastAction();
                        std::printf("%6.1fs  %-8s %s\n", w.seconds(), w.player(ais[i].player())->faction.c_str(), last[i].c_str());
                    }
                if (t % (60 * kTicksPerSecond) == 0) {
                    for (const auto& ai : ais) {
                        const Player* p = w.player(ai.player());
                        int structures = 0, units = 0;
                        for (const Unit& u : w.units())
                            if (u.owner == p->id) (u.structure ? structures : units)++;
                        std::printf("--- %2d:00 %-8s credits %6.0f earned %7.0f  structures %2d units %3d  abilities %4d research %zu %s\n",
                                    t / (60 * kTicksPerSecond), p->faction.c_str(), p->credits, p->earned, structures, units,
                                    p->abilitiesUsed, p->researched.size(), ai.attacking() ? "ATTACKING" : "");
                    }
                }
                const bool da = defeated(w, a), db2 = defeated(w, b);
                if (da || db2) {
                    std::printf("%6.1fs  %s wins\n", w.seconds(), da && db2 ? "nobody" : w.player(da ? b : a)->faction.c_str());
                    return 0;
                }
            }
            std::printf("time up, no winner\n");
            return 0;
        }
        if (cmd == "tournament" && argc > 4) {
            // Every pairing of the listed factions, both seat orders, on every listed map
            // (comma-separated <MAP>), for each seed. Prints the win table.
            std::vector<std::string> factions, maps;
            auto split = [](const std::string& s2, std::vector<std::string>& out) {
                size_t a = 0;
                while (a <= s2.size()) {
                    size_t b = s2.find(',', a);
                    if (b == std::string::npos) b = s2.size();
                    if (b > a) out.push_back(s2.substr(a, b - a));
                    a = b + 1;
                }
            };
            split(argv[4], factions);
            split(mapList, maps);
            const float minutes = argc > 5 ? float(std::atof(argv[5])) : 25.f;
            const int seeds = argc > 6 ? std::atoi(argv[6]) : 2;
            std::map<std::string, std::map<std::string, int>> wins; // wins[a][b]
            std::map<std::string, int> total, draws;
            std::map<std::string, double> winTime;
            for (const auto& mapName : maps) {
                Map m = loadTed(vfs.read(mapPath(mapName)));
                const auto ms = starts(m, db);
                if (ms.size() < 2) continue;
                for (size_t i = 0; i < factions.size(); ++i)
                    for (size_t j = 0; j < factions.size(); ++j) {
                        if (i == j) continue;
                        for (int seed = 1; seed <= seeds; ++seed) {
                            World w(m, db, &dmg, uint64_t(seed), &tech);
                            rules.configure(w);
                            const int a = addSkirmishPlayer(w, db, factions[i], ms[0], &forces);
                            const int b = addSkirmishPlayer(w, db, factions[j], ms[1], &forces);
                            SkirmishAI A(w, a, uint64_t(seed)), B(w, b, uint64_t(seed) + 7);
                            const int end = int(minutes * 60 * kTicksPerSecond);
                            int winner = -1;
                            for (int t = 0; t <= end && winner < 0; ++t) {
                                A.update();
                                B.update();
                                w.tick();
                                if (t % 30 == 0) {
                                    const bool da = defeated(w, a), dbb = defeated(w, b);
                                    if (da != dbb) winner = da ? 1 : 0;
                                    else if (da) winner = 2;
                                }
                            }
                            const std::string& fa = factions[i];
                            const std::string& fb = factions[j];
                            ++total[fa];
                            ++total[fb];
                            if (winner == 0) { ++wins[fa][fb]; winTime[fa] += w.seconds(); }
                            else if (winner == 1) { ++wins[fb][fa]; winTime[fb] += w.seconds(); }
                            else { ++draws[fa]; ++draws[fb]; }
                            std::printf("%-18s %-9s vs %-9s seed %d: %s (%.0fs)\n", mapName.c_str(), fa.c_str(), fb.c_str(), seed,
                                        winner == 0 ? fa.c_str() : winner == 1 ? fb.c_str() : "draw", w.seconds());
                            std::fflush(stdout);
                        }
                    }
            }
            std::printf("\nwins (row beat column):\n%-10s", "");
            for (const auto& f : factions) std::printf("%9s", f.substr(0, 8).c_str());
            std::printf("    total   draws\n");
            for (const auto& f : factions) {
                std::printf("%-10s", f.substr(0, 9).c_str());
                int sum = 0;
                for (const auto& g : factions) {
                    if (f == g) std::printf("%9s", "-");
                    else std::printf("%9d", wins[f][g]);
                    sum += wins[f][g];
                }
                std::printf("   %2d/%-3d  %3d\n", sum, total[f], draws[f]);
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
