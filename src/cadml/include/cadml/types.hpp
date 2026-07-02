// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cadml {

// ────────────────────────────────────────────────────────────────────────
// 1. Math
// ────────────────────────────────────────────────────────────────────────

struct Vec2 {
    double x = 0;
    double y = 0;

    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }

    double dot(Vec2 o) const { return x * o.x + y * o.y; }
    double cross(Vec2 o) const { return x * o.y - y * o.x; }
};

struct Vec3 {
    double x = 0;
    double y = 0;
    double z = 0;

    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }

    double dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(Vec3 o) const {
        return { y * o.z - z * o.y,
                 z * o.x - x * o.z,
                 x * o.y - y * o.x };
    }

    double length_sq() const { return x * x + y * y + z * z; }
    double length() const;
    Vec3 normalized() const;
};

// Column-major 4x4. Layout: m[col*4 + row]. Identity by default.
struct Mat4 {
    double m[16] = { 1, 0, 0, 0,
                     0, 1, 0, 0,
                     0, 0, 1, 0,
                     0, 0, 0, 1 };

    static Mat4 identity();
    static Mat4 translation(double tx, double ty, double tz);
    static Mat4 rotation(double angle_deg, double ax, double ay, double az);
    static Mat4 scaling(double sx, double sy, double sz);
    static Mat4 mirror(double nx, double ny, double nz);

    Mat4 operator*(const Mat4& other) const;

    Vec3 transform_point(Vec3 p) const;
    /// Same as transform_point but ignores translation. Used for normals
    /// and direction vectors that should not pick up the translation.
    Vec3 transform_direction(Vec3 d) const;
};

// Axis aliases per spec §7.4. Returns nullopt for non-alias strings;
// caller falls back to vec3 expression parsing.
std::optional<Vec3> parse_axis_alias(std::string_view s);

// Parse the `interference-tolerance` frontmatter value (spec §3.3) into
// a numeric volume in document-unit cubed. Format: a non-negative
// number, optionally followed by a volume-unit suffix (`mm³`, `cm³`,
// `m³`, `in³`, `ft³`, with optional whitespace between number and
// suffix; the ASCII variant `mm3`/`cm3`/etc. is also accepted for
// keyboard convenience). With no suffix the value is interpreted as
// `doc_units` cubed. Empty input returns 0; unparseable or negative
// input returns nullopt.
std::optional<double> parse_interference_tolerance(std::string_view text,
                                                     std::string_view doc_units);

// Compute the 4x4 transform that places the part owning port A so that
// port A aligns face-to-face with port B on a part already placed by
// `transform_b`. The translation puts A.position onto B's world position;
// the rotation makes A's normal anti-parallel to B's normal (flip-Z) and
// A.up parallel to B.up. Used by the assembly compiler (spec §6).
Mat4 compute_port_alignment(Vec3 a_pos, Vec3 a_normal, Vec3 a_up,
                             Vec3 b_pos, Vec3 b_normal, Vec3 b_up,
                             const Mat4& transform_b = Mat4::identity());

// Format a Mat4 as a CADML transform attribute string, emitting only
// the parts that differ from identity. Output looks like
// "translate(tx, ty, tz)" or "translate(...) rotate(angle, ax, ay, az)".
// Returns an empty string for the identity matrix.
std::string mat4_to_transform_string(const Mat4& m);

// ────────────────────────────────────────────────────────────────────────
// 2. Source mapping
// ────────────────────────────────────────────────────────────────────────

using SourceFileId = std::uint32_t;
inline constexpr SourceFileId NO_FILE = UINT32_MAX;

// One entry in a Document's source-file table. The compiler emits these
// to .fcadml's <sources> block; tools use them for click-to-source.
struct SourceFile {
    SourceFileId id = NO_FILE;
    std::string path;            // relative to entry directory (forward slashes)
    std::string hash;            // SHA-256 of UTF-8 bytes, lowercase hex
};

// Location of a node in a source file. file may be NO_FILE for
// compiler-generated nodes whose origin can't be cleanly attributed.
struct SourceRange {
    SourceFileId file = NO_FILE;
    std::uint32_t line = 0;      // 1-based
    std::uint32_t column = 0;    // 1-based
    std::uint32_t length = 0;    // byte length in the source

    bool valid() const { return file != NO_FILE && line > 0; }
};

// ────────────────────────────────────────────────────────────────────────
// 3. NodeType taxonomy
// ────────────────────────────────────────────────────────────────────────
//
// Every element parses into a Node whose `type` is one of these. The
// `Instance` variant covers any element name that's not a built-in —
// resolution to "import alias" or "local def" happens at compile time.
//
// `Unknown` is a parse-time fallback for elements the parser can't
// classify (e.g. malformed extension tags); the compiler errors on it.
//
// Numeric values are NOT stable across spec versions; treat them as
// opaque. The 28 reserved built-in element names per spec §4.3 each map
// to one of these enum values.
enum class NodeType {
    // Structural (9). `Svg` is a coordinate-frame wrapper that
    // applies SVG y-down→y-up semantics to its descendants.
    Part, Def, Assembly, Connect, Port, Group, Script, For, Svg,

    // 2D primitives (4)
    Circle, Rect, Path, Sketch,

    // 2D-to-3D (5)
    Extrude, Revolve, Sweep, Loft, Helix,

    // Mesh import (1) — an STL blob welded into a manifold solid
    Stl,

    // Booleans (3) + convex hull
    Union, Difference, Intersect, Hull,

    // Modifiers (5)
    Fillet, Chamfer, Shell, Cut, Pattern,

    // Param (3 — survives to .fcadml)
    Param, Sources, Source,

    // Instance: any name in the file's vocabulary that isn't a built-in.
    // Resolved against {imports} ∪ {<def>s} at compile time.
    Instance,

    // Fallback for unrecognised elements. Compiler errors on this.
    Unknown,
};

// Convert an element name to its NodeType. Returns Unknown if the name
// is not a built-in. (Instance is never returned here — that's a
// parser-side decision.)
NodeType node_type_from_builtin_name(std::string_view name);

// Inverse: returns the canonical built-in name, or empty for Instance/
// Unknown.
std::string_view builtin_name_from_node_type(NodeType type);

// Whether this type is one of the 28 reserved built-in element names.
bool is_builtin(NodeType type);

// ────────────────────────────────────────────────────────────────────────
// 4. Per-element attribute structs
// ────────────────────────────────────────────────────────────────────────

// Structural ─────────────────────────────────────────────────────────────

struct PartAttrs {
    std::string name;            // optional in source; defaulted to filename
    std::string color;           // "#RRGGBB" or "#RGB"; empty if unset
};

struct DefAttrs {
    std::string name;            // required, file-private
    // `color` is set by the bundler when an imported `<part color="…">`
    // is converted into a `<def>` (so the colour survives the
    // conversion). Hand-authored `<def>`s leave this empty; the
    // engine resolves colour from the containing `<part>` as usual.
    // When an Instance of a coloured def is evaluated inside a
    // colourless `<part>`, the engine propagates the def's colour up.
    std::string color;
};

struct AssemblyAttrs {
    std::string name;
};

struct ConnectAttrs {
    std::string a;               // "<instance-id>.<port-name>"
    std::string b;               // "<instance-id>.<port-name>"
    // When true, `cadml_check` suppresses interference reports between
    // the two instances' parts. Used for legitimate-overlap mates
    // (threaded fasteners that bite into a plate, press-fits, etc.).
    bool allow_interference = false;
};

struct PortAttrs {
    std::string name;
    std::string position_expr;   // point3d expression
    std::string normal_expr;     // vector3d expression
    std::string up_expr;         // vector3d expression (optional)
};

struct GroupAttrs {
    std::string id;              // user-supplied id (optional); compiler-
                                 // synthesised groups are tagged via
                                 // a separate attribute layer
    std::string transform;       // SVG-like transform chain
    std::string color;
};

struct ScriptAttrs {
    std::string lang = "lua";    // "lua" only in 0.1
    std::string source;          // raw script body (no CDATA needed)
};

struct ForAttrs {
    std::string var;             // loop variable name
    std::string from_expr;       // uniform mode
    std::string to_expr;         // uniform mode
    std::string steps_expr;      // uniform mode
    std::string values;          // explicit-values mode (space-separated)
};

// 2D primitives ──────────────────────────────────────────────────────────

// SVG-style presentation attributes shared by all 2D primitives.
// Carried through compilation so authored colour information
// survives the bundler. The flat evaluator promotes the first
// non-empty `fill` it sees inside a <part> body to the part's
// `color` when the <part> didn't declare one explicitly.
//
// `stroke` is parsed for round-trip fidelity but the engine doesn't
// render strokes — only filled bodies. Metadata only in 0.1.
struct FillStrokeAttrs {
    std::string fill;
    std::string stroke;
};

struct CircleAttrs : FillStrokeAttrs {
    std::string cx_expr = "0";
    std::string cy_expr = "0";
    std::string r_expr;
    // Optional override for the polygonal tessellation. Empty means
    // "let the engine pick" (adaptive based on radius and a chord-
    // deviation tolerance). Use a positive integer to force a
    // specific segment count — useful for reproducible tests, for
    // intentionally faceted parts (e.g. a 6-segment "circle" is a
    // hexagon), or to opt into a higher-detail render than the
    // default tolerance gives.
    std::string segments_expr;
};

struct RectAttrs : FillStrokeAttrs {
    std::string x_expr = "0";
    std::string y_expr = "0";
    std::string width_expr;
    std::string height_expr;
    std::string rx_expr = "0";
    std::string ry_expr;         // defaults to rx if empty
};

struct PathAttrs : FillStrokeAttrs {
    std::string d;               // SVG path data, may contain {expr}
};

struct SketchAttrs {
    std::string plane = "xy";    // xy / xz / yz
    std::string origin_expr = "0 0 0";
    std::string rotation_expr = "0";
    std::string normal_expr;     // optional override
};

// 2D-to-3D ───────────────────────────────────────────────────────────────

struct ExtrudeAttrs {
    std::string height_expr;
    std::string scale_expr = "1";
    std::string draft_expr = "0";
    bool symmetric = false;
    std::string direction_expr = "+z";
};

struct RevolveAttrs {
    std::string axis;            // "x" / "y" / "z" / "+x" / "-z" / etc.
    std::string angle_expr = "360";
    // Rotational facet count. Empty → engine default (32). Higher
    // values smooth the revolved surface at the cost of triangle
    // count; useful for hero renders and precision parts where the
    // facet edges would otherwise be visible.
    std::string segments_expr;
};

struct SweepAttrs {};            // semantics defined entirely by children
struct LoftAttrs {};             // ditto

struct HelixAttrs {
    std::string radius_expr;
    std::string pitch_expr;
    std::string turns_expr;
    std::string taper_expr = "0";
    std::string direction = "ccw"; // "ccw" / "cw"
};

// Mesh import ─────────────────────────────────────────────────────────────

// <stl> imports a triangle mesh from an STL blob and welds it into a
// manifold solid that composes with the boolean/hull primitives like any
// other 3D leaf. Exactly one source is expected:
//   * `src`  — path to an .stl file, resolved relative to the document (the
//              authoring form). The bundler reads it and lowers it to `data`
//              so the flat document stays self-contained; the engine never
//              touches the filesystem.
//   * `data` — the STL bytes embedded directly, per `encoding` (the flat /
//              single-file form). `base64` is the only encoding in 0.1.
// Binary and ASCII STL are both accepted; per-facet normals are ignored and
// recomputed. Placement/scaling is done with an enclosing <group transform>.
struct StlAttrs {
    std::string src;
    std::string data;
    std::string encoding = "base64";
};

// Booleans ───────────────────────────────────────────────────────────────

struct UnionAttrs {};
struct DifferenceAttrs {};
struct IntersectAttrs {};
struct HullAttrs {};
struct SvgAttrs {};   // marker only; semantics handled in the engine

// Modifiers ──────────────────────────────────────────────────────────────

struct FilletAttrs {
    std::string radius_expr;
    std::string select = "all";
};

struct ChamferAttrs {
    std::string distance_expr;
    std::string angle_expr = "45";
    std::string select = "all";
};

struct ShellAttrs {
    std::string thickness_expr;
    std::string open;            // selector expression or empty
};

// <cut> is authoring-only; lowered to <difference> with a synthesised
// wedge during compile (spec §12.5).
struct CutAttrs {
    std::string face;            // "start" / "end"
    std::string type;            // "miter" / "bevel" / "compound" / ""
    std::string angle_expr;      // single-axis cuts
    std::string miter_expr;      // compound cuts
    std::string bevel_expr;      // compound cuts
};

// <pattern> is authoring-only; unrolled to <group transform> siblings
// during compile (spec §12.6).
struct PatternAttrs {
    std::string type;            // "linear" / "circular"
    std::string count_expr;
    std::string axis = "z";
    std::string spacing_expr;    // linear
    std::string angle_expr = "360"; // circular
};

// Flat-output and frontmatter-emitted ────────────────────────────────────

// <param> element: appears as a child of <def> or <part> in .fcadml,
// emitted by the compiler from frontmatter param declarations.
struct ParamAttrs {
    std::string name;
    std::string value_expr;      // expression as written
    std::optional<double> min;   // literal in flat output
    std::optional<double> max;   // literal in flat output
};

// <sources> wrapper element in .fcadml body.
struct SourcesAttrs {};

// <source id="..." path="..." hash="..."/> child of <sources>.
struct SourceAttrs {
    std::uint32_t id = 0;
    std::string path;
    std::string hash;
};

// Instance: any element whose tag name isn't one of the 28 built-ins.
// At parse time, the parser stores the raw element name in `ref_name`.
// The compile pass resolves it against {import aliases} ∪ {<def>s}.
//
// Reserved structural attributes (id, at, port) are stored in named
// fields. Any other attribute is treated as a param override and
// stored in `param_overrides`.
struct InstanceAttrs {
    std::string ref_name;        // the element tag name
    std::string id;              // optional, for <connect> targeting
    std::string at;              // parent port (mating); empty = bare instance
    std::string port;            // own port (mating)

    // Param overrides keyed by param name. Values are raw expression
    // strings; min/max validation happens at compile time.
    std::unordered_map<std::string, std::string> param_overrides;
};

// Fallback for elements the parser couldn't classify. The body parser
// preserves the raw element name so the compiler can produce a useful
// error.
struct UnknownAttrs {
    std::string raw_tag_name;
    std::vector<std::pair<std::string, std::string>> raw_attrs;
};

// ────────────────────────────────────────────────────────────────────────
// 5. NodeAttrs variant
// ────────────────────────────────────────────────────────────────────────

using NodeAttrs = std::variant<
    PartAttrs,    DefAttrs,        AssemblyAttrs,   ConnectAttrs,
    PortAttrs,    GroupAttrs,      ScriptAttrs,     ForAttrs,
    SvgAttrs,
    CircleAttrs,  RectAttrs,       PathAttrs,       SketchAttrs,
    ExtrudeAttrs, RevolveAttrs,    SweepAttrs,      LoftAttrs,
    HelixAttrs,   StlAttrs,
    UnionAttrs,   DifferenceAttrs, IntersectAttrs,  HullAttrs,
    FilletAttrs,  ChamferAttrs,    ShellAttrs,      CutAttrs,
    PatternAttrs,
    ParamAttrs,   SourcesAttrs,    SourceAttrs,
    InstanceAttrs, UnknownAttrs
>;

// ────────────────────────────────────────────────────────────────────────
// 6. Node — intrusive scene-graph node
// ────────────────────────────────────────────────────────────────────────

inline constexpr std::uint32_t NO_NODE = UINT32_MAX;

// Flat-array tree node. Children are linked via first_child + next_sibling.
// Parent indices point back into the same Document.nodes array.
//
// Iterate children of node_idx via Document::children(node_idx).
struct Node {
    NodeType type = NodeType::Unknown;
    std::uint32_t parent = NO_NODE;
    std::uint32_t first_child = NO_NODE;
    std::uint32_t next_sibling = NO_NODE;

    NodeAttrs attrs;             // matches `type` 1:1 (variant alternative)
    SourceRange source;          // location in originating source file

    // Iteration helper: synthesised <group>s from <for>/<pattern>
    // unrolling carry the iteration index for tooling. UINT32_MAX = N/A.
    std::uint32_t iteration = UINT32_MAX;

    // True if the node has been "retired" by a compile pass (e.g. the
    // original <for>/<pattern> node after unrolling). Dead nodes remain
    // in doc.nodes for index stability but are ignored by tree walks
    // and serialisation.
    bool dead = false;
};

// ────────────────────────────────────────────────────────────────────────
// 7. Frontmatter types
// ────────────────────────────────────────────────────────────────────────

// Document-level metadata (frontmatter settings per spec §3.3).
struct DocumentMeta {
    std::string version = "0.1.0"; // normalised to three-component form
    std::string units = "mm";
    std::string description;
    std::string tags;              // space-separated
    std::string catalogue_version;
    std::string interference_tolerance; // raw text, e.g. "0.01mm³"; empty = 0

    // Pairs of part names whose mutual interference `cadml_check`
    // should suppress. Populated by the assembly compiler from
    // `<connect ... allow-interference="true"/>` elements (def-name
    // of inst_a, def-name of inst_b). Symmetric in semantics but
    // stored as written for diagnostic clarity.
    //
    // Not (yet) round-tripped via .fcadml frontmatter — consumers
    // that re-parse the flat text lose this list. Tools driving the
    // engine via the in-memory `Document` (cadml_check, the future
    // viewer) read it directly from this field. Adding a frontmatter
    // directive (`interference-allow "<a>" "<b>"`) is straightforward
    // when a tool needs the round-trip.
    std::vector<std::pair<std::string, std::string>> allow_interference_pairs;
};

// One `import "<path>" [as <alias>]` directive from frontmatter.
struct ImportDecl {
    std::string path;            // raw path as written in source
    std::string alias;            // explicit, or default-derived from filename
    SourceRange source;

    // True if path starts with "ctl/" (catalogue / standard library).
    bool is_catalogue = false;

    // True if the file referenced has the .lua extension. Set by the
    // import resolver, not the parser.
    bool is_lua = false;
};

// One `param <name> = <value> [(min=..., max=...)]` declaration from
// frontmatter. The compiler emits these as <param> elements in .fcadml.
struct ParamDecl {
    std::string name;
    std::string value_expr;       // expression as written
    std::optional<double> min;
    std::optional<double> max;
    SourceRange source;
};

// ────────────────────────────────────────────────────────────────────────
// 8. Document — the root container
// ────────────────────────────────────────────────────────────────────────

struct Document {
    // Frontmatter
    DocumentMeta meta;
    std::vector<ImportDecl> imports;
    std::vector<ParamDecl> params;

    // Body — flat array of all nodes; tree structure via parent/sibling.
    std::vector<Node> nodes;

    // Index of <def name="..."> elements by name → node index.
    std::unordered_map<std::string, std::uint32_t> defs;

    // Index of top-level <part>/<assembly> exports by name → node index.
    // After parse, exactly one entry (per the 1:1 file rule).
    std::unordered_map<std::string, std::uint32_t> exports;

    // Source-file table. Index 0 is reserved for the entry file by
    // convention; imported files take subsequent IDs.
    std::vector<SourceFile> source_files;

    // ─── Child iteration ────────────────────────────────────────────
    //
    // Usage:
    //     for (const auto& child : doc.children(node_idx)) { ... }
    //
    // Returns an iterator over the immediate children of the given node.
    struct ChildIterator {
        const Document* doc;
        std::uint32_t current;

        const Node& operator*() const { return doc->nodes[current]; }
        ChildIterator& operator++() {
            current = doc->nodes[current].next_sibling;
            return *this;
        }
        bool operator!=(const ChildIterator& o) const {
            return current != o.current;
        }
    };

    struct ChildRange {
        const Document* doc;
        std::uint32_t first;
        ChildIterator begin() const { return { doc, first }; }
        ChildIterator end() const { return { doc, NO_NODE }; }
    };

    ChildRange children(std::uint32_t node_idx) const {
        return { this, nodes[node_idx].first_child };
    }
};

// ────────────────────────────────────────────────────────────────────────
// 6. Numeric parsing helpers
// ────────────────────────────────────────────────────────────────────────

// Strict double parser. Returns nullopt when:
//   - the input is empty or whitespace-only
//   - the input has trailing non-numeric junk ("1.0x", "1, 2")
//   - the magnitude exceeds what double can represent
//
// Built on std::from_chars (locale-independent, no errno indirection,
// no implicit float-special parsing). Use this for every CADML
// attribute that's expected to be a literal numeric value (min/max
// constraints, frontmatter params, helix/sweep counts, …) so the
// pipeline never silently treats bad input as zero.
std::optional<double> parse_double_strict(std::string_view s);

// Linear scale factor from the given CADML units token to millimetres.
// Accepts "mm", "cm", "m", "in", "ft". Returns nullopt for any other
// input. Use this when a tool needs to scale geometry coordinates
// (which are in document units per docs/spec/coordinate-system.md §3)
// to a known reference unit — e.g. mass-property tools converting
// volume to kg before applying density in kg/m³.
std::optional<double> units_to_mm_scale(std::string_view unit);

// Format a double as a canonical decimal string with `precision`
// significant digits, using a `.` for the decimal point regardless of
// the host's LC_NUMERIC setting. Equivalent in intent to
// `snprintf("%.*g", precision, v)` but locale-independent — needed by
// the serializer, the mesh-cache key generator, and the 3MF / glTF
// emitters so cross-locale builds round-trip identically.
std::string format_double_canonical(double v, int precision);

}  // namespace cadml
