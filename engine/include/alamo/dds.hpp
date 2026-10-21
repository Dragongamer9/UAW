// DDS texture decoding to RGBA8 (top mip level).
// Supports what Universe at War ships: DXT1, DXT3, DXT5 and uncompressed
// 32-bit (A8R8G8B8 / X8R8G8B8) and 24-bit (R8G8B8) surfaces.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace alamo {

class DdsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, row 0 at the top
    std::string sourceFormat;  // "DXT1", "DXT5", "RGBA32", ...
};

Image decodeDds(std::span<const uint8_t> data);

// Uncompressed TGA (types 2 and 10, 24/32-bit), used by a few UI/art files.
Image decodeTga(std::span<const uint8_t> data);

// Minimal PNG encoder (zlib-compressed RGBA8).
std::vector<uint8_t> encodePng(const Image& img);

} // namespace alamo
