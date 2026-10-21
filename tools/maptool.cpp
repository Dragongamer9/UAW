// maptool: inspect Universe at War maps (.TED).
//   maptool <game-root> list                       every map with size, objects, start positions
//   maptool <game-root> check                      load every map, report problems
//   maptool <game-root> info <MAP>                 layers, editor groups, object types, start positions
//   maptool <game-root> overview <MAP> <out.png> [pixels] [--group NAME]
//                                                  top-down picture: shaded terrain, objects, starts
// <MAP> is a name like M16_Sahara or a full path inside the archives.
#include "alamo/dds.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/ted.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>

using namespace alamo;

static int usage() {
    std::cerr << "usage:\n"
                 "  maptool <game-root> list\n"
                 "  maptool <game-root> check\n"
                 "  maptool <game-root> info <MAP>\n"
                 "  maptool <game-root> overview <MAP> <out.png> [pixels] [--group NAME]\n";
    return 2;
}

static std::string mapPath(std::string name) {
    if (name.find('\\') == std::string::npos && name.find('/') == std::string::npos) name = "Data\\Art\\Maps\\" + name;
    std::string up = normalizePath(name);
    if (up.size() < 4 || up.compare(up.size() - 4, 4, ".TED") != 0) name += ".ted";
    return name;
}

static std::vector<std::string> allMaps(const MegVfs& vfs) {
    std::vector<std::string> out;
    for (const auto& p : vfs.list())
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".TED") == 0) out.push_back(p);
    return out;
}

static std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    if (b.size() > 4) b.resize(b.size() - 4);
    return b;
}

static const char* typeName(const GameDatabase& db, uint32_t crc) {
    const GameObjectType* t = db.findByCrc(crc);
    return t ? t->name.c_str() : nullptr;
}

// Start positions are marked with Marker_Player_<n>_Start objects.
static std::vector<std::pair<int, Vec3>> startPositions(const Map& m, const GameDatabase& db) {
    std::vector<std::pair<int, Vec3>> out;
    for (const auto& o : m.objects) {
        const char* n = typeName(db, o.typeCrc);
        int slot;
        if (n && std::sscanf(n, "Marker_Player_%d_Start", &slot) == 1) out.push_back({slot, o.position});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return out;
}

// ------------------------------------------------------------------ drawing

struct Canvas {
    Image img;
    Canvas(uint32_t w, uint32_t h) {
        img.width = w;
        img.height = h;
        img.rgba.assign(size_t(w) * h * 4, 255);
    }
    void put(int x, int y, const uint8_t c[3], float a = 1) {
        if (x < 0 || y < 0 || x >= int(img.width) || y >= int(img.height)) return;
        uint8_t* p = &img.rgba[(size_t(y) * img.width + x) * 4];
        for (int i = 0; i < 3; ++i) p[i] = uint8_t(p[i] * (1 - a) + c[i] * a);
    }
    void disc(float cx, float cy, float r, const uint8_t c[3], float a = 1) {
        for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
            for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
                float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                if (d <= r) put(x, y, c, a * std::clamp(r - d + 0.5f, 0.f, 1.f));
            }
    }
    void ring(float cx, float cy, float r, float w, const uint8_t c[3]) {
        for (int y = int(cy - r - w - 1); y <= int(cy + r + w + 1); ++y)
            for (int x = int(cx - r - w - 1); x <= int(cx + r + w + 1); ++x) {
                float d = std::fabs(std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r);
                if (d <= w) put(x, y, c, std::clamp(w - d + 0.5f, 0.f, 1.f));
            }
    }
    void square(float cx, float cy, float r, const uint8_t c[3]) {
        for (int y = int(cy - r); y <= int(cy + r); ++y)
            for (int x = int(cx - r); x <= int(cx + r); ++x) put(x, y, c);
    }
    void rect(float x0, float y0, float x1, float y1, const uint8_t c[3]) {
        for (int x = int(x0); x <= int(x1); ++x) { put(x, int(y0), c); put(x, int(y1), c); }
        for (int y = int(y0); y <= int(y1); ++y) { put(int(x0), y, c); put(int(x1), y, c); }
    }
    // 3x5 digit font, scaled.
    void digit(int d, float x, float y, float s, const uint8_t c[3]) {
        static const char* glyphs[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111",
                                         "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                         "111101111101111", "111101111001111"};
        if (d < 0 || d > 9) return;
        for (int r = 0; r < 5; ++r)
            for (int k = 0; k < 3; ++k)
                if (glyphs[d][r * 3 + k] == '1')
                    for (int yy = 0; yy < int(s); ++yy)
                        for (int xx = 0; xx < int(s); ++xx) put(int(x + k * s + xx), int(y + r * s + yy), c);
    }
};

// Average colour of a terrain texture, if the texture is available.
static std::optional<Vec3> textureColor(MegVfs& vfs, const std::string& name) {
    for (const char* ext : {".dds", ".tga"}) {
        std::string p = "Data\\Art\\Textures\\" + name + ext;
        if (!vfs.exists(p)) continue;
        try {
            auto bytes = vfs.read(p);
            Image img = std::string(ext) == ".dds" ? decodeDds(bytes) : decodeTga(bytes);
            double s[3] = {0, 0, 0};
            const size_t n = size_t(img.width) * img.height;
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k) s[k] += img.rgba[i * 4 + k];
            return Vec3{float(s[0] / n / 255), float(s[1] / n / 255), float(s[2] / n / 255)};
        } catch (...) {
        }
    }
    return std::nullopt;
}

// Plausible stand-in colour from the texture's name when the texture itself isn't available.
static Vec3 guessColor(const std::string& name, size_t index) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    struct Rule { const char* key; Vec3 c; };
    static const Rule rules[] = {
        {"snow", {0.88f, 0.9f, 0.93f}}, {"ice", {0.8f, 0.86f, 0.9f}},   {"grass", {0.36f, 0.48f, 0.24f}},
        {"forest", {0.3f, 0.4f, 0.22f}}, {"moss", {0.34f, 0.42f, 0.25f}}, {"sand", {0.78f, 0.68f, 0.48f}},
        {"snd", {0.76f, 0.66f, 0.47f}},  {"desert", {0.8f, 0.7f, 0.5f}},  {"dirt", {0.5f, 0.4f, 0.3f}},
        {"mud", {0.42f, 0.35f, 0.27f}},  {"rock", {0.5f, 0.48f, 0.45f}},  {"cliff", {0.45f, 0.43f, 0.4f}},
        {"stone", {0.55f, 0.53f, 0.5f}}, {"asphalt", {0.32f, 0.32f, 0.33f}}, {"road", {0.35f, 0.35f, 0.36f}},
        {"concrete", {0.6f, 0.6f, 0.6f}}, {"floor", {0.62f, 0.58f, 0.5f}}, {"water", {0.2f, 0.32f, 0.42f}},
        {"jungle", {0.25f, 0.38f, 0.2f}}, {"leaf", {0.35f, 0.38f, 0.22f}}, {"gravel", {0.52f, 0.5f, 0.46f}},
    };
    for (const auto& r : rules)
        if (n.find(r.key) != std::string::npos) return r.c;
    const float h = float((index * 37) % 100) / 100.f;
    return {0.45f + 0.15f * h, 0.45f + 0.1f * (1 - h), 0.38f};
}

static int overview(MegVfs& vfs, const GameDatabase& db, const Map& m, const std::string& out, uint32_t pixels,
                    const std::string& onlyGroup) {
    const float worldX = m.sizeX(), worldY = m.sizeY();
    const float scale = float(pixels) / std::max(worldX, worldY);
    const uint32_t W = std::max(1u, uint32_t(worldX * scale)), H = std::max(1u, uint32_t(worldY * scale));
    Canvas cv(W, H);

    std::vector<Vec3> layerColor(m.layers.size());
    size_t fromTextures = 0;
    for (size_t i = 0; i < m.layers.size(); ++i) {
        auto c = textureColor(vfs, m.layers[i].diffuse);
        fromTextures += c.has_value();
        Vec3 base = c ? *c : guessColor(m.layers[i].diffuse, i);
        for (int k = 0; k < 3; ++k) layerColor[i][k] = base[k] * m.layers[i].tint[k];
    }

    // Terrain: layer colour * hillshade. Image row 0 is the map's north (max Y).
    const float lx = -0.5f, ly = 0.6f, lz = 0.62f;
    for (uint32_t py = 0; py < H; ++py) {
        for (uint32_t px = 0; px < W; ++px) {
            const float wx = (px + 0.5f) / scale, wy = worldY - (py + 0.5f) / scale;
            const float e = m.spacing;
            const float dx = (m.heightAt(wx + e, wy) - m.heightAt(wx - e, wy)) / (2 * e);
            const float dy = (m.heightAt(wx, wy + e) - m.heightAt(wx, wy - e)) / (2 * e);
            const float inv = 1 / std::sqrt(dx * dx + dy * dy + 1);
            const float shade = std::clamp((-dx * lx - dy * ly + lz) * inv, 0.f, 1.f) * 0.75f + 0.3f;
            uint32_t gx = std::min(m.width - 1, uint32_t(std::lround(wx / m.spacing)));
            uint32_t gy = std::min(m.height - 1, uint32_t(std::lround(wy / m.spacing)));
            uint8_t li = m.layerIndex[size_t(gy) * m.width + gx];
            Vec3 c = li < layerColor.size() ? layerColor[li] : Vec3{0.5f, 0.5f, 0.5f};
            uint8_t col[3];
            for (int k = 0; k < 3; ++k) col[k] = uint8_t(std::clamp(c[k] * shade, 0.f, 1.f) * 255);
            cv.put(int(px), int(py), col);
        }
    }

    auto toPx = [&](const Vec3& p) { return std::pair<float, float>{p[0] * scale, (worldY - p[1]) * scale}; };

    // Playable area.
    if (m.playableMax[0] > m.playableMin[0]) {
        const uint8_t c[3] = {255, 255, 255};
        auto a = toPx(m.playableMin), b = toPx(m.playableMax);
        cv.rect(a.first, b.second, b.first, a.second, c);
    }

    static const uint8_t ownerColors[8][3] = {{230, 60, 50},  {60, 120, 235}, {70, 190, 80}, {200, 200, 200},
                                              {150, 150, 150}, {235, 200, 40}, {170, 80, 200}, {40, 200, 200}};
    const int groupFilter = onlyGroup.empty() ? -1 : m.groupIndex(onlyGroup);
    if (!onlyGroup.empty() && groupFilter < 0) std::cerr << "warning: no group named " << onlyGroup << "\n";
    const float unit = std::max(1.5f, pixels / 400.f);
    size_t drawn = 0;
    for (const auto& o : m.objects) {
        if (groupFilter >= 0 && !o.groups.empty() &&
            std::find(o.groups.begin(), o.groups.end(), uint32_t(groupFilter)) == o.groups.end())
            continue;
        const GameObjectType* t = db.findByCrc(o.typeCrc);
        if (!t) continue;
        const std::string& cat = t->category;
        if (t->name.rfind("Marker_", 0) == 0) continue; // markers drawn separately
        auto [x, y] = toPx(o.position);
        const uint8_t* oc = ownerColors[std::clamp(o.owner, 0, 7)];
        const bool structure = cat.find("Structure") != std::string::npos || cat.find("Walker") != std::string::npos;
        const bool unit_ = cat.find("Units") != std::string::npos || cat.find("Heroes") != std::string::npos;
        const bool resource = cat.find("Resource") != std::string::npos;
        if (structure) cv.square(x, y, unit * 2.2f, oc);
        else if (unit_) cv.disc(x, y, unit * 1.3f, oc);
        else if (resource) { const uint8_t c[3] = {120, 230, 255}; cv.disc(x, y, unit * 1.1f, c); }
        else { const uint8_t c[3] = {40, 35, 30}; cv.disc(x, y, unit * 0.7f, c, 0.55f); }
        ++drawn;
    }

    for (const auto& [slot, pos] : startPositions(m, db)) {
        auto [x, y] = toPx(pos);
        const uint8_t white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
        cv.disc(x, y, unit * 6, black, 0.55f);
        cv.ring(x, y, unit * 6, unit * 0.9f, white);
        const float s = std::max(2.f, unit * 1.6f);
        cv.digit(slot + 1, x - 1.5f * s, y - 2.5f * s, s, white);
    }

    auto png = encodePng(cv.img);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    std::printf("%s: %ux%u px, %zu objects drawn, %zu/%zu terrain layers from real textures\n", out.c_str(), W, H, drawn,
                fromTextures, m.layers.size());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[2];
    try {
        MegVfs vfs;
        mountGame(vfs, argv[1]);
        GameDatabase db;
        db.load(vfs);

        if (cmd == "list" || cmd == "check") {
            size_t failed = 0, unresolved = 0, objects = 0;
            std::printf("%-34s %9s %8s %7s %s\n", "map", "vertices", "size", "objects", "starts");
            for (const auto& p : allMaps(vfs)) {
                try {
                    Map m = loadTed(vfs.read(p));
                    size_t bad = 0;
                    for (const auto& o : m.objects) bad += db.findByCrc(o.typeCrc) == nullptr;
                    unresolved += bad;
                    objects += m.objects.size();
                    auto starts = startPositions(m, db);
                    std::printf("%-34s %4ux%-4u %8.0f %7zu %zu", baseName(p).c_str(), m.width, m.height, m.sizeX(),
                                m.objects.size(), starts.size());
                    if (bad) std::printf("  (%zu unknown types)", bad);
                    std::printf("\n");
                    if (cmd == "check")
                        for (const auto& w : m.warnings) std::printf("  warning: %s\n", w.c_str());
                } catch (const std::exception& e) {
                    ++failed;
                    std::printf("%-34s FAILED: %s\n", baseName(p).c_str(), e.what());
                }
            }
            std::printf("%zu objects, %zu with unknown types, %zu maps failed\n", objects, unresolved, failed);
            return failed ? 1 : 0;
        }

        if (argc < 4) return usage();
        const std::string path = mapPath(argv[3]);
        if (!vfs.exists(path)) { std::cerr << "no map " << path << "\n"; return 1; }
        Map m = loadTed(vfs.read(path));

        if (cmd == "info") {
            std::printf("%s (%s)\n", baseName(normalizePath(path)).c_str(), m.nameTextId.c_str());
            std::printf("terrain: %ux%u vertices, %.0f x %.0f units, heights %.1f..%.1f\n", m.width, m.height, m.sizeX(),
                        m.sizeY(), *std::min_element(m.heights.begin(), m.heights.end()),
                        *std::max_element(m.heights.begin(), m.heights.end()));
            std::printf("playable: (%.0f, %.0f) - (%.0f, %.0f)\n", m.playableMin[0], m.playableMin[1], m.playableMax[0],
                        m.playableMax[1]);
            std::printf("asset banks:");
            for (const auto& b : m.assetBanks) std::printf(" %s", b.c_str());
            std::printf("\ntexture layers: %zu\n", m.layers.size());
            std::map<uint8_t, size_t> used;
            for (uint8_t l : m.layerIndex) ++used[l];
            for (size_t i = 0; i < m.layers.size(); ++i)
                if (used.count(uint8_t(i)))
                    std::printf("  [%2zu] %-28s tiling %-5g %5.1f%% of terrain\n", i, m.layers[i].diffuse.c_str(),
                                m.layers[i].tiling, 100.0 * used[uint8_t(i)] / m.layerIndex.size());
            std::printf("editor groups:\n");
            for (const auto& g : m.groups) std::printf("  %-24s %zu objects\n", g.name.c_str(), g.objectIds.size());
            std::printf("start positions:\n");
            for (const auto& [slot, p] : startPositions(m, db))
                std::printf("  player %d at (%.0f, %.0f, %.1f)\n", slot + 1, p[0], p[1], p[2]);
            std::map<std::string, size_t> types;
            std::map<int, size_t> owners;
            for (const auto& o : m.objects) {
                const char* n = typeName(db, o.typeCrc);
                ++types[n ? n : "<unknown>"];
                ++owners[o.owner];
            }
            std::printf("objects: %zu (owners:", m.objects.size());
            for (auto [k, v] : owners) std::printf(" slot %d x%zu", k, v);
            std::printf(")\n");
            std::vector<std::pair<size_t, std::string>> sorted;
            for (auto& [k, v] : types) sorted.emplace_back(v, k);
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t i = 0; i < sorted.size() && i < 25; ++i)
                std::printf("  %5zu  %s\n", sorted[i].first, sorted[i].second.c_str());
            for (const auto& w : m.warnings) std::printf("warning: %s\n", w.c_str());
            return 0;
        }
        if (cmd == "overview" && argc > 4) {
            uint32_t px = 1024;
            std::string group;
            for (int i = 5; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--group" && i + 1 < argc) group = argv[++i];
                else px = uint32_t(std::stoul(a));
            }
            return overview(vfs, db, m, argv[4], std::clamp(px, 64u, 8192u), group);
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
