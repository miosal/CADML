// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "flat_stl_import.hpp"

#include <cadml/types.hpp>   // parse_double_strict

#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

// Binary STL stores little-endian IEEE-754 floats and a little-endian
// uint32 count. The reader below memcpy's raw bytes, which only matches
// the spec on a little-endian host — same assumption (and rationale) as
// the STL writer in flat_stl.cpp. Fail the build loudly on the rare
// big-endian target rather than silently mis-reading every coordinate.
static_assert(std::endian::native == std::endian::little,
              "flat_stl_import assumes a little-endian host; a big-endian "
              "target needs a byte-swap on every f32/u32 read.");

namespace cadml::engine::detail {

namespace {

float read_f32(const unsigned char* p) {
    float f;
    std::memcpy(&f, p, sizeof f);
    return f;
}

std::uint32_t read_u32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void push_vertex(FlatMesh& m, Vec3 v) {
    m.indices.push_back(static_cast<std::uint32_t>(m.vertices.size()));
    m.vertices.push_back(v);
}

// Binary layout: 80-byte header, uint32 triangle count, then per triangle
// 12 floats (normal + 3 vertices) and a 2-byte attribute. `ntri` and the
// blob length were already cross-checked by the caller, so every read here
// is in bounds.
StlParse parse_binary(std::string_view bytes, std::uint32_t ntri) {
    StlParse r;
    const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
    r.mesh.vertices.reserve(static_cast<std::size_t>(ntri) * 3);
    r.mesh.indices.reserve(static_cast<std::size_t>(ntri) * 3);

    std::size_t off = 84;               // past header + count
    for (std::uint32_t t = 0; t < ntri; ++t) {
        off += 12;                      // skip the per-facet normal
        for (int v = 0; v < 3; ++v) {
            const Vec3 p{ read_f32(u + off),
                          read_f32(u + off + 4),
                          read_f32(u + off + 8) };
            // NaN/Inf is not recoverable geometry — hard error, matching
            // the ASCII path (parse_double_strict rejects non-finite), so
            // it can never reach exported meshes via the best-effort path.
            if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                !std::isfinite(p.z)) {
                r.error = "binary STL: non-finite vertex coordinate";
                return r;
            }
            push_vertex(r.mesh, p);
            off += 12;
        }
        off += 2;                       // attribute byte count
    }

    if (r.mesh.indices.empty()) {
        r.error = "binary STL contains no triangles";
        return r;
    }
    r.ok = true;
    return r;
}

// ASCII grammar is `facet .. outer loop vertex x y z (x3) endloop endfacet`
// wrapped in `solid`/`endsolid`. We just harvest every `vertex x y z`
// triple in order — three consecutive vertices form one triangle — which
// tolerates whitespace/normal variations without a full tokenizer.
StlParse parse_ascii(std::string_view s) {
    StlParse r;
    std::size_t i = 0;
    auto next_token = [&]() -> std::string_view {
        while (i < s.size() &&
               std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() &&
               !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(start, i - start);
    };

    while (i < s.size()) {
        const std::string_view tok = next_token();
        if (tok.empty()) break;
        if (tok != "vertex") continue;

        double xyz[3];
        for (double& c : xyz) {
            const auto v = parse_double_strict(next_token());
            if (!v) {
                r.error = "ASCII STL: malformed vertex coordinate";
                return r;
            }
            c = *v;
        }
        push_vertex(r.mesh, { xyz[0], xyz[1], xyz[2] });
    }

    if (r.mesh.vertices.empty()) {
        r.error = "ASCII STL: no vertices found";
        return r;
    }
    if (r.mesh.vertices.size() % 3 != 0) {
        r.error = "ASCII STL: vertex count (" +
                  std::to_string(r.mesh.vertices.size()) +
                  ") is not a multiple of 3";
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace

StlParse parse_stl(std::string_view bytes) {
    // Binary detection: the layout is fully determined by the triangle
    // count, so a length-exact match is the robust discriminator (some
    // binary files start with the ASCII keyword "solid"). 64-bit math
    // avoids overflow on a hostile count field.
    if (bytes.size() >= 84) {
        const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
        const std::uint32_t ntri = read_u32(u + 80);
        if (84ull + static_cast<std::uint64_t>(ntri) * 50ull == bytes.size()) {
            return parse_binary(bytes, ntri);
        }
    }
    return parse_ascii(bytes);
}

}  // namespace cadml::engine::detail
