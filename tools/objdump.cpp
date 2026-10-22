// objdump: load Universe at War's game object database from an install.
//   objdump <game-root> summary
//   objdump <game-root> units [faction]     buildable units/structures with health, cost, speed
//   objdump <game-root> show <ObjectName>   every field of one object (variants resolved)
//   objdump <game-root> weapons <ObjectName> [VictimName...]  weapons, dps, armor modifiers
//   objdump <game-root> warnings
// <game-root> is the folder containing Data/MegaFiles.xml.
#include "alamo/combat.hpp"
#include "alamo/gamedata.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>

using namespace alamo;

static int usage() {
    std::cerr << "usage: objdump <game-root> summary | units [faction] | show <name> | warnings\n";
    return 2;
}

static std::string firstWord(const std::string* s) {
    if (!s) return "-";
    std::string t = trim(*s);
    size_t c = t.find_first_of(", \t\r\n");
    return c == std::string::npos ? t : t.substr(0, c);
}

static std::string num(std::optional<double> v) {
    if (!v) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", *v);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[2];
    try {
        MegVfs vfs;
        auto missing = mountGame(vfs, argv[1]);
        GameDatabase db;
        db.load(vfs);

        if (cmd == "summary") {
            std::printf("archives mounted: %zu (%zu listed but missing)\n", vfs.archiveCount(), missing.size());
            for (const auto& m : missing) std::printf("  missing: %s\n", m.c_str());
            std::printf("object files:     %zu\n", db.fileCount());
            std::printf("game objects:     %zu\n", db.objects().size());
            size_t variants = std::count_if(db.objects().begin(), db.objects().end(),
                                            [](const auto& o) { return !o.variantOf.empty(); });
            std::printf("  of which variants: %zu\n", variants);
            std::printf("warnings:         %zu\n", db.warnings().size());
            std::map<std::string, size_t> cats;
            for (const auto& o : db.objects()) ++cats[o.category];
            std::vector<std::pair<size_t, std::string>> sorted;
            for (auto& [k, v] : cats) sorted.emplace_back(v, k);
            std::sort(sorted.rbegin(), sorted.rend());
            std::printf("top categories:\n");
            for (size_t i = 0; i < sorted.size() && i < 15; ++i)
                std::printf("  %5zu  %s\n", sorted[i].first, sorted[i].second.c_str());
            std::printf("factions:\n");
            for (const auto& f : db.factions())
                std::printf("  %-28s human:%s ai:%s\n", f.name.c_str(), f.humanPlayable ? "yes" : "no",
                            f.aiPlayable ? "yes" : "no");
            return 0;
        }
        if (cmd == "units") {
            std::string filter = argc > 3 ? argv[3] : "";
            std::printf("%-40s %-10s %7s %6s %6s %6s %s\n", "name", "faction", "health", "cost", "build", "speed", "armor");
            size_t n = 0;
            for (const auto& o : db.objects()) {
                auto cost = o.number("Tactical_Build_Cost_Multiplayer");
                if (!cost) continue; // only things a player can build
                std::string fac = firstWord(o.find("Affiliation"));
                if (!filter.empty() && !iequals(fac, filter)) continue;
                std::printf("%-40s %-10s %7s %6s %6s %6s %s\n", o.name.c_str(), fac.c_str(),
                            num(o.number("Tactical_Health")).c_str(), num(cost).c_str(),
                            num(o.number("Tactical_Build_Time_Seconds")).c_str(), num(o.number("Max_Speed")).c_str(),
                            firstWord(o.find("Armor_Type")).c_str());
                ++n;
            }
            std::printf("%zu buildable objects\n", n);
            return 0;
        }
        if (cmd == "show" && argc > 3) {
            const GameObjectType* o = db.find(argv[3]);
            if (!o) { std::cerr << "no object named " << argv[3] << "\n"; return 1; }
            std::printf("%s  (%s, %s)\n", o->name.c_str(), o->category.c_str(), o->sourceFile.c_str());
            if (!o->variantOf.empty()) std::printf("variant of %s\n", o->variantOf.c_str());
            for (const auto& f : o->fields()) {
                for (const auto& v : f.values) {
                    std::string one = v;
                    std::replace_if(one.begin(), one.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
                    std::printf("  %s = %s\n", f.path.c_str(), one.c_str());
                }
            }
            return 0;
        }
        if (cmd == "weapons" && argc > 3) {
            const GameObjectType* o = db.find(argv[3]);
            if (!o) { std::cerr << "no object named " << argv[3] << "\n"; return 1; }
            const DamageTable table = loadDamageTable(vfs);
            const std::string* armor = o->find("Armor_Type");
            std::printf("%s: health %s, armor %s\n", o->name.c_str(), num(o->number("Tactical_Health")).c_str(),
                        armor ? trim(*armor).c_str() : "-");
            for (const WeaponSpec& w : weaponsFor(*o, db)) {
                std::printf("  %-34s via %-32s range %g-%g, %d x %d pellet(s), burst gap %d, recharge %d ticks\n",
                            w.projectile->name.c_str(), w.source->name.c_str(), w.minRange, w.maxRange, w.pulseCount,
                            w.pellets, w.pulseDelayTicks, w.rechargeTicks);
                std::printf("      %s: %g damage + %g blast (radius %g), speed %g, %.1f dps\n", w.damageType.c_str(), w.damage,
                            w.blastDamage, w.blastRange, w.projectileSpeed, w.damagePerSecond());
                if (argc > 4)
                    for (int i = 4; i < argc; ++i)
                        if (const GameObjectType* v = db.find(argv[i]))
                            if (const std::string* va = v->find("Armor_Type"))
                                std::printf("      vs %-28s (%s): x%g\n", v->name.c_str(), trim(*va).c_str(),
                                            table.modifier(w.damageType, trim(*va)));
            }
            return 0;
        }
        if (cmd == "warnings") {
            for (const auto& w : db.warnings()) std::printf("%s\n", w.c_str());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
