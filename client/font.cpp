#include "font.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include <cmath>
#include <stdexcept>

namespace uaw {

namespace {

// Decodes one UTF-8 codepoint; malformed bytes come back as '?'.
uint32_t nextCodepoint(std::string_view s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) return '?';
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

} // namespace

Font::Font(const std::vector<uint8_t>& ttf, float pixelHeight) {
    stbtt_fontinfo info;
    if (ttf.empty() || !stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)))
        throw std::runtime_error("bad font file");
    const float scale = stbtt_ScaleForPixelHeight(&info, pixelHeight);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    ascent_ = float(asc) * scale;
    lineHeight_ = float(asc - desc + gap) * scale;

    std::vector<uint8_t> alpha(size_t(atlasW_) * atlasH_);
    std::vector<stbtt_packedchar> chars(224);
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, alpha.data(), atlasW_, atlasH_, 0, 1, nullptr)) throw std::runtime_error("font pack failed");
    stbtt_PackSetOversampling(&pc, 2, 1);
    stbtt_PackFontRange(&pc, ttf.data(), 0, pixelHeight, 32, 224, chars.data());
    stbtt_PackEnd(&pc);

    glyphs_.resize(224);
    for (int i = 0; i < 224; ++i) {
        float px = 0, py = 0;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(chars.data(), atlasW_, atlasH_, i, &px, &py, &q, 0);
        glyphs_[size_t(i)] = {q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1, px};
    }
    // White texels with coverage in alpha, so the unlit shader tints them.
    std::vector<uint8_t> rgba(alpha.size() * 4, 255);
    for (size_t i = 0; i < alpha.size(); ++i) rgba[i * 4 + 3] = alpha[i];
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlasW_, atlasH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

Font::~Font() {
    if (texture_) glDeleteTextures(1, &texture_);
}

const Font::Glyph& Font::glyph(uint32_t cp) const {
    if (cp < 32 || cp > 255) cp = '?';
    return glyphs_[cp - 32];
}

float Font::width(std::string_view s) const {
    float w = 0;
    for (size_t i = 0; i < s.size();) w += glyph(nextCodepoint(s, i)).xadvance;
    return w;
}

float Font::append(std::vector<GpuVertex>& out, std::string_view s, float x, float y, const alamo::Vec4& c) const {
    const float baseline = y + ascent_;
    for (size_t i = 0; i < s.size();) {
        const Glyph& g = glyph(nextCodepoint(s, i));
        const float px = std::round(x);
        const float x0 = px + g.x0, y0 = baseline + g.y0, x1 = px + g.x1, y1 = baseline + g.y1;
        const float u0 = g.s0, v0 = g.t0, u1 = g.s1, v1 = g.t1;
        const GpuVertex a{x0, y0, 0, 0, 0, 1, u0, v0, c[0], c[1], c[2], c[3]};
        const GpuVertex b{x1, y0, 0, 0, 0, 1, u1, v0, c[0], c[1], c[2], c[3]};
        const GpuVertex d{x1, y1, 0, 0, 0, 1, u1, v1, c[0], c[1], c[2], c[3]};
        const GpuVertex e{x0, y1, 0, 0, 0, 1, u0, v1, c[0], c[1], c[2], c[3]};
        out.insert(out.end(), {a, b, d, a, d, e});
        x += g.xadvance;
    }
    return x;
}

} // namespace uaw
