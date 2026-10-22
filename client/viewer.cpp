// uaw-viewer: a Universe at War map with its objects placed and test squads to command.
//
//   uaw-viewer <game-root> <MAP> [options]
//     --player N          you control player N's squad; camera starts there (default 1)
//     --all-groups        also show campaign-mission objects
//     --no-units          don't spawn test squads
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
//   gamepad   left stick pan, right stick rotate/tilt, triggers zoom; reticle at screen centre:
//             A select (hold LB to add), X select all on screen, B clear, RB move/attack, D-pad up attack-move,
//             Y stand-in boxes, Back quit
//   right click / RB on an enemy unit orders an attack; units also fire at enemies in range on their own
#include "camera.hpp"
#include "gl.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "units.hpp"

#include "alamo/assets.hpp"
#include "alamo/combat.hpp"
#include "alamo/dds.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/sim.hpp"
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
    std::string gameRoot, map, screenshot;
    int width = 1600, height = 900, player = 1;
    bool allGroups = false, haveCamera = false, camSet = false, units = true, haveOrder = false, battle = false;
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
        mountGame(vfs, opt.gameRoot);
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
        World world(map, db, &damageTable);
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
        } else if (opt.units) {
            spawnTestSquads(world, starts, map);
        }
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
        auto drawFrame = [&](int w, int h) {
            Renderer::Camera rc{cam.eye(map), cam.target(map)};
            renderer.beginFrame(w, h, rc, std::max(6000.f, cam.distance * 4.f));
            view = View{rc.eye, rc.target, rc.fovY, w, h, renderer.viewProj()};
            scene.draw(renderer, placeholders);
            control.draw(renderer, world, world.seconds());
            renderer.endFrame();
        };

        if (headless) {
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
                world.tick();
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
                        if (e.key.key == SDLK_ESCAPE) running = false;
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
                        if (e.button.button == SDL_BUTTON_LEFT) { leftDown = true; downX = e.button.x; downY = e.button.y; }
                        if (e.button.button == SDL_BUTTON_RIGHT) { rightDown = true; rightDragged = false; }
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
                        switch (e.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_BACK: running = false; break;
                        case SDL_GAMEPAD_BUTTON_NORTH: placeholders = !placeholders; break;
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
                    world.tick();
                    simAccumulator -= 1.0 / kTicksPerSecond;
                    ++steps;
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
