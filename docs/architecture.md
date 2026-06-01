# CADML Architecture

This document gives the top-down picture of CADML — what the pieces are,
how they fit together, what flows between them. Read it first if you're
new to the codebase. Each stage links to a deeper document.

If you are reimplementing CADML from scratch, the right reading order is:

1. **This document** — to know what stages exist and why.
2. [`spec/language.md`](spec/language.md) — to know what valid `.cadml`
   looks like.
3. [`compiler.md`](compiler.md) — to know how the compiler turns
   `.cadml` into `.fcadml`.
4. [`evaluator.md`](evaluator.md) — to know how the evaluator turns
   `.fcadml` into meshes.
5. [`exporters.md`](exporters.md) — to know how meshes turn into STL /
   glTF.

Everything else (`spec/flat-ir.md`, `spec/expressions.md`, `csg.md`,
`lua-embedding.md`, `implementation-notes.md`, `error-model.md`) is
referenced from those five.

---

## 1. The pipeline

```
┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  .cadml      │ ─▶ │   Parser     │ ─▶ │   Bundler    │ ─▶ │  Evaluator   │ ─▶ │  Exporter    │
│  source +    │    │              │    │  (compiler)  │    │  (engine)    │    │              │
│  imports +   │    │  XML → AST + │    │              │    │              │    │  STL / glTF  │
│  .lua files  │    │  frontmatter │    │  .cadml AST  │    │  .fcadml AST │    │              │
│              │    │  → Document  │    │  → .fcadml   │    │  → meshes    │    │              │
└──────────────┘    └──────────────┘    └──────────────┘    └──────────────┘    └──────────────┘
                          │                    │                   │                    │
                          ▼                    ▼                   ▼                    ▼
                    SourceMap            inlined imports     FlatEvalResult       binary STL /
                    (line ↔ node)        + def expansion     {parts: [{mesh,      ASCII STL /
                                         + for unroll        color, name}],       glTF / glB
                                         + Lua exec           errors, warnings}
                                         + expr substitution
                                         + param snapshot
```

Each arrow is a pure transformation. Each stage's output is consumed by
the next; nothing feeds back. The bundler and evaluator are separate
processes in this design — you can compile a `.cadml` to `.fcadml`,
ship the `.fcadml`, and evaluate it on a different machine without the
source files or the Lua interpreter.

### 1.1 Stage responsibilities

| Stage | Reads | Produces | Lives in |
|---|---|---|---|
| **Parser** | one `.cadml` (or `.fcadml`) file | one `Document` (frontmatter + AST + source map) | `src/cadml/` |
| **Bundler** | entry `Document` + filesystem (transitive imports + `.lua`) | one self-contained `Document` (the `.fcadml`) | `src/cadml_compile/` |
| **Evaluator** | one `Document` (must be flat) | `FlatEvalResult` (per-part meshes + diagnostics) | `src/engine/` |
| **Exporter** | `FlatEvalResult` | bytes (binary STL, 3MF package, glTF JSON + bin buffers) | `src/engine/` (STL / 3MF / glTF) |

### 1.2 What flows between stages

**Parser → Bundler.** A `Document` containing:
- The frontmatter values (`version`, `units`, `description`, `param`s, `import`s).
- The body XML tree as a flat node array (`std::vector<Node>`), with
  parent / child relationships expressed as indices.
- A `SourceMap` recording per-node origin (file, line, column) so
  downstream errors can quote back to the user's editor.

**Bundler → Evaluator.** A *flat* `Document` (the in-memory `.fcadml`)
with the same shape, except:
- All `import` statements have been inlined. `.lua` imports become
  synthesised `<script>` blocks; `.cadml` imports become `<def>`s
  merged into the host's namespace.
- All `<for>` and `<pattern>` loops have been unrolled.
- All `<assembly>` elements have been compiled to `<part>`s with the
  mating transforms baked into `<group>` wrappers; `<connect>` and
  `at=`/`port=` mating attributes have been consumed.
- Lua-function-call expressions (`{module.fn(...)}`) have been
  evaluated to literal values. Param-reference and native-builtin
  expressions stay symbolic for the engine to evaluate at runtime.
- `<script>` blocks and `<cut>` elements **survive** lowering — they
  are handled on the evaluator side. See
  [`spec/flat-ir.md`](spec/flat-ir.md) §5 for the full survives /
  removed / added breakdown.
- A synthetic `<sources>` element records the original file IDs so
  error frames carry per-file provenance.

**Evaluator → Exporter.** A `FlatEvalResult` with:
- `parts: vector<Part>` where each `Part` is `{ name, color, mesh }` and
  the `mesh` carries `vertices`, `normals`, `indices`, plus optional
  `triangle_node` (which `Node` index each triangle came from — used for
  click-to-source).
- `errors: vector<FlatEvalError>` (fatal — empty mesh on `ok() == true`).
- `warnings: vector<FlatEvalError>` (non-fatal — geometry still produced).
- `node_world_transforms` — per-node accumulated world matrix, captured
  during evaluation. Surfaced so tooling can compute local-frame
  measurements without re-walking the tree.

**Exporter → bytes.** Format-specific. STL uses per-face normals
recomputed from geometry. glTF/glB carries per-vertex positions and
indices; per-vertex normals are emitted only when the engine produced
non-zero ones (the 0.1 primitive constructors leave them zero, so
viewers fall back to flat shading — see `docs/exporters.md` §4.3).
Each part's glTF node carries `extras.source` / `extras.line` pointing
at the originating `<part>` for editor integration.

---

## 2. Module layout

```
CADML/src/
├── cadml/           ─ Parser + Document + Node + SourceMap, plus shared
│                      foundation types (Vec3, Mat4, TriangleMesh,
│                      MeshStats). The "frontend".
│                      Reads .cadml / .fcadml; produces a Document AST.
│                      Depends on: pugixml.
│
├── cadml_compile/   ─ Bundler (the compiler). Takes a Document + its
│                      filesystem context, returns a flat Document.
│                      Depends on: cadml, sol2/Lua (for <script> execution).
│
├── engine/          ─ Flat evaluator. Takes a flat Document, returns
│                      a FlatEvalResult; also the STL / 3MF / glTF
│                      writers (flat_stl, flat_3mf, flat_gltf), the
│                      analysis primitives (flat_analysis), and the
│                      CLI tool mains. Depends on: cadml, manifold.
│
└── cli/             ─ Every command-line binary, including the LSP
                       server (cadmllsp). Each tool depends only on
                       the libraries it needs; library targets never
                       depend on the tools.
```

### 2.1 Why these boundaries

- **`cadml/` (parser) doesn't know about Lua or filesystem.** It accepts
  a UTF-8 byte stream. The bundler is what walks imports. The shared
  foundation types (`Vec3`, `Mat4`, `TriangleMesh`) live here so callers
  can consume meshes without pulling in the compiler.
- **`cadml_compile/` doesn't know about meshes.** Its output is still an
  XML-shaped `Document`, just flat. The split makes it possible to
  inspect, diff, and round-trip `.fcadml` files without the engine.
- **`engine/` carries its own STL / 3MF / glTF writers** alongside the
  flat evaluator, so a single library covers the full evaluate-and-emit
  path.
- **`cli/` is the only place that links all three layers** (and the
  `cadmllsp` LSP server lives here too). Library code never reaches the
  CLI.

### 2.2 The CLI tools

Built from `src/cli/`. The full set (see `src/cli/CMakeLists.txt::CADML_ALL_TOOLS`):

- `cadmlc` — compile `.cadml` to flat `.fcadml`.
- `cadmlbuild` — compile + run the validation checks (interference,
  disconnected instances, over-constrained mates) in one pass.
- `cadmlcheck` — interference + structural validation.
- `cadmlstl` — binary STL export.
- `cadml3mf` — 3MF package export (per-part color / units).
- `cadmlmass` — mass, COM, inertia per part.
- `cadmlbounds` — AABB / principal-axes box / Ritter sphere / axis cylinders.
- `cadmltopo` — per-element triangle / volume breakdown.
- `cadmldiff` — file-to-file regression diff.
- `cadmlmeasure` — element-keyed probes (bbox, min/mean/max distance).
- `cadmlholes` — drilled-hole inventory.
- `cadmlwalls` — wall-thickness statistics.
- `cadmllsp` — LSP server over stdio (parser diagnostics for editor
  integration).

Each tool depends *only* on the libraries it needs:

- `cadmlc` → `cadml`, `cadml_compile`.
- `cadmlstl` → `cadml`, `cadml_compile`, `engine`.

This is a hard rule: **library targets never depend on tool targets**.
You should be able to use the library in your own code without picking
up CLI argument parsing or `iostream` dependencies.

---

## 3. The source-map invariant

Every `Node` in every `Document` (parsed, flattened, evaluated) carries a
`SourceRange { file_id, line, col }`. The invariant is:

> A `SourceRange` always points at the *user-authored source line* that
> produced the node, even after bundling and unrolling.

When the bundler inlines an imported file, it expands the import's nodes
with their *original* file IDs — not the importer's. When the bundler
unrolls a `<for>` loop into 8 copies of the same subtree, each copy's
nodes still point at the original `<for>` element (or at the original
subtree element they're copied from — the bundler picks per node based
on what's most useful for "jump to source").

This is what makes diagnostics like

```
flange.cadml:42: warning: revolve has no 2D profile child
```

point to the user's actual editor line, even when the revolve was
produced by a `<for>` over a `<def>` imported from another file.

See [`error-model.md`](error-model.md) for the full taxonomy and
[`compiler.md`](compiler.md) §9 for how source attribution flows through
each compiler pass.

---

## 4. The `Document` data model

```
Document
├── frontmatter
│   ├── version: "0.1"
│   ├── units:   "mm"
│   ├── description: optional<string>
│   ├── tags:    optional<string>
│   ├── imports: vector<{ path: string, alias: optional<string> }>
│   └── params:  vector<{ name, expression, constraints }>
└── nodes: vector<Node>
    where each Node = {
        type:     enum  (Part, Def, Assembly, Group, Extrude, Revolve, ...)
        attrs:    variant (per-type attribute struct)
        children: vector<NodeIndex>     ← indices into the same vector
        source:   SourceRange { file_id, line, col }
        dead:     bool                  ← compile-time pruning marker
    }
```

The node array is flat; parent/child links are indices, not pointers,
so the whole `Document` is movable, copyable, and trivially
serialisable. The synthetic-root convention means there's no single
top-level "Root" node — the document's body is the set of nodes whose
parents are -1.

See [`spec/flat-ir.md`](spec/flat-ir.md) for the on-disk `.fcadml`
representation and the synthetic elements (`<sources>`, `<source>`,
`<param>`) that the bundler emits.

---

## 5. Threading model

The reference implementation is **single-threaded by default**. Specifically:

- The parser is single-threaded.
- The bundler is single-threaded.
- The evaluator is single-threaded; the per-part CSG it delegates to
  Manifold may itself parallelise (see the Manifold/TBB note below).
- The exporters are single-threaded.

There is no shared mutable state across pipeline stages. A caller may
safely run multiple pipelines in parallel on independent inputs. The
`Document` and `FlatEvalResult` types are not internally
thread-safe — callers must not mutate them from multiple threads.

The Manifold CSG library brings its own thread pool (TBB-based). The
default `cmake --preset default` build leaves `MANIFOLD_PAR=ON` for
throughput; pass `-DMANIFOLD_PAR=NONE` at the CMake configure step
(or set the variable before `add_subdirectory(cadml)` in a
downstream project) to switch to serial CSG when bit-stable output
across hardware threads is required.

---

## 6. Where things can go wrong

The pipeline has four "modes of failure", and they're distinct:

1. **Parse errors.** The `.cadml` is malformed XML or has frontmatter
   the lexer can't tokenize. The parser returns a `ParseResult` with
   `errors` populated; the `Document` is partially populated for
   tool use, but it's not safe to compile.

2. **Compile errors.** The frontmatter and body parsed, but the bundler
   can't resolve an import, can't find a referenced `<def>`, has a
   forward param reference, has a Lua error, etc. Returns a
   `CompileResult` with `errors`; the partial `.fcadml` is *not*
   safe to evaluate.

3. **Evaluation warnings.** The flat document evaluates, but some
   elements produced empty geometry (revolve missing profile child,
   loft section count mismatch, etc.). The evaluator returns a
   `FlatEvalResult` with `warnings` populated; the result is still
   safe to export — you'll just get less geometry than expected.

4. **Evaluation errors.** The flat document is structurally invalid
   (boolean of zero inputs, manifoldness failure in CSG). The
   evaluator returns a `FlatEvalResult` with `errors` populated;
   the affected part has an empty mesh.

Warnings never block compilation or rendering. Errors always do. See
[`error-model.md`](error-model.md) for the full taxonomy.

---

## 7. What CADML deliberately is *not*

- **CADML is not a general programming language.** Lua is the escape
  hatch for parametric helpers (NACA airfoils, gear teeth, splines).
  Most files have no Lua at all.
- **CADML is not a constraint solver.** Geometry is procedural: you
  describe how to build the part, not what shape it should be. There's
  no "this hole should be tangent to that face" relationship — you
  compute the right `transform` yourself (or via Lua).
- **CADML is not a B-rep / NURBS modeller.** All geometry is
  triangle-meshed. Booleans go through Manifold's mesh-based CSG.
  Curves are sampled, not stored analytically.
- **CADML is not a sketch-based parametric system.** There's no
  "sketch on a face, extrude, then edit the sketch and see the
  extrude update". Files are re-evaluated from scratch on every
  change.
