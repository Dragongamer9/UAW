// Export an ALO model to binary glTF (.glb) so it can be inspected in
// Blender or any glTF viewer. Bone transforms are baked into the vertices and
// the Alamo Z-up frame is converted to glTF's Y-up.
#pragma once

#include "alamo/alo.hpp"
#include "alamo/dds.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace alamo {

struct GltfOptions {
    bool includeHidden = false; // also export hidden meshes (LODs, shadow volumes, damage states)
};

struct GltfStats {
    size_t meshes = 0, primitives = 0, triangles = 0, textures = 0;
    std::vector<std::string> missingTextures;
};

// Resolves a material texture name (e.g. "NV_Corruptor.tga") to a decoded image.
using TextureLoader = std::function<std::optional<Image>(const std::string& name)>;

std::vector<uint8_t> exportGlb(const Model& model, const TextureLoader& textures, const GltfOptions& opt,
                               GltfStats* stats = nullptr);

} // namespace alamo
