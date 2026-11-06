// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
// Smoke test: every .giv / .svg shipped in examples/ must load without error
// and produce a scene with something in it.
#include "GivParser.h"

#include <gtest/gtest.h>

#include <filesystem>

#ifndef WIV_EXAMPLES_DIR
#error "WIV_EXAMPLES_DIR must be defined by the build"
#endif

namespace fs = std::filesystem;

TEST(Examples, EveryGivAndSvgLoads)
{
    int n = 0;
    for (const auto& e : fs::directory_iterator(WIV_EXAMPLES_DIR))
    {
        const auto ext = e.path().extension().string();
        if (ext != ".giv" && ext != ".svg") continue;
        SCOPED_TRACE(e.path().string());
        giv::SceneData scene;
        std::string err;
        giv::GivParser p;
        EXPECT_TRUE(p.parseFile(e.path().string(), scene, err)) << err;
        EXPECT_TRUE(!scene.datasets.empty() || !scene.images.empty());
        ++n;
    }
    EXPECT_GT(n, 20);
}

TEST(Examples, ExampleGivHasExpectedStructure)
{
    giv::SceneData scene;
    std::string err;
    giv::GivParser p;
    ASSERT_TRUE(p.parseFile(std::string(WIV_EXAMPLES_DIR) + "/example.giv", scene, err)) << err;
    ASSERT_GE(scene.datasets.size(), 2u);
    EXPECT_EQ(scene.datasets[0].pathName, "circles");
    EXPECT_EQ(scene.datasets[0].markType, giv::MarkType::Circle);
    EXPECT_DOUBLE_EQ(scene.datasets[0].lineWidth, 3.0);
    EXPECT_TRUE(scene.hasBounds());
}
