// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_color.hpp>

#include <gtest/gtest.h>

#include <cstdint>

using cadml::engine::parse_color_rgba;

namespace {

// Helper to make expected values readable. Mirrors the packing
// convention in flat_color.cpp (low byte = R, then G, B, A=0xFF).
constexpr std::uint32_t pack(int r, int g, int b) {
    return static_cast<std::uint32_t>(r)
         | (static_cast<std::uint32_t>(g) << 8)
         | (static_cast<std::uint32_t>(b) << 16)
         | (0xFFu << 24);
}

}  // namespace

TEST(FlatColor, EmptyStringYieldsZero) {
    EXPECT_EQ(parse_color_rgba(""), 0u);
}

TEST(FlatColor, HexLong) {
    EXPECT_EQ(parse_color_rgba("#FF0000"), pack(0xFF, 0x00, 0x00));
    EXPECT_EQ(parse_color_rgba("#00FF00"), pack(0x00, 0xFF, 0x00));
    EXPECT_EQ(parse_color_rgba("#0000FF"), pack(0x00, 0x00, 0xFF));
    EXPECT_EQ(parse_color_rgba("#3a7bd5"), pack(0x3A, 0x7B, 0xD5));
}

TEST(FlatColor, HexLongCaseInsensitive) {
    EXPECT_EQ(parse_color_rgba("#aabbcc"), pack(0xAA, 0xBB, 0xCC));
    EXPECT_EQ(parse_color_rgba("#AABBCC"), pack(0xAA, 0xBB, 0xCC));
    EXPECT_EQ(parse_color_rgba("#AaBbCc"), pack(0xAA, 0xBB, 0xCC));
}

TEST(FlatColor, HexShortDoublesNibbles) {
    // `#abc` expands to `#aabbcc`.
    EXPECT_EQ(parse_color_rgba("#abc"), pack(0xAA, 0xBB, 0xCC));
    EXPECT_EQ(parse_color_rgba("#f00"), pack(0xFF, 0x00, 0x00));
    EXPECT_EQ(parse_color_rgba("#0f0"), pack(0x00, 0xFF, 0x00));
    EXPECT_EQ(parse_color_rgba("#00f"), pack(0x00, 0x00, 0xFF));
}

TEST(FlatColor, HexInvalidYieldsZero) {
    // Wrong length.
    EXPECT_EQ(parse_color_rgba("#"),       0u);
    EXPECT_EQ(parse_color_rgba("#1"),      0u);
    EXPECT_EQ(parse_color_rgba("#12"),     0u);
    EXPECT_EQ(parse_color_rgba("#1234"),   0u);
    EXPECT_EQ(parse_color_rgba("#12345"),  0u);
    EXPECT_EQ(parse_color_rgba("#1234567"), 0u);
    // Non-hex digits.
    EXPECT_EQ(parse_color_rgba("#zzz"),     0u);
    EXPECT_EQ(parse_color_rgba("#GGGGGG"),  0u);
    EXPECT_EQ(parse_color_rgba("#ff00gg"),  0u);
}

TEST(FlatColor, AlphaForcedToFFOnAnyValidParse) {
    // The shader's "no override" sentinel is 0; any successful
    // parse must set alpha = 0xFF so the override is recognised.
    // Even pure black (#000000) needs alpha = 0xFF.
    EXPECT_EQ(parse_color_rgba("#000000") >> 24, 0xFFu);
    EXPECT_EQ(parse_color_rgba("#000")    >> 24, 0xFFu);
    EXPECT_EQ(parse_color_rgba("black")   >> 24, 0xFFu);
}

TEST(FlatColor, NamedCssLevel1) {
    EXPECT_EQ(parse_color_rgba("aqua"),    pack(0x00, 0xFF, 0xFF));
    EXPECT_EQ(parse_color_rgba("black"),   pack(0x00, 0x00, 0x00));
    EXPECT_EQ(parse_color_rgba("blue"),    pack(0x00, 0x00, 0xFF));
    EXPECT_EQ(parse_color_rgba("fuchsia"), pack(0xFF, 0x00, 0xFF));
    EXPECT_EQ(parse_color_rgba("gray"),    pack(0x80, 0x80, 0x80));
    EXPECT_EQ(parse_color_rgba("green"),   pack(0x00, 0x80, 0x00));
    EXPECT_EQ(parse_color_rgba("lime"),    pack(0x00, 0xFF, 0x00));
    EXPECT_EQ(parse_color_rgba("maroon"),  pack(0x80, 0x00, 0x00));
    EXPECT_EQ(parse_color_rgba("navy"),    pack(0x00, 0x00, 0x80));
    EXPECT_EQ(parse_color_rgba("olive"),   pack(0x80, 0x80, 0x00));
    EXPECT_EQ(parse_color_rgba("purple"),  pack(0x80, 0x00, 0x80));
    EXPECT_EQ(parse_color_rgba("red"),     pack(0xFF, 0x00, 0x00));
    EXPECT_EQ(parse_color_rgba("silver"),  pack(0xC0, 0xC0, 0xC0));
    EXPECT_EQ(parse_color_rgba("teal"),    pack(0x00, 0x80, 0x80));
    EXPECT_EQ(parse_color_rgba("white"),   pack(0xFF, 0xFF, 0xFF));
    EXPECT_EQ(parse_color_rgba("yellow"),  pack(0xFF, 0xFF, 0x00));
}

TEST(FlatColor, NamedAliases) {
    // cyan == aqua, magenta == fuchsia, grey == gray.
    EXPECT_EQ(parse_color_rgba("cyan"),    parse_color_rgba("aqua"));
    EXPECT_EQ(parse_color_rgba("magenta"), parse_color_rgba("fuchsia"));
    EXPECT_EQ(parse_color_rgba("grey"),    parse_color_rgba("gray"));
}

TEST(FlatColor, NamedExtras) {
    // CSS-Level-3 extra we include in the table.
    EXPECT_EQ(parse_color_rgba("orange"), pack(0xFF, 0xA5, 0x00));
}

TEST(FlatColor, NamedCaseInsensitive) {
    const auto red = parse_color_rgba("red");
    EXPECT_EQ(parse_color_rgba("RED"), red);
    EXPECT_EQ(parse_color_rgba("Red"), red);
    EXPECT_EQ(parse_color_rgba("rEd"), red);
}

TEST(FlatColor, UnknownNameYieldsZero) {
    // Graceful: an unrecognised name is treated as "no override",
    // so the shader falls back to its hash palette instead of
    // hard-erroring.
    EXPECT_EQ(parse_color_rgba("chartreuse"),  0u);
    EXPECT_EQ(parse_color_rgba("not-a-color"), 0u);
    EXPECT_EQ(parse_color_rgba("#"),            0u);
}

TEST(FlatColor, NoLeadingHashIsTreatedAsName) {
    // A bare hex string without `#` is NOT a hex color — it's
    // a name lookup. "ff0000" is not in our table → 0.
    EXPECT_EQ(parse_color_rgba("ff0000"), 0u);
}
