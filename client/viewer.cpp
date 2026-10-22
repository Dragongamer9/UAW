// uaw-viewer: a Universe at War map with its objects placed and test squads to command.
//
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
//     --size WxH          window size (default 1600x900)
//     --camera x,y,dist,yaw,pitch
//     --screenshot FILE   render one frame to a PNG and exit (works headless)
//     --order X,Y         (with --screenshot) select your squad and order it to X,Y
//     --run SECONDS       (with --screenshot) simulate this long before the picture
//
// Controls
//   keyboard  WASD / arrows pan, Q/E rotate, R/F tilt, wheel or +/- zoom,
//             Tab select all yours, Space jump to selection, X stop, G (or hold A) + right click attack-move,
//             P stand-in boxes, Esc quit
//   mouse     left click/drag select (Shift adds), right click move, middle or right drag rotate/tilt
//   build     select a structure or builder: 1-9 / click the menu (D-pad left/right + down on a gamepad);
//             builders then place the structure with a click / A (right click / B cancels);
//             Backspace (left stick click) cancels the last queued item; right click with a structure sets its rally point
//   gamepad   left stick pan, right stick rotate/tilt, triggers zoom; reticle at screen centre:
//             A select (hold LB to add), X select all on screen, B clear, RB move/attack, D-pad up attack-move,
//             Y stand-in boxes, Back quit
//   right click / RB on an enemy unit orders an attack; units also fire at enemies in range on their own
#include "camera.hpp"
#include "gl.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "hud.hpp"
#include "units.hpp"

#include "alamo/ai.hpp"
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
    std::string faction = "Novus", enemy = "Alien";
    float cam[5] = {0, 0, 0, 0, 0};
    float order[2] = {0, 0};
    float runSeconds = 0;
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
        o.map = "M16_Sahara";
        return !o.gameRoot.empty();
    }
    if (argc < 3) return false;
    o.gameRoot = argv[1];
    o.map = argv[2];
    for (int i = 3; i < argc; ++i) {
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
            "usage: uaw-viewer <game-root> <MAP> [--player N] [--all-groups] [--no-units] [--size WxH]\n"
            "                  [--camera x,y,dist,yaw,pitch] [--screenshot out.png [--order X,Y] [--run SECONDS]]\n\n"
            "<game-root> is the folder containing Data\\MegaFiles.xml.\n"
            "Run with no arguments from inside that folder to open M16_Sahara.";
        std::fprintf(stderr, "%s\n", usage);
        if (argc == 1) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "uaw-viewer", usage, nullptr);
        return 2;
    }

    MegVfs vfs;
    GameDatabase db;
    Map map;
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
        for (const auto& m : mods) SDL_Log("mod: %s", m.filename().string().c_str());
        mountGame(vfs, opt.gameRoot, mods);
        db.load(vfs);
        const std::string path = mapPath(opt.map);
        if (!vfs.exists(path)) throw std::runtime_error("map not found: " + path);
        map = loadTed(vfs.read(path));
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
        renderer.setTerrain(map);
        Scene scene(renderer, db, map, opt.allGroups);
        const SceneStats& st = scene.stats();
        std::printf("map %s: %ux%u, %zu objects (%zu with models, %zu placeholders), %zu start positions\n",
                    opt.map.c_str(), map.width, map.height, st.objects, st.withModel, st.placeholders, st.starts);

        // Simulation with a test squad at each start position.
        const DamageTable damageTable = loadDamageTable(vfs);
        const TechTree techTree = loadTechTree(vfs);
        const StringTable text = loadStrings(vfs);
        GameDatabase effects;
        try { effects.loadEffects(vfs); } catch (const std::exception& e) { SDL_Log("effects: %s", e.what()); }
        World world(map, db, &damageTable, 1, &techTree);
        world.setEffects(&effects);
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
        } else if (opt.units && opt.squads) {
            spawnTestSquads(world, starts, map);
        } else if (opt.units) {
            // Skirmish start: you at your start position, the opponent at the next one.
            const int you = opt.player - 1;
            const int them = (you + 1) % int(starts.size());
            const StartingForces forces = loadStartingForces(vfs);
            for (int slot = 0; slot < int(starts.size()); ++slot) {
                if (slot == you) addSkirmishPlayer(world, db, opt.faction, starts[size_t(slot)], &forces);
                else if (slot == them) addSkirmishPlayer(world, db, opt.enemy, starts[size_t(slot)], &forces);
                else world.addPlayer("Neutral", 0, 0);
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
                for (int tries = 0; tries < 64 && !placed; ++tries, ++k) {
                    const float ring = 260 + 110 * float(k / 8), a = toCentre + (float(k % 8) - 3.5f) * 0.42f;
                    const Vec3 p{base[0] + std::cos(a) * ring, base[1] + std::sin(a) * ring, 0};
                    if (!world.siteClear(*t, p)) continue;
                    placed = world.spawn(t->name, me, p, toCentre) != 0;
                }
                std::printf("spawn: %s %s\n", name.c_str(), placed ? "placed" : "no room");
            }
        }
        // Computer opponents (and your side with --spectate) for a skirmish start.
        std::vector<SkirmishAI> ais;
        if (opt.units && !opt.squads && !opt.battle && opt.ai)
            for (const Player& p : world.players())
                if (p.faction != "Neutral" && (p.id != opt.player - 1 || opt.spectate)) ais.emplace_back(world, p.id, uint64_t(p.id) + 1);
        auto stepWorld = [&]() {
            for (auto& ai : ais) ai.update();
            world.tick();
        };
        if (world.players().empty())
            for (size_t i = 0; i < std::max<size_t>(starts.size(), 4); ++i) world.addPlayer("Test", 10000, 90);
        std::printf("strings: %zu, tech tree: %zu entries\n", text.size(), techTree.size());
        UnitControl control(opt.player - 1);
        std::printf("spawned %zu units; models loaded %zu, missing %zu; textures missing %zu\n", world.units().size(),
                    renderer.modelsLoaded(), renderer.modelsMissing(), renderer.texturesMissing());

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

        bool placeholders = true;
        View view;
        Hud hud(vfs, std::max(0.75f, float(opt.height) / 900.f));
        bool padConnected = false;
        auto drawFrame = [&](int w, int h) {
            Renderer::Camera rc{cam.eye(map), cam.target(map)};
            renderer.beginFrame(w, h, rc, std::max(6000.f, cam.distance * 4.f));
            view = View{rc.eye, rc.target, rc.fovY, w, h, renderer.viewProj()};
            scene.draw(renderer, placeholders);
            control.draw(renderer, world, world.seconds());
            renderer.endFrame();
            hud.draw(renderer, world, control, db, text, padConnected);
        };

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
                        if (done) { std::printf("demo: constructing %s\n", t.c_str()); break; }
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
                stepWorld();
                if (opt.battle && t % (5 * kTicksPerSecond) == 0) census();
            }
            if (opt.haveOrder) {
                size_t moving = 0;
                for (const Unit& u : world.units()) moving += u.owner == control.player() && u.moving();
                std::printf("after %.1f s: %zu of your units still moving\n", world.seconds(), moving);
            }
            int w, h;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            drawFrame(w, h);
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
            SDL_Gamepad* pad = nullptr;
            bool running = true;
            uint64_t last = SDL_GetTicksNS(), fpsTimer = last;
            double simAccumulator = 0;
            int frames = 0;
            // Mouse drag state for box selection / right-click orders.
            bool leftDown = false, rightDown = false, rightDragged = false, attackMoveArmed = false;
            float downX = 0, downY = 0, mouseX = 0, mouseY = 0;
            while (running) {
                const bool* k = SDL_GetKeyboardState(nullptr);
                const bool shift = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    switch (e.type) {
                    case SDL_EVENT_QUIT: running = false; break;
                    case SDL_EVENT_KEY_DOWN:
                        if (e.key.key == SDLK_ESCAPE) {
                            if (control.placing()) control.cancelPlacement();
                            else running = false;
                        } else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_9 && !control.placing()) {
                            const auto items = control.menu(world, db, text);
                            const size_t i = size_t(e.key.key - SDLK_1);
                            if (i < items.size()) control.activate(world, items[i]);
                        } else if (e.key.key == SDLK_BACKSPACE) control.cancelQueue(world);
                        else if (e.key.key == SDLK_P) placeholders = !placeholders;
                        else if (e.key.key == SDLK_TAB) control.selectAll(world);
                        else if (e.key.key == SDLK_X) control.stop(world);
                        else if (e.key.key == SDLK_G) attackMoveArmed = true; // next right click is an attack-move
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
                            if (control.placing()) {
                                control.place(world);
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
                                if (view.groundAt(map, e.button.x, e.button.y, g)) control.clickSelect(world, g, shift);
                                else if (!shift) control.clear();
                            } else {
                                control.boxSelect(world, view, downX, downY, e.button.x, e.button.y, shift);
                            }
                        }
                        if (e.button.button == SDL_BUTTON_RIGHT && rightDown) {
                            rightDown = false;
                            Vec3 g;
                            if (!rightDragged && view.groundAt(map, e.button.x, e.button.y, g)) {
                                if (k[SDL_SCANCODE_A] || attackMoveArmed) control.attackMove(world, g);
                                else control.command(world, g);
                                attackMoveArmed = false;
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
                        case SDL_GAMEPAD_BUTTON_BACK: running = false; break;
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

                // Fixed-rate simulation, decoupled from the frame rate.
                simAccumulator += dt;
                int steps = 0;
                while (simAccumulator >= 1.0 / kTicksPerSecond && steps < 8) {
                    stepWorld();
                    simAccumulator -= 1.0 / kTicksPerSecond;
                    ++steps;
                }

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
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        exitCode = 1;
    }
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
