// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// dicom_plugin.cpp - wiv image plugin for a minimal subset of DICOM: an
// uncompressed, single-sample-per-pixel (grayscale) 8- or 16-bit dataset,
// Explicit VR Little Endian (the overwhelmingly common transfer syntax for
// simple CT/MR/US exports). Modeled on giv's standalone parser
// (~/hd/github/giv/src/plugins/simple_dicom.c - not the dcmtk-based
// dicom.cc, to avoid pulling in dcmtk as a new dependency), with its
// `return =` typo fixed and its ad-hoc "does this look like a VR code"
// heuristic for implicit-VR/undictioned tags kept as-is (same heuristic
// giv used). Multi-frame datasets (NumberOfFrames, tag 0028,0008) are
// exposed as a multi-slice image (WivPluginImage::depth) - giv's simple
// parser didn't read that tag at all; only its dcmtk-based loader did.
//
#include "../wiv_plugin_common.h"

#include <algorithm>
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

uint16_t readU16(FILE* fh, bool* ok)
{
    uint16_t v = 0;
    if (std::fread(&v, 1, 2, fh) != 2)
        *ok = false;
    return v; // little-endian file, assumed little-endian host
}

uint32_t readU32(FILE* fh, bool* ok)
{
    uint32_t v = 0;
    if (std::fread(&v, 1, 4, fh) != 4)
        *ok = false;
    return v;
}

// A DICOM data element's value representation is either a genuine 2-letter
// explicit-VR code (both bytes uppercase A-Z, e.g. "US", "OB"), or - for
// implicit-VR-encoded elements - the first two bytes of a 4-byte length
// field instead. giv's heuristic (kept as-is here): if it doesn't look like
// two uppercase letters, assume implicit VR and re-interpret accordingly.
bool looksLikeVrCode(const char vr[2])
{
    return vr[0] >= 'A' && vr[0] <= 'Z' && vr[1] >= 'A' && vr[1] <= 'Z';
}

bool isLongLengthVr(const char vr[2])
{
    return std::memcmp(vr, "OB", 2) == 0 || std::memcmp(vr, "OW", 2) == 0 ||
           std::memcmp(vr, "SQ", 2) == 0 || std::memcmp(vr, "UN", 2) == 0;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(filename, ".dcm");
}

// Content check for extensionless/oddly named files (e.g. "MR.NO.EXTENSION"):
// a DICOM Part 10 file has "DICM" at offset 128.
extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return false;
    char preamble[132];
    bool match = std::fread(preamble, 1, sizeof(preamble), fh) == sizeof(preamble) &&
                 std::memcmp(preamble + 128, "DICM", 4) == 0;
    std::fclose(fh);
    return match;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("dicom: failed to open %s", filename);
        return nullptr;
    }

    char preamble[132];
    bool ok = std::fread(preamble, 1, sizeof(preamble), fh) == sizeof(preamble);
    if (!ok || std::memcmp(preamble + 128, "DICM", 4) != 0)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("dicom: not a DICOM file (missing DICM magic): %s", filename);
        return nullptr;
    }

    int width = 0, height = 0, samplesPerPixel = 0, bitsAllocated = 0, numFrames = 1;
    std::vector<unsigned char> pixelData;

    while (ok)
    {
        uint16_t groupWord = readU16(fh, &ok);
        if (!ok)
            break; // clean EOF between elements
        uint16_t elementWord = readU16(fh, &ok);
        char vr[2];
        if (!ok || std::fread(vr, 1, 2, fh) != 2)
            break;

        uint32_t elementLength;
        if (looksLikeVrCode(vr))
        {
            if (isLongLengthVr(vr))
            {
                uint16_t reserved = readU16(fh, &ok); // skip 2 reserved bytes
                (void)reserved;
                elementLength = readU32(fh, &ok);
            }
            else
            {
                elementLength = readU16(fh, &ok);
            }
        }
        else
        {
            // Implicit VR: the 2 bytes just read (misread as `vr`) are the
            // low half of a 4-byte length field - read the other 2.
            unsigned char lenBytes[4] = {static_cast<unsigned char>(vr[0]), static_cast<unsigned char>(vr[1]), 0, 0};
            if (std::fread(lenBytes + 2, 1, 2, fh) != 2)
                ok = false;
            std::memcpy(&elementLength, lenBytes, 4);
        }
        if (!ok)
            break;

        if (elementLength == 0xffffffffu || (groupWord == 0xfffe && elementWord == 0xe000))
            continue; // sequence-of-items delimiter/item tag - not supported, skip

        std::vector<unsigned char> value(elementLength + 1, 0);
        if (elementLength > 0 && std::fread(value.data(), 1, elementLength, fh) != elementLength)
        {
            ok = false;
            break;
        }

        const uint16_t valueU16 = elementLength >= 2 ? (value[0] | (value[1] << 8)) : 0;

        if (groupWord == 0x0028)
        {
            switch (elementWord)
            {
                case 0x0002: samplesPerPixel = valueU16; break;
                case 0x0008: // NumberOfFrames (IS - ASCII integer string)
                    numFrames = std::max(1, std::atoi(reinterpret_cast<const char*>(value.data())));
                    break;
                case 0x0010: height = valueU16; break;
                case 0x0011: width = valueU16; break;
                case 0x0100: bitsAllocated = valueU16; break;
            }
        }
        else if (groupWord == 0x7fe0 && elementWord == 0x0010)
        {
            pixelData = std::move(value);
        }
    }
    std::fclose(fh);

    if (width <= 0 || height <= 0 || (bitsAllocated != 8 && bitsAllocated != 16) || samplesPerPixel != 1)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(
                "dicom: unsupported dataset (need 8/16-bit single-sample pixels): %s", filename);
        return nullptr;
    }

    const size_t bytesPerSample = static_cast<size_t>(bitsAllocated) / 8;
    const size_t sliceElemCount = static_cast<size_t>(width) * height;
    const size_t neededBytes = sliceElemCount * bytesPerSample * static_cast<size_t>(numFrames);
    if (pixelData.size() < neededBytes)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("dicom: pixel data shorter than Rows*Columns*Frames in %s", filename);
        return nullptr;
    }

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    const WivSampleType sampleType = bitsAllocated == 16 ? WIV_SAMPLE_U16 : WIV_SAMPLE_U8;
    wiv_plugin::allocSamples(img, sampleType, numFrames);
    std::memcpy(img->samples, pixelData.data(), sliceElemCount * bytesPerSample * static_cast<size_t>(numFrames));

    // Slice-0 grayscale preview - never actually displayed for a raw-sample
    // image (see DisplayImage.h), just a reasonable placeholder.
    for (size_t i = 0; i < sliceElemCount; ++i)
    {
        uint8_t g = bitsAllocated == 8 ? static_cast<const uint8_t*>(img->samples)[i]
                                       : static_cast<uint8_t>(static_cast<const uint16_t*>(img->samples)[i] >> 8);
        img->rgba[i * 4 + 0] = g;
        img->rgba[i * 4 + 1] = g;
        img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }

    return img;
}
