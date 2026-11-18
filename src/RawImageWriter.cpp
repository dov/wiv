// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "RawImageWriter.h"

#include "ImagePluginHost.h"
#include "PathUtils.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace giv
{
namespace
{
struct FileCloser
{
    void operator()(FILE* f) const
    {
        if (f) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

size_t bytesPerSample(WivSampleType t)
{
    switch (t)
    {
        case WIV_SAMPLE_U8: return 1;
        case WIV_SAMPLE_U16: return 2;
        case WIV_SAMPLE_FLOAT: return 4;
        default: return 0;
    }
}

bool setError(std::string* error, const std::string& msg)
{
    if (error) *error = msg;
    return false;
}

void put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// One classic-TIFF directory entry (12 bytes); `value` is stored inline.
void putEntry(std::vector<uint8_t>& v, uint16_t tag, uint16_t type, uint32_t count, uint32_t value)
{
    put16(v, tag);
    put16(v, type);
    put32(v, count);
    if (type == 3 && count == 1) // SHORT: left-justified in the 4-byte field
    {
        put16(v, uint16_t(value));
        put16(v, 0);
    }
    else
        put32(v, value);
}

bool writeBytes(FILE* f, const void* data, size_t n)
{
    return n == 0 || std::fwrite(data, 1, n, f) == n;
}

bool writeTiff(FILE* f, const LoadedImage& img, int firstSlice, int sliceCount, std::string* error)
{
    constexpr uint16_t kShort = 3, kLong = 4;
    const size_t bps = bytesPerSample(img.sampleType);
    const uint64_t pageBytes = uint64_t(img.width) * img.height * bps;
    constexpr int kEntries = 11;
    const uint64_t ifdBytes = 2 + kEntries * 12 + 4;
    const uint64_t total = 8 + sliceCount * (ifdBytes + pageBytes + 1);
    if (total >= 0xFFFFFFF0ull)
        return setError(error, "The data is too large for a classic (4 GB) TIFF file. Save a single slice, or use the .npy format.");

    std::vector<uint8_t> header;
    header.push_back('I');
    header.push_back('I');
    put16(header, 42);
    put32(header, 8); // first IFD directly after the header
    if (!writeBytes(f, header.data(), header.size())) return setError(error, "Write failed");

    uint64_t pos = 8;
    const uint16_t sampleFormat = img.sampleType == WIV_SAMPLE_FLOAT ? 3 : 1;
    for (int s = 0; s < sliceCount; ++s)
    {
        const uint32_t dataOffset = uint32_t(pos + ifdBytes);
        uint64_t next = dataOffset + pageBytes;
        const uint64_t pad = next & 1; // keep IFDs word aligned
        next += pad;

        std::vector<uint8_t> ifd;
        put16(ifd, kEntries);
        putEntry(ifd, 254, kLong, 1, s == 0 ? 0 : 2);       // NewSubfileType: later pages are "pages of a multi-page file"
        putEntry(ifd, 256, kLong, 1, uint32_t(img.width));  // ImageWidth
        putEntry(ifd, 257, kLong, 1, uint32_t(img.height)); // ImageLength
        putEntry(ifd, 258, kShort, 1, uint32_t(bps * 8));   // BitsPerSample
        putEntry(ifd, 259, kShort, 1, 1);                   // Compression: none
        putEntry(ifd, 262, kShort, 1, 1);                   // Photometric: BlackIsZero
        putEntry(ifd, 273, kLong, 1, dataOffset);           // StripOffsets (one strip)
        putEntry(ifd, 277, kShort, 1, 1);                   // SamplesPerPixel
        putEntry(ifd, 278, kLong, 1, uint32_t(img.height)); // RowsPerStrip
        putEntry(ifd, 279, kLong, 1, uint32_t(pageBytes));  // StripByteCounts
        putEntry(ifd, 339, kShort, 1, sampleFormat);        // SampleFormat: uint / IEEE float
        put32(ifd, s + 1 < sliceCount ? uint32_t(next) : 0);

        const uint8_t* data = img.samples.data() + uint64_t(firstSlice + s) * pageBytes;
        const uint8_t zero = 0;
        if (!writeBytes(f, ifd.data(), ifd.size()) || !writeBytes(f, data, pageBytes) || (pad && !writeBytes(f, &zero, 1)))
            return setError(error, "Write failed (disk full?)");
        pos = next;
    }
    return true;
}

bool writeNpy(FILE* f, const LoadedImage& img, int firstSlice, int sliceCount, bool stacked, std::string* error)
{
    const char* descr = img.sampleType == WIV_SAMPLE_U8 ? "|u1" : img.sampleType == WIV_SAMPLE_U16 ? "<u2" : "<f4";
    std::string shape = stacked ? "(" + std::to_string(sliceCount) + ", " : "(";
    shape += std::to_string(img.height) + ", " + std::to_string(img.width) + (stacked ? ")" : ")");
    std::string dict = std::string("{'descr': '") + descr + "', 'fortran_order': False, 'shape': " + shape + ", }";
    while ((10 + dict.size() + 1) % 64) dict += ' ';
    dict += '\n';

    std::vector<uint8_t> header = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    put16(header, uint16_t(dict.size()));
    header.insert(header.end(), dict.begin(), dict.end());

    const uint64_t pageBytes = uint64_t(img.width) * img.height * bytesPerSample(img.sampleType);
    if (!writeBytes(f, header.data(), header.size()) ||
        !writeBytes(f, img.samples.data() + uint64_t(firstSlice) * pageBytes, pageBytes * sliceCount))
        return setError(error, "Write failed (disk full?)");
    return true;
}
} // namespace

bool hasRawSamples(const LoadedImage& image)
{
    return !image.isPreview() && image.sampleType != WIV_SAMPLE_NONE && image.width > 0 && image.height > 0 &&
           image.depth >= 1 &&
           image.samples.size() >= uint64_t(image.width) * image.height * image.depth * bytesPerSample(image.sampleType);
}

bool rawFormatFromPath(const std::string& path, RawFormat& format)
{
    std::string ext = pathToUtf8(utf8ToPath(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".tif" || ext == ".tiff") format = RawFormat::Tiff;
    else if (ext == ".npy") format = RawFormat::Npy;
    else return false;
    return true;
}

bool writeRawImage(const std::string& path, const LoadedImage& image, const RawSaveOptions& options, std::string* error)
{
    if (!hasRawSamples(image)) return setError(error, "This image has no raw sample data.");
    if constexpr (std::endian::native != std::endian::little)
        return setError(error, "Saving raw data is only supported on little-endian machines.");

    const int firstSlice = options.allSlices ? 0 : std::clamp(options.slice, 0, image.depth - 1);
    const int sliceCount = options.allSlices ? image.depth : 1;

    FilePtr f(fopenUtf8(path, "wb"));
    if (!f) return setError(error, "Cannot open " + path + " for writing.");

    bool ok = options.format == RawFormat::Tiff
                  ? writeTiff(f.get(), image, firstSlice, sliceCount, error)
                  : writeNpy(f.get(), image, firstSlice, sliceCount, options.allSlices && image.depth > 1, error);
    if (ok && std::fflush(f.get()) != 0) ok = setError(error, "Write failed (disk full?)");
    f.reset();
    if (!ok)
    {
        std::error_code ec;
        std::filesystem::remove(utf8ToPath(path), ec);
    }
    return ok;
}

} // namespace giv
