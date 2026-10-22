#include "hud.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>

#include "alamo/ai.hpp"
#include "alamo/assets.hpp"

namespace uaw {

using alamo::Vec4;

namespace {

const Vec4 kPanel{0.05f, 0.07f, 0.09f, 0.78f};
const Vec4 kEdge{0.35f, 0.75f, 0.95f, 0.9f};
const Vec4 kText{0.92f, 0.95f, 0.98f, 1};
const Vec4 kDim{0.6f, 0.66f, 0.72f, 1};
const Vec4 kWarn{1.f, 0.55f, 0.35f, 1};
const Vec4 kGood{0.45f, 1.f, 0.55f, 1};

std::string fmt(const char* f, double a) {
    char b[64];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

} // namespace

Hud::Hud(alamo::MegVfs& vfs, float uiScale) : scale_(uiScale) {
    for (const char* path : {"Data\\Resources\\Fonts\\ARIAL.TTF", "Data\\Resources\\Fonts\\IMPACT.TTF"}) {
        if (!vfs.exists(path)) continue;
        try {
            const auto ttf = vfs.read(path);
            font_ = std::make_unique<Font>(ttf, 20 * scale_);
            small_ = std::make_unique<Font>(ttf, 15 * scale_);
            break;
        } catch (const std::exception&) {
            font_.reset();
            small_.reset();
        }
    }
}

std::vector<uint8_t> minimapPixels(const alamo::Map& map, alamo::MegVfs& vfs, int N) {
    // Layer colours, shaded by slope (light from the north-west), impassable ground darker.
    const std::vector<alamo::Vec3> colors = alamo::terrainLayerColors(vfs, map);
    std::vector<uint8_t> px(size_t(N) * N * 4);
    for (int py = 0; py < N; ++py)
        for (int pxl = 0; pxl < N; ++pxl) {
            const float wx = (float(pxl) + 0.5f) / N * map.sizeX(), wy = (1 - (float(py) + 0.5f) / N) * map.sizeY();
            const uint32_t vx = std::min(map.width - 1, uint32_t(wx / map.spacing + 0.5f));
            const uint32_t vy = std::min(map.height - 1, uint32_t(wy / map.spacing + 0.5f));
            const uint8_t li = map.layerIndex[size_t(vy) * map.width + vx];
            alamo::Vec3 c = li < colors.size() ? colors[li] : alamo::Vec3{0.5f, 0.5f, 0.5f};
            const float dx = map.heightAt(wx + 20, wy) - map.heightAt(wx - 20, wy), dy = map.heightAt(wx, wy + 20) - map.heightAt(wx, wy - 20);
            float shade = std::clamp(1.0f + (-dx + dy) * 0.012f, 0.55f, 1.35f);
            if (!map.vertexFlags.empty() && !(map.vertexFlags[size_t(vy) * map.width + vx] & 0x01)) shade *= 0.6f;
            for (int k = 0; k < 3; ++k) px[(size_t(py) * N + pxl) * 4 + k] = uint8_t(std::clamp(c[k] * shade * 255.f, 0.f, 255.f));
            px[(size_t(py) * N + pxl) * 4 + 3] = 255;
        }
    return px;
}

void Hud::setMap(Renderer& r, const alamo::Map& map, alamo::MegVfs& vfs) {
    map_ = &map;
    constexpr int N = 256;
    minimapTex_ = r.uploadTexture(minimapPixels(map, vfs, N), N, N);
}

bool Hud::minimapAt(float x, float y, alamo::Vec3& out) const {
    if (!map_ || x < minimap_.x0 || x > minimap_.x1 || y < minimap_.y0 || y > minimap_.y1) return false;
    out = {(x - minimap_.x0) / (minimap_.x1 - minimap_.x0) * map_->sizeX(),
           (1 - (y - minimap_.y0) / (minimap_.y1 - minimap_.y0)) * map_->sizeY(), 0};
    out[2] = map_->heightAt(out[0], out[1]);
    return true;
}

void Hud::drawMinimap(Renderer& r, alamo::World& w, const UnitControl& control, const View* view, float top) {
    if (!map_ || !minimapTex_) return;
    const float s = scale_, size = 210 * s, pad = 10 * s, W = float(r.width());
    const float aspect = map_->sizeX() / std::max(1.f, map_->sizeY());
    const float mw = aspect >= 1 ? size : size * aspect, mh = aspect >= 1 ? size / aspect : size;
    minimap_ = {W - pad - mw, top + pad, W - pad, top + pad + mh};
    const Rect& m = minimap_;
    r.drawOverlayRect(m.x0 - 3 * s, m.y0 - 3 * s, m.x1 + 3 * s, m.y1 + 3 * s, kPanel);
    const Vec4 white{1, 1, 1, 1};
    const GpuVertex a{m.x0, m.y0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1}, b{m.x1, m.y0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1},
                    c{m.x1, m.y1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1}, d{m.x0, m.y1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1};
    (void)white;
    r.drawOverlayTriangles({a, b, c, a, c, d}, minimapTex_);
    // Fog of war: unexplored nearly black, explored but unseen dimmed (row 0 of the grid is the south edge).
    if (const std::vector<uint8_t>* fog = control.fog ? w.fogGrid(control.player()) : nullptr) {
        const uint64_t version = w.tickCount() / 15;
        if (!fogTex_ || version != fogVersion_) {
            fogVersion_ = version;
            const int fw = w.fogWidth(), fh = w.fogHeight();
            std::vector<uint8_t> px(size_t(fw) * size_t(fh) * 4, 0);
            for (size_t i = 0; i < size_t(fw) * size_t(fh) && i < fog->size(); ++i) px[i * 4 + 3] = (*fog)[i] == 2 ? 0 : (*fog)[i] == 1 ? 120 : 225;
            if (fogTex_) glDeleteTextures(1, &fogTex_);
            fogTex_ = r.uploadTexture(px, fw, fh);
        }
        const GpuVertex fa{m.x0, m.y0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1}, fb{m.x1, m.y0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1},
                        fc{m.x1, m.y1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1}, fd{m.x0, m.y1, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1};
        r.drawOverlayTriangles({fa, fb, fc, fa, fc, fd}, fogTex_);
    }
    auto toMap = [&](float wx, float wy, float& x, float& y) {
        x = m.x0 + std::clamp(wx / map_->sizeX(), 0.f, 1.f) * (m.x1 - m.x0);
        y = m.y1 - std::clamp(wy / map_->sizeY(), 0.f, 1.f) * (m.y1 - m.y0);
    };
    // Units and structures: yours blue, enemies red, others grey; structures bigger.
    std::vector<GpuVertex> dots;
    auto quad = [&](float x0, float y0, float x1, float y1, const Vec4& col) {
        const GpuVertex p0{x0, y0, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]}, p1{x1, y0, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]},
                        p2{x1, y1, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]}, p3{x0, y1, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]};
        dots.insert(dots.end(), {p0, p1, p2, p0, p2, p3});
    };
    for (const alamo::Unit& u : w.units()) {
        if (!control.shown(w, u)) continue;
        float x, y;
        toMap(u.position[0], u.position[1], x, y);
        const bool player = u.owner >= 0 && w.player(u.owner) && w.player(u.owner)->faction != "Neutral";
        const Vec4 col = u.owner == control.player() ? Vec4{0.3f, 0.65f, 1.f, 1}
                         : w.enemies(control.player(), u.owner) && player ? Vec4{1.f, 0.3f, 0.25f, 1}
                         : player ? Vec4{0.4f, 0.95f, 0.45f, 1} // allies
                                  : Vec4{0.75f, 0.75f, 0.75f, 1};
        const float h = (u.structure ? 3.f : 1.6f) * s;
        quad(x - h, y - h, x + h, y + h, col);
    }
    // Alert ping: a ring shrinking on the place you were attacked, for four seconds.
    if (const float age = w.seconds() - alertTime_; hasAlert_ && age >= 0 && age < 4) {
        float x, y;
        toMap(alertPos_[0], alertPos_[1], x, y);
        const float rr = (4 - std::fmod(age, 1.f) * 3) * 3 * s, t = 1.2f * s;
        const Vec4 col{1, 0.25f, 0.2f, 1 - age / 4};
        quad(x - rr, y - rr, x + rr, y - rr + t, col);
        quad(x - rr, y + rr - t, x + rr, y + rr, col);
        quad(x - rr, y - rr, x - rr + t, y + rr, col);
        quad(x + rr - t, y - rr, x + rr, y + rr, col);
    }
    // The camera's view of the ground.
    if (view) {
        const float sx[4] = {0, float(view->width), float(view->width), 0}, sy[4] = {0, 0, float(view->height), float(view->height)};
        float px[4], py[4];
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            alamo::Vec3 g;
            if (!view->groundAt(*map_, sx[i], sy[i], g)) {
                // Above the horizon: a point far along the top edge's direction instead.
                if (!view->groundAt(*map_, sx[i], float(view->height) * 0.45f, g)) { ok = false; break; }
            }
            toMap(g[0], g[1], px[i], py[i]);
        }
        if (ok)
            for (int i = 0; i < 4; ++i) {
                const int j = (i + 1) % 4;
                const float dx = px[j] - px[i], dy = py[j] - py[i], len = std::max(std::hypot(dx, dy), 1e-3f);
                const float nx = -dy / len * s, ny = dx / len * s;
                const Vec4 col{1, 1, 1, 0.9f};
                const GpuVertex q0{px[i] + nx, py[i] + ny, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]},
                                q1{px[j] + nx, py[j] + ny, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]},
                                q2{px[j] - nx, py[j] - ny, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]},
                                q3{px[i] - nx, py[i] - ny, 0, 0, 0, 1, 0, 0, col[0], col[1], col[2], col[3]};
                dots.insert(dots.end(), {q0, q1, q2, q0, q2, q3});
            }
    }
    r.drawOverlayTriangles(dots, 0);
}

void Hud::banner(Renderer& r, const std::string& text, const Vec4& color) {
    if (!font_) return;
    const float W = float(r.width()), H = float(r.height()), s = scale_;
    const float tw = font_->width(text), lh = font_->lineHeight();
    const float scale = 3; // big letters: the font scaled through the overlay
    const float x = W / 2 - tw * scale / 2, y = H * 0.32f;
    r.drawOverlayRect(0, y - lh * 0.6f, W, y + lh * scale + lh * 0.6f, {0, 0, 0, 0.55f});
    std::vector<GpuVertex> v;
    font_->append(v, text, 0, 0, color);
    for (GpuVertex& g : v) {
        g.px = x + g.px * scale;
        g.py = y + g.py * scale;
    }
    r.drawOverlayTriangles(v, font_->texture());
    (void)s;
}

void Hud::centreText(Renderer& r, const std::string& t, bool small) {
    const Font* f = small ? small_.get() : font_.get();
    if (!f) return;
    const float W = float(r.width()), s = scale_, tw = f->width(t), lh = f->lineHeight();
    const float x = W / 2 - tw / 2, y = 70 * s;
    r.drawOverlayRect(x - 12 * s, y - 4 * s, x + tw + 12 * s, y + lh + 4 * s, {0, 0, 0, 0.6f});
    std::vector<GpuVertex> v;
    f->append(v, t, x, y, kText);
    r.drawOverlayTriangles(v, f->texture());
}

bool Hud::researchAt(float x, float y, std::string& path, int& index) const {
    if (!researchOpen) return false;
    for (const SuiteRect& sr : suiteRects_)
        if (x >= sr.r.x0 && x <= sr.r.x1 && y >= sr.r.y0 && y <= sr.r.y1) {
            path = sr.path;
            index = sr.index;
            return true;
        }
    return false;
}

void Hud::drawResearch(Renderer& r, alamo::World& w, const UnitControl& control, const alamo::StringTable& text,
                       std::vector<GpuVertex>& big, std::vector<GpuVertex>& small) {
    suiteRects_.clear();
    const alamo::ResearchTree* tree = w.researchTree(control.player());
    const alamo::Player* me = w.player(control.player());
    if (!tree || !me || !font_ || !small_) return;
    const float W = float(r.width()), H = float(r.height()), s = scale_, pad = 12 * s;
    const float lh = font_->lineHeight(), lhs = small_->lineHeight();
    // Paths side by side.
    std::vector<std::string> paths;
    for (const auto& su : tree->suites)
        if (std::find(paths.begin(), paths.end(), su.path) == paths.end()) paths.push_back(su.path);
    const float colW = std::min(360 * s, (W - 4 * pad) / float(std::max<size_t>(paths.size(), 1)));
    const float panelW = colW * float(paths.size()) + 2 * pad, x0 = (W - panelW) / 2, y0 = H * 0.14f;
    size_t rows = 0;
    for (const auto& p : paths) {
        size_t n = 0;
        for (const auto& su : tree->suites) n += su.path == p;
        rows = std::max(rows, n);
    }
    const float panelH = lh * 2.4f + float(rows) * (2 * lhs + 6 * s) + pad * 2;
    r.drawOverlayRect(x0, y0, x0 + panelW, y0 + panelH, {0.03f, 0.05f, 0.07f, 0.92f});
    r.drawOverlayRect(x0, y0, x0 + panelW, y0 + 2 * s, kEdge);
    const int done = int(me->researched.size()), busy = int(me->researching.size());
    font_->append(big, "Research  (" + std::to_string(done) + " of " + std::to_string(alamo::World::kMaxResearchSuites) + " suites)", x0 + pad, y0 + pad, kEdge);
    small_->append(small, "Click a suite to start it; T closes", x0 + panelW - pad - small_->width("Click a suite to start it; T closes"), y0 + pad + 4 * s, kDim);
    const auto available = w.availableResearch(control.player());
    for (size_t c = 0; c < paths.size(); ++c) {
        const float cx = x0 + pad + float(c) * colW;
        float cy = y0 + pad + lh * 1.4f;
        std::string branch;
        for (const auto& su : tree->suites)
            if (su.path == paths[c]) { branch = su.branch; break; }
        small_->append(small, "Path " + paths[c] + "  " + branch, cx, cy, kText);
        cy += lhs * 1.3f;
        for (const auto& su : tree->suites) {
            if (su.path != paths[c]) continue;
            const bool isDone = me->researched.count(su.id()) > 0;
            const alamo::Player::Research* running = nullptr;
            for (const auto& rr : me->researching)
                if (rr.suite == &su) running = &rr;
            const bool canStart = std::find(available.begin(), available.end(), &su) != available.end();
            const Vec4 col = isDone ? kGood : running ? Vec4{1, 0.85f, 0.35f, 1} : canStart ? kText : kDim;
            std::string title = su.title;
            if (const std::string* t = text.get(su.title)) title = *t;
            if (title.empty()) title = su.id();
            if (title.size() > 34) title = title.substr(0, 33) + "...";
            const Rect rc{cx - 4 * s, cy - 2 * s, cx + colW - pad, cy + 2 * lhs + 2 * s};
            if (canStart) {
                r.drawOverlayRect(rc.x0, rc.y0, rc.x1, rc.y1, {0.35f, 0.75f, 0.95f, 0.12f});
                suiteRects_.push_back({rc, su.path, su.index});
            }
            small_->append(small, std::to_string(su.index) + ". " + title, cx, cy, col);
            std::string sub = isDone ? "researched" : fmt("%.0f credits, ", su.cost) + fmt("%.0f s", su.seconds);
            if (running) {
                const float total = std::max(1.f, su.seconds * alamo::kTicksPerSecond);
                sub = fmt("researching %.0f%%", 100 * (1 - float(running->ticksLeft) / total));
            }
            small_->append(small, "    " + sub, cx, cy + lhs, isDone || running ? col : kDim);
            cy += 2 * lhs + 6 * s;
        }
    }
    (void)busy;
}

bool Hud::menuItemAt(float x, float y, int& index) const {
    for (size_t i = 0; i < itemRects_.size(); ++i) {
        const Rect& r = itemRects_[i];
        if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) {
            index = int(i);
            return true;
        }
    }
    return false;
}

void Hud::draw(Renderer& r, alamo::World& w, UnitControl& control, const alamo::GameDatabase& db,
               const alamo::StringTable& text, bool gamepad, const View* view) {
    // Under attack? (your units or structures hurt this tick; at most one alert every 15 seconds)
    if (w.seconds() - alertTime_ > 15)
        for (const alamo::Unit& u : w.units())
            if (u.owner == control.player() && u.damagedThisTick && !u.inside) {
                alertTime_ = w.seconds();
                alertPos_ = u.position;
                hasAlert_ = true;
                control.say(u.structure ? "Your base is under attack (Home to look)" : "Your units are under attack (Home to look)", w.seconds());
                break;
            }

    itemRects_.clear();
    const float W = float(r.width()), H = float(r.height()), s = scale_, pad = 10 * s;
    std::vector<GpuVertex> big, small;
    auto textBig = [&](const std::string& t, float x, float y, const Vec4& c) { return font_ ? font_->append(big, t, x, y, c) : x; };
    auto textSmall = [&](const std::string& t, float x, float y, const Vec4& c) { return small_ ? small_->append(small, t, x, y, c) : x; };
    const float lh = font_ ? font_->lineHeight() : 22 * s, lhs = small_ ? small_->lineHeight() : 16 * s;

    // --- top bar: faction, credits, population, clock
    const alamo::Player* me = w.player(control.player());
    r.drawOverlayRect(0, 0, W, lh + pad, kPanel);
    r.drawOverlayRect(0, lh + pad - 2 * s, W, lh + pad, kEdge);
    if (me) {
        float x = pad;
        x = textBig(me->faction, x, pad / 2, kEdge) + 24 * s;
        x = textBig("Credits " + fmt("%.0f", me->credits), x, pad / 2, kText) + 6 * s;
        const float cap = w.bankCap(me->id), rate = w.incomePerSecond(me->id);
        if (cap > 0) x = textSmall("/ " + fmt("%.0f", cap), x, pad / 2 + 4 * s, me->credits >= cap ? kWarn : kDim) + 6 * s;
        if (rate > 0) x = textSmall("+" + fmt("%.1f/s", rate), x, pad / 2 + 4 * s, kGood) + 6 * s;
        x += 18 * s;
        const int pop = w.popUsed(me->id);
        x = textBig("Population " + std::to_string(pop) + " / " + std::to_string(me->popCap), x, pad / 2,
                    pop >= me->popCap ? kWarn : kText);
    }
    const int secs = int(w.seconds());
    char clock[32];
    std::snprintf(clock, sizeof clock, "%d:%02d", secs / 60, secs % 60);
    if (font_) textBig(clock, W - pad - font_->width(clock), pad / 2, kDim);
    drawMinimap(r, w, control, view, lh + pad);

    // --- bottom panel: selection and build menu
    const auto& sel = control.selection();
    if (!sel.empty()) {
        const float panelH = std::min(H * 0.42f, 12 * lhs + 3 * lh);
        const float y0 = H - panelH;
        r.drawOverlayRect(0, y0, W, H, kPanel);
        r.drawOverlayRect(0, y0, W, y0 + 2 * s, kEdge);

        // Selection summary on the left.
        float y = y0 + pad;
        const alamo::Unit* first = w.unit(*sel.begin());
        if (sel.size() == 1 && first) {
            // The unit's encyclopedia icon (EncyclopediaBehaviorType Icon_Name) at the panel's right edge.
            if (const std::string* icon = first->type->find("Icon_Name"); icon && !alamo::trim(*icon).empty()) {
                if (const GLuint tex = r.textureNamed(alamo::trim(*icon))) {
                    const float sz = 72 * s, ix = W * 0.26f - sz - 2 * pad, iy = y0 + pad;
                    auto vtx = [&](float x, float yv, float u, float v) {
                        GpuVertex g{};
                        g.px = x, g.py = yv, g.u = u, g.v = v, g.r = g.g = g.b = g.a = 1;
                        return g;
                    };
                    std::vector<GpuVertex> q{vtx(ix, iy, 0, 0), vtx(ix + sz, iy, 1, 0), vtx(ix + sz, iy + sz, 1, 1),
                                             vtx(ix, iy, 0, 0), vtx(ix + sz, iy + sz, 1, 1), vtx(ix, iy + sz, 0, 1)};
                    r.drawOverlayTriangles(q, tex);
                }
            }
            textBig(text.displayName(*first->type), pad, y, kText);
            y += lh;
            std::string hp = "Health " + fmt("%.0f", first->health) + " / " + fmt("%.0f", first->maxHealth);
            textSmall(hp, pad, y, kDim);
            y += lhs;
            if (first->sellTicks >= 0) {
                textSmall("Selling: " + fmt("%.0f", float(first->sellTicks) / alamo::kTicksPerSecond) + " s", pad, y, kWarn);
                y += lhs;
            }
            if (first->underConstruction) {
                textSmall("Under construction " + fmt("%.0f%%", first->buildProgress * 100), pad, y, kWarn);
                y += lhs;
            }
            if (!first->weapons.empty()) {
                // What it can shoot at: aircraft only, ground only, or both.
                static const std::vector<std::string> air{"FLYING", "MEDIUM"}, ground{"MEDIUM"};
                bool hitsAir = false, hitsGround = false;
                for (const alamo::Unit::WeaponState& ws : first->weapons) {
                    const std::string* r = ws.spec.restrictions;
                    hitsAir = hitsAir || !r || !alamo::hasAnyCategory(air, *r);
                    hitsGround = hitsGround || !r || !alamo::hasAnyCategory(ground, *r);
                }
                const char* targets = hitsAir && hitsGround ? "" : hitsAir ? ", aircraft only" : ", ground only";
                std::string line = "Weapon range " + fmt("%.0f", first->maxRange()) + targets;
                if (first->deployNeed > 0)
                    line += first->deployed >= first->deployNeed ? ", deployed" : ", sets up when it stops";
                textSmall(line, pad, y, kDim);
                y += lhs;
            }
            if (const alamo::GarrisonSpec* g = w.garrisonOf(*first->type)) {
                textSmall("Carrying " + std::to_string(first->passengers.size()) + " / " + std::to_string(g->capacity) +
                              (first->passengers.empty() ? "" : "  (U to unload)"),
                          pad, y, kDim);
                y += lhs;
            }
            if (const float sw = w.superweaponCountdown(first->id); sw >= 0) {
                if (sw == 0) textSmall("Superweapon ready: V, then right-click the target", pad, y, kWarn);
                else textSmall("Superweapon ready in " + fmt("%.0f", sw) + " s", pad, y, kDim);
                y += lhs;
            }
        } else {
            textBig(std::to_string(sel.size()) + " selected", pad, y, kText);
            y += lh;
            std::map<std::string, int> counts;
            for (uint32_t id : sel)
                if (const alamo::Unit* u = w.unit(id)) ++counts[text.displayName(*u->type)];
            for (const auto& [name, n] : counts) {
                if (y > H - lhs - pad) break;
                textSmall(std::to_string(n) + " x " + name, pad, y, kDim);
                y += lhs;
            }
        }

        // Abilities of the selection (F1..F8).
        const auto abilities = control.abilities(w);
        if (!abilities.empty() && y + lhs < H - pad) {
            y += 4 * s;
            for (size_t i = 0; i < abilities.size() && y + lhs < H - pad; ++i) {
                const AbilityButton& b = abilities[i];
                const bool armed = control.armedAbility() == b.name;
                std::string line = "F" + std::to_string(i + 1) + "  " + b.label + (b.on ? " (on)" : "");
                if (!b.ready) line += fmt("  (%.0f s)", std::ceil(b.cooldown));
                textSmall(line, pad, y, armed ? kWarn : b.ready ? kGood : kDim);
                y += lhs;
            }
        }

        // Build menu in the middle.
        const auto items = control.menu(w, db, text);
        const uint32_t ownerId = control.menuOwner(w);
        const alamo::Unit* owner = w.unit(ownerId);
        int& hl = control.highlight();
        if (!items.empty()) hl = std::clamp(hl, 0, int(items.size()) - 1);
        const float queueX = W - std::min(W * 0.25f, 320 * s);
        const float mx = W * 0.26f, colW = std::min(380 * s, (queueX - mx) / 2 - 2 * pad);
        float my = y0 + pad;
        if (!items.empty()) {
            textBig(items.front().construct ? "Construct" : "Build", mx, my, kEdge);
            if (gamepad) textSmall("D-pad left/right choose, down select", mx + 140 * s, my + 4 * s, kDim);
            else textSmall("1-9 or click", mx + 120 * s, my + 4 * s, kDim);
            my += lh;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            const MenuItem& it = items[i];
            const int col = int(i) / 10, row = int(i) % 10;
            const float x = mx + float(col) * (colW + pad), yy = my + float(row) * lhs;
            if (yy + lhs > H - pad || x + colW > W) break;
            if (int(i) == hl && gamepad) r.drawOverlayRect(x - 4 * s, yy, x + colW, yy + lhs, {0.35f, 0.75f, 0.95f, 0.25f});
            itemRects_.push_back({x - 4 * s, yy, x + colW, yy + lhs});
            const Vec4 c = !it.affordable ? kWarn : it.upgrade ? Vec4{0.8f, 0.75f, 1.f, 1} : kText;
            const std::string key = i < 9 ? std::to_string(i + 1) + "  " : "    ";
            textSmall(key + it.label + (it.upgrade ? " (upgrade)" : ""), x, yy, c);
            const std::string cost = fmt("%.0f", it.cost) + "  " + fmt("%.0fs", it.seconds);
            if (small_) textSmall(cost, x + colW - small_->width(cost) - 4 * s, yy, kDim);
        }

        // Production queue / construction progress on the right.
        if (owner && !owner->queue.empty()) {
            const float qx = queueX;
            float qy = y0 + pad;
            textBig("Queue", qx, qy, kEdge);
            qy += lh;
            for (size_t i = 0; i < owner->queue.size() && qy + lhs < H - pad; ++i) {
                const alamo::GameObjectType* t = owner->queue[i];
                textSmall(text.displayName(*t), qx, qy, i == 0 ? kText : kDim);
                if (i == 0) {
                    const float total = float(alamo::buildInfo(*t, db).ticks);
                    const float f = std::clamp(float(owner->queueTicks) / std::max(total, 1.f), 0.f, 1.f);
                    r.drawOverlayRect(qx, qy + lhs - 3 * s, qx + 200 * s, qy + lhs - 1 * s, {0.2f, 0.25f, 0.3f, 1});
                    r.drawOverlayRect(qx, qy + lhs - 3 * s, qx + 200 * s * f, qy + lhs - 1 * s, kGood);
                }
                qy += lhs;
            }
            textSmall(gamepad ? "Y-hold + B to cancel last" : "Backspace cancels last", qx, qy, kDim);
        }
    }

    // --- end of the match (once you have had a base to lose)
    if (!sawPlayer_)
        for (const alamo::Unit& u : w.units())
            if (u.owner == control.player() && u.structure) { sawPlayer_ = true; break; }
    // Under the banner: what each side built, lost and earned.
    auto matchStats = [&]() {
        if (!small_) return;
        const float W = float(r.width()), lhs2 = small_->lineHeight();
        float y = float(r.height()) * 0.32f + (font_ ? font_->lineHeight() * 4.2f : 80);
        const float cols[] = {W / 2 - 360 * s, W / 2 - 160 * s, W / 2 + 40 * s, W / 2 + 200 * s};
        int rows = 1;
        for (int p = 0; p < 8 && w.player(p); ++p) rows += w.player(p)->faction != "Neutral";
        r.drawOverlayRect(cols[0] - 16 * s, y - 8 * s, W / 2 + 360 * s, y + lhs2 * float(rows) + 8 * s, {0, 0, 0, 0.55f});
        textSmall("Player", cols[0], y, kDim);
        textSmall("Built: units / bases", cols[1], y, kDim);
        textSmall("Lost", cols[2], y, kDim);
        textSmall("Credits earned", cols[3], y, kDim);
        y += lhs2;
        for (int p = 0; p < 8 && w.player(p); ++p) {
            const alamo::Player& pl = *w.player(p);
            if (pl.faction == "Neutral") continue;
            const Vec4 c = p == control.player() ? kText : kDim;
            textSmall(std::to_string(p + 1) + "  " + pl.faction + (alamo::defeated(w, p) ? "  (out)" : ""), cols[0], y, c);
            textSmall(std::to_string(pl.unitsBuilt) + " / " + std::to_string(pl.structuresBuilt), cols[1], y, c);
            textSmall(std::to_string(pl.unitsLost) + " / " + std::to_string(pl.structuresLost), cols[2], y, c);
            textSmall(fmt("%.0f", pl.earned), cols[3], y, c);
            y += lhs2;
        }
    };
    if (!sawPlayer_) {
    } else if (alamo::defeated(w, control.player())) {
        banner(r, "Defeat", {1.f, 0.4f, 0.35f, 1});
        matchStats();
    } else {
        bool anyEnemy = false, enemyAlive = false;
        for (size_t p = 0; p < 8; ++p) {
            if (!w.player(int(p))) break;
            if (int(p) == control.player() || !w.enemies(control.player(), int(p))) continue;
            anyEnemy = true;
            enemyAlive = enemyAlive || !alamo::defeated(w, int(p));
        }
        if (anyEnemy && !enemyAlive) {
            banner(r, "Victory", kGood);
            matchStats();
        }
    }

    // --- message line
    const float age = w.seconds() - control.messageTime();
    if (!control.message().empty() && age < 4 && font_) {
        const float a = std::clamp(4 - age, 0.f, 1.f);
        const float tw = font_->width(control.message());
        const float y = sel.empty() ? H - lh * 2 : H - std::min(H * 0.42f, 12 * lhs + 3 * lh) - lh * 1.6f;
        r.drawOverlayRect(W / 2 - tw / 2 - pad, y - 4 * s, W / 2 + tw / 2 + pad, y + lh, {0, 0, 0, 0.55f * a});
        textBig(control.message(), W / 2 - tw / 2, y, {1, 1, 1, a});
    }

    if (font_) r.drawOverlayTriangles(big, font_->texture());
    if (small_) r.drawOverlayTriangles(small, small_->texture());
    if (researchOpen) {
        // On top of everything else (its own pass so the panel covers the text below it).
        std::vector<GpuVertex> rb, rs;
        drawResearch(r, w, control, text, rb, rs);
        if (font_) r.drawOverlayTriangles(rb, font_->texture());
        if (small_) r.drawOverlayTriangles(rs, small_->texture());
    } else {
        suiteRects_.clear();
    }
}

} // namespace uaw
