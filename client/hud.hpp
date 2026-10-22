// 2D heads-up display: resources, selection, build menu and messages.
#pragma once

#include "font.hpp"
#include "units.hpp"

#include <memory>

namespace uaw {

// Top-down picture of a map (N x N RGBA, north up): layer colours shaded by slope.
std::vector<uint8_t> minimapPixels(const alamo::Map& map, alamo::MegVfs& vfs, int N);

class Hud {
public:
    // Uses the game's Arial; without it the HUD draws boxes only.
    Hud(alamo::MegVfs& vfs, float uiScale);

    void draw(Renderer& r, alamo::World& w, UnitControl& control, const alamo::GameDatabase& db,
              const alamo::StringTable& text, bool gamepad, const View* view = nullptr);

    // Minimap: built once per map from its terrain layers and heights.
    void setMap(Renderer& r, const alamo::Map& map, alamo::MegVfs& vfs);
    // World ground point under a minimap pixel, or false if the pixel is outside the minimap.
    bool minimapAt(float x, float y, alamo::Vec3& out) const;

    // Pixel rectangle of menu item i from the last draw (for mouse clicks), or false.
    bool menuItemAt(float x, float y, int& index) const;
    bool hasFont() const { return font_ != nullptr; }
    // A short line in a dark box near the top centre ("Paused", game speed).
    void centreText(Renderer& r, const std::string& text, bool small = false);

    // Research panel (T): the faction's tree, click a suite to start it.
    bool researchOpen = false;
    bool researchAt(float x, float y, std::string& path, int& index) const;

private:
    std::unique_ptr<Font> font_, small_;
    float scale_;
    struct Rect { float x0, y0, x1, y1; };
    std::vector<Rect> itemRects_;
    const alamo::Map* map_ = nullptr;
    GLuint minimapTex_ = 0;
    GLuint fogTex_ = 0;
    uint64_t fogVersion_ = ~0ull;
    Rect minimap_{0, 0, 0, 0}; // last drawn
    void banner(Renderer& r, const std::string& text, const alamo::Vec4& color);
    struct SuiteRect { Rect r; std::string path; int index; };
    std::vector<SuiteRect> suiteRects_;
    void drawResearch(Renderer& r, alamo::World& w, const UnitControl& control, const alamo::StringTable& text,
                      std::vector<GpuVertex>& big, std::vector<GpuVertex>& small);
    void drawMinimap(Renderer& r, alamo::World& w, const UnitControl& control, const View* view, float top);
};

} // namespace uaw
