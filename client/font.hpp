// Bitmap font baked from a TrueType file (the game ships Data/Resources/Fonts/*.ttf).
#pragma once

#include "renderer.hpp"

#include <string_view>
#include <vector>

namespace uaw {

class Font {
public:
    // Bakes Latin-1 at `pixelHeight`. Throws on a bad font file.
    Font(const std::vector<uint8_t>& ttf, float pixelHeight);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    float lineHeight() const { return lineHeight_; }
    float ascent() const { return ascent_; }
    float width(std::string_view utf8) const;

    // Appends quads; (x, y) is the top-left of the line box. Returns the x after the text.
    float append(std::vector<GpuVertex>& out, std::string_view utf8, float x, float y, const alamo::Vec4& color) const;
    GLuint texture() const { return texture_; }

private:
    struct Glyph {
        float x0, y0, x1, y1;   // quad relative to the pen position on the baseline (pixels)
        float s0, t0, s1, t1;   // atlas texture coordinates
        float xadvance;
    };
    std::vector<Glyph> glyphs_; // codepoints 32..255
    GLuint texture_ = 0;
    int atlasW_ = 1024, atlasH_ = 512;
    float lineHeight_ = 0, ascent_ = 0;
    const Glyph& glyph(uint32_t cp) const;
};

} // namespace uaw
