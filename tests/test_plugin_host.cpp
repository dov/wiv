// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ImagePluginHost.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
// Writes a little-endian version 1.0 .npy file of `count` elements of `descr` ('<f4', '<u2', ...).
std::string writeNpy(const std::string& name, const char* descr, size_t w, size_t h, const void* data, size_t bytes)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::string header = std::string("{'descr': '") + descr + "', 'fortran_order': False, 'shape': (" + std::to_string(h) + ", " +
                         std::to_string(w) + "), }";
    while ((10 + header.size() + 1) % 64) header += ' ';
    header += '\n';
    std::ofstream f(path, std::ios::binary);
    f.write("\x93NUMPY\x01\x00", 8);
    const uint16_t len = static_cast<uint16_t>(header.size());
    f.write(reinterpret_cast<const char*>(&len), 2);
    f.write(header.data(), static_cast<std::streamsize>(header.size()));
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return path.string();
}

class PluginHostTest : public testing::Test
{
protected:
    void SetUp() override { setenv("WIV_PLUGIN_DIR", WIV_TEST_PLUGIN_DIR, 1); }
};
} // namespace

// The min/max scan is split across threads (and ignores NaN): it must still find the extremes, wherever they sit.
TEST_F(PluginHostTest, FloatMinMaxIgnoresNaNAndSpansThreads)
{
    const size_t w = 1500, h = 1400; // over a million samples, so the scan is threaded
    std::vector<float> v(w * h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(i % 1000) * 0.01f;
    v[3] = std::numeric_limits<float>::quiet_NaN();
    v[v.size() - 1] = 123.5f;  // maximum, in the last thread's part
    v[v.size() / 3 + 5] = -42.25f; // minimum, in a middle part
    const std::string path = writeNpy("wiv_test_minmax.npy", "<f4", w, h, v.data(), v.size() * sizeof(float));

    auto img = giv::ImagePluginHost::load(path);
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->width, static_cast<int>(w));
    EXPECT_EQ(img->height, static_cast<int>(h));
    EXPECT_FLOAT_EQ(img->sampleMin, -42.25f);
    EXPECT_FLOAT_EQ(img->sampleMax, 123.5f);
    std::filesystem::remove(path);
}

TEST_F(PluginHostTest, Uint16MinMax)
{
    const size_t w = 1300, h = 1300;
    std::vector<uint16_t> v(w * h, 1000);
    v[7] = 3;
    v[v.size() - 2] = 65535;
    const std::string path = writeNpy("wiv_test_minmax16.npy", "<u2", w, h, v.data(), v.size() * 2);

    auto img = giv::ImagePluginHost::load(path);
    ASSERT_TRUE(img.has_value());
    EXPECT_FLOAT_EQ(img->sampleMin, 3.0f);
    EXPECT_FLOAT_EQ(img->sampleMax, 65535.0f);
    std::filesystem::remove(path);
}

// The host keeps the plugin's own buffers (no copy): their contents must be intact, survive copies of the image,
// and be released cleanly when the last copy goes.
TEST_F(PluginHostTest, AdoptedPluginBuffersStayValid)
{
    const size_t w = 64, h = 48;
    std::vector<uint16_t> v(w * h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint16_t>(i * 7);
    const std::string path = writeNpy("wiv_test_adopt.npy", "<u2", w, h, v.data(), v.size() * 2);

    giv::LoadedImage copy;
    {
        auto img = giv::ImagePluginHost::load(path);
        ASSERT_TRUE(img.has_value());
        ASSERT_EQ(img->samples.size(), v.size() * 2);
        copy = *img; // shares the buffers
    }
    ASSERT_EQ(copy.samples.size(), v.size() * 2);
    EXPECT_EQ(std::memcmp(copy.samples.data(), v.data(), v.size() * 2), 0);
    EXPECT_EQ(copy.rgba.size(), w * h * 4);
    std::filesystem::remove(path);
}
