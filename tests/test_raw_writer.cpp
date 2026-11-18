// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ImagePluginHost.h"
#include "RawImageWriter.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
// Minimal independent classic-TIFF reader: walks the IFD chain and returns each page's
// geometry and the offset of its (single) strip, so float pages can be checked even though
// wiv's own TIFF plugin cannot read 32-bit samples.
struct TiffPage { uint32_t w = 0, h = 0, bits = 0, format = 1, offset = 0, bytes = 0; };
std::vector<TiffPage> walkTiff(const std::vector<uint8_t>& f)
{
    std::vector<TiffPage> pages;
    auto u16 = [&](size_t o) { return uint16_t(f.at(o) | f.at(o + 1) << 8); };
    auto u32 = [&](size_t o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
    if (f.size() < 8 || f[0] != 'I' || f[1] != 'I' || u16(2) != 42) return pages;
    for (uint32_t ifd = u32(4); ifd != 0 && pages.size() < 100; )
    {
        TiffPage p;
        const uint16_t n = u16(ifd);
        for (int i = 0; i < n; ++i)
        {
            const size_t e = ifd + 2 + i * 12;
            const uint16_t tag = u16(e), type = u16(e + 2);
            const uint32_t v = type == 3 ? u16(e + 8) : u32(e + 8);
            if (tag == 256) p.w = v;
            else if (tag == 257) p.h = v;
            else if (tag == 258) p.bits = v;
            else if (tag == 273) p.offset = v;
            else if (tag == 279) p.bytes = v;
            else if (tag == 339) p.format = v;
        }
        pages.push_back(p);
        ifd = u32(ifd + 2 + n * 12);
    }
    return pages;
}

std::vector<uint8_t> slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
class RawWriterTest : public testing::Test
{
protected:
    void SetUp() override { setenv("WIV_PLUGIN_DIR", WIV_TEST_PLUGIN_DIR, 1); }

    static std::string tmp(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

    template <class T>
    static giv::LoadedImage makeImage(WivSampleType type, int w, int h, int depth)
    {
        giv::LoadedImage img;
        img.width = w;
        img.height = h;
        img.depth = depth;
        img.sampleType = type;
        std::vector<T> v(size_t(w) * h * depth);
        for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<T>((i * 7 + 3) % (std::is_same_v<T, uint8_t> ? 250 : 60000));
        std::vector<uint8_t> bytes(v.size() * sizeof(T));
        std::memcpy(bytes.data(), v.data(), bytes.size());
        img.samples = std::move(bytes);
        img.rgba = std::vector<uint8_t>(size_t(w) * h * 4, 0);
        return img;
    }

    // The loaded image must hold exactly the samples of slice(s) first..first+count-1 of `src`.
    static void expectSameSamples(const giv::LoadedImage& src, const giv::LoadedImage& got, int first, int count)
    {
        ASSERT_EQ(got.width, src.width);
        ASSERT_EQ(got.height, src.height);
        ASSERT_EQ(got.sampleType, src.sampleType);
        ASSERT_EQ(got.depth, count);
        const size_t page = size_t(src.width) * src.height * (src.sampleType == WIV_SAMPLE_U8 ? 1 : src.sampleType == WIV_SAMPLE_U16 ? 2 : 4);
        ASSERT_GE(got.samples.size(), page * count);
        EXPECT_EQ(0, std::memcmp(got.samples.data(), src.samples.data() + page * first, page * count));
    }
};
} // namespace

TEST_F(RawWriterTest, FormatFromPath)
{
    giv::RawFormat f;
    EXPECT_TRUE(giv::rawFormatFromPath("/a/b.TIF", f));
    EXPECT_EQ(f, giv::RawFormat::Tiff);
    EXPECT_TRUE(giv::rawFormatFromPath("x.tiff", f));
    EXPECT_TRUE(giv::rawFormatFromPath("x.npy", f));
    EXPECT_EQ(f, giv::RawFormat::Npy);
    EXPECT_FALSE(giv::rawFormatFromPath("x.png", f));
}

TEST_F(RawWriterTest, NoRawSamplesIsRefused)
{
    giv::LoadedImage img;
    img.width = img.height = 4;
    img.rgba = std::vector<uint8_t>(64, 0);
    EXPECT_FALSE(giv::hasRawSamples(img));
    std::string err;
    EXPECT_FALSE(giv::writeRawImage(tmp("wiv_raw_none.tif"), img, {}, &err));
    EXPECT_FALSE(err.empty());
}

TEST_F(RawWriterTest, NpyRoundTripAllTypesAndSlices)
{
    for (WivSampleType type : {WIV_SAMPLE_U8, WIV_SAMPLE_U16, WIV_SAMPLE_FLOAT})
    {
        giv::LoadedImage src = type == WIV_SAMPLE_U8    ? makeImage<uint8_t>(type, 13, 7, 3)
                               : type == WIV_SAMPLE_U16 ? makeImage<uint16_t>(type, 13, 7, 3)
                                                        : makeImage<float>(type, 13, 7, 3);
        const std::string path = tmp("wiv_raw_rt.npy");
        // all slices
        ASSERT_TRUE(giv::writeRawImage(path, src, {giv::RawFormat::Npy, true, 0}));
        auto all = giv::ImagePluginHost::load(path);
        ASSERT_TRUE(all.has_value());
        expectSameSamples(src, *all, 0, 3);
        // one slice
        ASSERT_TRUE(giv::writeRawImage(path, src, {giv::RawFormat::Npy, false, 1}));
        auto one = giv::ImagePluginHost::load(path);
        ASSERT_TRUE(one.has_value());
        expectSameSamples(src, *one, 1, 1);
        std::filesystem::remove(path);
    }
}

TEST_F(RawWriterTest, TiffStructureAndSamplesAllTypes)
{
    for (WivSampleType type : {WIV_SAMPLE_U8, WIV_SAMPLE_U16, WIV_SAMPLE_FLOAT})
    {
        // Odd sizes exercise the word-alignment padding between pages.
        giv::LoadedImage src = type == WIV_SAMPLE_U8    ? makeImage<uint8_t>(type, 5, 3, 3)
                               : type == WIV_SAMPLE_U16 ? makeImage<uint16_t>(type, 5, 3, 3)
                                                        : makeImage<float>(type, 5, 3, 3);
        const size_t bps = type == WIV_SAMPLE_U8 ? 1 : type == WIV_SAMPLE_U16 ? 2 : 4;
        const size_t pageBytes = 5 * 3 * bps;
        const std::string path = tmp("wiv_raw_rt.tif");

        ASSERT_TRUE(giv::writeRawImage(path, src, {giv::RawFormat::Tiff, true, 0}));
        const auto file = slurp(path);
        const auto pages = walkTiff(file);
        ASSERT_EQ(pages.size(), 3u) << "type " << type;
        for (int s = 0; s < 3; ++s)
        {
            EXPECT_EQ(pages[s].w, 5u);
            EXPECT_EQ(pages[s].h, 3u);
            EXPECT_EQ(pages[s].bits, bps * 8);
            EXPECT_EQ(pages[s].format, type == WIV_SAMPLE_FLOAT ? 3u : 1u);
            ASSERT_EQ(pages[s].bytes, pageBytes);
            ASSERT_LE(pages[s].offset + pageBytes, file.size());
            EXPECT_EQ(0, std::memcmp(file.data() + pages[s].offset, src.samples.data() + s * pageBytes, pageBytes))
                << "type " << type << " page " << s;
        }

        // A single slice is a one-page file holding exactly that slice.
        ASSERT_TRUE(giv::writeRawImage(path, src, {giv::RawFormat::Tiff, false, 2}));
        const auto one = slurp(path);
        const auto onePages = walkTiff(one);
        ASSERT_EQ(onePages.size(), 1u);
        EXPECT_EQ(0, std::memcmp(one.data() + onePages[0].offset, src.samples.data() + 2 * pageBytes, pageBytes));

        // wiv's own reader handles the integer types (it has no float TIFF support).
        if (type != WIV_SAMPLE_FLOAT)
        {
            auto got = giv::ImagePluginHost::load(path);
            ASSERT_TRUE(got.has_value()) << "type " << type;
            expectSameSamples(src, *got, 2, 1);
            ASSERT_TRUE(giv::writeRawImage(path, src, {giv::RawFormat::Tiff, true, 0}));
            for (int page = 0; page < 3; ++page)
            {
                auto p = giv::ImagePluginHost::load(path, page);
                ASSERT_TRUE(p.has_value()) << "type " << type << " page " << page;
                EXPECT_EQ(p->pageCount, 3);
                expectSameSamples(src, *p, page, 1);
            }
        }
        std::filesystem::remove(path);
    }
}

TEST_F(RawWriterTest, FloatNaNAndExtremesSurvive)
{
    giv::LoadedImage src = makeImage<float>(WIV_SAMPLE_FLOAT, 4, 4, 1);
    float* v = reinterpret_cast<float*>(const_cast<uint8_t*>(src.samples.data()));
    v[0] = std::numeric_limits<float>::quiet_NaN();
    v[1] = -1e30f;
    v[2] = 1e-30f;
    const std::string path = tmp("wiv_raw_nan.npy");
    ASSERT_TRUE(giv::writeRawImage(path, src, {giv::RawFormat::Npy, false, 0}));
    auto got = giv::ImagePluginHost::load(path);
    ASSERT_TRUE(got.has_value());
    const float* g = reinterpret_cast<const float*>(got->samples.data());
    EXPECT_TRUE(std::isnan(g[0]));
    EXPECT_EQ(g[1], -1e30f);
    EXPECT_EQ(g[2], 1e-30f);
    std::filesystem::remove(path);
}

TEST_F(RawWriterTest, UnwritablePathFails)
{
    giv::LoadedImage src = makeImage<uint8_t>(WIV_SAMPLE_U8, 4, 4, 1);
    std::string err;
    EXPECT_FALSE(giv::writeRawImage("/nonexistent-dir/x.tif", src, {}, &err));
    EXPECT_FALSE(err.empty());
}
