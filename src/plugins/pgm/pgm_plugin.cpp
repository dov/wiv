//
// pgm_plugin.cpp - wiv image plugin for binary PGM (P5, gray) and PPM
// (P6, RGB) files. No external dependency. Modeled on giv's
// ~/hd/github/giv/src/plugins/pgm.cc, extended to also handle P6 and to
// carry the error-message parameter that pgm.cc's ABI was (accidentally)
// missing.
//
#include "../wiv_plugin_common.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    std::string tail = filename.substr(filename.size() - n);
    for (char& c : tail)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ext;
}

// Reads one whitespace-separated token from a binary PNM header, skipping
// '#' comments, matching the netpbm header grammar.
bool readToken(FILE* fh, char* buf, size_t bufSize)
{
    int c;
    // Skip whitespace and comments.
    for (;;)
    {
        c = fgetc(fh);
        if (c == EOF)
            return false;
        if (c == '#')
        {
            while ((c = fgetc(fh)) != EOF && c != '\n')
                ;
            continue;
        }
        if (!std::isspace(c))
            break;
    }

    size_t i = 0;
    while (c != EOF && !std::isspace(c) && i + 1 < bufSize)
    {
        buf[i++] = static_cast<char>(c);
        c = fgetc(fh);
    }
    buf[i] = '\0';
    return i > 0;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".pgm") || hasExtension(f, ".ppm");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("pgm: failed to open %s", filename);
        return nullptr;
    }

    char tok[64];
    bool ok = readToken(fh, tok, sizeof(tok));
    bool isColor;
    if (ok && std::strcmp(tok, "P5") == 0)
        isColor = false;
    else if (ok && std::strcmp(tok, "P6") == 0)
        isColor = true;
    else
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("pgm: not a binary PGM/PPM file: %s", filename);
        return nullptr;
    }

    int width = 0, height = 0, maxval = 0;
    if (readToken(fh, tok, sizeof(tok)))
        width = std::atoi(tok);
    if (readToken(fh, tok, sizeof(tok)))
        height = std::atoi(tok);
    if (readToken(fh, tok, sizeof(tok)))
        maxval = std::atoi(tok);

    if (width <= 0 || height <= 0 || maxval <= 0)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("pgm: malformed header in %s", filename);
        return nullptr;
    }

    // A single whitespace byte separates the header from the binary data
    // (already consumed by readToken's trailing fgetc, since readToken
    // stops at - but does not push back - the delimiter).

    int srcChannels = isColor ? 3 : 1;
    bool is16bit = maxval > 255;
    size_t sampleBytes = is16bit ? 2 : 1;
    size_t rowBytes = static_cast<size_t>(width) * srcChannels * sampleBytes;
    std::vector<unsigned char> row(rowBytes);

    WivPluginImage* img = wiv_plugin::allocImage(width, height);

    // P5 (grayscale) carries real dynamic range - keep it as a raw sample
    // buffer for the Contrast/Color Table tools, in addition to the RGBA8
    // preview below (which still downshifts 16-bit samples for display).
    if (!isColor)
        wiv_plugin::allocSamples(img, is16bit ? WIV_SAMPLE_U16 : WIV_SAMPLE_U8);

    for (int y = 0; y < height; ++y)
    {
        if (std::fread(row.data(), 1, rowBytes, fh) != rowBytes)
        {
            std::fclose(fh);
            wiv_plugin_free_image(img);
            if (error_msg)
                *error_msg = wiv_plugin::makeError("pgm: truncated pixel data in %s", filename);
            return nullptr;
        }

        unsigned char* dst = img->rgba + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            unsigned char r, g, b;
            if (isColor)
            {
                size_t off = static_cast<size_t>(x) * 3 * sampleBytes;
                // PNM is big-endian; downshift 16-bit samples to 8-bit for
                // the RGBA8 preview (color PNM has no raw-sample path).
                r = is16bit ? row[off] : row[off];
                g = is16bit ? row[off + sampleBytes] : row[off + 1];
                b = is16bit ? row[off + 2 * sampleBytes] : row[off + 2];
            }
            else
            {
                size_t off = static_cast<size_t>(x) * sampleBytes;
                r = g = b = is16bit ? row[off] : row[off];

                // PNM is big-endian: reassemble the full-precision 16-bit
                // sample (row[off] is the high byte) for the raw buffer.
                if (is16bit)
                    reinterpret_cast<uint16_t*>(img->samples)[static_cast<size_t>(y) * width + x] =
                        static_cast<uint16_t>((row[off] << 8) | row[off + 1]);
                else
                    static_cast<uint8_t*>(img->samples)[static_cast<size_t>(y) * width + x] = row[off];
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = 255;
        }
    }

    std::fclose(fh);
    return img;
}
