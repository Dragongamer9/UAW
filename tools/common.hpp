// Shared command-line helpers for the tools.
#pragma once

#include "alamo/gamedata.hpp"

#include <cstring>
#include <iostream>
#include <string>

namespace tools {

// Removes "--name value" from argv (wherever it appears) and returns the value, or "".
inline std::string takeOption(int& argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) != 0) continue;
        std::string v = argv[i + 1];
        for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
        argc -= 2;
        return v;
    }
    return {};
}

inline bool takeFlag(int& argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) != 0) continue;
        for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
        --argc;
        return true;
    }
    return false;
}

struct ModChoice {
    bool none = false;   // --no-mods: the original game only
    std::string list;    // --mods A,B,C (bottom to top) or --mod A; empty = every installed mod
};

inline ModChoice takeModOptions(int& argc, char** argv) {
    ModChoice c;
    c.none = takeFlag(argc, argv, "--no-mods");
    c.list = takeOption(argc, argv, "--mods");
    if (c.list.empty()) c.list = takeOption(argc, argv, "--mod");
    return c;
}

// Resolves the mod choice to folders, bottom to top. Throws on an unknown name.
inline std::vector<std::filesystem::path> resolveMods(const std::string& gameRoot, const ModChoice& c) {
    if (c.none) return {};
    if (c.list.empty()) return alamo::installedMods(gameRoot);
    std::vector<std::filesystem::path> out;
    size_t start = 0;
    while (start <= c.list.size()) {
        size_t end = c.list.find(',', start);
        if (end == std::string::npos) end = c.list.size();
        const std::string name = c.list.substr(start, end - start);
        start = end + 1;
        if (name.empty()) continue;
        auto p = alamo::findMod(gameRoot, name);
        if (p.empty()) throw std::runtime_error("mod not found: " + name);
        out.push_back(p);
    }
    return out;
}

// Mounts the base game and the chosen mods (by default all of Game\\Mods), reporting them.
inline std::vector<std::string> mountWithMods(alamo::MegVfs& vfs, const std::string& gameRoot, const ModChoice& c) {
    const auto mods = resolveMods(gameRoot, c);
    for (const auto& m : mods) std::cerr << "mod: " << m.filename().string() << "\n";
    return alamo::mountGame(vfs, gameRoot, mods);
}

} // namespace tools
