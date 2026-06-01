// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/selector.hpp>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

namespace cadml {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Parse an axis alias: +x -x +y -y +z -z, or bare x/y/z (treated as +).
bool parse_axis_alias(std::string_view t, Vec3& out) {
    t = trim(t);
    double sign = 1.0;
    if (!t.empty() && (t.front() == '+' || t.front() == '-')) {
        if (t.front() == '-') sign = -1.0;
        t.remove_prefix(1);
    }
    if (t.size() != 1) return false;
    switch (t.front()) {
        case 'x': out = { sign, 0, 0 }; return true;
        case 'y': out = { 0, sign, 0 }; return true;
        case 'z': out = { 0, 0, sign }; return true;
        default:  return false;
    }
}

// Parse a single floating-point number that consumes the whole token.
bool parse_number(std::string_view t, double& out) {
    if (auto v = parse_double_strict(t)) {
        out = *v;
        return true;
    }
    return false;
}

// Parse "x,y,z" or "x y z" into a Vec3.
bool parse_vector3(std::string_view t, Vec3& out) {
    t = trim(t);
    // Tokenise on commas and/or whitespace.
    double comps[3];
    int n = 0;
    std::size_t i = 0;
    while (i < t.size() && n < 3) {
        while (i < t.size() &&
               (t[i] == ',' || std::isspace(static_cast<unsigned char>(t[i]))))
            ++i;
        const std::size_t start = i;
        while (i < t.size() && t[i] != ',' &&
               !std::isspace(static_cast<unsigned char>(t[i])))
            ++i;
        if (i == start) break;
        if (!parse_number(t.substr(start, i - start), comps[n])) return false;
        ++n;
    }
    // Reject trailing junk after three components.
    while (i < t.size() &&
           (t[i] == ',' || std::isspace(static_cast<unsigned char>(t[i]))))
        ++i;
    if (n != 3 || i != t.size()) return false;
    out = { comps[0], comps[1], comps[2] };
    return true;
}

bool cmp_num(double a, SelectorCmp cmp, double b) {
    // Scale-relative tolerance so `length=10` and `length=1e7` get
    // the same proportional slack instead of sharing a tiny absolute
    // tolerance that misses anything past a few significant figures.
    const double tol = std::max(kSelectorNumberEqTol,
                                  1e-6 * std::max(std::fabs(a),
                                                   std::fabs(b)));
    switch (cmp) {
        case SelectorCmp::Eq: return std::fabs(a - b) <= tol;
        case SelectorCmp::Ge: return a >= b - tol;
        case SelectorCmp::Le: return a <= b + tol;
        case SelectorCmp::Gt: return a >  b + tol;
        case SelectorCmp::Lt: return a <  b - tol;
    }
    return false;
}

SelectorParseResult fail(std::string msg) {
    SelectorParseResult r;
    r.ok = false;
    r.error = std::move(msg);
    return r;
}

}  // namespace

SelectorParseResult parse_selector(std::string_view spec) {
    const auto s = trim(spec);

    SelectorParseResult r;
    if (s.empty() || s == "all") {
        r.ok = true;
        r.selector.scope = SelectorScope::All;
        return r;
    }

    const auto colon = s.find(':');
    if (colon == std::string_view::npos) {
        return fail("selector `" + std::string(s) +
                    "` must be `all` or `<edge|face>:<field><cmp><value>`");
    }
    const auto scope_str = trim(s.substr(0, colon));
    const auto pred       = trim(s.substr(colon + 1));

    Selector sel;
    if (scope_str == "edge")      sel.scope = SelectorScope::Edge;
    else if (scope_str == "face") sel.scope = SelectorScope::Face;
    else {
        return fail("unknown selector scope `" + std::string(scope_str) +
                    "` (expected `edge` or `face`)");
    }

    // Locate the comparator: the first of <, >, = in the predicate.
    std::size_t ci = std::string_view::npos;
    for (std::size_t i = 0; i < pred.size(); ++i) {
        if (pred[i] == '<' || pred[i] == '>' || pred[i] == '=') { ci = i; break; }
    }
    if (ci == std::string_view::npos) {
        return fail("selector predicate `" + std::string(pred) +
                    "` has no comparator (expected one of =, >, <, >=, <=)");
    }

    SelectorCmp cmp;
    std::size_t val_start;
    if (pred[ci] == '=') { cmp = SelectorCmp::Eq; val_start = ci + 1; }
    else if (pred[ci] == '>') {
        if (ci + 1 < pred.size() && pred[ci + 1] == '=') { cmp = SelectorCmp::Ge; val_start = ci + 2; }
        else                                             { cmp = SelectorCmp::Gt; val_start = ci + 1; }
    } else { // '<'
        if (ci + 1 < pred.size() && pred[ci + 1] == '=') { cmp = SelectorCmp::Le; val_start = ci + 2; }
        else                                             { cmp = SelectorCmp::Lt; val_start = ci + 1; }
    }

    const auto field_str = trim(pred.substr(0, ci));
    const auto value_str = trim(pred.substr(val_start));
    if (field_str.empty())
        return fail("selector predicate is missing a field name");
    if (value_str.empty())
        return fail("selector predicate is missing a value");

    // Map field by scope.
    bool numeric_field = true;       // false for axis/vector fields
    if (sel.scope == SelectorScope::Edge) {
        if      (field_str == "along")      { sel.field = SelectorField::Along;    numeric_field = false; }
        else if (field_str == "dihedral")   { sel.field = SelectorField::Dihedral; }
        else if (field_str == "position.x") { sel.field = SelectorField::EdgePosX; }
        else if (field_str == "position.y") { sel.field = SelectorField::EdgePosY; }
        else if (field_str == "position.z") { sel.field = SelectorField::EdgePosZ; }
        else if (field_str == "length")     { sel.field = SelectorField::Length; }
        else return fail("unknown edge field `" + std::string(field_str) +
                         "` (expected along, dihedral, position.x/.y/.z, or length)");
    } else { // Face
        if      (field_str == "normal")     { sel.field = SelectorField::Normal;   numeric_field = false; }
        else if (field_str == "position.x") { sel.field = SelectorField::FacePosX; }
        else if (field_str == "position.y") { sel.field = SelectorField::FacePosY; }
        else if (field_str == "position.z") { sel.field = SelectorField::FacePosZ; }
        else if (field_str == "area")       { sel.field = SelectorField::Area; }
        else return fail("unknown face field `" + std::string(field_str) +
                         "` (expected normal, position.x/.y/.z, or area)");
    }

    // Comparator legality: axis/vector fields (along, normal) only `=`.
    if (!numeric_field && cmp != SelectorCmp::Eq) {
        return fail("field `" + std::string(field_str) +
                    "` only supports the `=` comparator");
    }
    sel.cmp = cmp;

    // Parse the value per field type.
    if (sel.field == SelectorField::Along) {
        Vec3 v;
        if (!parse_axis_alias(value_str, v))
            return fail("malformed value `" + std::string(value_str) +
                        "` for `along` (expected an axis alias like +x, -z)");
        sel.value_is_vector = true;
        sel.vector = v;
    } else if (sel.field == SelectorField::Normal) {
        Vec3 v;
        if (!parse_axis_alias(value_str, v) && !parse_vector3(value_str, v))
            return fail("malformed value `" + std::string(value_str) +
                        "` for `normal` (expected an axis alias like +z or a "
                        "vector `x,y,z`)");
        sel.value_is_vector = true;
        sel.vector = v;
    } else {
        double num;
        if (!parse_number(value_str, num))
            return fail("malformed value `" + std::string(value_str) +
                        "` for field `" + std::string(field_str) +
                        "` (expected a number)");
        sel.number = num;
    }

    r.ok = true;
    r.selector = sel;
    return r;
}

bool Selector::matches(const EdgeProps& e) const {
    if (scope == SelectorScope::All) return true;
    if (scope != SelectorScope::Edge) return false;
    switch (field) {
        case SelectorField::Along: {
            const Vec3 axis = vector.normalized();
            const Vec3 dir  = e.direction.normalized();
            // Edges are undirected: match either orientation.
            const double cos_ang = std::fabs(dir.dot(axis));
            const double cos_tol = std::cos(kSelectorAngleTolDeg * kPi / 180.0);
            return cos_ang >= cos_tol;
        }
        case SelectorField::Dihedral: return cmp_num(e.dihedral_deg, cmp, number);
        case SelectorField::EdgePosX: return cmp_num(e.midpoint.x, cmp, number);
        case SelectorField::EdgePosY: return cmp_num(e.midpoint.y, cmp, number);
        case SelectorField::EdgePosZ: return cmp_num(e.midpoint.z, cmp, number);
        case SelectorField::Length:   return cmp_num(e.length, cmp, number);
        default: return false;   // face field on an edge — never matches
    }
}

bool Selector::matches(const FaceProps& f) const {
    if (scope == SelectorScope::All) return true;
    if (scope != SelectorScope::Face) return false;
    switch (field) {
        case SelectorField::Normal: {
            // Faces are treated as undirected for selector matching
            // (matches the `along=` convention on edges). Authors
            // wanting signed normal selection can use the
            // dihedral / position fields instead.
            const Vec3 want = vector.normalized();
            const Vec3 have = f.normal.normalized();
            const double cos_ang = std::fabs(have.dot(want));
            const double cos_tol = std::cos(kSelectorAngleTolDeg * kPi / 180.0);
            return cos_ang >= cos_tol;
        }
        case SelectorField::FacePosX: return cmp_num(f.centroid.x, cmp, number);
        case SelectorField::FacePosY: return cmp_num(f.centroid.y, cmp, number);
        case SelectorField::FacePosZ: return cmp_num(f.centroid.z, cmp, number);
        case SelectorField::Area:     return cmp_num(f.area, cmp, number);
        default: return false;   // edge field on a face — never matches
    }
}

}  // namespace cadml
