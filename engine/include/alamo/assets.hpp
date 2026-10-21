// Shared asset lookups used by the tools and the viewer.
#pragma once

#include "alamo/dds.hpp"
#include "alamo/meg.hpp"
#include "alamo/ted.hpp"

#include <optional>
#include <string>

namespace alamo {

// Materials and maps name textures as .tga; shipped files are usually .dds with
// the same stem in Data/Art/Textures. Returns nullopt if neither exists.
std::optional<Image> loadGameTexture(MegVfs& vfs, const std::string& name);

// "Data\Art\Models\X.ALO" for a bare model name.
std::string modelPath(const std::string& name);

// Average colour of each terrain layer: from the real texture when available,
// otherwise a guess from the texture name, multiplied by the layer tint.
// `fromTextures` (optional) receives how many came from real textures.
std::vector<Vec3> terrainLayerColors(MegVfs& vfs, const Map& map, size_t* fromTextures = nullptr);

} // namespace alamo
