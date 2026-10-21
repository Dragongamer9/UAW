// uaw-viewer: fly around a Universe at War map with its objects placed.
//
//   uaw-viewer <game-root> <MAP> [options]
//     --player N          start the camera over player N's start position (default 1)
//     --all-groups        also show campaign-mission objects
//     --size WxH          window size (default 1600x900)
//     --camera x,y,dist,yaw,pitch
//     --screenshot FILE   render one frame to a PNG and exit (works headless)
//
// Controls
//   keyboard  WASD / arrows pan, Q/E rotate, R/F tilt, wheel or +/- zoom, P placeholders, Esc quit
//   mouse     wheel zoom, middle or right drag rotate/tilt, left drag pan
//   gamepad   left stick pan, right stick rotate/tilt, triggers zoom, Y placeholders, Back quit
#include "camera.hpp"
#include "gl.hpp"
#include "renderer.hpp"
#include "scene.hpp"

#include "alamo/assets.hpp"
#include "alamo/dds.hpp"
#include "alamo/gamedata.hpp"
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
    bool allGroups = false, haveCamera = false;
    float cam[5] = {0, 0, 0, 0, 0};
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
        else if (a == "--player") o.player = std::atoi(next());
        else if (a == "--size") std::sscanf(next(), "%dx%d", &o.width, &o.height);
        else if (a == "--camera")
            o.haveCamera = std::sscanf(next(), "%f,%f,%f,%f,%f", &o.cam[0], &o.cam[1], &o.cam[2], &o.cam[3], &o.cam[4]) == 5;
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
            "usage: uaw-viewer <game-root> <MAP> [--player N] [--all-groups] [--size WxH]\n"
            "                  [--camera x,y,dist,yaw,pitch] [--screenshot out.png]\n\n"
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
        std::printf("models loaded %zu, missing %zu; textures missing %zu\n", renderer.modelsLoaded(),
                    renderer.modelsMissing(), renderer.texturesMissing());

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

        auto render = [&](int w, int h) {
            Renderer::Camera rc{cam.eye(map), cam.target(map)};
            renderer.beginFrame(w, h, rc, std::max(6000.f, cam.distance * 4.f));
            scene.draw(renderer, true);
            renderer.endFrame();
        };

        if (headless) {
            int w, h;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            render(w, h);
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
            bool placeholders = true, running = true;
            uint64_t last = SDL_GetTicksNS(), fpsTimer = last;
            int frames = 0;
            while (running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    switch (e.type) {
                    case SDL_EVENT_QUIT: running = false; break;
                    case SDL_EVENT_KEY_DOWN:
                        if (e.key.key == SDLK_ESCAPE) running = false;
                        if (e.key.key == SDLK_P) placeholders = !placeholders;
                        break;
                    case SDL_EVENT_MOUSE_WHEEL: cam.zoom(std::pow(0.88f, e.wheel.y)); break;
                    case SDL_EVENT_MOUSE_MOTION:
                        if (e.motion.state & (SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) cam.rotate(e.motion.xrel * 0.3f, -e.motion.yrel * 0.2f);
                        else if (e.motion.state & SDL_BUTTON_LMASK) {
                            // Drag the ground: about 0.15% of the zoom distance per pixel.
                            cam.pan(e.motion.yrel * 0.0011f, -e.motion.xrel * 0.0011f, 1.f);
                        }
                        break;
                    case SDL_EVENT_GAMEPAD_ADDED:
                        if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
                        break;
                    case SDL_EVENT_GAMEPAD_REMOVED:
                        if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = nullptr; }
                        break;
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) running = false;
                        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) placeholders = !placeholders;
                        break;
                    default: break;
                    }
                }
                const uint64_t now = SDL_GetTicksNS();
                const float dt = std::min(float(now - last) * 1e-9f, 0.1f);
                last = now;

                const bool* k = SDL_GetKeyboardState(nullptr);
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

                int w, h;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                Renderer::Camera rc{cam.eye(map), cam.target(map)};
                renderer.beginFrame(w, h, rc, std::max(6000.f, cam.distance * 4.f));
                scene.draw(renderer, placeholders);
                renderer.endFrame();
                SDL_GL_SwapWindow(window);

                ++frames;
                if (now - fpsTimer > 1'000'000'000ull) {
                    char title[160];
                    std::snprintf(title, sizeof title, "Universe at War - %s - %d fps%s", opt.map.c_str(), frames,
                                  pad ? " - gamepad" : "");
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
