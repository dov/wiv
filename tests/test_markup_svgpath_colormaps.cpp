// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "Colormaps.h"
#include "SvgPath.h"
#include "TextMarkup.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace giv;

// ---- Pango markup ---------------------------------------------------------

TEST(TextMarkup, PlainTextIsOneSpan)
{
    std::vector<MarkupSpan> spans;
    ASSERT_TRUE(parsePangoMarkup("hello", spans));
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].text, "hello");
    EXPECT_FALSE(spans[0].style.italic.has_value());
}

TEST(TextMarkup, BoldAndItalicNest)
{
    std::vector<MarkupSpan> spans;
    ASSERT_TRUE(parsePangoMarkup("a<b>b<i>c</i></b>", spans));
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].text, "a");
    EXPECT_EQ(spans[1].text, "b");
    ASSERT_TRUE(spans[1].style.weight.has_value());
    EXPECT_GE(*spans[1].style.weight, 700);
    EXPECT_EQ(spans[2].text, "c");
    ASSERT_TRUE(spans[2].style.italic.has_value());
    EXPECT_TRUE(*spans[2].style.italic);
    EXPECT_GE(*spans[2].style.weight, 700);
}

TEST(TextMarkup, SpanForegroundColor)
{
    std::vector<MarkupSpan> spans;
    ASSERT_TRUE(parsePangoMarkup("<span foreground=\"red\">x</span>", spans));
    ASSERT_EQ(spans.size(), 1u);
    ASSERT_TRUE(spans[0].style.fg.has_value());
    EXPECT_FLOAT_EQ(spans[0].style.fg->r, 1.0f);
    EXPECT_FLOAT_EQ(spans[0].style.fg->g, 0.0f);
}

TEST(TextMarkup, EntitiesAreDecoded)
{
    std::vector<MarkupSpan> spans;
    ASSERT_TRUE(parsePangoMarkup("a &amp; b &lt;c&gt; &#65;&#x42;", spans));
    std::string all;
    for (auto& s : spans) all += s.text;
    EXPECT_EQ(all, "a & b <c> AB");
}

TEST(TextMarkup, UnbalancedTagsFail)
{
    std::vector<MarkupSpan> spans;
    std::string err;
    EXPECT_FALSE(parsePangoMarkup("<b>unclosed", spans, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(parsePangoMarkup("</b>", spans));
    EXPECT_FALSE(parsePangoMarkup("<b>x</i>", spans));
}

TEST(TextMarkup, UnknownTagAndBadEntityFail)
{
    std::vector<MarkupSpan> spans;
    EXPECT_FALSE(parsePangoMarkup("<bogus>x</bogus>", spans));
    EXPECT_FALSE(parsePangoMarkup("&notanentity;", spans));
}

// ---- SVG path flattening --------------------------------------------------

TEST(SvgPath, StraightLineLength)
{
    MotionPath p = flattenPath("M 0 0 L 3 4");
    EXPECT_NEAR(p.total(), 5.0, 1e-9);
}

TEST(SvgPath, RelativeAndClosedSquare)
{
    MotionPath p = flattenPath("M 0 0 h 10 v 10 h -10 z");
    EXPECT_NEAR(p.total(), 40.0, 1e-6);
}

TEST(SvgPath, PositionAndAngleAlongLine)
{
    MotionPath p = flattenPath("M 0 0 L 10 0");
    double x, y, a;
    p.at(0.5, x, y, a);
    EXPECT_NEAR(x, 5.0, 1e-9);
    EXPECT_NEAR(y, 0.0, 1e-9);
    EXPECT_NEAR(a, 0.0, 1e-6);
    p.atDistance(1e9, x, y, a); // clamped to the end
    EXPECT_NEAR(x, 10.0, 1e-9);
}

TEST(SvgPath, CubicBezierIsSubdividedAndBounded)
{
    MotionPath p = flattenPath("M 0 0 C 0 10 10 10 10 0");
    EXPECT_GT(p.pts.size(), 4u);
    // Longer than the chord, shorter than the control polygon.
    EXPECT_GT(p.total(), 10.0);
    EXPECT_LT(p.total(), 30.0);
}

TEST(SvgPath, ArcHalfCircleLength)
{
    MotionPath p = flattenPath("M 0 0 A 5 5 0 0 1 10 0");
    EXPECT_NEAR(p.total(), 3.14159265358979323846 * 5.0, 0.05);
}

TEST(SvgPath, EmptyPath)
{
    MotionPath p = flattenPath("");
    EXPECT_EQ(p.total(), 0.0);
}

// ---- Colormaps ------------------------------------------------------------

TEST(Colormaps, EveryIdHasNameAndLut)
{
    for (int i = 0; i < static_cast<int>(colormaps::Id::Count); ++i)
    {
        auto id = static_cast<colormaps::Id>(i);
        ASSERT_NE(colormaps::name(id), nullptr);
        EXPECT_GT(std::string(colormaps::name(id)).size(), 0u);
        ASSERT_NE(colormaps::lut(id), nullptr) << i;
    }
}

TEST(Colormaps, NoneIsIdentityAndInvertReverses)
{
    const uint8_t* none = colormaps::lut(colormaps::Id::None);
    const uint8_t* inv = colormaps::lut(colormaps::Id::Invert);
    for (int i = 0; i < 256; ++i)
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_EQ(none[i * 3 + c], i);
            EXPECT_EQ(inv[i * 3 + c], 255 - i);
        }
}

TEST(Colormaps, AdaptiveFlags)
{
    EXPECT_TRUE(colormaps::isAdaptive(colormaps::Id::Equalize));
    EXPECT_TRUE(colormaps::isAdaptive(colormaps::Id::Normalize));
    EXPECT_FALSE(colormaps::isAdaptive(colormaps::Id::Rainbow));
}
