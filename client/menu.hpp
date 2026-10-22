// Front end: the skirmish setup screen (map, players, factions, teams) and the in-game pause menu.
#pragma once

#include "font.hpp"
#include "renderer.hpp"

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"
#include "alamo/ted.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <vector>

namespace uaw {

struct PlayerSlot {
    enum class Kind { Open, Human, Computer };
    Kind kind = Kind::Open;
    std::string faction = "Novus";
    int team = -1; // -1: on its own
    int netId = -1; // LAN games: which connected player controls a Human slot (0 = the host)
};

struct SkirmishSetup {
    std::string map;                // e.g. "M25_GRAND_CANYON"
    std::vector<PlayerSlot> slots;  // one per start position, in slot order
    bool fog = true;                // fog of war
    int aiLevel = 1;                // computer players: 0 easy, 1 normal, 2 hard (their income x0.7 / x1 / x1.4)
    int credits = 1;                // starting credits: 0 low, 1 normal, 2 high, 3 very high (x0.5 / x1 / x2 / x5)
    static float aiIncome(int level) { return level <= 0 ? 0.7f : level >= 2 ? 1.4f : 1.f; }
    static float creditScale(int level) { return level <= 0 ? 0.5f : level == 2 ? 2.f : level >= 3 ? 5.f : 1.f; }
    int human() const {             // slot you play, or -1 (spectating)
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].kind == PlayerSlot::Kind::Human) return int(i);
        return -1;
    }
};

// Small immediate-mode toolkit shared by the menus: text and clickable boxes in pixels.
class MenuPainter {
public:
    MenuPainter(alamo::MegVfs& vfs, float scale);
    float scale() const { return scale_; }
    bool hasFont() const { return big_ != nullptr; }
    float lineHeight(bool big = true) const;
    float textWidth(const std::string& s, bool big = true) const;
    void text(const std::string& s, float x, float y, const alamo::Vec4& c, bool big = true);
    // A button; returns true when (mx, my) is inside it (for hover). Records it for clicks.
    bool button(Renderer& r, int id, const std::string& label, float x0, float y0, float x1, float y1, bool active,
                float mx, float my);
    void area(int id, float x0, float y0, float x1, float y1) { buttons_.push_back({id, x0, y0, x1, y1}); } // clickable, not drawn
    int buttonAt(float x, float y) const; // id of the button under a point, or -1
    void begin() { buttons_.clear(); bigText_.clear(); smallText_.clear(); }
    void flush(Renderer& r); // draws the queued text

private:
    std::unique_ptr<Font> big_, small_;
    float scale_;
    struct Button { int id; float x0, y0, x1, y1; };
    std::vector<Button> buttons_;
    std::vector<GpuVertex> bigText_, smallText_;
};

class SkirmishMenu {
public:
    SkirmishMenu(alamo::MegVfs& vfs, const alamo::GameDatabase& db, std::vector<std::string> modNames, float scale);

    enum class Result { None, Start, Quit, Load, Host, Join, Replay };
    std::string quickSave; // shown as a "Load saved game" button when this file exists
    Result handle(const SDL_Event& e);
    void draw(Renderer& r);
    const SkirmishSetup& setup() const { return setup_; }
    SkirmishSetup& editSetup() { return setup_; } // LAN lobby: the host's changes (players joining)
    // A setup from the LAN host: map and slots as given (map picked in the list too).
    void takeSetup(const SkirmishSetup& s);

    // LAN lobby state shown on the screen. Host: the setup can be changed and started; client: it
    // only shows the host's setup. Player names by connection id.
    enum class Lobby { None, Host, Client } lobby = Lobby::None;
    std::string lobbyStatus;
    std::vector<std::pair<int, std::string>> lobbyPlayers;
    int lobbyId = -1;          // this machine's connection id
    std::string joinAddress;   // Result::Join: what was typed
    bool empty() const { return maps_.empty(); }
    // Picks the map by name (case-insensitive) if it is in the list.
    void selectMap(const std::string& name);

private:
    struct MapEntry {
        std::string path, name, title;     // archive path, file stem, display name
        std::vector<alamo::Vec3> starts;   // by slot
        float sizeX = 1, sizeY = 1;
    };
    alamo::MegVfs& vfs_;
    const alamo::GameDatabase& db_;
    std::vector<std::string> mods_;
    std::vector<std::string> factions_;
    std::vector<MapEntry> maps_;
    MenuPainter paint_;
    size_t selected_ = 0, scroll_ = 0;
    SkirmishSetup setup_;
    std::vector<std::vector<PlayerSlot>> slotsByMap_; // remembered per map
    GLuint preview_ = 0;
    size_t previewFor_ = size_t(-1);
    float mouseX_ = 0, mouseY_ = 0;
    size_t visibleRows_ = 10;
    bool typing_ = false;      // entering the address to join
    std::string typed_;
    void choose(size_t i);
    void cycle(size_t slot, int field);
};

// The Esc menu during a game.
class PauseMenu {
public:
    explicit PauseMenu(alamo::MegVfs& vfs, float scale) : paint_(vfs, scale) {}
    enum class Result { None, Resume, Save, Load, Restart, ToMenu, Quit, Sound, Music, Graphics };
    bool open = false;
    bool soundOn = true, musicOn = true; // shown on the Sound / Music items
    bool fullGraphics = true;            // shadows and normal maps (off: faster on slow graphics cards)
    Result handle(const SDL_Event& e);
    void draw(Renderer& r);

private:
    MenuPainter paint_;
    float mouseX_ = 0, mouseY_ = 0;
    int highlight_ = 0;
};

} // namespace uaw
