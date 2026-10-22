// Saved games: the skirmish setup plus every order the player gave, with its tick. The
// simulation is deterministic, so loading plays the game again from the start up to the
// saved moment (a few seconds for a long game).
#pragma once

#include "menu.hpp"

#include "alamo/sim.hpp"

#include <string>
#include <vector>

namespace uaw {

struct SaveGame {
    SkirmishSetup setup;
    std::vector<std::string> mods;   // mod folder names when it was saved (a warning if they differ)
    uint64_t tick = 0;               // the saved moment
    int player = 0;                  // the viewing player
    bool spectate = false;
    float camera[5] = {0, 0, 0, 0, 0}; // x, y, distance, yaw, pitch
    std::vector<alamo::Command> commands;
};

// One order as text (also what LAN games send): "tick kind target owner index x y z name n ids..."
std::string encodeCommand(const alamo::Command& c);
bool decodeCommand(const std::string& text, alamo::Command& c);
// A skirmish setup on one line (LAN lobby).
std::string encodeSetup(const SkirmishSetup& s);
bool decodeSetup(const std::string& text, SkirmishSetup& s);

bool writeSave(const std::string& path, const SaveGame& s);
bool readSave(const std::string& path, SaveGame& s, std::string* error = nullptr);

// Where quick saves go (the user's preferences folder for the game), with a trailing separator.
std::string saveFolder();
// Save slots: 0 is the quick save (F5 / F9), 1.. are numbered.
std::string slotPath(int slot);
// "Map, game time - when saved" for a save file, or "" when there is none.
std::string describeSave(const std::string& path);
// The newest save file among the slots ("" if none).
std::string newestSave();

// Settings kept between runs (settings.txt in the save folder): sound and music volume, graphics detail.
struct Settings {
    float sound = 0.8f, music = 0.6f;
    bool fullGraphics = true;
    bool fullscreen = false;
};
Settings loadSettings();
void saveSettings(const Settings& s);

} // namespace uaw
