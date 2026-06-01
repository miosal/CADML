# CSG — Constructive Solid Geometry

CADML's booleans (`<union>`, `<difference>`, `<intersect>`) and
`<hull>` are implemented via the [Manifold](https://github.com/elalish/manifold)
library — a mesh-based CSG kernel that operates on triangle meshes and
guarantees manifold output.

This document covers Manifold's contract, manifoldness invariants, the
coplanar-face failure mode (and why "overshoot" matters), and how the
reference implementation preserves per-triangle attribution across
boolean operations.

If you're reimplementing CADML, you have three reasonable choices for
the CSG kernel:
- **Use Manifold** — the reference choice. Apache 2.0, ~10× faster
  than CGAL on most workloads.
- **Use a B-rep kernel (OpenCascade, ACIS)** — much heavier dep, but
  exact-arithmetic results.
- **Roll your own polygon-clipping CSG** — possible for simple inputs
  (axis-aligned boxes), pathological in general.

This document describes the reference implementation's choices around
Manifold.

---

## 1. Manifoldness

A mesh is **manifold** if:
- Every edge is shared by **exactly two** triangles. No naked edges
  (shared by one), no T-junctions (shared by three).
- The surface has **no self-intersections**.
- The surface is **closed** (in 3D, has a well-defined inside/outside).

Manifold's CSG operations require manifold input. Non-manifold input
produces an empty result with an internal error.

The reference implementation produces manifold output from every
primitive constructor:
- `<extrude>` of a simple polygon → closed prism, manifold by
  construction.
- `<revolve>` of a non-axis-crossing profile → closed surface,
  manifold.
- `<sweep>` along a non-self-intersecting helix → manifold.
- `<loft>` between matching cross-sections → manifold.

It can produce non-manifold output if:
- A `<path>` profile is **self-intersecting** (figure-eight).
- A `<revolve>` profile **crosses the axis** (rejected with an error).
- A `<sweep>` helix self-intersects (e.g., very tight spiral).
- A user-supplied mesh has duplicated or missing triangles (only
  possible via non-CADML inputs to the engine).

---

## 2. Boolean operations

Manifold provides:

```
Manifold::Boolean(other, OpType)  → Manifold
where OpType = Add | Subtract | Intersect
```

CADML's elements map directly:

| CADML | Manifold |
|---|---|
| `<union>` | `Add` |
| `<difference>` | `Subtract` (1st child minus the rest) |
| `<intersect>` | `Intersect` |

For `n > 2` children, the operations are left-folded:

```
<union><A/><B/><C/></union>          → A.Add(B).Add(C)
<difference><A/><B/><C/></difference> → A.Subtract(B).Subtract(C)
<intersect><A/><B/><C/></intersect>   → A.Intersect(B).Intersect(C)
```

This is associative for `<union>` and `<intersect>` (the order
doesn't affect the final mesh shape, only triangulation). It is
**not commutative for `<difference>`** — the first child is the
subject, the rest are cutters.

---

## 3. The coplanar-face failure mode

The classic CSG pathology: two faces that are **exactly coplanar**
cannot be classified as "inside" or "outside" of each other. Manifold
detects this and refuses to produce a result (returning an empty
mesh).

Concrete example:

```xml
<difference>
  <extrude height="10">              ← cuttee, top face at z=10
    <rect width="20" height="20"/>
  </extrude>
  <extrude height="10">              ← cutter, top face at z=10  ← coplanar
    <circle r="3"/>
  </extrude>
</difference>
```

Both meshes have a face at `z=10`. Manifold's CSG can't decide which
"side" of the cutter's top face is inside vs outside the cuttee
(because the cutter's top face *is* on the cuttee's top face). Result:
empty mesh.

### 3.1 Overshoot — the standard remedy

Extend the cutter beyond the cuttee's faces by a small amount on each
side:

```xml
param overshoot = 1

<difference>
  <extrude height="10">
    <rect width="20" height="20"/>
  </extrude>
  <group transform="translate(0, 0, {-overshoot})">   ← shift down
    <extrude height="{10 + 2*overshoot}">             ← extra height
      <circle r="3"/>
    </extrude>
  </group>
</difference>
```

Now the cutter spans `z ∈ [-1, 11]`, the cuttee spans `z ∈ [0, 10]`,
and there's no coplanar face. The cut hole comes out clean.

**Convention:** overshoot by **1 mm** on each face (or proportional
for very small parts). The choice is heuristic — anything > the
manifold tolerance (~1e-6 mm) works geometrically, but 1 mm has the
right *visual* margin (the overshoot doesn't show in slicer previews
because it's well inside both meshes).

### 3.2 Where coplanar problems hide

In simple cases, you can see whether the cutter sticks through. In
assemblies and parametric models, two surfaces can become
coplanar accidentally — for example:

- A part's top face at `z = plate-t`, another part's bottom face at
  `z = plate-t` after a `translate`.
- Two adjacent thread passes in a swept helix where the per-step
  axial increment exactly equals the pitch.

Defensive programming: parameterise with explicit overshoot, even
when you "know" the faces don't line up.

---

## 4. Hull

`<hull>` computes the convex hull of the union of all input vertices.
Manifold provides this directly:

```
Manifold::Hull(meshes)  → Manifold
```

Use cases:

| Pattern | Result |
|---|---|
| Two thin extrudes at different `z` | Rounded prism (capsule-like). |
| `<hull>` over `<for>` of small spheres | Smooth ribbon along the locus. |
| Hull of two `<sketch>`-positioned circles | Cleaner than a 2-section `<loft>` for convex transitions. |

Hull is **always manifold output**. Inputs with fewer than 4
non-coplanar vertices produce a degenerate (zero-volume) result;
the engine warns and returns empty.

---

## 5. Per-triangle attribution

Each triangle in a `FlatMesh` carries a `triangle_node` entry —
the index of the `Node` in the document that produced it. This enables
click-to-source navigation: pick a triangle in the viewer, look up its
`triangle_node`, look up the node's `SourceRange`, open the source
file at that line.

Booleans preserve this attribution by inheriting from inputs:

- **Triangles inside both meshes** (intersection region) → take the
  source from the *first* mesh.
- **Triangles in one mesh's interior but the other's exterior**
  (union-only region) → take the source from the mesh that survived.
- **Triangles cleaved by the cutter** (one large source triangle
  becomes two small ones along the cut plane) → both halves take the
  source of the cuttee (the geometry that "survived" intact in spirit,
  even though it got subdivided).

Manifold has a "MeshProperties" mechanism that propagates per-vertex
or per-face attributes through CSG operations. The reference
implementation uses this to carry the source-node ID through.

---

## 6. Performance

Typical hex-bolt build (4 booleans, ~12k triangles total):

```
[load_part] compile 8ms · eval 880ms · pack 1ms · upload 4ms · total 893ms
```

The vast majority of the time is in the **evaluator** — mostly the
boolean ops (the 32-turn helical thread sweep + boolean subtract from
the cylindrical shank).

Manifold uses TBB for parallelism by default. The default thread count
matches `std::thread::hardware_concurrency()`. For benchmarking
purposes, build Manifold with `MANIFOLD_PAR=NONE` to disable.

**Mesh cache.** The reference evaluator supports an opt-in
`FlatMeshCache` (see [`evaluator.md`](evaluator.md) §15) that
memoises primitive tessellation results. The cache key is the
element's attribute values + child subtree hashes. Cache hits skip
both tessellation and boolean re-evaluation for unchanged sub-trees,
making interactive editing subsecond even on large parts.

---

## 7. Numerical robustness

Manifold uses **exact predicates** for topology (point-in-tetrahedron
tests, segment-segment intersection classification) implemented via
the `clipper2` or similar exact-arithmetic library, even though
coordinates themselves are stored as `float`. This makes its booleans
robust against floating-point error in the inputs.

In practice this means:
- Two faces that *should* be coplanar (modulo floating-point noise)
  still produce a clean result if neither is *exactly* coplanar with
  the other. Tolerance ≈ `1e-6 mm`.
- Two faces that are *exactly* coplanar produce the failure described
  in §3 above.

So the overshoot trick works because it forces non-exact coplanarity.

---

## 8. Trade-offs

What you give up with mesh-based CSG vs. B-rep:

- **No analytic curves** in the output. A "perfect" 360° revolve of a
  semicircle produces a sphere, but the sphere is faceted (whatever
  `segments` was passed in). B-rep tools would store the sphere
  analytically and tessellate at view time.
- **No exact tangent info.** Fillet ops use the mesh's discretised
  normals; they're close to analytic but not exact.
- **Booleans return triangle soups.** The output mesh's triangulation
  doesn't reflect the original face structure cleanly — a planar face
  may get cut into many small triangles after a boolean.

What you gain:
- A few hundred lines of CADML-side code; the kernel is a library.
- Manifold's mesh-based CSG is ~10× faster than CGAL's
  arrangement-based CSG on typical CAD workloads.
- Output is already a triangle mesh; STL / glTF export is direct
  with no B-rep-to-mesh tessellation step.

The reference implementation has chosen this trade-off. If your
reimplementation needs B-rep fidelity (for downstream consumption by a
CAM toolchain that expects analytic surfaces), swap Manifold for
OpenCascade — the rest of the pipeline doesn't care which CSG kernel
is underneath.
