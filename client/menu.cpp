#include "menu.hpp"
#include "hud.hpp"

#include "alamo/sim.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace uaw {

using alamo::Vec4;

namespace {

const Vec4 kBack{0.03f, 0.045f, 0.06f, 1};
const Vec4 kPanel{0.07f, 0.09f, 0.12f, 0.92f};
const Vec4 kEdge{0.35f, 0.75f, 0.95f, 0.9f};
const Vec4 kText{0.92f, 0.95f, 0.98f, 1};
const Vec4 kDim{0.6f, 0.66f, 0.72f, 1};
const Vec4 kHot{0.55f, 0.9f, 1.f, 1};

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "M25_GRAND_CANYON" -> "Grand Canyon"
std::string prettyMapName(const std::string& stem) {
    std::string s = stem;
    if (s.size() > 1 && (s[0] == 'M' || s[0] == 'm') && std::isdigit(static_cast<unsigned char>(s[1]))) {
        const size_t u = s.find('_');
        if (u != std::string::npos) s = s.substr(u + 1);
    }
    std::string out;
    bool start = true;
    for (char c : s) {
        if (c == '_') { out += ' '; start = true; continue; }
        out += start ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
        start = false;
    }
    if (out == "Washingtondc") out = "Washington D.C.";
    if (out == "Brazillian Highlands") out = "Brazilian Highlands";
    return out;
}

std::string factionTitle(const std::string& f) {
    const std::string u = upper(f);
    if (u == "ALIEN") return "Hierarchy";
    if (u == "NOVUS") return "Novus";
    if (u == "MASARI") return "Masari";
    if (u == "MILITARY") return "Military";
    return f;
}

const Vec4 kSlotColors[8] = {{0.25f, 0.55f, 1, 1}, {1, 0.3f, 0.25f, 1}, {0.35f, 0.9f, 0.4f, 1}, {1, 0.85f, 0.25f, 1},
                             {0.75f, 0.4f, 1, 1},  {0.3f, 0.9f, 0.9f, 1}, {1, 0.6f, 0.2f, 1},  {0.85f, 0.85f, 0.85f, 1}};

} // namespace

// ---------------------------------------------------------------- painter

MenuPainter::MenuPainter(alamo::MegVfs& vfs, float scale) : scale_(scale) {
    for (const char* path : {"Data\\Resources\\Fonts\\ARIAL.TTF", "Data\\Resources\\Fonts\\IMPACT.TTF"}) {
        if (!vfs.exists(path)) continue;
        try {
            const auto ttf = vfs.read(path);
            big_ = std::make_unique<Font>(ttf, 24 * scale_);
            small_ = std::make_unique<Font>(ttf, 17 * scale_);
            break;
        } catch (const std::exception&) {
            big_.reset();
            small_.reset();
        }
    }
}

float MenuPainter::lineHeight(bool big) const {
    const Font* f = big ? big_.get() : small_.get();
    return f ? f->lineHeight() : (big ? 28 : 20) * scale_;
}

float MenuPainter::textWidth(const std::string& s, bool big) const {
    const Font* f = big ? big_.get() : small_.get();
    return f ? f->width(s) : float(s.size()) * 10 * scale_;
}

void MenuPainter::text(const std::string& s, float x, float y, const Vec4& c, bool big) {
    if (const Font* f = big ? big_.get() : small_.get()) f->append(big ? bigText_ : smallText_, s, x, y, c);
}

bool MenuPainter::button(Renderer& r, int id, const std::string& label, float x0, float y0, float x1, float y1, bool active,
                         float mx, float my) {
    const bool hover = mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
    const float s = scale_;
    r.drawOverlayRect(x0, y0, x1, y1, active ? Vec4{0.12f, 0.3f, 0.42f, 0.95f} : hover ? Vec4{0.12f, 0.17f, 0.22f, 0.95f} : Vec4{0.09f, 0.11f, 0.14f, 0.95f});
    if (active || hover) {
        r.drawOverlayRect(x0, y0, x1, y0 + 1.5f * s, kEdge);
        r.drawOverlayRect(x0, y1 - 1.5f * s, x1, y1, kEdge);
    }
    const bool big = (y1 - y0) >= lineHeight(true) + 6 * s;
    text(label, (x0 + x1) / 2 - textWidth(label, big) / 2, (y0 + y1) / 2 - lineHeight(big) / 2, hover || active ? kText : kDim, big);
    buttons_.push_back({id, x0, y0, x1, y1});
    return hover;
}

int MenuPainter::buttonAt(float x, float y) const {
    for (const Button& b : buttons_)
        if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return b.id;
    return -1;
}

void MenuPainter::flush(Renderer& r) {
    if (big_ && !bigText_.empty()) r.drawOverlayTriangles(bigText_, big_->texture());
    if (small_ && !smallText_.empty()) r.drawOverlayTriangles(smallText_, small_->texture());
    bigText_.clear();
    smallText_.clear();
}

// ---------------------------------------------------------------- skirmish setup

namespace {
enum : int { kStart = 1, kQuit = 2, kFog = 3, kLoad = 4, kHost = 5, kJoin = 6, kAiLevel = 7, kCredits = 8, kReplay = 9, kMapBase = 100, kSlotBase = 1000 }; // slot buttons: kSlotBase + slot * 4 + field
}

SkirmishMenu::SkirmishMenu(alamo::MegVfs& vfs, const alamo::GameDatabase& db, std::vector<std::string> modNames, float scale)
    : vfs_(vfs), db_(db), mods_(std::move(modNames)), paint_(vfs, scale) {
    // Factions with a skirmish start.
    const alamo::StartingForces forces = alamo::loadStartingForces(vfs);
    for (const char* f : {"NOVUS", "ALIEN", "MASARI", "MILITARY"})
        if (forces.count(f) && db.faction(f)) {
            std::string name = f;
            for (size_t i = 1; i < name.size(); ++i) name[i] = char(std::tolower(static_cast<unsigned char>(name[i])));
            factions_.push_back(name);
        }
    if (factions_.empty()) factions_ = {"Novus", "Alien", "Masari"};
    // Skirmish maps: two or more start markers, no test or story variants.
    for (const std::string& p : vfs.list()) {
        if (p.size() < 5 || p.compare(p.size() - 4, 4, ".TED") != 0) continue;
        const size_t slash = p.find_last_of("\\/");
        const std::string stem = p.substr(slash == std::string::npos ? 0 : slash + 1, p.size() - 4 - (slash == std::string::npos ? 0 : slash + 1));
        if (stem.find("TEST") != std::string::npos || stem.find("STORY") != std::string::npos || stem.find("TUT_") == 0) continue;
        try {
            const alamo::Map m = alamo::loadTed(vfs.read(p));
            MapEntry e;
            e.path = p;
            e.name = stem;
            e.title = prettyMapName(stem);
            e.sizeX = m.sizeX();
            e.sizeY = m.sizeY();
            std::vector<std::pair<int, alamo::Vec3>> st;
            for (const auto& o : m.objects) {
                const alamo::GameObjectType* t = db.findByCrc(o.typeCrc);
                int slot;
                if (t && std::sscanf(t->name.c_str(), "Marker_Player_%d_Start", &slot) == 1) st.push_back({slot, o.position});
            }
            if (st.size() < 2) continue;
            std::sort(st.begin(), st.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [slot, pos] : st) e.starts.push_back(pos);
            maps_.push_back(std::move(e));
        } catch (const std::exception&) {
        }
    }
    std::sort(maps_.begin(), maps_.end(), [](const MapEntry& a, const MapEntry& b) { return a.title < b.title; });
    slotsByMap_.resize(maps_.size());
    if (!maps_.empty()) choose(0);
}

void SkirmishMenu::takeSetup(const SkirmishSetup& s) {
    for (size_t i = 0; i < maps_.size(); ++i)
        if (upper(maps_[i].name) == upper(s.map)) {
            selected_ = i;
            if (i < scroll_ || i >= scroll_ + visibleRows_) scroll_ = i >= visibleRows_ / 2 ? i - visibleRows_ / 2 : 0;
        }
    setup_ = s;
}

void SkirmishMenu::selectMap(const std::string& name) {
    for (size_t i = 0; i < maps_.size(); ++i)
        if (upper(maps_[i].name) == upper(name)) {
            choose(i);
            scroll_ = i >= visibleRows_ / 2 ? i - visibleRows_ / 2 : 0;
        }
}

void SkirmishMenu::choose(size_t i) {
    if (i >= maps_.size()) return;
    if (!setup_.slots.empty() && selected_ < slotsByMap_.size()) slotsByMap_[selected_] = setup_.slots;
    selected_ = i;
    setup_.map = maps_[i].name;
    if (!slotsByMap_[i].empty()) {
        setup_.slots = slotsByMap_[i];
        return;
    }
    // You against one computer opponent; keep earlier factions where there are slots for them.
    std::vector<PlayerSlot> old = setup_.slots;
    setup_.slots.assign(std::min<size_t>(maps_[i].starts.size(), 8), PlayerSlot{});
    for (size_t k = 0; k < setup_.slots.size(); ++k) {
        if (k < old.size()) setup_.slots[k] = old[k];
        else setup_.slots[k].faction = factions_[k % factions_.size()];
    }
    if (old.empty()) {
        setup_.slots[0] = {PlayerSlot::Kind::Human, factions_[0], -1};
        setup_.slots[1] = {PlayerSlot::Kind::Computer, factions_.size() > 1 ? factions_[1] : factions_[0], -1};
    }
}

void SkirmishMenu::cycle(size_t slot, int field) {
    if (slot >= setup_.slots.size()) return;
    PlayerSlot& p = setup_.slots[slot];
    if (field == 0) { // Open -> Computer -> You -> Open (only one human)
        using K = PlayerSlot::Kind;
        if (p.kind == K::Open) p.kind = K::Computer;
        else if (p.kind == K::Computer) {
            for (PlayerSlot& o : setup_.slots)
                if (o.kind == K::Human) o.kind = K::Computer;
            p.kind = K::Human;
        } else {
            p.kind = K::Open;
        }
    } else if (field == 1) {
        size_t f = 0;
        for (size_t k = 0; k < factions_.size(); ++k)
            if (upper(factions_[k]) == upper(p.faction)) f = k;
        p.faction = factions_[(f + 1) % factions_.size()];
    } else if (field == 2) {
        p.team = p.team >= 3 ? -1 : p.team + 1;
    }
}

SkirmishMenu::Result SkirmishMenu::handle(const SDL_Event& e) {
    if (typing_) { // the address to join
        if (e.type == SDL_EVENT_TEXT_INPUT) typed_ += e.text.text;
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.key == SDLK_BACKSPACE && !typed_.empty()) typed_.pop_back();
            if (e.key.key == SDLK_ESCAPE) typing_ = false;
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                typing_ = false;
                joinAddress = typed_.empty() ? "127.0.0.1" : typed_;
                return Result::Join;
            }
        }
        if (e.type == SDL_EVENT_QUIT) return Result::Quit;
        return Result::None;
    }
    if (lobby == Lobby::Client) { // only leaving is up to us
        if (e.type == SDL_EVENT_QUIT) return Result::Quit;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return Result::Quit;
        if (e.type == SDL_EVENT_MOUSE_MOTION) { mouseX_ = e.motion.x; mouseY_ = e.motion.y; }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && paint_.buttonAt(e.button.x, e.button.y) == kQuit) return Result::Quit;
        return Result::None;
    }
    auto canStart = [&]() {
        int players = 0;
        for (const PlayerSlot& p : setup_.slots) players += p.kind != PlayerSlot::Kind::Open;
        return players >= 2;
    };
    switch (e.type) {
    case SDL_EVENT_QUIT: return Result::Quit;
    case SDL_EVENT_MOUSE_MOTION:
        mouseX_ = e.motion.x;
        mouseY_ = e.motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e.wheel.y < 0 && scroll_ + visibleRows_ < maps_.size()) ++scroll_;
        if (e.wheel.y > 0 && scroll_ > 0) --scroll_;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) break;
        const int id = paint_.buttonAt(e.button.x, e.button.y);
        if (id == kStart && canStart()) return Result::Start;
        if (id == kQuit) return Result::Quit;
        if (id == kFog) setup_.fog = !setup_.fog;
        if (id == kAiLevel) setup_.aiLevel = (setup_.aiLevel + (e.button.button == SDL_BUTTON_RIGHT ? 2 : 1)) % 3;
        if (id == kCredits) setup_.credits = (setup_.credits + (e.button.button == SDL_BUTTON_RIGHT ? 3 : 1)) % 4;
        if (id == kLoad) return Result::Load;
        if (id == kReplay) return Result::Replay;
        if (id == kHost && lobby == Lobby::None) return Result::Host;
        if (id == kJoin && lobby == Lobby::None) {
            typing_ = true;
            typed_.clear();
            return Result::None;
        }
        if (id >= kMapBase && id < kSlotBase) {
            const size_t i = size_t(id - kMapBase);
            if (i == selected_ && e.button.clicks >= 2 && canStart()) return Result::Start;
            choose(i);
        }
        if (id >= kSlotBase && (id - kSlotBase) % 4 == 0 && lobby == Lobby::Host) {
            const size_t slot = size_t((id - kSlotBase) / 4);
            if (slot < setup_.slots.size() && setup_.slots[slot].kind == PlayerSlot::Kind::Human) break; // a player's seat
            if (slot < setup_.slots.size()) // open <-> computer only
                setup_.slots[slot].kind = setup_.slots[slot].kind == PlayerSlot::Kind::Open ? PlayerSlot::Kind::Computer : PlayerSlot::Kind::Open;
            break;
        }
        if (id >= kSlotBase) {
            const int n = e.button.button == SDL_BUTTON_RIGHT ? 3 : 1; // right click cycles backwards
            for (int k = 0; k < n; ++k) cycle(size_t((id - kSlotBase) / 4), (id - kSlotBase) % 4);
            if (n == 3 && (id - kSlotBase) % 4 == 1) // factions: step back by one
                for (size_t k = 0; k + 4 < factions_.size(); ++k) cycle(size_t((id - kSlotBase) / 4), 1);
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) return Result::Quit;
        if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && canStart()) return Result::Start;
        if (e.key.key == SDLK_DOWN && selected_ + 1 < maps_.size()) choose(selected_ + 1);
        if (e.key.key == SDLK_UP && selected_ > 0) choose(selected_ - 1);
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + visibleRows_) scroll_ = selected_ + 1 - visibleRows_;
        if (e.key.key >= SDLK_1 && e.key.key <= SDLK_8) cycle(size_t(e.key.key - SDLK_1), 1); // number: change that slot's faction
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN && selected_ + 1 < maps_.size()) choose(selected_ + 1);
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP && selected_ > 0) choose(selected_ - 1);
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) cycle(1, 1);
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) cycle(0, 1);
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH && canStart()) return Result::Start;
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) return Result::Quit;
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + visibleRows_) scroll_ = selected_ + 1 - visibleRows_;
        break;
    default: break;
    }
    return Result::None;
}

void SkirmishMenu::draw(Renderer& r) {
    const float W = float(r.width()), H = float(r.height()), s = paint_.scale();
    const float lh = paint_.lineHeight(), lhs = paint_.lineHeight(false), pad = 24 * s;
    paint_.begin();
    r.drawOverlayRect(0, 0, W, H, kBack);
    // Title.
    paint_.text("UNIVERSE AT WAR", pad, pad, kHot);
    paint_.text("Skirmish", pad + paint_.textWidth("UNIVERSE AT WAR") + 16 * s, pad, kDim);
    r.drawOverlayRect(pad, pad + lh + 6 * s, W - pad, pad + lh + 8 * s, kEdge);
    const float top = pad + lh + 24 * s, bottom = H - pad - lh * 2;

    // Map list (left).
    const float listW = std::min(380 * s, W * 0.32f), rowH = lh + 8 * s;
    r.drawOverlayRect(pad, top, pad + listW, bottom, kPanel);
    visibleRows_ = std::max<size_t>(1, size_t((bottom - top - 8 * s) / rowH));
    if (scroll_ + visibleRows_ > maps_.size()) scroll_ = maps_.size() > visibleRows_ ? maps_.size() - visibleRows_ : 0;
    for (size_t k = 0; k < visibleRows_ && scroll_ + k < maps_.size(); ++k) {
        const size_t i = scroll_ + k;
        const float y = top + 4 * s + float(k) * rowH;
        const MapEntry& m = maps_[i];
        const bool hover = mouseX_ >= pad && mouseX_ <= pad + listW && mouseY_ >= y && mouseY_ < y + rowH;
        if (i == selected_) r.drawOverlayRect(pad + 3 * s, y, pad + listW - 3 * s, y + rowH - 2 * s, {0.12f, 0.3f, 0.42f, 0.95f});
        else if (hover) r.drawOverlayRect(pad + 3 * s, y, pad + listW - 3 * s, y + rowH - 2 * s, {0.12f, 0.17f, 0.22f, 0.95f});
        paint_.text(m.title, pad + 12 * s, y + 4 * s, i == selected_ ? kText : kDim);
        const std::string n = std::to_string(m.starts.size()) + "P";
        paint_.text(n, pad + listW - 12 * s - paint_.textWidth(n, false), y + 4 * s + (lh - lhs) / 2, kDim, false);
        paint_.area(kMapBase + int(i), pad, y, pad + listW, y + rowH);
    }
    if (maps_.size() > visibleRows_) { // scroll bar
        const float h = bottom - top, f0 = float(scroll_) / float(maps_.size()), f1 = float(scroll_ + visibleRows_) / float(maps_.size());
        r.drawOverlayRect(pad + listW - 4 * s, top + h * f0, pad + listW - 1 * s, top + h * f1, kEdge);
    }

    // Preview (right, top).
    const float rx = pad + listW + pad, rw = W - pad - rx;
    if (maps_.empty()) {
        paint_.text("No skirmish maps found.", rx, top, kText);
    } else {
        const MapEntry& m = maps_[selected_];
        if (previewFor_ != selected_) {
            if (preview_) glDeleteTextures(1, &preview_);
            preview_ = 0;
            try {
                const alamo::Map map = alamo::loadTed(vfs_.read(m.path));
                constexpr int N = 256;
                preview_ = r.uploadTexture(minimapPixels(map, vfs_, N), N, N);
            } catch (const std::exception&) {
            }
            previewFor_ = selected_;
        }
        const float slotsH = float(setup_.slots.size()) * (lh + 10 * s) + lh + 16 * s;
        const float size = std::max(80 * s, std::min(rw * 0.55f, bottom - top - slotsH - 16 * s));
        const float aspect = m.sizeX / std::max(1.f, m.sizeY);
        const float pw = aspect >= 1 ? size : size * aspect, ph = aspect >= 1 ? size / aspect : size;
        const float px = rx, py = top;
        r.drawOverlayRect(px - 2 * s, py - 2 * s, px + pw + 2 * s, py + ph + 2 * s, kEdge);
        if (preview_) {
            std::vector<GpuVertex> q(6);
            auto set = [&](GpuVertex& v, float x, float y, float u, float t) {
                v = GpuVertex{};
                v.px = x; v.py = y; v.u = u; v.v = t;
                v.r = v.g = v.b = v.a = 1;
            };
            set(q[0], px, py, 0, 0); set(q[1], px + pw, py, 1, 0); set(q[2], px + pw, py + ph, 1, 1);
            set(q[3], px, py, 0, 0); set(q[4], px + pw, py + ph, 1, 1); set(q[5], px, py + ph, 0, 1);
            r.drawOverlayTriangles(q, preview_);
        }
        for (size_t k = 0; k < m.starts.size() && k < 8; ++k) {
            const float sx = px + m.starts[k][0] / m.sizeX * pw, sy = py + (1 - m.starts[k][1] / m.sizeY) * ph;
            const bool used = k < setup_.slots.size() && setup_.slots[k].kind != PlayerSlot::Kind::Open;
            const Vec4 c = used ? kSlotColors[k] : Vec4{0.4f, 0.4f, 0.4f, 1};
            r.drawOverlayRect(sx - 11 * s, sy - 11 * s, sx + 11 * s, sy + 11 * s, {0, 0, 0, 0.7f});
            r.drawOverlayRect(sx - 9 * s, sy - 9 * s, sx + 9 * s, sy + 9 * s, c);
            const std::string n = std::to_string(k + 1);
            paint_.text(n, sx - paint_.textWidth(n, false) / 2, sy - lhs / 2, {0, 0, 0, 1}, false);
        }
        // Map info beside the preview.
        const float ix = px + pw + 20 * s;
        paint_.text(m.title, ix, py, kText);
        char info[96];
        std::snprintf(info, sizeof info, "%zu start positions, %.0f x %.0f", m.starts.size(), m.sizeX, m.sizeY);
        paint_.text(info, ix, py + lh + 4 * s, kDim, false);
        paint_.text(m.name, ix, py + lh + lhs + 8 * s, kDim, false);
        if (!mods_.empty()) {
            float y = py + lh + 3 * lhs + 20 * s;
            paint_.text("Mods:", ix, y, kDim, false);
            for (const std::string& mod : mods_) paint_.text(mod, ix, y += lhs, kDim, false);
        }

        // Player slots.
        float y = py + ph + 18 * s;
        const float c0 = rx, c1 = rx + 150 * s, c2 = c1 + 190 * s, c3 = c2 + 220 * s, c4 = c3 + 130 * s;
        paint_.text("Start", c0, y, kDim, false);
        paint_.text("Player", c1, y, kDim, false);
        paint_.text("Faction", c2, y, kDim, false);
        paint_.text("Team", c3, y, kDim, false);
        y += lhs + 6 * s;
        for (size_t k = 0; k < setup_.slots.size(); ++k) {
            const PlayerSlot& p = setup_.slots[k];
            const float bh = lh + 4 * s;
            r.drawOverlayRect(c0, y + 4 * s, c0 + 14 * s, y + bh - 4 * s, kSlotColors[k]);
            paint_.text("Position " + std::to_string(k + 1), c0 + 22 * s, y + 2 * s, kText);
            std::string kind = p.kind == PlayerSlot::Kind::Open ? "Open" : p.kind == PlayerSlot::Kind::Human ? "You" : "Computer";
            if (lobby != Lobby::None && p.kind == PlayerSlot::Kind::Human) {
                kind = p.netId == lobbyId ? "You" : "Player";
                for (const auto& [pid, n] : lobbyPlayers)
                    if (pid == p.netId && p.netId != lobbyId) kind = n;
            }
            paint_.button(r, kSlotBase + int(k) * 4 + 0, kind, c1, y, c2 - 10 * s, y + bh, p.kind == PlayerSlot::Kind::Human, mouseX_, mouseY_);
            if (p.kind != PlayerSlot::Kind::Open) {
                paint_.button(r, kSlotBase + int(k) * 4 + 1, factionTitle(p.faction), c2, y, c3 - 10 * s, y + bh, false, mouseX_, mouseY_);
                paint_.button(r, kSlotBase + int(k) * 4 + 2, p.team < 0 ? "-" : std::to_string(p.team + 1), c3, y, c4 - 10 * s,
                              y + bh, false, mouseX_, mouseY_);
            }
            y += bh + 6 * s;
        }
        paint_.text("Click to change, right click to go back. Players without a team fight everyone.", rx, y + 4 * s, kDim, false);
    }

    // Buttons (bottom right).
    const float bw = 200 * s, bh = lh + 16 * s, by = H - pad - bh;
    int players = 0;
    for (const PlayerSlot& p : setup_.slots) players += p.kind != PlayerSlot::Kind::Open;
    if (lobby == Lobby::None) {
        paint_.button(r, kHost, "Host LAN game", pad + 260 * s, by, pad + 260 * s + bw, by + bh, false, mouseX_, mouseY_);
        paint_.button(r, kJoin, "Join LAN game", pad + 276 * s + bw, by, pad + 276 * s + bw * 2, by + bh, false, mouseX_, mouseY_);
    } else {
        paint_.text(lobbyStatus, pad, by + (bh - lh) / 2 - lhs, kHot);
    }
    if (typing_) {
        const float tw = 520 * s, th = lh * 3;
        r.drawOverlayRect(W / 2 - tw / 2, H / 2 - th, W / 2 + tw / 2, H / 2 + th, kPanel);
        paint_.text("Join a LAN game: the host's address (Enter to join, Esc to cancel)", W / 2 - tw / 2 + 16 * s, H / 2 - th + 12 * s, kDim, false);
        r.drawOverlayRect(W / 2 - tw / 2 + 16 * s, H / 2 - lh / 2 - 4 * s, W / 2 + tw / 2 - 16 * s, H / 2 + lh / 2 + 4 * s, {0, 0, 0, 0.8f});
        paint_.text(typed_ + "_", W / 2 - tw / 2 + 24 * s, H / 2 - lh / 2, kText);
    }
    if (lobby == Lobby::None && !quickSave.empty() && std::ifstream(quickSave).good()) {
        // In the info column, under the game options.
        const float x2 = W - pad - bw * 1.2f, y2 = 300 * s + 2 * (bh + 8 * s) + 16 * s;
        paint_.button(r, kLoad, "Load saved game", x2, y2, W - pad, y2 + bh, false, mouseX_, mouseY_);
        paint_.button(r, kReplay, "Watch it as a replay", x2, y2 + bh + 8 * s, W - pad, y2 + bh * 2 + 8 * s, false, mouseX_, mouseY_);
    }
    {
        static const char* ai[] = {"Computer: easy", "Computer: normal", "Computer: hard"};
        static const char* cr[] = {"Credits: low", "Credits: normal", "Credits: high", "Credits: very high"};
        // In the info column on the right, under the map's details.
        const float y2 = 300 * s, x2 = W - pad - bw * 1.2f;
        paint_.button(r, kAiLevel, ai[std::clamp(setup_.aiLevel, 0, 2)], x2, y2, W - pad, y2 + bh, setup_.aiLevel != 1, mouseX_, mouseY_);
        paint_.button(r, kCredits, cr[std::clamp(setup_.credits, 0, 3)], x2, y2 + bh + 8 * s, W - pad, y2 + bh * 2 + 8 * s,
                      setup_.credits != 1, mouseX_, mouseY_);
    }
    paint_.button(r, kFog, setup_.fog ? "Fog of war: on" : "Fog of war: off", W - pad - bw * 3 - 32 * s, by, W - pad - bw * 2 - 32 * s,
                  by + bh, setup_.fog, mouseX_, mouseY_);
    paint_.button(r, kQuit, "Quit", W - pad - bw * 2 - 16 * s, by, W - pad - bw - 16 * s, by + bh, false, mouseX_, mouseY_);
    paint_.button(r, kStart, players >= 2 ? "Start game" : "Need 2 players", W - pad - bw, by, W - pad, by + bh, players >= 2, mouseX_,
                  mouseY_);
    paint_.text("Up/Down: map   Enter: start   Esc: quit", pad, H - pad - lhs, kDim, false);
    paint_.flush(r);
}

// ---------------------------------------------------------------- pause menu

namespace {
const char* kPauseItems[] = {"Resume", "Save game (F5)", "Load game (F9)", "Sound", "Music", "Restart", "Quit to setup", "Exit game"};
constexpr int kPauseCount = 8;
}

PauseMenu::Result PauseMenu::handle(const SDL_Event& e) {
    auto result = [](int i) {
        switch (i) {
        case 0: return Result::Resume;
        case 1: return Result::Save;
        case 2: return Result::Load;
        case 3: return Result::Sound;
        case 4: return Result::Music;
        case 5: return Result::Restart;
        case 6: return Result::ToMenu;
        default: return Result::Quit;
        }
    };
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        mouseX_ = e.motion.x;
        mouseY_ = e.motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT)
            if (const int id = paint_.buttonAt(e.button.x, e.button.y); id >= 0) return result(id);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) return Result::Resume;
        if (e.key.key == SDLK_DOWN) highlight_ = (highlight_ + 1) % kPauseCount;
        if (e.key.key == SDLK_UP) highlight_ = (highlight_ + kPauseCount - 1) % kPauseCount;
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) return result(highlight_);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START || e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return Result::Resume;
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) highlight_ = (highlight_ + 1) % kPauseCount;
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) highlight_ = (highlight_ + kPauseCount - 1) % kPauseCount;
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return result(highlight_);
        break;
    default: break;
    }
    return Result::None;
}

void PauseMenu::draw(Renderer& r) {
    const float W = float(r.width()), H = float(r.height()), s = paint_.scale();
    const float lh = paint_.lineHeight();
    paint_.begin();
    r.drawOverlayRect(0, 0, W, H, {0, 0, 0, 0.55f});
    const float bw = 300 * s, bh = lh + 18 * s, gap = 12 * s;
    const float x0 = W / 2 - bw / 2, y0 = H / 2 - (kPauseCount * bh + (kPauseCount - 1) * gap) / 2;
    r.drawOverlayRect(x0 - 24 * s, y0 - lh - 40 * s, x0 + bw + 24 * s, y0 + kPauseCount * bh + (kPauseCount - 1) * gap + 24 * s, kPanel);
    paint_.text("Paused", W / 2 - paint_.textWidth("Paused") / 2, y0 - lh - 24 * s, kHot);
    for (int i = 0; i < kPauseCount; ++i) {
        const float y = y0 + float(i) * (bh + gap);
        std::string label = kPauseItems[i];
        if (i == 3) label = soundOn ? "Sound: on" : "Sound: off";
        if (i == 4) label = musicOn ? "Music: on" : "Music: off";
        if (paint_.button(r, i, label, x0, y, x0 + bw, y + bh, i == highlight_, mouseX_, mouseY_)) highlight_ = i;
    }
    paint_.flush(r);
}

} // namespace uaw
