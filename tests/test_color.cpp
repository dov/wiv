// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "GivColor.h"

#include <gtest/gtest.h>

using giv::Color;
using giv::parseGivColor;

TEST(GivColor, NamedColor)
{
    Color c;
    ASSERT_TRUE(parseGivColor("red", c));
    EXPECT_FLOAT_EQ(c.r, 1.0f);
    EXPECT_FLOAT_EQ(c.g, 0.0f);
    EXPECT_FLOAT_EQ(c.b, 0.0f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
    EXPECT_FALSE(c.isNone);
}

TEST(GivColor, HexLongAndShort)
{
    Color c;
    ASSERT_TRUE(parseGivColor("#00ff00", c));
    EXPECT_FLOAT_EQ(c.g, 1.0f);
    EXPECT_FLOAT_EQ(c.r, 0.0f);
    ASSERT_TRUE(parseGivColor("#00f", c));
    EXPECT_FLOAT_EQ(c.b, 1.0f);
    EXPECT_FLOAT_EQ(c.r, 0.0f);
}

TEST(GivColor, NoneIsRecognized)
{
    Color c;
    ASSERT_TRUE(parseGivColor("none", c));
    EXPECT_TRUE(c.isNone);
}

TEST(GivColor, AlphaFloat)
{
    Color c;
    ASSERT_TRUE(parseGivColor("blue/0.5", c));
    EXPECT_FLOAT_EQ(c.b, 1.0f);
    EXPECT_NEAR(c.a, 0.5f, 1e-4);
}

TEST(GivColor, AlphaHexByte)
{
    Color c;
    ASSERT_TRUE(parseGivColor("blue/0x80", c));
    EXPECT_NEAR(c.a, 128.0f / 255.0f, 1e-3);
    ASSERT_TRUE(parseGivColor("blue/0xff", c));
    EXPECT_NEAR(c.a, 1.0f, 1e-4);
}

TEST(GivColor, UnknownNameFailsAndLeavesOutUnchanged)
{
    Color c = Color::opaque(0.25f, 0.5f, 0.75f, 0.1f);
    EXPECT_FALSE(parseGivColor("definitely-not-a-color", c));
    EXPECT_FLOAT_EQ(c.r, 0.25f);
    EXPECT_FLOAT_EQ(c.g, 0.5f);
    EXPECT_FLOAT_EQ(c.b, 0.75f);
    EXPECT_FLOAT_EQ(c.a, 0.1f);
}
