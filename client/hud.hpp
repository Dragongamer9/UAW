// 2D heads-up display: resources, selection, build menu and messages.
#pragma once

#include "font.hpp"
#include "units.hpp"

#include <memory>

namespace uaw {

class Hud {
public:
    // Uses the game's Arial; without it the HUD draws boxes only.
    Hud(alamo::MegVfs& vfs, float uiScale);

    void draw(Renderer& r, alamo::World& w, UnitControl& control, const alamo::GameDatabase& db,
              const alamo::StringTable& text, bool gamepad);

    // Pixel rectangle of menu item i from the last draw (for mouse clicks), or false.
    bool menuItemAt(float x, float y, int& index) const;
    bool hasFont() const { return font_ != nullptr; }

private:
    std::unique_ptr<Font> font_, small_;
    float scale_;
    struct Rect { float x0, y0, x1, y1; };
    std::vector<Rect> itemRects_;
};

} // namespace uaw
