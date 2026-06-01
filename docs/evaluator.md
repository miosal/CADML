# CADML Evaluator

This document specifies the **flat evaluator** — the stage that turns a
flat `Document` (the `.fcadml` output of the bundler) into a
`FlatEvalResult` (per-part meshes + diagnostics).

Background: [`architecture.md`](architecture.md) for the pipeline;
[`compiler.md`](compiler.md) for what the bundler hands us.

---

## 1. Contract

```cpp
struct FlatEvalResult {
    vector<Part> parts;                     // one per <part>, in document order
    vector<FlatEvalError> errors;           // empty on ok()
    vector<FlatEvalError> warnings;         // does not affect ok()
    unordered_map<NodeIdx, Mat4> node_world_transforms;
};

FlatEvalResult evaluate_flat(const Document& doc, const EvalOptions& = {});
```

- **Input:** a `Document` produced by the bundler. All `<for>`s have been
  unrolled. All `<def>` references have been resolved. All `{…}`
  expressions have been substituted. All `<import>`s have been inlined.
- **Output:** one `Part` per top-level `<part>`. Each `Part` has a
  `FlatMesh { vertices, normals, indices, triangle_node }` and a
  `color` carried from the part attribute.
- **Diagnostics:** errors block; warnings inform. Both carry
  `SourceRange` so tooling can quote back to the user's editor.

The evaluator is **pure**: it does not read the filesystem, write any
files, or mutate global state. Given the same `Document` it produces
byte-identical output (modulo a `FlatMeshCache` for incremental builds,
which is opt-in).

---

## 2. Walk order

The top-level evaluator iterates `Document::nodes` looking for
`NodeType::Part`. For each part:

1. **Push a fresh ExpressionEvaluator** seeded with the part's locally-scoped
   params.
2. **Walk children depth-first**, building up the part's mesh from
   leaf to root.

Element-handling lives in three dispatch tables:

| Returns | Function | Handles |
|---|---|---|
| `optional<Polygon2D>` | `eval_2d` | `<rect>`, `<circle>`, `<path>` |
| `optional<LoftSection>` | `eval_sketch` | `<sketch>` |
| `FlatMesh` | `eval_3d` | everything else (extrude, revolve, sweep, loft, booleans, modifiers, group, svg) |

The element type determines which dispatcher is invoked. An element
whose immediate children are 2D primitives evaluates them via
`eval_2d`. An element whose children are 3D meshes recurses into
`eval_3d`. Mismatched shapes — e.g., a `<union>` of 2D polygons — are
errors.

---

## 3. 2D primitives

### 3.1 `<rect>`

```
<rect x="..." y="..." width="..." height="..." [rx="..."] [ry="..."]/>
```

Inputs: `x`, `y` (lower-left corner), `width`, `height`. Optional
`rx` / `ry` for rounded corners (`ry` defaults to `rx`; both default
to 0).

Output: a `Polygon2D` with vertices in **CCW** order, starting at the
lower-left corner and wrapping counter-clockwise.

Algorithm: for sharp corners, emit four vertices. For rounded corners,
emit four quarter-arcs (one per corner) at adaptive segment count
(same algorithm as `<circle>` — see §3.2). Returns the union of the
straight sides and the quarter-arcs as one closed loop.

### 3.2 `<circle>`

```
<circle [cx="0"] [cy="0"] r="..." [segments="N"]/>
```

Inputs: `cx`, `cy` (centre, default origin), `r` (radius). Optional
`segments` (explicit count).

Output: a `Polygon2D` with `segments` vertices in CCW order.

Algorithm: when `segments` is omitted, the count is chosen so the
chord-to-arc deviation (the **sagitta**) stays below a fixed tolerance
of **0.005 document units**. For a regular N-gon inscribed in a circle of
radius `r`, the sagitta is `s = r·(1 − cos(π/N))`; solving for N and
clamping:

```
N = ceil( π / acos(1 − s/r) ),   s = 0.005 (document units)
N = clamp(N, 8, 256)
```

This is a *sagitta* bound (perpendicular chord-to-arc distance), not an
arc-*length* bound. Worked values (assuming `units mm`): `r = 4 mm`
(a typical M8 shank) → **N ≈ 64**; `r = 8 mm` → N ≈ 89; the count
saturates the upper clamp (256) around `r ≈ 80 mm`. The tolerance is in
the document's own coordinate units — CADML's `units` is a pure label
that applies no internal scaling (see
[`spec/coordinate-system.md`](spec/coordinate-system.md)), so a `units in`
document tessellates to 0.005 *inch*. Tessellation is a property of the
geometry, not of any particular display zoom.

`r` is the **geometric radius**: vertices are placed at
`(cx + r·cos θ, cy + r·sin θ)`. The polygon **inscribes** the
geometric circle — vertex positions are exactly on the circle, but
the chord midpoints sit *inside* by up to one sagitta. This means the
polygon's bbox under-shoots the geometric `cx ± r, cy ± r` extents by
up to one sagitta when no vertex falls on the cardinal axes.

Explicit `segments="N"` is honoured verbatim and bypasses **both**
clamp bounds; `segments="3"` produces a triangle, `segments="4"` a
diamond, and a value above 256 is also honoured as-is.

> **Implementation note.** The above describes the v0.1 engine
> (`flat_geometry.cpp::tessellate_circle`), which is what every CLI tool
> runs.

### 3.3 `<path>`

```
<path d="M x,y L x,y A rx ry rot la sw x,y C x1,y1 x2,y2 x,y Q x1,y1 x,y Z"/>
```

The `d` attribute uses an SVG-subset path grammar — see
[`spec/svg-subset.md`](spec/svg-subset.md). Supported commands:
`M` / `L` / `A` / `C` / `Q` / `Z` plus their lowercase relative
variants.

Output: a `Polygon2D` formed by flattening curves (arcs, cubic and
quadratic Béziers) until the chord-to-curve deviation falls below the
path-flattening tolerance, **0.005 document units**
(`flat_geometry.cpp::kPathTolerance`), with a recursion-depth cap on the
adaptive subdivision. This is the **same** bound as `<circle>`'s sagitta
(§3.2), so an arc drawn as a `<path>` flattens to the same smoothness as
the equivalent `<circle>` — curve quality is independent of which
primitive produced it.

The path **must close** with `Z` (or with a final position matching
the most recent `M`). Open paths produce a warning and the open
polygon (treated as a polyline) — caller behaviour for subsequent
extrude/revolve is undefined.

A path that tessellates to fewer than 3 points is a warning + null
result (typical cause: only `M x,y Z` with no `L` commands).

---

## 4. `<sketch>`

```
<sketch [plane="xy"|"xz"|"yz"] [origin="0 0 0"] [rotation="0"] [normal="..."]>
  <one 2D primitive>
</sketch>
```

`<sketch>` is a 3D-positioned wrapper around one 2D primitive. It
produces a `LoftSection` — the 2D profile *plus* the 3D frame
(`origin`, `right`, `up`, `normal`) used to position it.

**Frame derivation:**

If `normal` is omitted, it's derived from `plane`:
- `plane="xy"` → normal `+z`, up `+y`, right `+x`.
- `plane="xz"` → normal `+y`, up `+z`, right `+x`.
- `plane="yz"` → normal `+x`, up `+z`, right `+y`.

If `normal` is given, it overrides `plane`. The frame is then
constructed:
- `up` = projection of `+z` onto the plane perpendicular to `normal`,
  normalised. If `normal` is parallel to `+z` (within `1e-6`), `up`
  falls back to `+y`.
- `right` = `up × normal`.

`rotation` (degrees, default 0) rotates the 2D profile around the
`normal` axis in the `(right, up)` plane *before* lifting to 3D.

`origin` (default `0 0 0`) translates the lifted profile in world space.

**Important constraint:** the engine's `<extrude>`, `<revolve>`, and
`<sweep>` do **not** accept `<sketch>` as their profile child. `<sketch>`
is *only* useful as a `<loft>` cross-section. The spec aspires to
allow `<sketch>` wrapping for extrude/revolve/sweep but the reference
implementation has not landed that path; see
[`implementation-notes.md`](implementation-notes.md).

---

## 5. 2D → 3D

### 5.1 `<extrude>`

```
<extrude height="..." [scale="1"] [draft="0"] [symmetric="false"] [direction="+z"]>
  <2D primitive>
</extrude>
```

Children: exactly one 2D primitive (`<rect>`, `<circle>`, `<path>`) as
a **direct** child. The 2D primitive must close.

Algorithm:
1. Evaluate the child to a `Polygon2D`.
2. Tessellate to triangles (ear-clip).
3. Emit two caps (bottom at `z=0`, top at `z=height`).
4. Emit side quads connecting bottom-edge to top-edge vertices.
5. If `scale != 1`, scale the top cap's `(x, y)` coordinates uniformly.
6. If `draft != 0`, taper the side walls by the given angle.
7. If `symmetric="true"`, shift the whole mesh by `-height/2` so the
   extrude is centred on the sketch plane.
8. If `direction` is set, rotate the result so `+z` maps to `direction`.

`scale` and `draft` are independent: scale changes the end-cap size;
draft tapers the sidewalls. Both are applied in the local extruded
frame before the final orientation by `direction`.

**Profile winding:** CADML's convention is CCW from the side facing
the extrusion direction. CW profiles produce inverted normals; the
engine emits a warning and silently reverses winding to keep the
extrude valid. The warning is silenced inside `<svg>` (where y-down
CW input is expected — see §10).

### 5.2 `<revolve>`

```
<revolve axis="x"|"y"|"z" [angle="360"] [segments="64"]>
  <2D primitive>
</revolve>
```

Children: exactly one 2D primitive (`<rect>`, `<circle>`, `<path>`) as
a **direct** child.

**Profile interpretation:** the 2D primitive's `x` coordinate maps to
*radial* distance from the axis; the `y` coordinate maps to *axial*
distance along the axis. For `axis="z"`, that means `(x, y)` →
`(r, z)`. The profile must not cross the axis (no negative `x`); a
profile that does is an error.

Algorithm:
1. Evaluate the child to a `Polygon2D`.
2. For each of `segments` rotation steps, rotate the profile around
   the axis by `θ_k = k * angle / segments`.
3. Connect consecutive rotated profiles with quad strips (ring-to-ring).
4. If `angle < 360`, ear-clip the start and end profiles as caps; the
   side surface is open.
5. If `angle == 360`, no caps are needed (the surface closes on itself).

`segments` default is 32; use 128 or 180 when curvature shows up
faceted. The cost is linear in `segments`.

### 5.3 `<sweep>`

```
<sweep>
  <2D primitive>
  <helix radius="..." pitch="..." turns="..." [taper="0"] [direction="ccw"]/>
</sweep>
```

Children: exactly two — one 2D primitive and one `<helix>`, in any
order. The `<helix>` defines the guide curve.

**Profile convention:** the 2D primitive's `x` coordinate maps to the
**radial direction outward from the helix axis**, the `y` coordinate
maps to `+Z` (the helix axis). This is the *axis-perpendicular*
convention: the profile is placed at each sample frame parallel to
the helix axis, not perpendicular to the curve tangent. (Useful for
thread modelling — a triangular profile produces a clean spiral
groove.)

Algorithm:
1. Sample the helix at a fixed number of frames per turn (the engine
   uses 32 by default).
2. At each frame, compute the position and the *axis-perpendicular
   transform* (radial direction outward + `+Z`).
3. Place the 2D profile at each frame.
4. Connect consecutive profiles with quad strips.

### 5.4 `<loft>`

```
<loft>
  <sketch plane="..." origin="..." ...>
    <primitive/>
  </sketch>
  <sketch ...>
    <primitive/>
  </sketch>
  ...   (2 or more sketches)
</loft>
```

Children: two or more `<sketch>` elements (cross-sections), in order
from start to end. Each `<sketch>` contains exactly one 2D primitive.

**Vertex-count constraint:** all sections must produce the same number
of tessellated 2D vertices. The ruled side surface pairs corresponding
vertex indices ring-to-ring, so a square at one section and a circle
at another would require either resampling or an explicit matching
mode. Mismatched counts produce a warning and an empty mesh.

Algorithm:
1. Evaluate each `<sketch>` to a `LoftSection` (3D-positioned profile).
2. Verify equal vertex counts across all sections.
3. Connect consecutive sections with quad strips (vertex `i` of section
   `k` to vertex `i` of section `k+1`).
4. Ear-clip the start and end sections as caps.

**Transparent group passthrough.** `<loft>` may contain `<group>`
wrappers without transforms (these are produced by `<for>` unrolling).
The evaluator descends through them transparently to find the actual
`<sketch>` children.

**For two-section convex transitions**, `<hull>` of two thin extrudes
is usually simpler than authoring a `<loft>`.

### 5.5 `<helix>`

```
<helix radius="..." pitch="..." turns="..." [taper="0"] [direction="ccw"]/>
```

A helical guide curve. Standalone `<helix>` produces no geometry — it
is only meaningful as a child of `<sweep>`. A bare `<helix>` outside a
sweep produces a warning.

Parameterisation:
```
x(t) = (radius + taper * t) * cos(2π * t * dir)
y(t) = (radius + taper * t) * sin(2π * t * dir)
z(t) =  pitch * t,                            t ∈ [0, turns]
```

`dir` is `+1` for `ccw`, `-1` for `cw`. `taper` adds a linear radius
ramp (useful for tapered springs).

---

## 6. Booleans

```
<union> | <difference> | <intersect>
  <child mesh/>
  <child mesh/>
  ...
</element>
```

Algorithm: each child is evaluated to a `FlatMesh`. The meshes are
converted to Manifold solids, the boolean operation is applied via
Manifold's CSG kernel, and the result is converted back to a `FlatMesh`.

**Operation semantics:**

- `<union>`: combine all children. Commutative.
- `<difference>`: subtract the 2nd, 3rd, ... children from the 1st.
  *Not* commutative — order matters.
- `<intersect>`: keep only the geometry present in all children.

**Manifoldness:** all inputs to a boolean must be manifold (every edge
shared by exactly two faces, no self-intersections). Non-manifold
input causes Manifold to return an empty mesh; the engine surfaces
this as an `Internal` error.

**Coplanar-face failure.** Boolean operations between two solids
whose faces are exactly coplanar (e.g., a cutter that ends precisely
at the cuttee's face) produce a degenerate result — Manifold cannot
classify the coplanar region as "inside" or "outside". The remedy is
**overshoot**: extend the cutter past the cuttee's face by at least
1 mm. This is a convention, not validated by the engine. See
[`csg.md`](csg.md) for the full story.

**Per-triangle attribution.** The bundler tags every triangle with the
`Node` index of its originating source element. Boolean operations
preserve this tagging: a triangle in the union of `A` and `B` carries
the source-node from whichever input it came from. Tri-divided
triangles (where a cutter cleaves a single triangle into two) get the
source of the *cuttee* (the geometry that "survived").

---

## 7. `<hull>`

```
<hull>
  <child mesh/>
  <child mesh/>
  ...
</hull>
```

Convex hull of the union of all children's vertices. Implemented via
Manifold's hull algorithm.

Useful for capsule-like geometry: two thin extrudes at different `z`
levels, hulled, yields a rounded prism. Cheaper than authoring a
`<loft>` for two-section convex transitions.

Degenerate inputs (a single point, collinear points, an empty input)
produce a warning and an empty mesh.

---

## 8. Modifiers

The four modifiers — `<fillet>`, `<chamfer>`, `<shell>`, `<cut>` — are
**Tier 1**: they accept a restricted set of inputs (extrude of a
convex 2D profile, revolve of a profile curve, or a `<union>` of
either). They do not accept arbitrary CSG results. Apply them
*before* booleans, not after.

### 8.1 `<fillet radius="..." [select="all"]>`

Rounds edges of the contained geometry by `radius`. `select` defaults
to `"all"`; future versions will accept face/edge selectors.

Tier 1 only handles **90° corners**. Corners at other angles (within
±5° tolerance: 87°–93° is fine, anything outside is rejected) produce
an error.

### 8.2 `<chamfer distance="..." [angle="45"] [select="all"]>`

Bevels edges of the contained geometry. `distance` is measured
**perpendicular to the original edge** (not along the face normal).

### 8.3 `<shell thickness="..." [open="..."]>`

Hollows the contained geometry, leaving walls of thickness `thickness`.
The input must be a single closed surface (`<extrude>` of a convex
2D profile, or a `<revolve>` of a profile curve).

`open="start"` removes the start-cap (the `z=0` face for an extrude);
`open="end"` removes the end-cap. Both faces remain by default.

The full selector grammar from spec §13 is deferred — only `start` and
`end` are accepted in 0.1.

### 8.4 `<cut>`

A miter / bevel / compound cut. Lowered by the bundler to a
`<difference>` against a wedge cutter; the evaluator only sees the
resulting difference. See [`compiler.md`](compiler.md) §2.5 for the
lowering algorithm.

---

## 9. `<group transform="...">`

A transparent grouping element that applies an affine transform to its
children. The transform string is a chain of commands separated by
whitespace:

| Command | Effect |
|---|---|
| `translate(x, y, z)` | Translation |
| `rotate(angle, ax, ay, az)` | Rotation by `angle` degrees around axis `(ax, ay, az)` |
| `scale(sx, sy, sz)` | Per-axis scale (use `scale(s, s, s)` for uniform) |

Commands compose **left-to-right** — leftmost is applied last to the
local coordinate. So `translate(10, 0, 0) rotate(90, 0, 0, 1)` first
rotates the child by 90° around Z, then translates the rotated child
by 10 along X.

**Rotation convention:** angles are in **degrees**. The right-hand rule
applies: curl the right hand's fingers around the axis in the direction
of rotation, the thumb points in the positive axis direction.

A `<group>` with no `transform` attribute is a **transparent** group —
its children are emitted as if the group were not there. The bundler
uses transparent groups to wrap `<for>`-unrolled subtrees; the
evaluator descends through them when looking for type-specific
children (e.g., inside `<loft>`).

---

## 10. `<svg>`

```
<svg>
  <path d="..."/>
  <rect x="..." .../>
</svg>
```

An SVG-coordinate-system wrapper. Inside `<svg>`, descendant geometry
is implicitly transformed by `scale(1, -1, 1)` — Y axis flipped — so
that SVG paths copy-pasted from a design tool render right-side-up in
CADML's y-up convention.

The Y flip has two side-effects:
1. Face winding inverts (CW → CCW), so the engine reverses triangle
   winding post-transform to restore the convention.
2. The CW-input warning that fires elsewhere is silenced inside
   `<svg>` — y-down CW paths are *expected* here.

---

## 11. Param / Port elements

`<param>` and `<port>` are metadata-only at evaluation time. The
evaluator skips them (they produce no geometry). Their values have
already been resolved by the bundler.

`<port>` positions are surfaced in `FlatEvalResult::node_world_transforms`
so tools can compute port world-space positions for things like
assembly visualisation or measurement.

---

## 12. Instance dispatch

```
<my-def-name attr1="..." attr2="..."/>
```

After bundling, instance references (`<my-def-name/>`) have been
expanded inline — the `<def>` body has been copied into the instance's
location, with any attribute overrides applied to the corresponding
`<param>` elements inside the def body.

The evaluator therefore never sees an unresolved instance reference.
If one appears in a flat document, it's a bundler bug: the evaluator
emits an `Internal` error.

---

## 13. Error model

Each evaluator dispatch returns either a mesh or a sentinel (empty
optional / empty mesh). On the way, it may push to `warnings` or
`errors`:

- **Warnings** are non-fatal. The element returned empty (or a
  partial result), but evaluation continues — the part will be
  rendered with whatever geometry the other elements produced.
- **Errors** are fatal for the affected part. The part's mesh will
  be empty in the output; subsequent siblings still evaluate.

Specific warning conditions seen in practice:
- `extrude has no 2D profile child`
- `revolve has no 2D profile child` (typically because someone wrapped
  the profile in `<sketch>` — see [`implementation-notes.md`](implementation-notes.md))
- `sweep: missing 2D profile child`
- `loft: section N has X vertices but section 0 has Y; all sections
  must have the same vertex count`
- `extrude: profile polygon is wound clockwise` (the polygon is
  silently reversed, but the warning surfaces the unusual case)
- `path: tessellated to fewer than 3 points`
- `boolean produced empty mesh` (overshoot missing, or a child was
  non-manifold)

Specific error conditions:
- `revolve profile crosses axis` (profile has `x < 0` for `axis="z"`)
- `boolean kernel failure` (Manifold internal — usually non-manifold
  input)
- `fillet: corner angle X° not in [85°, 95°]`
- `internal: unresolved instance` (bundler bug)

See [`error-model.md`](error-model.md) for the full taxonomy.

---

## 14. Source-map flow

Each `FlatMesh` carries a `triangle_node` vector parallel to
`triangle_count()`, recording the `Node` index that produced each
triangle. The mapping flows through every pass:

- **Primitives** stamp every triangle with the primitive's own node
  index.
- **Extrude / revolve / sweep / loft / hull** stamp every triangle with
  the containing 3D-op's node index.
- **Booleans** preserve the source node of whichever input contributed
  each triangle (the cuttee, for `<difference>`-style operations on
  the boundary).
- **Group transforms** don't change the source attribution — they only
  transform vertex positions.

This is what enables click-to-source navigation in editors: pick a
triangle, look up its `triangle_node` → look up the node's
`SourceRange` → open that file at that line.

---

## 15. Performance notes

The reference evaluator is **single-threaded** for correctness — many
of the primitive tessellators have small temporary state that's not
trivially shareable. Per-element parallelism (each `<part>` on its own
thread) is safe and is what tooling typically does.

The **mesh cache** (`EvalOptions::cache`) is an opt-in incremental
mode: callers pass a `FlatMeshCache` that the evaluator consults
before re-tessellating a primitive. Cache keys include the element's
attribute values and child subtree hashes; a hit reuses the prior
mesh. The cache survives across `evaluate_flat` calls, so a UI that
re-evaluates the document on every edit sees subsecond rebuilds for
unchanged sub-trees.

The **Manifold CSG library** brings its own internal parallelism (via
TBB). Callers who need bit-exact reproducibility across runs should
disable Manifold parallelism (`MANIFOLD_PAR=NONE` at Manifold build
time).
