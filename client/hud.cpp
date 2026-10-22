#include "hud.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

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
               const alamo::StringTable& text, bool gamepad) {
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
            textBig(text.displayName(*first->type), pad, y, kText);
            y += lh;
            std::string hp = "Health " + fmt("%.0f", first->health) + " / " + fmt("%.0f", first->maxHealth);
            textSmall(hp, pad, y, kDim);
            y += lhs;
            if (first->underConstruction) {
                textSmall("Under construction " + fmt("%.0f%%", first->buildProgress * 100), pad, y, kWarn);
                y += lhs;
            }
            if (!first->weapons.empty()) {
                textSmall("Weapon range " + fmt("%.0f", first->maxRange()), pad, y, kDim);
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
}

} // namespace uaw
