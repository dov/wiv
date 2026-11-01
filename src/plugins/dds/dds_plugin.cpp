//
// dds_plugin.cpp - wiv image plugin for DirectDraw Surface (.dds) files:
// the top mip level of 2D textures, either block compressed (BC1-BC5, BC7;
// DXT1/3/5 and ATI1/2 FourCCs as well as the DX10 header) or uncompressed
// (8/16/24/32-bit masks, DX10 RGBA8/BGRA8). Block decoding is done by the
// vendored single-header bcdec (third_party/bcdec.h). Cube maps, volumes and
// texture arrays show their first face/slice/element.
//
#include "../wiv_plugin_common.h"

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    return std::equal(filename.end() - n, filename.end(), ext, [](char a, char b)
                       { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

uint32_t rd32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

constexpr uint32_t fourcc(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40,
                   DDPF_LUMINANCE = 0x20000;

// DXGI formats handled via the DX10 extension header.
enum
{
    DXGI_RGBA8 = 28,
    DXGI_RGBA8_SRGB = 29,
    DXGI_BC1 = 71,
    DXGI_BC1_SRGB = 72,
    DXGI_BC2 = 74,
    DXGI_BC2_SRGB = 75,
    DXGI_BC3 = 77,
    DXGI_BC3_SRGB = 78,
    DXGI_BC4 = 80,
    DXGI_BC5 = 83,
    DXGI_BGRA8 = 87,
    DXGI_BGRX8 = 88,
    DXGI_BGRA8_SRGB = 91,
    DXGI_BGRX8_SRGB = 93,
    DXGI_BC7 = 98,
    DXGI_BC7_SRGB = 99,
};

enum class Kind
{
    None,
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC7,
    RGBA8,
    BGRA8,
    BGRX8,
    Masked,
};

// Expand `value & mask` (contiguous bits) to 8 bits.
unsigned expandChannel(uint32_t value, uint32_t mask)
{
    if (!mask)
        return 0;
    int shift = 0;
    while (!((mask >> shift) & 1))
        ++shift;
    uint32_t v = (value & mask) >> shift;
    uint32_t maxv = mask >> shift;
    return maxv == 255 ? v : (v * 255 + maxv / 2) / maxv;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(std::string(filename), ".dds");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    auto fail = [&](const char* fmt) -> WivPluginImage*
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(fmt, filename);
        return nullptr;
    };

    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
        return fail("dds: failed to open %s");
    std::fseek(fh, 0, SEEK_END);
    long size = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    if (size < 128)
    {
        std::fclose(fh);
        return fail("dds: file too small: %s");
    }
    std::vector<unsigned char> data(static_cast<size_t>(size));
    size_t nread = std::fread(data.data(), 1, data.size(), fh);
    std::fclose(fh);
    if (nread != data.size())
        return fail("dds: failed to read %s");

    const unsigned char* h = data.data();
    if (std::memcmp(h, "DDS ", 4) != 0 || rd32(h + 4) != 124 || rd32(h + 76) != 32)
        return fail("dds: not a valid DDS file: %s");

    const int height = static_cast<int>(rd32(h + 12));
    const int width = static_cast<int>(rd32(h + 16));
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536)
        return fail("dds: bad dimensions in %s");

    const uint32_t pfFlags = rd32(h + 80), pfFourCC = rd32(h + 84), pfBits = rd32(h + 88);
    const uint32_t mR = rd32(h + 92), mG = rd32(h + 96), mB = rd32(h + 100), mA = rd32(h + 104);
    size_t offset = 128;

    Kind kind = Kind::None;
    if (pfFlags & DDPF_FOURCC)
    {
        if (pfFourCC == fourcc("DX10"))
        {
            if (size < 148)
                return fail("dds: truncated DX10 header in %s");
            const uint32_t dxgi = rd32(h + 128);
            offset = 148;
            switch (dxgi)
            {
                case DXGI_BC1: case DXGI_BC1_SRGB: kind = Kind::BC1; break;
                case DXGI_BC2: case DXGI_BC2_SRGB: kind = Kind::BC2; break;
                case DXGI_BC3: case DXGI_BC3_SRGB: kind = Kind::BC3; break;
                case DXGI_BC4: kind = Kind::BC4; break;
                case DXGI_BC5: kind = Kind::BC5; break;
                case DXGI_BC7: case DXGI_BC7_SRGB: kind = Kind::BC7; break;
                case DXGI_RGBA8: case DXGI_RGBA8_SRGB: kind = Kind::RGBA8; break;
                case DXGI_BGRA8: case DXGI_BGRA8_SRGB: kind = Kind::BGRA8; break;
                case DXGI_BGRX8: case DXGI_BGRX8_SRGB: kind = Kind::BGRX8; break;
                default:
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "dds: unsupported DXGI format %u in %%s", dxgi);
                    return fail(buf);
                }
            }
        }
        else if (pfFourCC == fourcc("DXT1")) kind = Kind::BC1;
        else if (pfFourCC == fourcc("DXT2") || pfFourCC == fourcc("DXT3")) kind = Kind::BC2;
        else if (pfFourCC == fourcc("DXT4") || pfFourCC == fourcc("DXT5")) kind = Kind::BC3;
        else if (pfFourCC == fourcc("ATI1") || pfFourCC == fourcc("BC4U")) kind = Kind::BC4;
        else if (pfFourCC == fourcc("ATI2") || pfFourCC == fourcc("BC5U")) kind = Kind::BC5;
        else
            return fail("dds: unsupported FourCC compression in %s");
    }
    else if (pfFlags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA))
    {
        if (pfBits != 8 && pfBits != 16 && pfBits != 24 && pfBits != 32)
            return fail("dds: unsupported bit depth in %s");
        kind = Kind::Masked;
    }
    else
        return fail("dds: unsupported pixel format in %s");

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    unsigned char* out = img->rgba;
    const unsigned char* src = h + offset;
    const size_t avail = data.size() - offset;

    if (kind == Kind::BC1 || kind == Kind::BC2 || kind == Kind::BC3 || kind == Kind::BC4 || kind == Kind::BC5 ||
        kind == Kind::BC7)
    {
        const int bw = (width + 3) / 4, bh = (height + 3) / 4;
        const size_t blockBytes = (kind == Kind::BC1 || kind == Kind::BC4) ? 8 : 16;
        if (avail < blockBytes * bw * bh)
        {
            wiv_plugin_free_image(img);
            return fail("dds: truncated pixel data in %s");
        }
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                const unsigned char* blk = src + (static_cast<size_t>(by) * bw + bx) * blockBytes;
                unsigned char tile[16 * 4];
                switch (kind)
                {
                    case Kind::BC1: bcdec_bc1(blk, tile, 16); break;
                    case Kind::BC2: bcdec_bc2(blk, tile, 16); break;
                    case Kind::BC3: bcdec_bc3(blk, tile, 16); break;
                    case Kind::BC7: bcdec_bc7(blk, tile, 16); break;
                    case Kind::BC4:
                    {
                        unsigned char r[16];
                        bcdec_bc4(blk, r, 4);
                        for (int i = 0; i < 16; ++i)
                        {
                            tile[i * 4 + 0] = tile[i * 4 + 1] = tile[i * 4 + 2] = r[i];
                            tile[i * 4 + 3] = 255;
                        }
                        break;
                    }
                    default: // BC5: two channels, typically a normal map's X/Y
                    {
                        unsigned char rg[16 * 2];
                        bcdec_bc5(blk, rg, 8);
                        for (int i = 0; i < 16; ++i)
                        {
                            tile[i * 4 + 0] = rg[i * 2];
                            tile[i * 4 + 1] = rg[i * 2 + 1];
                            tile[i * 4 + 2] = 0;
                            tile[i * 4 + 3] = 255;
                        }
                        break;
                    }
                }
                // bc1/2/3/7 wrote with a pitch of 16 bytes (4 px); bc4/bc5 are
                // repacked above into the same layout.
                for (int y = 0; y < 4 && by * 4 + y < height; ++y)
                    for (int x = 0; x < 4 && bx * 4 + x < width; ++x)
                        std::memcpy(out + ((static_cast<size_t>(by) * 4 + y) * width + bx * 4 + x) * 4,
                                    tile + (y * 4 + x) * 4, 4);
            }
        return img;
    }

    // Uncompressed.
    const size_t bpp = (kind == Kind::Masked ? pfBits : 32) / 8;
    if (avail < bpp * width * height)
    {
        wiv_plugin_free_image(img);
        return fail("dds: truncated pixel data in %s");
    }
    const bool hasAlpha = (pfFlags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) && mA;
    for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
    {
        const unsigned char* p = src + i * bpp;
        unsigned char* o = out + i * 4;
        switch (kind)
        {
            case Kind::RGBA8: std::memcpy(o, p, 4); break;
            case Kind::BGRA8: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;
            case Kind::BGRX8: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
            default:
            {
                uint32_t v = 0;
                for (size_t b = 0; b < bpp; ++b)
                    v |= uint32_t(p[b]) << (8 * b);
                if (pfFlags & DDPF_LUMINANCE)
                {
                    unsigned l = expandChannel(v, mR);
                    o[0] = o[1] = o[2] = static_cast<unsigned char>(l);
                    o[3] = hasAlpha ? expandChannel(v, mA) : 255;
                }
                else if (pfFlags & DDPF_RGB)
                {
                    o[0] = expandChannel(v, mR);
                    o[1] = expandChannel(v, mG);
                    o[2] = expandChannel(v, mB);
                    o[3] = hasAlpha ? expandChannel(v, mA) : 255;
                }
                else // alpha-only
                {
                    o[0] = o[1] = o[2] = 255;
                    o[3] = expandChannel(v, mA);
                }
            }
        }
    }
    return img;
}
