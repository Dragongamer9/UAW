#include "alamo/assets.hpp"

#include <algorithm>
#include <cctype>

namespace alamo {

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
    return std::nullopt;
}

std::string modelPath(const std::string& name) {
    if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos) return name;
    return "Data\\Art\\Models\\" + name;
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
