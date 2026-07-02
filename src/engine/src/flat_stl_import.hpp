// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>   // FlatMesh

#include <string>
#include <string_view>

namespace cadml::engine::detail {

// Result of parsing an STL blob into a raw triangle soup.
//   mesh  — positions only, three independent vertices per triangle with
//           sequential indices (normals/attribution left for the welder).
//   ok    — true when at least one triangle was read and the blob is
//           well-formed; false leaves `error` set and `mesh` best-effort.
struct StlParse {
    FlatMesh    mesh;
    bool        ok = false;
    std::string error;
};

// Parse a binary or ASCII STL blob. The format is auto-detected: a blob
// whose length matches the binary layout (80-byte header + uint32 count +
// count * 50 bytes) is read as binary, otherwise it is read as ASCII.
// Per-facet normals are ignored (recomputed downstream from geometry).
StlParse parse_stl(std::string_view bytes);

}  // namespace cadml::engine::detail
