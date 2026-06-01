// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_color.hpp>

#include <array>
#include <cctype>
#include <string>
#include <utility>

namespace cadml::engine {

namespace {

constexpr std::uint32_t pack(int r, int g, int b) {
    return static_cast<std::uint32_t>(r)
         | (static_cast<std::uint32_t>(g) << 8)
         | (static_cast<std::uint32_t>(b) << 16)
         | (0xFFu << 24);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Static rather than constexpr because std::string isn't a
// constexpr-friendly value type pre-C++20-extended-constexpr in
// MSVC. Linear scan: 19 entries, called once per part — not hot.
struct NamedColor { const char* name; std::uint32_t rgba; };
const std::array<NamedColor, 20> kNamedColors{{
    { "aqua",    pack(0x00, 0xFF, 0xFF) },
    { "black",   pack(0x00, 0x00, 0x00) },
    { "blue",    pack(0x00, 0x00, 0xFF) },
    { "cyan",    pack(0x00, 0xFF, 0xFF) },   // alias of aqua
    { "fuchsia", pack(0xFF, 0x00, 0xFF) },
    { "gray",    pack(0x80, 0x80, 0x80) },
    { "green",   pack(0x00, 0x80, 0x00) },
    { "grey",    pack(0x80, 0x80, 0x80) },   // alias of gray
    { "lime",    pack(0x00, 0xFF, 0x00) },
    { "magenta", pack(0xFF, 0x00, 0xFF) },   // alias of fuchsia
    { "maroon",  pack(0x80, 0x00, 0x00) },
    { "navy",    pack(0x00, 0x00, 0x80) },
    { "olive",   pack(0x80, 0x80, 0x00) },
    { "orange",  pack(0xFF, 0xA5, 0x00) },
    { "purple",  pack(0x80, 0x00, 0x80) },
    { "red",     pack(0xFF, 0x00, 0x00) },
    { "silver",  pack(0xC0, 0xC0, 0xC0) },
    { "teal",    pack(0x00, 0x80, 0x80) },
    { "white",   pack(0xFF, 0xFF, 0xFF) },
    { "yellow",  pack(0xFF, 0xFF, 0x00) },
}};

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

}  // namespace

std::uint32_t parse_color_rgba(std::string_view s) {
    if (s.empty()) return 0;

    if (s.front() == '#') {
        s.remove_prefix(1);
        int r = -1, g = -1, b = -1;
        if (s.size() == 3) {
            r = hex_digit(s[0]);
            g = hex_digit(s[1]);
            b = hex_digit(s[2]);
            if (r < 0 || g < 0 || b < 0) return 0;
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;
        } else if (s.size() == 6) {
            const int rh = hex_digit(s[0]);
            const int rl = hex_digit(s[1]);
            const int gh = hex_digit(s[2]);
            const int gl = hex_digit(s[3]);
            const int bh = hex_digit(s[4]);
            const int bl = hex_digit(s[5]);
            if (rh < 0 || rl < 0 || gh < 0 || gl < 0 || bh < 0 || bl < 0)
                return 0;
            r = (rh << 4) | rl;
            g = (gh << 4) | gl;
            b = (bh << 4) | bl;
        } else {
            return 0;
        }
        return pack(r, g, b);
    }

    for (const auto& nc : kNamedColors) {
        if (ieq(s, nc.name)) return nc.rgba;
    }
    return 0;
}

}  // namespace cadml::engine
