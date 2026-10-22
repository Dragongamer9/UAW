// uaw-viewer: a Universe at War map with its objects placed and test squads to command.
//
//   uaw-viewer                       (from the game folder) the skirmish setup screen
//   uaw-viewer <game-root> [options] the skirmish setup screen
//   uaw-viewer <game-root> <MAP> [options]
//     --player N          you control player N's squad; camera starts there (default 1)
//     --all-groups        also show campaign-mission objects
//     --no-mods           the original game only (by default every mod in Game\\Mods is
//                         layered on top, in name order; later mods win where they overlap)
//     --mods A,B          only these mods, bottom to top (names under Game\\Mods or folders)
//     --faction F         your faction for the skirmish start: Novus (default), Alien, Masari
//                         (or Military with a mod that makes it playable, e.g. Human Resurgence)
//     --enemy F           opponent's faction (default Alien)
//     --squads            instead of a skirmish start, give every start position a mixed test squad
//     --no-units          no units at all
//     --no-ai             the opponent doesn't play (skirmish start only)
//     --spectate          an AI plays your side too
//     --demo              (with --screenshot) start a construction and a production order first
//     --spawn TYPE        place a finished TYPE for you near your base (repeatable), e.g. Novus_Input_Station
//     --battle            Novus and Hierarchy squads face off mid-map (with --run: prints the battle)
//     --lineup F[:units|:structures[:NAME]]  every buildable unit (or structure) of faction F,
//                         optionally only names containing NAME, on a flat floor without terrain
//                         or props, largest at the back: a model gallery
//     --size WxH          window size (default 1600x900)
//     --camera x,y,dist,yaw,pitch
//     --screenshot FILE   render one frame to a PNG and exit (works headless)
//     --order X,Y         (with --screenshot) select your squad and order it to X,Y
//     --run SECONDS       (with --screenshot) simulate this long before the picture
//     --select NAME       (with --screenshot) select your units whose type name contains NAME
//     --research          open the research panel
//     --menu              (with --screenshot) picture of the setup screen for MAP
//     --no-fog            no fog of war
//     --load FILE         continue a saved game (F5 saves, F9 loads the quick save)
//     --replay FILE       watch a saved game from the start, its orders given again as it plays
//     --host              open a LAN game (the setup screen waits for players; port 7470)
//     --join ADDRESS      join a LAN game at ADDRESS[:PORT]; --name NAME shows you by NAME
//     --players N         (with --host and --screenshot) start once N players (you included) are in
//     --effect NAME[@S]   (with --screenshot) a particle effect (Particles.xml name or .alo) at the
//                         camera target, pictured S seconds after it starts (default 0.3)
//     --slots LIST        every start position in order: you|ai|open[:FACTION[:TEAM]], e.g.
//                         you:Novus:1,ai:Masari:1,ai:Alien:2,ai:Military:2 (2 v 2)
//
// Controls
//   keyboard  WASD / arrows pan, Q/E rotate, R/F tilt, wheel or +/- zoom,
//             Tab select all yours, . next idle builder, , your army, H your base, M sound on/off, Space jump to selection, X stop, G (or hold A) + right click attack-move,
//             P stand-in boxes, T research, F1-F8 abilities of the selection (then right click a target),
//             V + right click fires a selected superweapon, minimap click moves the camera (right click: orders there),
//             [ ] game speed, Pause pauses, Esc (with nothing to cancel) opens the menu,
//             Home jumps to the last attack alert, U unloads selected transports (right click your transport to get in), Delete twice sells,
//             Ctrl+0-9 stores a control group, 0-9 recalls it (twice: camera to it), double click
//             selects every unit of that type on screen
//   mouse     left click/drag select (Shift adds), right click move (Shift: queue after the current move), middle or right drag rotate/tilt
//   build     select a structure or builder: 1-9 / click the menu (D-pad left/right + down on a gamepad);
//             builders then place the structure with a click / A (right click / B cancels);
//             Backspace (left stick click) cancels the last queued item; right click with a structure sets its rally point
//   gamepad   left stick pan, right stick rotate/tilt, triggers zoom; reticle at screen centre:
//             A select (hold LB to add), X select all on screen, B clear, RB move/attack, D-pad up attack-move,
//             Y stand-in boxes, Back quit
//   right click / RB on an enemy unit orders an attack; units also fire at enemies in range on their own
#include "audio.hpp"
#include "camera.hpp"
#include "gl.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "hud.hpp"
#include "units.hpp"
#include "menu.hpp"
#include "particles.hpp"
#include "savegame.hpp"
#include "lockstep.hpp"

#include "alamo/ai.hpp"
#include "alamo/sfx.hpp"

#include <unordered_set>
#include "alamo/alo.hpp"
#include "alamo/assets.hpp"
#include "alamo/combat.hpp"
#include "alamo/dds.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/sim.hpp"
#include "alamo/strings.hpp"
#include "alamo/techtree.hpp"
#include "alamo/ted.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <string>

using namespace alamo;
using namespace uaw;

namespace {

struct Options {
    std::string gameRoot, map, screenshot, mods;
    bool noMods = false;
    int width = 1600, height = 900, player = 1;
    bool allGroups = false, haveCamera = false, camSet = false, units = true, haveOrder = false, battle = false;
    bool squads = false, demo = false, ai = true, spectate = false;
    std::vector<std::string> spawns;
    std::string lineup;
    bool research = false; // open the research panel (screenshots)
    std::string select;    // (screenshots) select your units whose type contains this
    std::string faction = "Novus", enemy = "Alien";
    float cam[5] = {0, 0, 0, 0, 0};
    float order[2] = {0, 0};
    float runSeconds = 0;
    bool fog = true;       // fog of war (--no-fog)
    std::string load;      // --load FILE: a saved game
    bool replay = false;   // --replay FILE: watch a saved game from the start
    std::string saveAs;    // --save FILE: (screenshots) save the game after --run
    bool host = false;     // --host: open a LAN game
    std::string join;      // --join ADDRESS[:PORT]: join one
    std::string name = "Player";
    int waitPlayers = 0;   // --host --players N (headless): start once N players are in
    std::string effect;    // --effect NAME[@SECONDS]: (screenshots) a particle effect at the camera target
    std::string slots;     // --slots you:Novus:1,ai:Alien:2,open,...
    bool menu = false;     // start at the skirmish setup screen (no map on the command line)
    float speed = 1;       // game speed multiplier
};

// With no arguments (e.g. double-clicked), look for the game next to the executable
// or in the current folder, and open the Sahara map.
std::string findGameRoot() {
    std::vector<std::string> candidates;
    if (const char* base = SDL_GetBasePath()) candidates.push_back(base);
    candidates.push_back(".");
    for (const auto& c : candidates) {
        std::error_code ec;
        for (const char* sub : {"", "Game/Universe At War Earth Assault", "Universe At War Earth Assault"}) {
            std::filesystem::path p = std::filesystem::path(c) / sub;
            if (std::filesystem::exists(p / "Data" / "MegaFiles.xml", ec)) return p.string();
        }
    }
    return {};
}

bool parse(int argc, char** argv, Options& o) {
    if (argc == 1) {
        o.gameRoot = findGameRoot();
        o.menu = true;
        return !o.gameRoot.empty();
    }
    o.gameRoot = argv[1];
    int first = 2;
    if (argc >= 3 && argv[2][0] != '-') {
        o.map = argv[2];
        first = 3;
    } else {
        o.menu = true; // just the game folder (and options): the setup screen
    }
    for (int i = first; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--screenshot") o.screenshot = next();
        else if (a == "--all-groups") o.allGroups = true;
        else if (a == "--player") o.player = std::max(1, std::atoi(next()));
        else if (a == "--no-units") o.units = false;
        else if (a == "--no-ai") o.ai = false;
        else if (a == "--spectate") o.spectate = true;
        else if (a == "--squads") o.squads = true;
        else if (a == "--faction") o.faction = next();
        else if (a == "--mods" || a == "--mod") o.mods = next();
        else if (a == "--no-mods") o.noMods = true;
        else if (a == "--enemy") o.enemy = next();
        else if (a == "--demo") o.demo = true;
        else if (a == "--spawn") o.spawns.push_back(next());
        else if (a == "--order") o.haveOrder = std::sscanf(next(), "%f,%f", &o.order[0], &o.order[1]) == 2;
        else if (a == "--run") o.runSeconds = float(std::atof(next()));
        else if (a == "--size") std::sscanf(next(), "%dx%d", &o.width, &o.height);
        else if (a == "--camera")
            o.camSet = o.haveCamera = std::sscanf(next(), "%f,%f,%f,%f,%f", &o.cam[0], &o.cam[1], &o.cam[2], &o.cam[3], &o.cam[4]) == 5;
        else if (a == "--battle") o.battle = true;
        else if (a == "--lineup") o.lineup = next();
        else if (a == "--research") o.research = true;
        else if (a == "--select") o.select = next();
        else if (a == "--menu") o.menu = true;
        else if (a == "--slots") o.slots = next();
        else if (a == "--no-fog") o.fog = false;
        else if (a == "--load") { o.load = next(); o.menu = true; }
        else if (a == "--replay") { o.load = next(); o.menu = true; o.replay = true; }
        else if (a == "--save") o.saveAs = next();
        else if (a == "--host") o.host = true;
        else if (a == "--join") { o.join = next(); o.menu = true; }
        else if (a == "--name") o.name = next();
        else if (a == "--players") o.waitPlayers = std::atoi(next());
        else if (a == "--effect") o.effect = next();
        else return false;
    }
    o.width = std::max(o.width, 64);
    o.height = std::max(o.height, 64);
    return true;
}

std::string mapPath(std::string name) {
    if (name.find('\\') == std::string::npos && name.find('/') == std::string::npos) name = "Data\\Art\\Maps\\" + name;
    std::string up = normalizePath(name);
    if (up.size() < 4 || up.compare(up.size() - 4, 4, ".TED") != 0) name += ".ted";
    return name;
}

float axis(SDL_Gamepad* gp, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(gp, a) / 32767.f;
    const float dead = 0.18f;
    if (std::fabs(v) < dead) return 0;
    return (v - std::copysign(dead, v)) / (1 - dead);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        const char* usage =
            "usage: uaw-viewer <game-root> [<MAP>] [--player N] [--faction F] [--enemy F] [--all-groups] [--no-units]\n"
            "                  [--size WxH] [--camera x,y,dist,yaw,pitch] [--screenshot out.png [--order X,Y] [--run SECONDS]]\n\n"
            "<game-root> is the folder containing Data\\MegaFiles.xml. Without a map the skirmish setup screen opens.\n"
            "Run with no arguments from inside that folder (or next to it) to open the setup screen.";
        std::fprintf(stderr, "%s\n", usage);
        if (argc == 1) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "uaw-viewer", usage, nullptr);
        return 2;
    }

    MegVfs vfs;
    GameDatabase db;
    std::vector<std::string> modNames;
    try {
        std::vector<std::filesystem::path> mods;
        if (!opt.noMods && opt.mods.empty()) mods = installedMods(opt.gameRoot);
        for (size_t s = 0; !opt.noMods && s <= opt.mods.size() && !opt.mods.empty();) {
            size_t e = opt.mods.find(',', s);
            if (e == std::string::npos) e = opt.mods.size();
            const std::string name = opt.mods.substr(s, e - s);
            s = e + 1;
            if (name.empty()) continue;
            auto p = findMod(opt.gameRoot, name);
            if (p.empty()) throw std::runtime_error("mod not found: " + name);
            mods.push_back(p);
        }
        for (const auto& m : mods) {
            SDL_Log("mod: %s", m.filename().string().c_str());
            modNames.push_back(m.filename().string());
        }
        mountGame(vfs, opt.gameRoot, mods);
        db.load(vfs);
        if (!opt.map.empty() && !vfs.exists(mapPath(opt.map))) throw std::runtime_error("map not found: " + mapPath(opt.map));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    const bool headless = !opt.screenshot.empty();
    if (headless && !SDL_getenv("SDL_VIDEO_DRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO | (headless ? 0 : SDL_INIT_GAMEPAD))) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (!headless) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    }
    SDL_Window* window = SDL_CreateWindow("Universe at War - uaw-native viewer", opt.width, opt.height,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (headless ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        std::fprintf(stderr, "OpenGL 3.3 context failed: %s\n", SDL_GetError());
        return 1;
    }
    if (const char* missing = gl::load([](const char* n) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(n)); })) {
        std::fprintf(stderr, "OpenGL function missing: %s\n", missing);
        return 1;
    }
    std::printf("OpenGL: %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    SDL_GL_SetSwapInterval(1);

    int exitCode = 0;
    try {
        Renderer renderer(vfs);
        const DamageTable damageTable = loadDamageTable(vfs);
        const TechTree techTree = loadTechTree(vfs);
        const StringTable text = loadStrings(vfs);
        GameRules rules;
        rules.load(vfs);
        SDL_Log("%zu effects, %zu research trees", rules.effectLibrary.size(), rules.research.size());
        // Sound: the SFX events, and their samples from wherever the game keeps them.
        SfxLibrary sfxLibrary;
        sfxLibrary.load(vfs);
        MusicLibrary musicLibrary;
        musicLibrary.load(vfs);
        std::unique_ptr<Audio> audio;
        if (!headless) audio = std::make_unique<Audio>(vfs, sfxLibrary);
        Settings settings = headless ? Settings{} : loadSettings();
        if (audio) audio->volume = settings.sound, audio->musicVolume = settings.music;
        if (!settings.fullGraphics) renderer.setDetail(false);
        auto keepSettings = [&]() {
            if (headless) return;
            if (audio) settings.sound = audio->volume, settings.music = audio->musicVolume;
            settings.fullGraphics = renderer.detail();
            saveSettings(settings);
        };
        std::printf("sound: %zu SFX events%s\n", sfxLibrary.size(), audio && audio->ok() ? "" : " (no audio device)");
        const float uiScale = std::max(0.75f, float(opt.height) / 900.f);
        enum class End { Quit, ToMenu, Restart, Load };
        std::string loadPath; // End::Load: the file to load

    // One game: load the map, set up the players, play until quit / back to the menu / restart.
    std::unique_ptr<Lockstep> lockstep; // a LAN game
    auto runSession = [&](const SkirmishSetup& setup, const SaveGame* save) -> End {
        Lockstep* ls = lockstep && lockstep->started() ? lockstep.get() : nullptr;
        opt.map = setup.map;
        Map map = loadTed(vfs.read(mapPath(opt.map)));
        renderer.setTerrain(map);
        Scene scene(renderer, db, map, opt.allGroups);
        scene.useEnvironmentSky(renderer, db);
        renderer.setEnvironment(map.environment); // the map's sun, ambient and haze
        renderer.setWater(map.water, map.sizeX(), map.sizeY());
        const SceneStats& st = scene.stats();
        std::printf("map %s: %ux%u, %zu objects (%zu with models, %zu placeholders), %zu start positions\n",
                    opt.map.c_str(), map.width, map.height, st.objects, st.withModel, st.placeholders, st.starts);
        World world(map, db, &damageTable, 1, &techTree);
        world.setFogOfWar(setup.fog && !opt.battle && opt.lineup.empty() && !opt.squads && opt.units);
        bool lineupMode = false; // model gallery: no map props
        Vec3 lineupFloor{};      // its floor: centre x, y and size
        rules.configure(world);
        std::vector<Vec3> starts;
        for (int slot = 0; slot < int(std::max<size_t>(st.starts, 2)); ++slot) starts.push_back(scene.startPosition(slot));
        if (opt.battle) {
            // Squads face off in the middle of the playable area.
            const Vec3 c{(map.playableMin[0] + map.playableMax[0]) / 2, (map.playableMin[1] + map.playableMax[1]) / 2, 0};
            spawnBattle(world, c, 420);
            if (!opt.camSet) {
                opt.haveCamera = true;
                opt.cam[0] = c[0];
                opt.cam[1] = c[1] - 120;
                opt.cam[2] = 650;
                opt.cam[3] = 0;
                opt.cam[4] = 50;
            }
        } else if (!opt.lineup.empty()) {
            // Model gallery: the faction's buildable types in rows, facing a camera to the south.
            const std::string fac = opt.lineup.substr(0, opt.lineup.find(':'));
            const bool structures = opt.lineup.find(":structures") != std::string::npos;
            // Optional third field: only types whose name contains it (F:units:Tank).
            const size_t c2 = opt.lineup.find(':', opt.lineup.find(':') + 1);
            const std::string only = c2 == std::string::npos || opt.lineup.find(':') == std::string::npos ? "" : opt.lineup.substr(c2 + 1);
            std::vector<const GameObjectType*> types;
            auto up = [](std::string v) { for (char& ch : v) ch = char(std::toupper(static_cast<unsigned char>(ch))); return v; };
            for (const std::string& name : techTree.objects()) {
                const GameObjectType* t = db.find(name);
                if (!t || isStructureType(*t) != structures) continue;
                if (!only.empty() && up(t->name).find(up(only)) == std::string::npos) continue;
                const std::string* aff = t->find("Affiliation");
                if (!aff || up(*aff).find(up(fac)) == std::string::npos) continue;
                if (!structures && !buildInfo(*t, db).spawnsUnit) continue; // upgrades and patches
                if (!t->find("Land_Model_Name") && squadMembers(*t, db).empty()) continue;
                types.push_back(t);
            }
            std::sort(types.begin(), types.end(), [](auto* a, auto* b) { return a->name < b->name; });
            // Each slot is as wide as its type: hard extents, or the spread of a squad's members.
            auto extent = [&](const GameObjectType& t) {
                // Half the widest horizontal span of the model as drawn (XML extents as fallback).
                auto one = [&](const GameObjectType& m) {
                    float r = float(std::max(m.number("Custom_Hard_XExtent").value_or(m.number("Custom_Soft_Footprint_Radius").value_or(6.0)),
                                             m.number("Custom_Hard_YExtent").value_or(0.0)));
                    if (const std::string* model = m.find("Land_Model_Name"); model && !trim(*model).empty())
                        if (const GpuModel* gm = renderer.model(trim(*model))) {
                            const float scale = float(m.number("Scale_Factor").value_or(1.0));
                            r = scale * std::max({std::fabs(gm->boundsMin[0]), std::fabs(gm->boundsMax[0]),
                                                  std::fabs(gm->boundsMin[1]), std::fabs(gm->boundsMax[1])});
                        }
                    return r;
                };
                const auto members = squadMembers(t, db);
                if (members.empty()) return one(t);
                float r = 0;
                for (const auto* m : members) r = std::max(r, one(*m));
                return r * 2.2f * std::sqrt(float(members.size())) + 8;
            };
            std::vector<float> slot;
            float area = 0;
            {
                std::vector<std::pair<float, const GameObjectType*>> sized;
                for (const auto* t : types) sized.push_back({std::max(structures ? 90.f : 34.f, extent(*t) * 2.4f + 16), t});
                // Biggest at the back, so rows hold similar sizes and nothing hides behind a larger model.
                std::stable_sort(sized.begin(), sized.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                types.clear();
                for (const auto& [s, t] : sized) {
                    types.push_back(t);
                    slot.push_back(s);
                    area += s * s;
                }
            }
            // Rows about 16:9 wide, flowed left to right; each row centred, rows spaced by their tallest slot.
            const float rowWidth = std::max(slot.empty() ? 0.f : *std::max_element(slot.begin(), slot.end()), std::sqrt(area * 1.9f));
            struct Row { std::vector<size_t> items; float width = 0, depth = 0; };
            std::vector<Row> layout(1);
            for (size_t i = 0; i < types.size(); ++i) {
                if (!layout.back().items.empty() && layout.back().width + slot[i] > rowWidth) layout.emplace_back();
                layout.back().items.push_back(i);
                layout.back().width += slot[i];
                layout.back().depth = std::max(layout.back().depth, slot[i]);
            }
            float total = 0;
            for (const Row& r : layout) total += r.depth;
            // Gallery on a flat floor at the map's middle: terrain and props are hidden and every
            // unit is put at floor height (keeping its flying altitude).
            const Vec3 c{(map.playableMin[0] + map.playableMax[0]) / 2, (map.playableMin[1] + map.playableMax[1]) / 2, 0};
            const int pid = world.addPlayer(fac, 0, 999);
            float y = c[1] + total / 2;
            for (const Row& r : layout) {
                y -= r.depth / 2;
                float x = c[0] - r.width / 2;
                for (size_t i : r.items) {
                    world.spawn(types[i]->name, pid, {x + slot[i] / 2, y, 0}, -1.5707963f);
                    std::printf("lineup: %-34s slot %6.0f at %6.0f,%6.0f\n", types[i]->name.c_str(), slot[i], x + slot[i] / 2, y);
                    x += slot[i];
                }
                y -= r.depth / 2;
            }
            lineupMode = true;
            for (const Unit& u : world.units())
                if (Unit* m = world.unit(u.id)) m->position[2] -= map.heightAt(u.position[0], u.position[1]);
            lineupFloor = {c[0], c[1], std::max(rowWidth, total) + 400};
            if (!opt.camSet) {
                opt.haveCamera = true;
                opt.cam[0] = c[0];
                opt.cam[1] = c[1] - total * 0.12f;
                opt.cam[2] = std::max(rowWidth * 0.5f, total * 0.95f) + 40;
                opt.cam[3] = 0;
                opt.cam[4] = 48;
            }
            opt.ai = false;
            opt.units = false; // no skirmish start
        } else if (opt.units && opt.squads) {
            spawnTestSquads(world, starts, map);
        } else if (opt.units) {
            // Skirmish start: every taken slot at its start position; open ones stay empty.
            const StartingForces forces = loadStartingForces(vfs);
            for (int slot = 0; slot < int(starts.size()); ++slot) {
                const PlayerSlot* ps = size_t(slot) < setup.slots.size() ? &setup.slots[size_t(slot)] : nullptr;
                if (ps && ps->kind != PlayerSlot::Kind::Open) {
                    const int id = addSkirmishPlayer(world, db, ps->faction, starts[size_t(slot)], &forces);
                    world.setTeam(id, ps->team);
                    world.setHandicap(id, ps->kind == PlayerSlot::Kind::Computer ? SkirmishSetup::aiIncome(setup.aiLevel) : 1.f,
                                      SkirmishSetup::creditScale(setup.credits));
                } else {
                    world.addPlayer("Neutral", 0, 0);
                }
            }
        }
        if (!opt.spawns.empty() && opt.units && !opt.squads) {
            // Ring of free sites around your HQ, nearest first, facing the map centre.
            const int me = opt.player - 1;
            Vec3 base = starts.empty() ? Vec3{map.sizeX() / 2, map.sizeY() / 2, 0} : starts[size_t(std::min<int>(me, int(starts.size()) - 1))];
            const float toCentre = std::atan2(map.sizeY() / 2 - base[1], map.sizeX() / 2 - base[0]);
            int k = 0;
            for (const auto& name : opt.spawns) {
                const GameObjectType* t = db.find(name);
                if (!t) { std::printf("spawn: no type %s\n", name.c_str()); continue; }
                bool placed = false;
                Vec3 where{};
                for (int tries = 0; tries < 64 && !placed; ++tries, ++k) {
                    const float ring = 260 + 110 * float(k / 8), a = toCentre + (float(k % 8) - 3.5f) * 0.42f;
                    const Vec3 p{base[0] + std::cos(a) * ring, base[1] + std::sin(a) * ring, 0};
                    if (!world.siteClear(*t, p)) continue;
                    placed = world.spawn(t->name, me, p, toCentre) != 0;
                    where = p;
                }
                if (placed) std::printf("spawn: %s placed at %.0f,%.0f\n", name.c_str(), where[0], where[1]);
                else std::printf("spawn: %s no room\n", name.c_str());
            }
        }
        // Computer opponents (and your side with --spectate) for a skirmish start.
        std::vector<SkirmishAI> ais;
        if (opt.units && !opt.squads && !opt.battle && opt.ai)
            for (const Player& p : world.players())
                if (p.faction != "Neutral") {
                    // Computer slots; your own too when spectating. Other people's slots stay theirs.
                    const PlayerSlot* slot = size_t(p.id) < setup.slots.size() ? &setup.slots[size_t(p.id)] : nullptr;
                    const bool computer = !slot || slot->kind == PlayerSlot::Kind::Computer;
                    const bool mine = p.id == opt.player - 1;
                    if (computer || (mine && (opt.spectate || setup.human() < 0)))
                        ais.emplace_back(world, p.id, uint64_t(p.id) + 1);
                }
        // Orders given outside the simulation step (the player's) are recorded for saved games.
        auto stepWorld = [&]() {
            world.setRecording(false);
            world.setCapture(false);
            for (auto& ai : ais) ai.update();
            world.tick();
            world.setRecording(!ls);
            world.setCapture(ls != nullptr);
        };
        world.setRecording(!ls);
        world.setCapture(ls != nullptr);
        // One tick, if the network allows it (LAN games: the turn's orders must be known; at a turn's
        // first tick its orders run). Returns false when it has to wait.
        int64_t turnBegun = -1;
        uint64_t waitingSince = 0; // LAN: when the simulation last had to wait for the others (0 = not waiting)
        // Watching a saved game (--replay): its orders are given again at their ticks as it plays.
        const bool replaying = save && opt.replay;
        size_t replayNext = 0;
        auto advance = [&]() -> bool {
            if (replaying) {
                world.setRecording(true);
                while (replayNext < save->commands.size() && save->commands[replayNext].tick <= world.tickCount())
                    world.apply(save->commands[replayNext++]);
            }
            if (ls) {
                const uint64_t t = world.tickCount();
                if (!ls->canRun(t)) return false;
                if (t % Lockstep::kTurnTicks == 0 && int64_t(t) != turnBegun) {
                    turnBegun = int64_t(t);
                    const std::vector<alamo::Command> orders = ls->beginTurn(t, world.stateHash());
                    world.setCapture(false);
                    for (const alamo::Command& c : orders) world.apply(c);
                }
            }
            stepWorld();
            return true;
        };
        auto pumpNetwork = [&]() {
            if (!ls) return;
            ls->submit(world.takeCaptured(), world.tickCount());
            ls->poll();
        };
        if (world.players().empty())
            for (size_t i = 0; i < std::max<size_t>(starts.size(), 4); ++i) world.addPlayer("Test", 10000, 90);
        std::printf("strings: %zu, tech tree: %zu entries\n", text.size(), techTree.size());
        UnitControl control(replaying ? -1 : opt.player - 1); // a replay is watched, not played
        ParticleEffects effects(vfs, db);
        // Sound hooks: what has been heard so far this session.
        std::set<uint32_t> heardSelection;
        float heardOrderTime = -100;
        size_t heardDeaths = world.deaths().size();
        std::unordered_set<uint32_t> heardOwn;
        std::set<uint32_t> heardFlowing;
        bool heardFirst = true;
        float lastCombat = -100;
        const std::string environment = mapEnvironment(setup.map);
        auto sounds = [&](const Vec3& listenerAt, float yawDeg) {
            if (!audio) { control.takeSounds(); return; }
            // Music: the faction's ambient piece, battle music while your units fight (and for 15 s
            // after), the win or lose piece at the end.
            if (const Player* me = world.player(control.player()); me && me->faction != "Neutral") {
                for (const Unit& u : world.units())
                    if (u.owner == me->id && (u.firedThisTick || u.damagedThisTick)) { lastCombat = world.seconds(); break; }
                std::string opponent;
                bool enemyLeft = false;
                for (const Player& p : world.players())
                    if (p.id != me->id && world.enemies(me->id, p.id) && p.faction != "Neutral") {
                        if (opponent.empty()) opponent = p.faction;
                        enemyLeft = enemyLeft || !defeated(world, p.id);
                    }
                const MusicEvent* piece = defeated(world, me->id) ? musicLibrary.lose(me->faction, opponent)
                                          : (!opponent.empty() && !enemyLeft) ? musicLibrary.win(me->faction, opponent)
                                          : world.seconds() - lastCombat < 15 ? musicLibrary.battle(me->faction, environment)
                                                                              : musicLibrary.ambient(me->faction, environment);
                audio->playMusic(piece);
            }
            audio->setListener(listenerAt, yawDeg * 3.14159265f / 180.f);
            auto event = [&](const GameObjectType& t, const char* field) -> std::string {
                const std::string* v = t.find(field);
                return v ? alamo::trim(*v) : std::string();
            };
            // Your selection answers; your orders are acknowledged.
            const std::set<uint32_t>& sel = control.selection();
            if (sel != heardSelection) {
                if (!sel.empty())
                    if (const Unit* u = world.unit(*sel.begin()); u && u->owner == control.player())
                        audio->play(event(*u->type, "SFXEvent_Select"));
                heardSelection = sel;
            }
            if (control.lastOrderTime() != heardOrderTime) {
                heardOrderTime = control.lastOrderTime();
                if (!sel.empty())
                    if (const Unit* u = world.unit(*sel.begin()); u && u->owner == control.player() && !u->structure)
                        audio->play(event(*u->type, control.lastOrderWasAttack() ? "SFXEvent_Attack" : "SFXEvent_Move"));
            }
            // Gunfire and deaths near the camera.
            for (const Unit& u : world.units()) {
                if (!u.firedThisTick || !control.shown(world, u)) continue;
                if (std::hypot(u.position[0] - listenerAt[0], u.position[1] - listenerAt[1]) > 1800) continue;
                std::string fire = event(*u.type, "SFXEvent_Fire");
                if (fire.empty())
                    for (const auto& wsp : u.weapons) // a hardpoint's own gun sound
                        if (wsp.spec.source) { fire = event(*wsp.spec.source, "SFXEvent_Fire"); if (!fire.empty()) break; }
                if (!fire.empty()) audio->play(fire, &u.position);
            }
            const auto& deaths = world.deaths();
            for (; heardDeaths < deaths.size(); ++heardDeaths) {
                const auto& d = deaths[heardDeaths];
                if (!d.type || d.sold) continue;
                const std::string die = event(*d.type, "Death_SFXEvent_Start_Die");
                if (!die.empty()) audio->play(die, &d.position);
            }
            // Novus flow: a whoosh in and out of the power network.
            {
                std::set<uint32_t> now;
                for (const Unit& u : world.units())
                    if (u.flowTicks > 0) {
                        now.insert(u.id);
                        if (!heardFlowing.count(u.id)) audio->play("SFX_Novus_Flow_Start", &u.position);
                    }
                for (uint32_t id : heardFlowing)
                    if (!now.count(id))
                        if (const Unit* u = world.unit(id)) audio->play("SFX_Novus_Flow_Stop", &u->position);
                heardFlowing = std::move(now);
            }
            // New units and finished buildings of yours announce themselves.
            for (const Unit& u : world.units()) {
                if (u.owner != control.player() || u.underConstruction || heardOwn.count(u.id)) continue;
                heardOwn.insert(u.id);
                if (!heardFirst) audio->play(event(*u.type, "SFXEvent_Tactical_Build_Complete"));
            }
            heardFirst = false;
            for (const std::string& s : control.takeSounds()) audio->play(s);
            audio->update();
        };
        control.effects = &effects;
        control.db_ = &db;
        float lastFrameTime = world.seconds();
        control.showResources = !lineupMode;
        control.fog = !opt.spectate && !replaying; // spectators see everything
        std::printf("spawned %zu units; models loaded %zu, missing %zu; textures missing %zu\n", world.units().size(),
                    renderer.modelsLoaded(), renderer.modelsMissing(), renderer.texturesMissing());

        // A saved game: play it again, with the player's orders at their ticks, up to where it was saved.
        if (save && !replaying) {
            SDL_SetWindowTitle(window, "Universe at War - loading saved game...");
            size_t next = 0;
            const uint64_t t0 = SDL_GetTicks();
            while (world.tickCount() < save->tick) {
                world.setRecording(true);
                while (next < save->commands.size() && save->commands[next].tick <= world.tickCount()) world.apply(save->commands[next++]);
                stepWorld();
            }
            std::printf("loaded: %llu ticks replayed with %zu orders in %.1f s, state %016llx\n", (unsigned long long)save->tick,
                        save->commands.size(), double(SDL_GetTicks() - t0) / 1000.0, (unsigned long long)world.stateHash());
            opt.haveCamera = true;
            for (int k = 0; k < 5; ++k) opt.cam[k] = save->camera[k];
        }
        RtsCamera cam;
        const Vec3 start = scene.startPosition(opt.player - 1);
        cam.x = start[0];
        cam.y = start[1];
        // Face the map centre from the start position.
        cam.yaw = std::atan2(map.sizeX() / 2 - cam.x, map.sizeY() / 2 - cam.y) * 180.f / 3.14159265f;
        if (opt.haveCamera) {
            cam.x = opt.cam[0];
            cam.y = opt.cam[1];
            cam.distance = opt.cam[2];
            cam.yaw = opt.cam[3];
            cam.pitch = opt.cam[4];
        }

        auto saveTo = [&](const std::string& path) {
            SaveGame sg;
            sg.setup = setup;
            sg.mods = modNames;
            sg.tick = world.tickCount();
            sg.player = opt.player;
            sg.spectate = opt.spectate;
            sg.camera[0] = cam.x;
            sg.camera[1] = cam.y;
            sg.camera[2] = cam.distance;
            sg.camera[3] = cam.yaw;
            sg.camera[4] = cam.pitch;
            sg.commands = world.commandLog();
            return writeSave(path, sg);
        };
        bool placeholders = true;
        View view;
        Hud hud(vfs, uiScale);
        PauseMenu pause(vfs, uiScale);
        pause.soundOn = audio && audio->volume > 0;
        pause.musicOn = audio && audio->musicVolume > 0;
        pause.fullGraphics = renderer.detail();
        bool paused = false;
        if (!lineupMode) hud.setMap(renderer, map, vfs);
        hud.researchOpen = opt.research;
        hud.helpOpen = std::getenv("UAW_SHOW_KEYS") != nullptr; // pictures of the key panel
        bool padConnected = false;
        auto drawFrame = [&](int w, int h) {
            Renderer::Camera rc{cam.eye(map), cam.target(map)};
            renderer.setFogOfWar(control.fog ? world.fogGrid(control.player()) : nullptr, world.fogWidth(), world.fogHeight(),
                                 world.fogCell(), world.tickCount() / 15);
            renderer.setTime(world.seconds());
            renderer.beginFrame(w, h, rc, std::max(6000.f, cam.distance * 4.f));
            view = View{rc.eye, rc.target, rc.fovY, w, h, renderer.viewProj()};
            if (lineupMode)
                renderer.drawBox(Mat4::translate(lineupFloor[0], lineupFloor[1], -1) * Mat4::scale3(lineupFloor[2], lineupFloor[2], 2),
                                 {0.36f, 0.38f, 0.36f, 1});
            else {
                scene.drawSky(renderer);
                scene.draw(renderer, placeholders);
            }
            control.draw(renderer, world, world.seconds());
            if (!lineupMode) renderer.drawWater();
            sounds(cam.target(map), cam.yaw);
            effects.watch(world, control);
            effects.update(std::clamp(world.seconds() - lastFrameTime, 0.f, 0.25f));
            lastFrameTime = world.seconds();
            renderer.endFrame();
            static const bool noParticles = std::getenv("UAW_NO_PARTICLES") != nullptr;
            if (!noParticles) effects.draw(renderer, view);
            {
                float mx = -1, my = -1;
                if (!headless) SDL_GetMouseState(&mx, &my);
                else if (const char* m = std::getenv("UAW_MOUSE")) std::sscanf(m, "%f,%f", &mx, &my); // pictures of tooltips
                hud.setMouse(mx, my);
            }
            hud.draw(renderer, world, control, db, text, padConnected, &view);
            if (ls && !ls->problem().empty()) hud.centreText(renderer, ls->problem());
            else if (ls && waitingSince && SDL_GetTicks() - waitingSince > 600) hud.centreText(renderer, "Waiting for the other players...");
            else if (paused && !pause.open) hud.centreText(renderer, "Paused");
            else if (opt.speed != 1) hud.centreText(renderer, opt.speed > 1 ? "Speed x" + std::to_string(int(opt.speed)) : "Speed x0.5", true);
            if (pause.open) pause.draw(renderer);
        };

        End end = End::Quit;
        if (headless) {
            if (opt.demo) {
                // Builder starts the first structure it can make; the HQ queues its first unit.
                uint32_t hq = 0, builder = 0;
                for (const Unit& u : world.units())
                    if (u.owner == control.player()) (u.structure ? hq : builder) = u.id;
                if (builder) {
                    const auto opts = world.constructible(builder);
                    const Vec3 b = world.unit(builder)->position;
                    for (const auto& t : opts) {
                        if (t.find("Remote_Terminal") != std::string::npos || t.find("Arrival_Site") != std::string::npos ||
                            t.find("Foundation") != std::string::npos)
                            continue;
                        bool done = false;
                        for (int k = 0; k < 16 && !done; ++k) {
                            const float a = float(k) * 0.4f;
                            const Vec3 site{b[0] + std::cos(a) * 200, b[1] + std::sin(a) * 200, 0};
                            done = world.commandConstruct(builder, t, site) == OrderResult::Ok;
                        }
                        if (done) {
                            for (const Unit& u : world.units())
                                if (u.underConstruction && u.owner == control.player())
                                    std::printf("demo: constructing %s at %.0f,%.0f\n", t.c_str(), u.position[0], u.position[1]);
                            break;
                        }
                    }
                }
                if (hq) {
                    for (const auto& t : world.producible(hq))
                        if (world.queueProduction(hq, t) == OrderResult::Ok) { std::printf("demo: queued %s\n", t.c_str()); break; }
                    control.clickSelect(world, world.unit(hq)->position, false);
                }
            }
            if (opt.haveOrder) {
                control.selectAll(world);
                const Vec3 target{opt.order[0], opt.order[1], map.heightAt(opt.order[0], opt.order[1])};
                control.moveTo(world, target);
            }
            const int ticks = int(opt.runSeconds * kTicksPerSecond);
            auto census = [&]() {
                int alive[4] = {0, 0, 0, 0};
                float hp[4] = {0, 0, 0, 0};
                for (const Unit& u : world.units()) {
                    alive[std::clamp(u.owner, 0, 3)]++;
                    hp[std::clamp(u.owner, 0, 3)] += u.health;
                }
                std::printf("t=%5.1fs  player1: %2d units %6.0f hp   player2: %2d units %6.0f hp   shots in flight %zu\n",
                            world.seconds(), alive[0], hp[0], alive[1], hp[1], world.projectiles().size());
            };
            if (opt.battle) census();
            for (int t = 1; t <= ticks; ++t) {
                if (ls) {
                    pumpNetwork();
                    const uint64_t waited = SDL_GetTicks();
                    while (!advance()) {
                        pumpNetwork();
                        SDL_Delay(1);
                        if (SDL_GetTicks() - waited > 15000 || !ls->problem().empty()) break;
                    }
                    if (SDL_GetTicks() - waited > 15000) { std::printf("network: stalled at tick %llu\n", (unsigned long long)world.tickCount()); break; }
                    if (!ls->problem().empty()) { std::printf("network: %s\n", ls->problem().c_str()); break; }
                } else {
                    advance();
                }
                if (ticks - t < 30 * 20) { // particles for the last 20 s, so the picture shows effects in flight
                    effects.watch(world, control);
                    effects.update(1.f / kTicksPerSecond);
                }
                if (opt.battle && t % (5 * kTicksPerSecond) == 0) census();
            }
            std::printf("state at tick %llu: %016llx\n", (unsigned long long)world.tickCount(), (unsigned long long)world.stateHash());
            if (opt.haveOrder) {
                size_t moving = 0;
                for (const Unit& u : world.units()) moving += u.owner == control.player() && u.moving();
                std::printf("after %.1f s: %zu of your units still moving\n", world.seconds(), moving);
            }
            if (!opt.select.empty()) {
                // Select your units whose type name contains the text; camera on them.
                control.clear();
                float sx = 0, sy = 0;
                int n = 0;
                std::string want = opt.select;
                for (char& c : want) c = char(std::toupper(static_cast<unsigned char>(c)));
                for (const Unit& u : world.units()) {
                    std::string name = u.type->name;
                    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
                    if (u.owner != control.player() || name.find(want) == std::string::npos) continue;
                    control.clickSelect(world, u.position, true);
                    sx += u.position[0];
                    sy += u.position[1];
                    ++n;
                }
                if (n && !opt.camSet) {
                    cam.x = sx / float(n);
                    cam.y = sy / float(n);
                }
                std::printf("selected %d units matching %s\n", n, opt.select.c_str());
            }
            if (!opt.saveAs.empty())
                std::printf("save %s: %s, state %016llx\n", opt.saveAs.c_str(), saveTo(opt.saveAs) ? "written" : "FAILED",
                            (unsigned long long)world.stateHash());
            int w, h;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (!opt.effect.empty()) {
                const size_t at = opt.effect.find('@');
                const std::string name = opt.effect.substr(0, at);
                const float age = at == std::string::npos ? 0.3f : float(std::atof(opt.effect.c_str() + at + 1));
                const Vec3 t = cam.target(map);
                std::printf("effect %s: %s\n", name.c_str(), effects.spawn(name, t) ? "started" : "not found");
                for (float tt = 0; tt < age; tt += 1.f / 30) effects.update(1.f / 30);
            }
            lastFrameTime = world.seconds();
            // Let attached effects (engine glows, flames) build up for half a second first.
            const auto frameStart = std::chrono::steady_clock::now();
            for (int k = 0; k < 15; ++k) {
                drawFrame(w, h);
                effects.update(1.f / 30);
            }
            drawFrame(w, h);
            (void)renderer.readPixels(1, 1); // wait for the GPU
            std::printf("frame time: %.1f ms\n",
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count() / 16);
            std::printf("particles: %zu in %zu systems\n", effects.particleCount(), effects.systemCount());
            Image img;
            img.width = uint32_t(w);
            img.height = uint32_t(h);
            img.rgba = renderer.readPixels(w, h);
            for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
            auto png = encodePng(img);
            std::ofstream(opt.screenshot, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            std::printf("screenshot %s (%dx%d), camera %.0f,%.0f,%.0f,%.0f,%.0f\n", opt.screenshot.c_str(), w, h, cam.x,
                        cam.y, cam.distance, cam.yaw, cam.pitch);
        } else {
            end = End::Quit;
            SDL_Gamepad* pad = nullptr;
            bool running = true;
            uint64_t last = SDL_GetTicksNS(), fpsTimer = last;
            double simAccumulator = 0;
            int frames = 0;
            // Mouse drag state for box selection / right-click orders.
            bool leftDown = false, rightDown = false, rightDragged = false, attackMoveArmed = false, superweaponArmed = false,
                 patrolArmed = false;
            int lastGroupKey = -1;
            uint64_t lastDeleteTime = 0;
            uint64_t lastGroupTime = 0;
            float downX = 0, downY = 0, mouseX = 0, mouseY = 0;
            while (running) {
                const bool* k = SDL_GetKeyboardState(nullptr);
                const bool shift = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (pause.open && e.type != SDL_EVENT_QUIT && e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) {
                        switch (pause.handle(e)) {
                        case PauseMenu::Result::Resume: pause.open = false; break;
                        case PauseMenu::Result::Restart: end = End::Restart; running = false; break;
                        case PauseMenu::Result::ToMenu: end = End::ToMenu; running = false; break;
                        case PauseMenu::Result::Quit: end = End::Quit; running = false; break;
                        case PauseMenu::Result::Save:
                            control.say(saveTo(saveFolder() + "quicksave.uawsave") ? "Game saved" : "Could not save the game", world.seconds());
                            pause.open = false;
                            break;
                        case PauseMenu::Result::Load:
                            loadPath = saveFolder() + "quicksave.uawsave";
                            opt.replay = false;
                            end = End::Load;
                            running = false;
                            break;
                        case PauseMenu::Result::Sound:
                            if (audio) audio->volume = audio->volume > 0 ? 0.f : 0.8f;
                            pause.soundOn = audio && audio->volume > 0;
                            keepSettings();
                            break;
                        case PauseMenu::Result::Music:
                            if (audio) audio->musicVolume = audio->musicVolume > 0 ? 0.f : 0.6f;
                            pause.musicOn = audio && audio->musicVolume > 0;
                            keepSettings();
                            break;
                        case PauseMenu::Result::Graphics:
                            renderer.setDetail(!renderer.detail());
                            pause.fullGraphics = renderer.detail();
                            keepSettings();
                            break;
                        case PauseMenu::Result::None: break;
                        }
                        continue;
                    }
                    switch (e.type) {
                    case SDL_EVENT_QUIT: running = false; end = End::Quit; break;
                    case SDL_EVENT_KEY_DOWN:
                        if (e.key.key == SDLK_ESCAPE) {
                            if (control.placing()) control.cancelPlacement();
                            else if (control.abilityArmed()) control.cancelAbility();
                            else if (hud.researchOpen) hud.researchOpen = false;
                            else if (hud.helpOpen) hud.helpOpen = false;
                            else pause.open = true;
                        } else if (e.key.key == SDLK_F5 && ls) {
                            control.say("Network games can't be saved", world.seconds());
                        } else if (e.key.key == SDLK_F5) {
                            const std::string path = saveFolder() + "quicksave.uawsave";
                            control.say(saveTo(path) ? "Game saved" : "Could not save the game", world.seconds());
                            std::printf("saved %s\n", path.c_str());
                        } else if (e.key.key == SDLK_F9) {
                            loadPath = saveFolder() + "quicksave.uawsave";
                            opt.replay = false;
                            end = End::Load;
                            running = false;
                        } else if (e.key.key == SDLK_PAUSE) {
                            paused = !paused;
                        } else if (e.key.key == SDLK_RIGHTBRACKET) {
                            opt.speed = std::min(4.f, opt.speed * 2);
                        } else if (e.key.key == SDLK_LEFTBRACKET) {
                            opt.speed = std::max(0.5f, opt.speed / 2);
                        } else if (e.key.key >= SDLK_F1 && e.key.key <= SDLK_F8) {
                            control.useAbility(world, size_t(e.key.key - SDLK_F1));
                        } else if (e.key.key >= SDLK_0 && e.key.key <= SDLK_9 && (e.key.mod & SDL_KMOD_CTRL)) {
                            control.storeGroup(int(e.key.key - SDLK_0));
                            control.say("Group " + std::to_string(int(e.key.key - SDLK_0)) + " set", world.seconds());
                        } else if (e.key.key >= SDLK_0 && e.key.key <= SDLK_9 && !control.placing() &&
                                   control.hasGroup(world, int(e.key.key - SDLK_0))) {
                            // A control group: select it; pressed twice quickly, look at it too.
                            const int n = int(e.key.key - SDLK_0);
                            const uint64_t nowMs = SDL_GetTicks();
                            control.recallGroup(world, n);
                            if (n == lastGroupKey && nowMs - lastGroupTime < 400 && !control.selection().empty()) {
                                float sx = 0, sy = 0;
                                for (uint32_t id : control.selection())
                                    if (const Unit* u = world.unit(id)) { sx += u->position[0]; sy += u->position[1]; }
                                cam.x = sx / float(control.selection().size());
                                cam.y = sy / float(control.selection().size());
                            }
                            lastGroupKey = n;
                            lastGroupTime = nowMs;
                        } else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_9 && !control.placing()) {
                            const auto items = control.menu(world, db, text);
                            const size_t i = size_t(e.key.key - SDLK_1);
                            if (i < items.size()) control.activate(world, items[i]);
                        } else if (e.key.key == SDLK_BACKSPACE) control.cancelQueue(world);
                        else if (e.key.key == SDLK_F11) placeholders = !placeholders;
                        else if (e.key.key == SDLK_P && !control.selection().empty()) {
                            patrolArmed = true; // next right click: patrol between here and there
                            control.say("Patrol: right click the far end", world.seconds());
                        }
                        else if (e.key.key == SDLK_TAB) control.selectAll(world);
                        else if (e.key.key == SDLK_PERIOD) { // next idle builder, camera to it
                            Vec3 at;
                            if (control.selectIdleBuilder(world, at)) { cam.x = at[0]; cam.y = at[1]; }
                            else control.say("No idle builders", world.seconds());
                        }
                        else if (e.key.key == SDLK_COMMA) control.selectArmy(world);
                        else if (e.key.key == SDLK_H) { // camera to your base (the oldest structure you have)
                            for (const Unit& u : world.units())
                                if (u.owner == control.player() && u.structure) { cam.x = u.position[0]; cam.y = u.position[1]; break; }
                        }
                        else if (e.key.key == SDLK_M && audio) { // sound on / off
                            audio->volume = audio->volume > 0 ? 0.f : 0.8f;
                            control.say(audio->volume > 0 ? "Sound on" : "Sound off", world.seconds());
                            keepSettings();
                            pause.soundOn = audio->volume > 0;
                        }
                        else if (e.key.key == SDLK_X) control.stop(world);
                        else if (e.key.key == SDLK_U) control.unload(world);
                        else if (e.key.key == SDLK_HOME) {
                            Vec3 where;
                            if (hud.lastAlert(where)) { cam.x = where[0]; cam.y = where[1]; }
                        }
                        else if (e.key.key == SDLK_DELETE) {
                            // Twice within two seconds, so a stray key press doesn't sell the base.
                            const uint64_t nowMs = SDL_GetTicks();
                            if (nowMs - lastDeleteTime < 2000) control.sell(world);
                            else control.say("Press Delete again to sell", world.seconds());
                            lastDeleteTime = nowMs;
                        }
                        else if (e.key.key == SDLK_G) attackMoveArmed = true; // next right click is an attack-move
                        else if (e.key.key == SDLK_V) superweaponArmed = true; // next right click fires the selected superweapon
                        else if (e.key.key == SDLK_T) hud.researchOpen = !hud.researchOpen;
                        else if (e.key.key == SDLK_F10 || e.key.key == SDLK_QUESTION || (e.key.key == SDLK_SLASH && shift))
                            hud.helpOpen = !hud.helpOpen;
                        else if (e.key.key == SDLK_SPACE && !control.selection().empty()) {
                            float sx = 0, sy = 0;
                            for (uint32_t id : control.selection())
                                if (const Unit* u = world.unit(id)) { sx += u->position[0]; sy += u->position[1]; }
                            cam.x = sx / float(control.selection().size());
                            cam.y = sy / float(control.selection().size());
                        }
                        break;
                    case SDL_EVENT_MOUSE_WHEEL: cam.zoom(std::pow(0.88f, e.wheel.y)); break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        if (e.button.button == SDL_BUTTON_LEFT) {
                            int item;
                            Vec3 mm;
                            std::string rpath;
                            int rindex = 0;
                            if (control.placing()) {
                                control.place(world);
                            } else if (hud.researchAt(e.button.x, e.button.y, rpath, rindex)) {
                                const OrderResult rr = world.startResearch(control.player(), rpath, rindex);
                                control.say(rr == OrderResult::Ok ? "Research started" : rr == OrderResult::NotEnoughCredits ? "Not enough credits" : "Can't research that now", world.seconds());
                            } else if (hud.minimapAt(e.button.x, e.button.y, mm)) {
                                cam.x = mm[0];
                                cam.y = mm[1];
                            } else if (hud.menuItemAt(e.button.x, e.button.y, item)) {
                                const auto items = control.menu(world, db, text);
                                if (size_t(item) < items.size()) control.activate(world, items[size_t(item)]);
                            } else {
                                leftDown = true;
                                downX = e.button.x;
                                downY = e.button.y;
                            }
                        }
                        if (e.button.button == SDL_BUTTON_RIGHT) {
                            if (control.placing()) control.cancelPlacement();
                            else { rightDown = true; rightDragged = false; }
                        }
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_UP:
                        if (e.button.button == SDL_BUTTON_LEFT && leftDown) {
                            leftDown = false;
                            if (std::hypot(e.button.x - downX, e.button.y - downY) < 6) {
                                Vec3 g;
                                if (e.button.clicks >= 2 && view.groundAt(map, e.button.x, e.button.y, g)) control.selectSameType(world, view, g);
                                else if (view.groundAt(map, e.button.x, e.button.y, g)) control.clickSelect(world, g, shift);
                                else if (!shift) control.clear();
                            } else {
                                control.boxSelect(world, view, downX, downY, e.button.x, e.button.y, shift);
                            }
                        }
                        if (e.button.button == SDL_BUTTON_RIGHT && rightDown) {
                            rightDown = false;
                            Vec3 g, mm;
                            if (!rightDragged && hud.minimapAt(e.button.x, e.button.y, mm)) {
                                // Right click on the minimap: the same orders, at that spot of the map.
                                mm[2] = map.heightAt(mm[0], mm[1]);
                                if (patrolArmed) control.patrol(world, mm);
                                else if (k[SDL_SCANCODE_A] || attackMoveArmed) control.attackMove(world, mm);
                                else control.command(world, mm, k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]);
                                attackMoveArmed = patrolArmed = false;
                            } else if (!rightDragged && view.groundAt(map, e.button.x, e.button.y, g)) {
                                if (control.abilityArmed()) control.targetAbility(world, g);
                                else if (superweaponArmed) control.fireSuperweapon(world, g);
                                else if (patrolArmed) control.patrol(world, g);
                                else if (k[SDL_SCANCODE_A] || attackMoveArmed) control.attackMove(world, g);
                                else control.command(world, g, k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]);
                                attackMoveArmed = superweaponArmed = patrolArmed = false;
                            }
                        }
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                        mouseX = e.motion.x;
                        mouseY = e.motion.y;
                        if (control.placing() && !pad) {
                            Vec3 g;
                            if (view.groundAt(map, mouseX, mouseY, g)) control.setGhost(g);
                        }
                        if (e.motion.state & (SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) {
                            if (std::fabs(e.motion.xrel) + std::fabs(e.motion.yrel) > 0.5f) rightDragged = true;
                            cam.rotate(e.motion.xrel * 0.3f, -e.motion.yrel * 0.2f);
                        }
                        break;
                    case SDL_EVENT_GAMEPAD_ADDED:
                        if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
                        break;
                    case SDL_EVENT_GAMEPAD_REMOVED:
                        if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = nullptr; }
                        break;
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                        Vec3 g;
                        const bool reticle = view.groundAt(map, view.width / 2.f, view.height / 2.f, g);
                        if (control.placing()) {
                            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH && reticle) { control.setGhost(g); control.place(world); }
                            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) control.cancelPlacement();
                            break;
                        }
                        switch (e.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_BACK:
                        case SDL_GAMEPAD_BUTTON_START: pause.open = true; break;
                        case SDL_GAMEPAD_BUTTON_NORTH: placeholders = !placeholders; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: --control.highlight(); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: ++control.highlight(); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: {
                            const auto items = control.menu(world, db, text);
                            const int h = control.highlight();
                            if (h >= 0 && size_t(h) < items.size()) control.activate(world, items[size_t(h)]);
                            break;
                        }
                        case SDL_GAMEPAD_BUTTON_LEFT_STICK: control.cancelQueue(world); break;
                        case SDL_GAMEPAD_BUTTON_SOUTH:
                            if (reticle) control.clickSelect(world, g, pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
                            break;
                        case SDL_GAMEPAD_BUTTON_WEST: control.selectAllVisible(world, view); break;
                        case SDL_GAMEPAD_BUTTON_EAST: control.clear(); break;
                        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                            if (reticle) control.command(world, g);
                            break;
                        case SDL_GAMEPAD_BUTTON_DPAD_UP:
                            if (reticle) control.attackMove(world, g);
                            break;
                        default: break;
                        }
                        break;
                    }
                    default: break;
                    }
                }
                const uint64_t now = SDL_GetTicksNS();
                const float dt = std::min(float(now - last) * 1e-9f, 0.1f);
                last = now;

                float fwd = float(k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) - float(k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]);
                float right = float(k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - float(k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]);
                float turn = float(k[SDL_SCANCODE_E]) - float(k[SDL_SCANCODE_Q]);
                float tilt = float(k[SDL_SCANCODE_R]) - float(k[SDL_SCANCODE_F]);
                float zoomIn = float(k[SDL_SCANCODE_EQUALS] || k[SDL_SCANCODE_KP_PLUS]) - float(k[SDL_SCANCODE_MINUS] || k[SDL_SCANCODE_KP_MINUS]);
                if (pad) {
                    fwd -= axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
                    right += axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
                    turn += axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
                    tilt -= axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
                    zoomIn += (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) / 32767.f;
                }
                cam.pan(fwd, right, dt);
                cam.rotate(turn * 90.f * dt, tilt * 45.f * dt);
                if (zoomIn != 0) cam.zoom(std::pow(0.3f, zoomIn * dt));
                cam.clampTo(map);

                // Fixed-rate simulation, decoupled from the frame rate (stopped while paused).
                if (ls) { // network games run at normal speed and can't be paused
                    opt.speed = 1;
                    paused = false;
                }
                pumpNetwork();
                if (!paused && (!pause.open || ls)) simAccumulator += dt * opt.speed;
                int steps = 0;
                while (simAccumulator >= 1.0 / kTicksPerSecond && steps < 8 * int(std::max(1.f, opt.speed))) {
                    if (!advance()) {
                        simAccumulator = std::min(simAccumulator, 0.2); // waiting for the others: don't race afterwards
                        if (!waitingSince) waitingSince = SDL_GetTicks();
                        break;
                    }
                    waitingSince = 0;
                    simAccumulator -= 1.0 / kTicksPerSecond;
                    ++steps;
                }
                if (ls && !ls->problem().empty() && int(world.seconds()) % 5 == 0) control.say(ls->problem(), world.seconds());

                padConnected = pad != nullptr;
                if (pad && control.placing()) {
                    Vec3 g;
                    if (view.groundAt(map, view.width / 2.f, view.height / 2.f, g)) control.setGhost(g);
                }
                int w, h;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                drawFrame(w, h);
                if (leftDown && std::hypot(mouseX - downX, mouseY - downY) >= 6) control.drawBox(renderer, view, downX, downY, mouseX, mouseY);
                if (pad) {
                    const float cx = w / 2.f, cy = h / 2.f;
                    renderer.drawOverlayRect(cx - 10, cy - 1, cx + 10, cy + 1, {1, 1, 1, 0.8f});
                    renderer.drawOverlayRect(cx - 1, cy - 10, cx + 1, cy + 10, {1, 1, 1, 0.8f});
                }
                SDL_GL_SwapWindow(window);

                ++frames;
                if (now - fpsTimer > 1'000'000'000ull) {
                    char title[200];
                    std::snprintf(title, sizeof title, "Universe at War - %s - %d fps - %zu selected%s", opt.map.c_str(),
                                  frames, control.selection().size(), pad ? " - gamepad" : "");
                    SDL_SetWindowTitle(window, title);
                    frames = 0;
                    fpsTimer = now;
                }
            }
            if (pad) SDL_CloseGamepad(pad);
        }
        return end;
    };

        // Setup screen, or straight into the map from the command line; then games until quit.
        SkirmishSetup setup;
        if (!opt.map.empty()) {
            // Command line: you in slot --player, the opponent in the next slot.
            setup.map = opt.map;
            setup.fog = opt.fog;
            const Map probe = loadTed(vfs.read(mapPath(opt.map)));
            int nStarts = 0;
            for (const auto& o : probe.objects)
                if (const GameObjectType* t = db.findByCrc(o.typeCrc); t && t->name.rfind("Marker_Player_", 0) == 0) ++nStarts;
            setup.slots.resize(size_t(std::max(nStarts, 2)));
            const int you = std::min(opt.player - 1, int(setup.slots.size()) - 1);
            opt.player = you + 1;
            setup.slots[size_t(you)] = {PlayerSlot::Kind::Human, opt.faction, -1};
            setup.slots[size_t((you + 1) % int(setup.slots.size()))] = {PlayerSlot::Kind::Computer, opt.enemy, -1};
            if (!opt.slots.empty()) {
                setup.slots.assign(setup.slots.size(), PlayerSlot{});
                size_t k = 0, pos = 0;
                while (pos <= opt.slots.size() && k < setup.slots.size()) {
                    size_t e = opt.slots.find(',', pos);
                    if (e == std::string::npos) e = opt.slots.size();
                    const std::string item = opt.slots.substr(pos, e - pos);
                    pos = e + 1;
                    PlayerSlot& ps = setup.slots[k++];
                    const size_t c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
                    const std::string kind = item.substr(0, c1);
                    ps.kind = kind == "you" ? PlayerSlot::Kind::Human : kind == "ai" ? PlayerSlot::Kind::Computer : PlayerSlot::Kind::Open;
                    if (c1 != std::string::npos) ps.faction = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                    if (c2 != std::string::npos) ps.team = std::atoi(item.c_str() + c2 + 1) - 1;
                }
                const int human = setup.human();
                opt.player = human >= 0 ? human + 1 : 1;
                opt.spectate = opt.spectate || human < 0; // --spectate: an AI plays your slot too
            }
        }
        std::unique_ptr<SkirmishMenu> menu;
        End end = !opt.load.empty() ? End::Load : (opt.menu || opt.host || !opt.join.empty()) ? End::ToMenu : End::Restart;
        if (!opt.load.empty()) loadPath = opt.load;
        SaveGame loaded;
        while (end != End::Quit) {
            if (end == End::Load) {
                std::string err;
                if (readSave(loadPath, loaded, &err)) {
                    if (loaded.mods != modNames) std::fprintf(stderr, "warning: the game was saved with other mods loaded\n");
                    setup = loaded.setup;
                    opt.player = loaded.player;
                    opt.spectate = loaded.spectate;
                    opt.map = setup.map;
                    end = runSession(setup, &loaded);
                    if (headless) break;
                    continue;
                }
                std::fprintf(stderr, "can't load %s: %s\n", loadPath.c_str(), err.c_str());
                end = menu || opt.menu ? End::ToMenu : End::Quit;
                if (headless) break;
                continue;
            }
            if (end == End::ToMenu) {
                if (!menu) {
                    menu = std::make_unique<SkirmishMenu>(vfs, db, modNames, uiScale);
                    if (!opt.map.empty()) menu->selectMap(opt.map);
                    else if (!headless) { // the last game's setup (map, players, options)
                        std::ifstream last(saveFolder() + "lastsetup.txt");
                        std::string line;
                        SkirmishSetup prev;
                        if (std::getline(last, line) && decodeSetup(line, prev)) {
                            menu->selectMap(prev.map);
                            if (alamo::iequals(menu->setup().map, prev.map) && menu->setup().slots.size() == prev.slots.size()) {
                                bool human = false;
                                for (PlayerSlot& p : prev.slots) { // a LAN game's other people become computers
                                    p.netId = -1;
                                    if (p.kind == PlayerSlot::Kind::Human && human) p.kind = PlayerSlot::Kind::Computer;
                                    human = human || p.kind == PlayerSlot::Kind::Human;
                                }
                                menu->takeSetup(prev);
                            }
                        }
                    }
                }
                if (menu->empty()) throw std::runtime_error("no skirmish maps found");
                bool done = false, quit = false, loadGame = false;
                menu->quickSave = saveFolder() + "quicksave.uawsave";
                // LAN games: host or join (from the command line, or the buttons on the screen).
                auto startHost = [&]() {
                    std::string err;
                    lockstep = Lockstep::host(net::kDefaultPort, err);
                    if (!lockstep) {
                        menu->lobbyStatus = "Can't host a game: " + err;
                        std::fprintf(stderr, "can't host: %s\n", err.c_str());
                        return;
                    }
                    menu->lobby = SkirmishMenu::Lobby::Host;
                    menu->lobbyId = 0;
                    for (PlayerSlot& slot : menu->editSetup().slots)
                        if (slot.kind == PlayerSlot::Kind::Human) slot.netId = 0;
                };
                auto startJoin = [&](const std::string& address) {
                    std::string err;
                    lockstep = Lockstep::join(address, opt.name, err);
                    if (!lockstep) {
                        menu->lobbyStatus = "Can't join: " + err;
                        std::fprintf(stderr, "can't join %s: %s\n", address.c_str(), err.c_str());
                        return;
                    }
                    menu->lobby = SkirmishMenu::Lobby::Client;
                };
                auto leaveLobby = [&]() {
                    lockstep.reset();
                    menu->lobby = SkirmishMenu::Lobby::None;
                    menu->lobbyPlayers.clear();
                };
                if (opt.host && !lockstep) startHost();
                if (!opt.join.empty() && !lockstep) startJoin(opt.join);
                opt.host = false;
                opt.join.clear();
                auto pumpLobby = [&]() {
                    if (!lockstep) return;
                    if (lockstep->isHost()) {
                        lockstep->pollLobby(menu->editSetup());
                    } else {
                        SkirmishSetup t = menu->setup();
                        if (lockstep->pollLobby(t)) menu->takeSetup(t);
                        if (lockstep->started()) done = true;
                        if (!lockstep->problem().empty()) {
                            menu->lobbyStatus = lockstep->problem();
                            leaveLobby();
                            return;
                        }
                    }
                    menu->lobbyStatus = lockstep->status();
                    menu->lobbyPlayers = lockstep->players();
                    menu->lobbyId = lockstep->id();
                };
                if (headless && lockstep) {
                    // Headless LAN game (tests): the host starts once --players are in; a client waits.
                    const uint64_t t0 = SDL_GetTicks();
                    while (!done && SDL_GetTicks() - t0 < 30000) {
                        pumpLobby();
                        if (lockstep && lockstep->isHost() && int(lockstep->players().size()) >= std::max(2, opt.waitPlayers)) {
                            SDL_Delay(200);
                            pumpLobby();
                            lockstep->start(menu->setup());
                            done = true;
                        }
                        SDL_Delay(5);
                    }
                    if (!done || !lockstep) {
                        std::printf("network: no game started\n");
                        break;
                    }
                } else if (headless) { // a picture of the setup screen
                    int w, h;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    renderer.beginFrame(w, h, Renderer::Camera{{0, 0, 1}, {0, 1, 0}}, 100);
                    renderer.endFrame();
                    menu->draw(renderer);
                    Image img;
                    img.width = uint32_t(w);
                    img.height = uint32_t(h);
                    img.rgba = renderer.readPixels(w, h);
                    for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
                    auto png = encodePng(img);
                    std::ofstream(opt.screenshot, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
                    std::printf("screenshot %s (setup screen)\n", opt.screenshot.c_str());
                    break;
                }
                SDL_SetWindowTitle(window, "Universe at War - Skirmish");
                SDL_StartTextInput(window);
                SDL_Gamepad* pad = nullptr;
                while (!done) {
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) {
                        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
                        const SkirmishMenu::Result r = menu->handle(e);
                        if (r == SkirmishMenu::Result::Start) {
                            if (lockstep && lockstep->isHost()) lockstep->start(menu->setup());
                            std::ofstream(saveFolder() + "lastsetup.txt") << encodeSetup(menu->setup()) << "\n";
                            done = true;
                        }
                        if (r == SkirmishMenu::Result::Quit) {
                            if (lockstep && !lockstep->isHost() && e.type != SDL_EVENT_QUIT) leaveLobby(); // back to the setup screen
                            else done = quit = true;
                        }
                        if (r == SkirmishMenu::Result::Load) { done = loadGame = true; opt.replay = false; }
                        if (r == SkirmishMenu::Result::Replay) { done = loadGame = true; opt.replay = true; }
                        if (r == SkirmishMenu::Result::Host) startHost();
                        if (r == SkirmishMenu::Result::Join) startJoin(menu->joinAddress);
                    }
                    pumpLobby();
                    int w, h;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    renderer.beginFrame(w, h, Renderer::Camera{{0, 0, 1}, {0, 1, 0}}, 100);
                    renderer.endFrame();
                    menu->draw(renderer);
                    SDL_GL_SwapWindow(window);
                    if (lockstep) SDL_Delay(5);
                }
                SDL_StopTextInput(window);
                if (pad) SDL_CloseGamepad(pad);
                if (quit) break;
                if (loadGame) {
                    loadPath = menu->quickSave;
                    end = End::Load;
                    continue;
                }
                setup = menu->setup();
                int human = setup.human();
                if (lockstep) { // your seat is the one with your connection id
                    human = -1;
                    for (size_t k = 0; k < setup.slots.size(); ++k)
                        if (setup.slots[k].kind == PlayerSlot::Kind::Human && setup.slots[k].netId == lockstep->id()) human = int(k);
                }
                opt.player = human >= 0 ? human + 1 : 1;
                opt.spectate = human < 0;
                opt.haveCamera = opt.camSet = false;
            }
            end = runSession(setup, nullptr);
            if (lockstep) { // a LAN game is over for this machine
                lockstep.reset();
                if (menu) {
                    menu->lobby = SkirmishMenu::Lobby::None;
                    menu->lobbyPlayers.clear();
                }
            }
            if (headless) break;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        exitCode = 1;
    }
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
