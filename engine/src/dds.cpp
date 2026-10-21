#include "alamo/dds.hpp"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace alamo {

namespace {

uint32_t u32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw DdsError("truncated header");
    return uint32_t(d[off]) | uint32_t(d[off + 1]) << 8 | uint32_t(d[off + 2]) << 16 | uint32_t(d[off + 3]) << 24;
}

void rgb565(uint16_t c, uint8_t out[4]) {
    uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = uint8_t((r << 3) | (r >> 2));
    out[1] = uint8_t((g << 2) | (g >> 4));
    out[2] = uint8_t((b << 3) | (b >> 2));
    out[3] = 255;
}

// Decodes one 4x4 colour block (8 bytes). dxt1Alpha enables the 3-colour + transparent mode.
void colorBlock(const uint8_t* b, bool dxt1Alpha, uint8_t px[16][4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1Alpha) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(bits >> (2 * i)) & 3], 4);
}

void dxt5Alpha(const uint8_t* b, uint8_t px[16][4]) {
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
        for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
}

void dxt3Alpha(const uint8_t* b, uint8_t px[16][4]) {
    for (int i = 0; i < 16; ++i) {
        uint8_t nib = (b[i / 2] >> ((i & 1) * 4)) & 15;
        px[i][3] = uint8_t(nib * 17);
    }
}

int shiftOf(uint32_t mask) {
    if (!mask) return 0;
    int s = 0;
    while (!(mask & 1)) { mask >>= 1; ++s; }
    return s;
}

uint8_t extract(uint32_t px, uint32_t mask) {
    if (!mask) return 255;
    uint32_t v = (px & mask) >> shiftOf(mask);
    uint32_t max = mask >> shiftOf(mask);
    return uint8_t(v * 255 / max);
}

} // namespace

Image decodeDds(std::span<const uint8_t> d) {
    if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) throw DdsError("not a DDS file");
    Image img;
    img.height = u32(d, 12);
    img.width = u32(d, 16);
    if (!img.width || !img.height || img.width > 16384 || img.height > 16384) throw DdsError("bad dimensions");
    const uint32_t pfFlags = u32(d, 80);
    const uint32_t fourcc = u32(d, 84);
    const uint32_t bits = u32(d, 88);
    const uint32_t rMask = u32(d, 92), gMask = u32(d, 96), bMask = u32(d, 100), aMask = u32(d, 104);
    const auto pixels = d.subspan(128);
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);

    if (pfFlags & 0x4) { // FOURCC
        char fc[5] = {char(fourcc), char(fourcc >> 8), char(fourcc >> 16), char(fourcc >> 24), 0};
        img.sourceFormat = fc;
        int kind = img.sourceFormat == "DXT1" ? 1 : img.sourceFormat == "DXT3" ? 3 : img.sourceFormat == "DXT5" ? 5 : 0;
        if (!kind) throw DdsError("unsupported FourCC " + img.sourceFormat);
        const size_t blockBytes = kind == 1 ? 8 : 16;
        const uint32_t bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
        if (pixels.size() < size_t(bw) * bh * blockBytes) throw DdsError("truncated pixel data");
        uint8_t px[16][4];
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const uint8_t* blk = pixels.data() + (size_t(by) * bw + bx) * blockBytes;
                if (kind == 1) colorBlock(blk, true, px);
                else {
                    colorBlock(blk + 8, false, px);
                    if (kind == 3) dxt3Alpha(blk, px); else dxt5Alpha(blk, px);
                }
                for (int i = 0; i < 16; ++i) {
                    uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < img.width && y < img.height) std::memcpy(&img.rgba[(size_t(y) * img.width + x) * 4], px[i], 4);
                }
            }
        }
        return img;
    }

    if ((pfFlags & 0x40) && (bits == 32 || bits == 24)) { // RGB, optional alpha (0x1)
        const uint32_t bpp = bits / 8;
        img.sourceFormat = bits == 32 ? ((pfFlags & 1) ? "ARGB32" : "XRGB32") : "RGB24";
        if (pixels.size() < size_t(img.width) * img.height * bpp) throw DdsError("truncated pixel data");
        const uint32_t useA = (pfFlags & 1) ? aMask : 0;
        for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
            uint32_t p = 0;
            for (uint32_t k = 0; k < bpp; ++k) p |= uint32_t(pixels[i * bpp + k]) << (8 * k);
            img.rgba[i * 4 + 0] = extract(p, rMask);
            img.rgba[i * 4 + 1] = extract(p, gMask);
            img.rgba[i * 4 + 2] = extract(p, bMask);
            img.rgba[i * 4 + 3] = extract(p, useA);
        }
        return img;
    }
    throw DdsError("unsupported DDS pixel format");
}

Image decodeTga(std::span<const uint8_t> d) {
    if (d.size() < 18) throw DdsError("truncated TGA");
    const uint8_t idLen = d[0], cmap = d[1], type = d[2], bpp = d[16], desc = d[17];
    if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32)) throw DdsError("unsupported TGA type");
    Image img;
    img.width = uint32_t(d[12] | d[13] << 8);
    img.height = uint32_t(d[14] | d[15] << 8);
    img.sourceFormat = type == 10 ? "TGA-RLE" : "TGA";
    const size_t n = size_t(img.width) * img.height, bytes = bpp / 8;
    img.rgba.assign(n * 4, 255);
    size_t pos = 18 + idLen, i = 0;
    auto put = [&](size_t at) {
        if (pos + bytes > d.size()) throw DdsError("truncated TGA");
        img.rgba[i * 4 + 0] = d[at + 2];
        img.rgba[i * 4 + 1] = d[at + 1];
        img.rgba[i * 4 + 2] = d[at + 0];
        if (bytes == 4) img.rgba[i * 4 + 3] = d[at + 3];
        ++i;
    };
    while (i < n) {
        if (type == 2) { put(pos); pos += bytes; continue; }
        if (pos >= d.size()) throw DdsError("truncated TGA");
        uint8_t h = d[pos++];
        size_t count = (h & 0x7F) + 1;
        if (h & 0x80) { size_t at = pos; for (size_t k = 0; k < count && i < n; ++k) put(at); pos += bytes; }
        else for (size_t k = 0; k < count && i < n; ++k) { put(pos); pos += bytes; }
    }
    if (!(desc & 0x20)) { // bottom-up: flip
        const size_t row = size_t(img.width) * 4;
        for (uint32_t y = 0; y < img.height / 2; ++y)
            std::swap_ranges(img.rgba.begin() + y * row, img.rgba.begin() + (y + 1) * row,
                             img.rgba.begin() + (img.height - 1 - y) * row);
    }
    return img;
}

std::vector<uint8_t> encodePng(const Image& img) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(img.width) * 4 + 1) * img.height);
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(0); // filter: none
        auto row = img.rgba.begin() + size_t(y) * img.width * 4;
        raw.insert(raw.end(), row, row + size_t(img.width) * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK) throw DdsError("zlib failed");
    z.resize(zlen);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(uint32_t(data.size()));
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        be32(uint32_t(::crc32(0, out.data() + start, uInt(out.size() - start))));
    };
    std::vector<uint8_t> ihdr;
    for (uint32_t v : {img.width, img.height}) for (int s = 24; s >= 0; s -= 8) ihdr.push_back(uint8_t(v >> s));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

} // namespace alamo
