#include "alamo/assets.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>

namespace alamo {

namespace {

// Megatextures: the GUI's small pictures (build icons, buttons) are packed into atlases,
// Data\Art\Textures\MT_*.TGA with an .MTD directory beside each: uint32 count, then per entry a
// 64-byte name, x, y, width, height (uint32) and one alpha byte.
struct AtlasEntry {
    std::string atlas; // the .tga beside the .mtd
    uint32_t x, y, w, h;
};
struct Atlases {
    std::map<std::string, AtlasEntry> entries; // upper-case stem -> where
    std::map<std::string, std::optional<Image>> images;
};
std::mutex atlasMutex;
std::map<const MegVfs*, Atlases> atlasCache;

std::string upperStem(std::string n) {
    if (size_t slash = n.find_last_of("\\/"); slash != std::string::npos) n = n.substr(slash + 1);
    if (size_t dot = n.rfind('.'); dot != std::string::npos && n.size() - dot <= 4) n.resize(dot);
    for (char& c : n) c = char(std::toupper(static_cast<unsigned char>(c)));
    return n;
}

Atlases& atlasesFor(MegVfs& vfs) {
    auto [it, fresh] = atlasCache.try_emplace(&vfs);
    if (!fresh) return it->second;
    for (const std::string& path : vfs.listDirectory("Data\\Art\\Textures")) {
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".MTD") != 0) continue;
        std::vector<uint8_t> d;
        try { d = vfs.read(path); } catch (const std::exception&) { continue; }
        if (d.size() < 4) continue;
        uint32_t n = 0;
        std::memcpy(&n, d.data(), 4);
        if (4 + size_t(n) * 81 > d.size()) continue;
        const std::string atlas = path.substr(0, path.size() - 4) + ".TGA";
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* e = d.data() + 4 + size_t(i) * 81;
            std::string name(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 64));
            AtlasEntry a{atlas, 0, 0, 0, 0};
            std::memcpy(&a.x, e + 64, 4);
            std::memcpy(&a.y, e + 68, 4);
            std::memcpy(&a.w, e + 72, 4);
            std::memcpy(&a.h, e + 76, 4);
            it->second.entries.emplace(upperStem(name), a); // first atlas listing a name wins
        }
    }
    return it->second;
}

std::optional<Image> fromAtlas(MegVfs& vfs, const std::string& name) {
    std::lock_guard<std::mutex> lock(atlasMutex);
    Atlases& at = atlasesFor(vfs);
    const auto e = at.entries.find(upperStem(name));
    if (e == at.entries.end()) return std::nullopt;
    auto [img, fresh] = at.images.try_emplace(e->second.atlas);
    if (fresh) {
        try {
            img->second = decodeTga(vfs.read(e->second.atlas));
        } catch (const std::exception&) {
        }
    }
    if (!img->second) return std::nullopt;
    const Image& big = *img->second;
    const AtlasEntry& r = e->second;
    if (r.w == 0 || r.h == 0 || r.x + r.w > big.width || r.y + r.h > big.height) return std::nullopt;
    Image out;
    out.width = r.w;
    out.height = r.h;
    out.sourceFormat = "atlas";
    out.rgba.resize(size_t(r.w) * r.h * 4);
    for (uint32_t row = 0; row < r.h; ++row)
        std::memcpy(out.rgba.data() + size_t(row) * r.w * 4, big.rgba.data() + (size_t(r.y + row) * big.width + r.x) * 4, size_t(r.w) * 4);
    return out;
}

} // namespace

std::optional<Image> loadGameTexture(MegVfs& vfs, const std::string& name) {
    std::string stem = name;
    if (size_t dot = stem.rfind('.'); dot != std::string::npos && stem.size() - dot <= 4) stem.resize(dot);
    for (const char* ext : {".dds", ".tga"}) {
        const std::string p = "Data\\Art\\Textures\\" + stem + ext;
        if (!vfs.exists(p)) continue;
        try {
            auto bytes = vfs.read(p);
            return std::string(ext) == ".dds" ? decodeDds(bytes) : decodeTga(bytes);
        } catch (const std::exception&) {
            // fall through to the next extension
        }
    }
    return fromAtlas(vfs, name); // a GUI picture packed in a megatexture
}

std::string modelPath(const std::string& name) {
    std::string n = name;
    // Tolerate a doubled extension ("ABHP_Habitat_Armor_Leg.alo.alo" in Remix's XML).
    auto endsAlo = [](const std::string& s) {
        return s.size() >= 4 && s[s.size() - 4] == '.' && std::tolower(static_cast<unsigned char>(s[s.size() - 3])) == 'a' &&
               std::tolower(static_cast<unsigned char>(s[s.size() - 2])) == 'l' && std::tolower(static_cast<unsigned char>(s[s.size() - 1])) == 'o';
    };
    while (n.size() > 8 && endsAlo(n) && endsAlo(n.substr(0, n.size() - 4))) n.resize(n.size() - 4);
    if (n.find('\\') != std::string::npos || n.find('/') != std::string::npos) return n;
    return "Data\\Art\\Models\\" + n;
}

namespace {

Vec3 guessColor(const std::string& name, size_t index) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    struct Rule {
        const char* key;
        Vec3 c;
    };
    static const Rule rules[] = {
        {"snow", {0.88f, 0.9f, 0.93f}},   {"ice", {0.8f, 0.86f, 0.9f}},      {"grass", {0.36f, 0.48f, 0.24f}},
        {"forest", {0.3f, 0.4f, 0.22f}},  {"moss", {0.34f, 0.42f, 0.25f}},   {"sand", {0.78f, 0.68f, 0.48f}},
        {"snd", {0.76f, 0.66f, 0.47f}},   {"desert", {0.8f, 0.7f, 0.5f}},    {"dirt", {0.5f, 0.4f, 0.3f}},
        {"mud", {0.42f, 0.35f, 0.27f}},   {"rock", {0.5f, 0.48f, 0.45f}},    {"cliff", {0.45f, 0.43f, 0.4f}},
        {"stone", {0.55f, 0.53f, 0.5f}},  {"asphalt", {0.32f, 0.32f, 0.33f}}, {"road", {0.35f, 0.35f, 0.36f}},
        {"concrete", {0.6f, 0.6f, 0.6f}}, {"floor", {0.62f, 0.58f, 0.5f}},   {"water", {0.2f, 0.32f, 0.42f}},
        {"jungle", {0.25f, 0.38f, 0.2f}}, {"leaf", {0.35f, 0.38f, 0.22f}},   {"gravel", {0.52f, 0.5f, 0.46f}},
    };
    for (const auto& r : rules)
        if (n.find(r.key) != std::string::npos) return r.c;
    const float h = float((index * 37) % 100) / 100.f;
    return {0.45f + 0.15f * h, 0.45f + 0.1f * (1 - h), 0.38f};
}

} // namespace

std::vector<Vec3> terrainLayerColors(MegVfs& vfs, const Map& map, size_t* fromTextures) {
    std::vector<Vec3> out(map.layers.size());
    size_t real = 0;
    for (size_t i = 0; i < map.layers.size(); ++i) {
        Vec3 base;
        if (auto img = loadGameTexture(vfs, map.layers[i].diffuse)) {
            double s[3] = {0, 0, 0};
            const size_t n = size_t(img->width) * img->height;
            for (size_t p = 0; p < n; ++p)
                for (int k = 0; k < 3; ++k) s[k] += img->rgba[p * 4 + k];
            base = {float(s[0] / n / 255), float(s[1] / n / 255), float(s[2] / n / 255)};
            ++real;
        } else {
            base = guessColor(map.layers[i].diffuse, i);
        }
        for (int k = 0; k < 3; ++k) out[i][k] = base[k] * map.layers[i].tint[k];
    }
    if (fromTextures) *fromTextures = real;
    return out;
}

} // namespace alamo
