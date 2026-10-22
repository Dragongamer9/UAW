// TED map files.
//
// Worked out from the retail maps (no public documentation exists):
//
//   0x0000 file header (mini-chunks): 0x08 UTF-16 name text ID, 0x0D/0x0E world extent X/Y
//   0x0001 map
//     0x010D asset banks (mini 0x08 strings)
//     0x0100 environment (lighting, sky, fog)
//     0x0101 terrain
//       0x00 header: 0x00 width, 0x01 height (vertices), 0x04 vertex count, 0x0E cloud texture,
//                    0x1D/0x1E water bump textures
//       0x0D vertices: width*height x { int16 height; uint8 layer; uint8 layer2 }
//            height in world units = value * 25/512 (verified against every placed object on all
//            41 retail maps: objects sit exactly on the surface)
//       0x02 texture layers: 0x03 per layer (mini 0x0C diffuse, 0x13 bump, 0x07 tiling,
//            0x08 rotation, 0x0A tint)
//       0x0A, 0x0B decals/roads, 0x0C per-layer flags (3 bytes each)
//     0x010A per-vertex flags (uint32)
//     0x0102 objects
//       0x17 editor layers: 0x100 { mini 0x00 name, 0x01 object id (repeated) }
//       0x01 placed objects: 0x44C { 0x454 ids, 0x459 { 0x4B0 mini-chunks:
//            0x00 object id, 0x01 CRC-32 of the uppercase type name, 0x02 owner slot,
//            0x03 scale, 0x04 position, 0x05 rotation (degrees, Z = facing) } }
//     0x0103 playable area: 0x05 min, 0x06 max
//
// The vertex grid's rows run along +Y, columns along +X, 20 world units apart.
#pragma once

#include "alamo/alo.hpp"

#include <span>
#include <string>
#include <vector>

namespace alamo {

struct TerrainLayer {
    std::string diffuse, bump;
    float tiling = 0, rotation = 0;
    Vec3 tint{1, 1, 1};
};

struct MapObject {
    uint32_t id = 0;
    uint32_t typeCrc = 0;     // crc32(uppercase type name)
    int32_t owner = -1;       // player slot
    float scale = 1;
    Vec3 position{};
    Vec3 rotation{};          // degrees
    std::vector<uint32_t> groups; // indices into Map::groups
};

struct MapGroup {
    std::string name;         // e.g. "Multiplayer", "ZM01", "Intro_Cinematic"
    std::vector<uint32_t> objectIds;
};

// The map's lighting and atmosphere (chunk 0x0100 -> 0x04 -> the first 0x06 preset). Worked out
// from the retail maps: 0x00 sun colour, 0x01/0x02 fill light colours, 0x04 ambient, 0x05 sun
// intensity, 0x0B-0x0D sun direction (pointing down into the map), 0x0F fog colour, 0x10 fog on,
// 0x11/0x12 fog start/end, 0x14 preset name, 0x19 sky dome object type.
struct MapEnvironment {
    bool valid = false;
    std::string name, skyDome;
    Vec3 sun{1, 1, 1}, fill1{0.6f, 0.6f, 0.7f}, fill2{0.6f, 0.6f, 0.6f}, ambient{0.1f, 0.1f, 0.1f};
    float sunIntensity = 1;
    Vec3 sunDirection{0.45f, 0.35f, -0.82f};
    Vec3 fogColor{0.62f, 0.68f, 0.75f};
    bool fog = false;
    float fogStart = 500, fogEnd = 3000;
};

// Water (terrain header): 0x16 on, 0x15 level (world height), 0x1A colour (RGBA), 0x1D bump
// texture, 0x1E texture to use without shaders.
struct MapWater {
    bool on = false;
    float level = 0;
    std::array<float, 4> color{1, 1, 1, 1};
    std::string texture, bump;
};

struct Map {
    std::string nameTextId;   // e.g. TEXT_REGION_16
    uint32_t width = 0, height = 0;   // vertices
    float spacing = 20;               // world units between vertices
    std::vector<float> heights;       // width*height, row-major, row = Y
    std::vector<uint8_t> layerIndex;  // width*height
    std::vector<uint32_t> vertexFlags; // width*height from chunk 0x010A (see nav.hpp), empty if absent
    std::vector<TerrainLayer> layers;
    Vec3 playableMin{}, playableMax{};
    std::vector<std::string> assetBanks;
    std::vector<MapObject> objects;
    std::vector<MapGroup> groups;
    MapEnvironment environment;
    MapWater water;
    std::vector<std::string> warnings;

    float sizeX() const { return float(width - 1) * spacing; }
    float sizeY() const { return float(height - 1) * spacing; }
    float vertexHeight(uint32_t x, uint32_t y) const { return heights[size_t(y) * width + x]; }
    float heightAt(float x, float y) const; // bilinear, clamped to the map
    int groupIndex(const std::string& name) const;
};

constexpr float kTedHeightScale = 25.0f / 512.0f;

Map loadTed(std::span<const uint8_t> data);

} // namespace alamo
