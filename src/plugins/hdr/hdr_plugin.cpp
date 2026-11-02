//
// hdr_plugin.cpp - wiv image plugin for Radiance RGBE (.hdr, own parser:
// flat, old-style and new-style RLE scanlines, all eight axis orientations)
// and Portable Float Map (.pfm, "PF" colour / "Pf" grayscale). No external
// dependency. The header text is shown in the Image Info panel by
// ExifReader, not here.
//
// Grayscale PFM keeps its float samples for the Contrast/Color Table tools.
// Colour data is clamped to [0,1] and sRGB-encoded for the RGBA8 preview;
// use a PFM/HDR viewer's exposure tools if you need to look beyond that.
//
#include "../wiv_plugin_common.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

unsigned char encodeSrgb(float v)
{
    if (!(v > 0.f))
        return 0;
    if (v >= 1.f)
        return 255;
    float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(s * 255.f + 0.5f);
}

bool readToken(FILE* fh, char* buf, size_t bufSize)
{
    int c;
    do
        c = fgetc(fh);
    while (c != EOF && std::isspace(c));
    size_t i = 0;
    while (c != EOF && !std::isspace(c) && i + 1 < bufSize)
    {
        buf[i++] = static_cast<char>(c);
        c = fgetc(fh);
    }
    buf[i] = '\0'; // the single whitespace byte ending the token is consumed
    return i > 0;
}

bool readLine(FILE* fh, std::string& line)
{
    line.clear();
    int c;
    while ((c = fgetc(fh)) != EOF && c != '\n')
        line.push_back(static_cast<char>(c));
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return c != EOF || !line.empty();
}

// Reads one scanline of `len` RGBE pixels (4 bytes each) into `out`.
bool readScanline(FILE* fh, unsigned char* out, int len)
{
    int c0 = fgetc(fh), c1 = fgetc(fh), c2 = fgetc(fh), c3 = fgetc(fh);
    if (c3 == EOF)
        return false;
    if (len >= 8 && len < 32768 && c0 == 2 && c1 == 2 && !(c2 & 0x80))
    {
        // New-style RLE: each of the four channels is coded separately.
        if (((c2 << 8) | c3) != len)
            return false;
        for (int ch = 0; ch < 4; ++ch)
            for (int x = 0; x < len;)
            {
                int n = fgetc(fh);
                if (n == EOF)
                    return false;
                if (n > 128)
                {
                    n -= 128;
                    int v = fgetc(fh);
                    if (v == EOF || x + n > len)
                        return false;
                    for (int i = 0; i < n; ++i)
                        out[(x++) * 4 + ch] = static_cast<unsigned char>(v);
                }
                else
                {
                    if (n == 0 || x + n > len)
                        return false;
                    for (int i = 0; i < n; ++i)
                    {
                        int v = fgetc(fh);
                        if (v == EOF)
                            return false;
                        out[(x++) * 4 + ch] = static_cast<unsigned char>(v);
                    }
                }
            }
        return true;
    }
    // Flat pixels, with the old-style RLE (1,1,1,n) repeat marker.
    int shift = 0;
    unsigned char prev[4] = {};
    int x = 0;
    unsigned char px[4] = {static_cast<unsigned char>(c0), static_cast<unsigned char>(c1),
                           static_cast<unsigned char>(c2), static_cast<unsigned char>(c3)};
    for (;;)
    {
        if (px[0] == 1 && px[1] == 1 && px[2] == 1)
        {
            if (x == 0)
                return false;
            int n = px[3] << shift;
            if (x + n > len)
                return false;
            for (int i = 0; i < n; ++i)
                std::memcpy(out + (x++) * 4, prev, 4);
            shift += 8;
        }
        else
        {
            std::memcpy(out + (x++) * 4, px, 4);
            std::memcpy(prev, px, 4);
            shift = 0;
        }
        if (x >= len)
            return true;
        if (fread(px, 1, 4, fh) != 4)
            return false;
    }
}

WivPluginImage* loadHdr(const char* filename, char** error_msg)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("hdr: failed to open %s", filename);
        return nullptr;
    }
    auto fail = [&](const char* msg, WivPluginImage* img = nullptr) -> WivPluginImage*
    {
        std::fclose(fh);
        if (img)
            wiv_plugin_free_image(img);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("hdr: %s", msg);
        return nullptr;
    };

    std::string line;
    if (!readLine(fh, line) || line.compare(0, 2, "#?") != 0)
        return fail("not a Radiance HDR file");
    bool rgbe = false;
    for (int guard = 0;; ++guard)
    {
        if (guard > 1000 || !readLine(fh, line))
            return fail("truncated header");
        if (line.empty())
            break; // blank line ends the header
        if (line == "FORMAT=32-bit_rle_rgbe")
            rgbe = true;
        else if (line.compare(0, 7, "FORMAT=") == 0)
            return fail("unsupported pixel format (only 32-bit_rle_rgbe)");
    }
    (void)rgbe; // FORMAT is optional; RGBE is the default

    // Resolution line: "-Y h +X w" (standard) or any of the eight variants.
    if (!readLine(fh, line))
        return fail("missing resolution line");
    char a0, a1;
    char s0, s1;
    int n0, n1;
    if (std::sscanf(line.c_str(), " %c%c %d %c%c %d", &s0, &a0, &n0, &s1, &a1, &n1) != 6 ||
        (s0 != '+' && s0 != '-') || (s1 != '+' && s1 != '-') ||
        !((a0 == 'Y' && a1 == 'X') || (a0 == 'X' && a1 == 'Y')) || n0 <= 0 || n1 <= 0 ||
        static_cast<int64_t>(n0) * n1 > (1ll << 31) / 16)
        return fail("malformed resolution line");

    const bool transposed = a0 == 'X'; // scanlines run along Y
    const int width = transposed ? n0 : n1;
    const int height = transposed ? n1 : n0;
    WivPluginImage* img = wiv_plugin::allocImage(width, height);

    std::vector<unsigned char> scan(static_cast<size_t>(n1) * 4);
    for (int sIdx = 0; sIdx < n0; ++sIdx)
    {
        if (!readScanline(fh, scan.data(), n1))
            return fail("truncated or corrupt pixel data", img);
        for (int p = 0; p < n1; ++p)
        {
            // Axis 0 indexes scanlines, axis 1 pixels within one.
            const int i0 = s0 == '+' ? (a0 == 'Y' ? n0 - 1 - sIdx : sIdx) : (a0 == 'Y' ? sIdx : n0 - 1 - sIdx);
            const int i1 = s1 == '+' ? (a1 == 'Y' ? n1 - 1 - p : p) : (a1 == 'Y' ? p : n1 - 1 - p);
            const int x = transposed ? i0 : i1;
            const int y = transposed ? i1 : i0;
            const unsigned char* px = &scan[static_cast<size_t>(p) * 4];
            unsigned char* dst = img->rgba + (static_cast<size_t>(y) * width + x) * 4;
            if (px[3] == 0)
            {
                dst[0] = dst[1] = dst[2] = 0;
            }
            else
            {
                const float f = std::ldexp(1.f, px[3] - 136);
                dst[0] = encodeSrgb(px[0] * f);
                dst[1] = encodeSrgb(px[1] * f);
                dst[2] = encodeSrgb(px[2] * f);
            }
            dst[3] = 255;
        }
    }
    std::fclose(fh);
    return img;
}

WivPluginImage* loadPfm(const char* filename, char** error_msg)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("pfm: failed to open %s", filename);
        return nullptr;
    }
    char tok[64];
    bool isColor = false;
    bool ok = readToken(fh, tok, sizeof(tok));
    if (ok && std::strcmp(tok, "PF") == 0)
        isColor = true;
    else if (!(ok && std::strcmp(tok, "Pf") == 0))
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("pfm: not a PFM file: %s", filename);
        return nullptr;
    }
    int w = 0, h = 0;
    double scale = 0;
    if (readToken(fh, tok, sizeof(tok)))
        w = std::atoi(tok);
    if (readToken(fh, tok, sizeof(tok)))
        h = std::atoi(tok);
    if (readToken(fh, tok, sizeof(tok)))
        scale = std::atof(tok);
    if (w <= 0 || h <= 0 || scale == 0 || static_cast<int64_t>(w) * h > (1ll << 31) / 16)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("pfm: malformed header in %s", filename);
        return nullptr;
    }

    // Negative scale = little-endian samples.
    const bool fileLittle = scale < 0;
    const uint16_t probe = 1;
    const bool hostLittle = *reinterpret_cast<const uint8_t*>(&probe) == 1;
    const bool swap = fileLittle != hostLittle;

    const int ch = isColor ? 3 : 1;
    const size_t rowFloats = static_cast<size_t>(w) * ch;
    std::vector<float> row(rowFloats);

    WivPluginImage* img = wiv_plugin::allocImage(w, h);
    if (!isColor)
        wiv_plugin::allocSamples(img, WIV_SAMPLE_FLOAT);

    // PFM stores scanlines bottom-to-top.
    for (int fileRow = 0; fileRow < h; ++fileRow)
    {
        if (std::fread(row.data(), sizeof(float), rowFloats, fh) != rowFloats)
        {
            std::fclose(fh);
            wiv_plugin_free_image(img);
            if (error_msg)
                *error_msg = wiv_plugin::makeError("pfm: truncated pixel data in %s", filename);
            return nullptr;
        }
        if (swap)
            for (float& f : row)
            {
                uint32_t u;
                std::memcpy(&u, &f, 4);
                u = __builtin_bswap32(u);
                std::memcpy(&f, &u, 4);
            }
        const int y = h - 1 - fileRow;
        unsigned char* dst = img->rgba + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x)
        {
            float r, g, b;
            if (isColor)
            {
                r = row[x * 3];
                g = row[x * 3 + 1];
                b = row[x * 3 + 2];
            }
            else
            {
                r = g = b = row[x];
                static_cast<float*>(img->samples)[static_cast<size_t>(y) * w + x] = row[x];
            }
            dst[x * 4 + 0] = encodeSrgb(r);
            dst[x * 4 + 1] = encodeSrgb(g);
            dst[x * 4 + 2] = encodeSrgb(b);
            dst[x * 4 + 3] = 255;
        }
    }
    std::fclose(fh);
    return img;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".hdr") || hasExtension(f, ".pfm");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    return hasExtension(filename, ".pfm") ? loadPfm(filename, error_msg) : loadHdr(filename, error_msg);
}
