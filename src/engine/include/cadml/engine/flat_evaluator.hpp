// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>     // libcadml — Document, Node, Vec3, SourceRange

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cadml::engine {

// One mesh in libcadml types. `triangle_node` carries source
// attribution: each triangle records the index of the originating
// Node in the flat document, so glTF extras / picking can hop back
// to the source line.
struct FlatMesh {
    std::vector<Vec3>          vertices;
    std::vector<Vec3>          normals;        // per-vertex
    std::vector<std::uint32_t> indices;        // 3 entries per triangle
    std::vector<std::uint32_t> triangle_node;  // doc.nodes index per triangle

    std::size_t vertex_count() const   { return vertices.size(); }
    std::size_t triangle_count() const { return indices.size() / 3; }
};

// Diagnostic from flat-doc evaluation. Boundary violations land in
// errors; recoverable issues (unknown nodes, fall-through expressions)
// land in warnings.
struct FlatEvalError {
    std::string message;
    SourceRange source;
};

// Mesh cache keyed on (def-name, resolved-overrides). When the same
// def is instantiated twice with the same numeric overrides, the
// second eval reuses the first's mesh.
//
// Caller-managed: construct, pass to one or more evaluate_flat calls
// via EvalOptions. Within a single call the cache deduplicates
// repeated instances; across calls (e.g., a viewer re-rendering on
// param change) it survives and amortizes def evaluation.
//
// **Document-scoped**: the cached FlatMesh's `triangle_node` values
// are doc.nodes indices. Sharing one cache across DIFFERENT documents
// would carry over indices that don't make sense in the new doc.
// Each Document should have its own FlatMeshCache.
//
// **Single-threaded**: no internal synchronization. `evaluate_flat`
// is single-threaded; a parallel evaluator would need either external
// locking or per-thread caches.
//
// **Unbounded growth**: no LRU eviction or size cap. A long-running
// tool (viewer with param sweeps) could accumulate memory indefinitely.
// Callers are responsible for `reset()` between document loads or when
// memory pressure matters.
//
// **Cache hit copies the full mesh**: retrieval costs O(verts + tris)
// per hit because the consumer (`eval_geometry_children` → `merge_mesh`)
// needs an owned mesh to apply group transforms onto. For large,
// frequently-instantiated defs this can dominate; the eventual fix is
// to return `shared_ptr<const FlatMesh>` and teach `merge_mesh` to
// handle shared input.
class FlatMeshCache {
public:
    FlatMeshCache();
    ~FlatMeshCache();

    FlatMeshCache(const FlatMeshCache&) = delete;
    FlatMeshCache& operator=(const FlatMeshCache&) = delete;

    // Reset all entries. Diagnostics:
    //   `size()`  — number of entries currently cached
    //   `hits()`  — running total of cache hits since last reset
    //   `misses()`— running total of cache misses since last reset
    void reset();
    std::size_t size() const;
    std::size_t hits() const;
    std::size_t misses() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class FlatMeshCacheAccess;   // internal-only handle
};

struct EvalOptions {
    // Optional caller-supplied cache. Nullptr disables caching.
    FlatMeshCache* cache = nullptr;
};

// Output of evaluating a flat document.
//
//   parts  — one entry per top-level <part>, in document order. Empty
//            on validation failure.
//   errors — non-empty iff `ok()` returns false.
struct [[nodiscard]] FlatEvalResult {
    struct Part {
        std::string name;
        // `color` carries the part's `<part color="#RRGGBB">` attribute
        // as authored — empty string when the part declared no color.
        // Renderer consumers parse this; the engine doesn't validate it
        // (the parser already shape-checks `#RGB` / `#RRGGBB`).
        std::string color;
        FlatMesh    mesh;
    };

    std::vector<Part>           parts;
    std::vector<FlatEvalError>  errors;
    std::vector<FlatEvalError>  warnings;

    // Per-node accumulated world transform, captured during eval.
    // Keyed by `doc.nodes` index. Only nodes that the document
    // walk visits (every reachable, non-dead node) appear here.
    // Surfaced so callers can compute local-frame measurements
    // (frame="local:N") and resolve <port> positions to world space
    // without re-walking the document themselves.
    std::unordered_map<std::uint32_t, Mat4> node_world_transforms;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// Evaluate a flat document.
//
// The document must be the output of `cadml::compile::compile_*`
// (i.e., post-bundler) or hand-authored .fcadml. Authoring constructs
// trigger a hard error.
[[nodiscard]] FlatEvalResult evaluate_flat(const Document& doc,
                              const EvalOptions& opts = {});

// Accumulated world transform for every node in the document.
//
// Walks the tree top-down, composing parent transforms with each
// `<group transform="...">` it encounters. Roots get identity.
// Every node — group, part, primitive, port — receives the
// accumulated frame at its position in the tree. Vertices the
// evaluator writes for that node are `world * v` for the returned
// world matrix.
//
// Used by:
//   * frame="local:N" measurement — invert the returned Mat4 to map
//     world-space output vectors back into the node's authored
//     frame.
//   * `<port>` resolution — evaluate the port's
//     position_expr / normal_expr in the owning part's frame and
//     multiply by the part's world matrix to get the world origin
//     and axes that physics / assembly tools need.
//
// Independent of evaluate_flat — works on the raw flat document.
// Transform expression errors land in `warnings`; the corresponding
// node uses identity for that step and the walk continues.
std::unordered_map<std::uint32_t, Mat4>
accumulated_transforms(const Document& doc,
                        std::vector<FlatEvalError>& warnings);

// ─── glTF export ──────────────────────────────────────────────────────

struct GltfExportOptions {
    // When true, every glTF node carries an `extras: { source, line }`
    // pair pointing at the originating CADML <part>. The viewer uses
    // this for click-to-source navigation. Defaults to true.
    bool include_source_extras = true;
};

// Serialise an evaluation result to a glTF 2.0 JSON document. The
// binary buffer (vertex positions + indices) is embedded as a base64
// data URI so the returned string is self-contained.
//
// Each FlatEvalResult::Part becomes one glTF mesh + one node;
// per-vertex normals are emitted only if the mesh actually carries
// non-zero normals. Per-triangle source attribution is not yet
// surfaced into the glTF (deferred until the renderer needs it).
//
// `doc` is required to translate `triangle_node` SourceRange file IDs
// back to filenames for the per-node `extras`.
std::string export_gltf(const FlatEvalResult& result,
                         const Document& doc,
                         const GltfExportOptions& opts = {});

}  // namespace cadml::engine
