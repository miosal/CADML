// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>     // Vec3

#include <string>
#include <string_view>

namespace cadml {

enum class SelectorScope { All, Edge, Face };

enum class SelectorField {
    // edge-scope fields (§13.2)
    Along, Dihedral, EdgePosX, EdgePosY, EdgePosZ, Length,
    // face-scope fields (§13.3)
    Normal, FacePosX, FacePosY, FacePosZ, Area,
};

enum class SelectorCmp { Eq, Gt, Lt, Ge, Le };

// Numeric equality tolerance for `=` on numeric fields (spec §13.2).
inline constexpr double kSelectorNumberEqTol = 1e-6;
// Angle tolerance (degrees) for `face:normal=` and `edge:along=`
// (spec §13.3 specifies 1° for normal; we reuse it for axis alignment).
inline constexpr double kSelectorAngleTolDeg = 1.0;

// Geometric properties of one edge, supplied by the caller.
struct EdgeProps {
    Vec3   direction{};          // unit vector along the edge (sign-agnostic)
    double dihedral_deg = 0.0;   // interior dihedral angle, degrees
    Vec3   midpoint{};           // edge midpoint
    double length = 0.0;
};

// Geometric properties of one face, supplied by the caller.
struct FaceProps {
    Vec3   normal{};             // unit outward normal
    Vec3   centroid{};
    double area = 0.0;
};

struct Selector {
    SelectorScope scope = SelectorScope::All;
    SelectorField field = SelectorField::Along;
    SelectorCmp   cmp   = SelectorCmp::Eq;
    bool          value_is_vector = false;
    double        number = 0.0;
    Vec3          vector{};

    bool is_all()  const { return scope == SelectorScope::All; }
    bool is_edge() const { return scope == SelectorScope::Edge; }
    bool is_face() const { return scope == SelectorScope::Face; }

    // Match predicates. An `all` selector matches everything. A selector
    // whose scope does not match the overload (e.g. a face selector
    // tested against an EdgeProps) returns false.
    bool matches(const EdgeProps& e) const;
    bool matches(const FaceProps& f) const;
};

struct SelectorParseResult {
    bool        ok = false;
    Selector    selector{};
    std::string error;     // human-readable; populated iff !ok
};

// Parse a §13 selector string. An empty string or "all" yields the
// All selector. Returns ok=false with a specific message on any
// malformation (unknown scope/field, illegal comparator for the field,
// or a value that doesn't parse for the field's type).
SelectorParseResult parse_selector(std::string_view spec);

}  // namespace cadml
