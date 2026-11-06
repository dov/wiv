// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "GivParser.h"

#include <gtest/gtest.h>

using namespace giv;

namespace
{
SceneData parse(const std::string& text)
{
    SceneData scene;
    std::string err;
    GivParser p;
    EXPECT_TRUE(p.parseString(text, scene, err)) << err;
    return scene;
}
} // namespace

TEST(GivParser, EmptyInputHasNoDatasetsAndNoBounds)
{
    SceneData s = parse("");
    EXPECT_FALSE(s.hasBounds());
}

TEST(GivParser, PolylinePointsAndBounds)
{
    SceneData s = parse("$color red\n0 0\n10 0\n10 20\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    const Dataset& d = s.datasets[0];
    ASSERT_EQ(d.pointCount(), 3u);
    EXPECT_EQ(d.op[0], Op::Move);
    EXPECT_EQ(d.op[1], Op::Draw);
    EXPECT_FLOAT_EQ(d.x[2], 10.0f);
    EXPECT_FLOAT_EQ(d.y[2], 20.0f);
    EXPECT_FLOAT_EQ(d.color.r, 1.0f);
    ASSERT_TRUE(s.hasBounds());
    EXPECT_DOUBLE_EQ(s.minX, 0.0);
    EXPECT_DOUBLE_EQ(s.maxX, 10.0);
    EXPECT_DOUBLE_EQ(s.minY, 0.0);
    EXPECT_DOUBLE_EQ(s.maxY, 20.0);
}

TEST(GivParser, MoveCommandStartsNewSubpath)
{
    SceneData s = parse("0 0\n1 1\nm 5 5\n6 6\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    const Dataset& d = s.datasets[0];
    ASSERT_EQ(d.pointCount(), 4u);
    EXPECT_EQ(d.op[2], Op::Move);
    EXPECT_EQ(d.op[3], Op::Draw);
}

TEST(GivParser, DirectivesAreCaseInsensitive)
{
    // example.giv uses $mark_SIZE, $LW and $PATH.
    SceneData s = parse("$PATH Foo\n$LW 5\n$mark_SIZE 12\n1 1\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    EXPECT_EQ(s.datasets[0].pathName, "Foo");
    EXPECT_DOUBLE_EQ(s.datasets[0].lineWidth, 5.0);
    EXPECT_DOUBLE_EQ(s.datasets[0].markSize, 12.0);
}

TEST(GivParser, MarkTypes)
{
    struct Case { const char* name; MarkType t; } cases[] = {
        {"fcircle", MarkType::FCircle}, {"fsquare", MarkType::FSquare},
        {"circle", MarkType::Circle},   {"square", MarkType::Square},
        {"pixel", MarkType::Pixel},
    };
    for (auto& c : cases)
    {
        SceneData s = parse(std::string("$marks ") + c.name + "\n1 1\n");
        ASSERT_EQ(s.datasets.size(), 1u) << c.name;
        EXPECT_EQ(s.datasets[0].markType, c.t) << c.name;
        EXPECT_TRUE(s.datasets[0].doDrawMarks) << c.name;
    }
}

TEST(GivParser, NolineAndLineToggle)
{
    SceneData s = parse("$noline\n1 1\n2 2\n");
    EXPECT_FALSE(s.datasets[0].doDrawLines);
    s = parse("$noline\n$line\n1 1\n2 2\n");
    EXPECT_TRUE(s.datasets[0].doDrawLines);
}

TEST(GivParser, HideAndBalloon)
{
    SceneData s = parse("$hide\n$balloon hello there\n1 1\n2 2\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    EXPECT_FALSE(s.datasets[0].isVisible);
    EXPECT_EQ(s.datasets[0].balloon, "hello there");
}

TEST(GivParser, BlankLineStartsANewDataset)
{
    // As in giv, a blank line ends the current dataset; $path only names it.
    SceneData s = parse("$path a\n0 0\n1 1\n\n$path b\n2 2\n3 3\n");
    ASSERT_EQ(s.datasets.size(), 2u);
    EXPECT_EQ(s.datasets[0].pathName, "a");
    EXPECT_EQ(s.datasets[1].pathName, "b");
}

TEST(GivParser, PixelsizeBackgroundImage)
{
    SceneData s = parse("$pixelsize 0.5 mm\n$background navy\n$image foo.png\n0 0\n1 1\n");
    ASSERT_TRUE(s.pixelSize.has_value());
    EXPECT_DOUBLE_EQ(*s.pixelSize, 0.5);
    EXPECT_EQ(s.pixelSizeUnit, "mm");
    ASSERT_TRUE(s.background.has_value());
    EXPECT_NEAR(s.background->b, 128.0f / 255.0f, 0.01f);
    ASSERT_EQ(s.images.size(), 1u);
    EXPECT_EQ(s.images[0], "foo.png");
}

TEST(GivParser, PixelsizeAbsentMeansNullopt)
{
    SceneData s = parse("0 0\n1 1\n");
    EXPECT_FALSE(s.pixelSize.has_value());
    EXPECT_FALSE(s.background.has_value());
}

TEST(GivParser, FramesFpsLoop)
{
    SceneData s = parse("$fps 10\n$loop 3\n$color red\n0 0\n1 1\n$frame\n$color blue\n2 2\n3 3\n$frame\n4 4\n5 5\n");
    EXPECT_EQ(s.frameCount, 2);
    ASSERT_TRUE(s.fps.has_value());
    EXPECT_DOUBLE_EQ(*s.fps, 10.0);
    EXPECT_EQ(s.loopCount, 3);
    // The dataset before the first $frame is the static background.
    EXPECT_EQ(s.datasets.front().frame, -1);
    EXPECT_EQ(s.datasets.back().frame, 1);
}

TEST(GivParser, DefStyleAndStyle)
{
    SceneData s = parse("$def_style hot color red\n$def_style hot lw 4\n$style hot\n0 0\n1 1\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    EXPECT_FLOAT_EQ(s.datasets[0].color.r, 1.0f);
    EXPECT_DOUBLE_EQ(s.datasets[0].lineWidth, 4.0);
}

TEST(GivParser, TextMarkCreatesTextItem)
{
    SceneData s = parse("$color black\nt 5 5 Hello world\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    ASSERT_EQ(s.datasets[0].texts.size(), 1u);
    EXPECT_EQ(s.datasets[0].texts[0].text, "Hello world");
}

TEST(GivParser, PolygonFlag)
{
    SceneData s = parse("$polygon\n0 0\n1 0\n1 1\n");
    EXPECT_TRUE(s.datasets[0].doDrawPolygon);
}

TEST(GivParser, DashList)
{
    SceneData s = parse("$linedash 4 2\n0 0\n10 0\n");
    EXPECT_EQ(s.datasets[0].dashes.size(), 2u);
}

TEST(GivParser, CrLfLineEndings)
{
    SceneData s = parse("$color red\r\n0 0\r\n3 4\r\n");
    ASSERT_EQ(s.datasets.size(), 1u);
    EXPECT_EQ(s.datasets[0].pointCount(), 2u);
    EXPECT_FLOAT_EQ(s.datasets[0].y[1], 4.0f);
}

TEST(GivParser, ParseFileMissingFails)
{
    SceneData s;
    std::string err;
    GivParser p;
    EXPECT_FALSE(p.parseFile("/nonexistent/dir/nothing.giv", s, err));
    EXPECT_FALSE(err.empty());
}
