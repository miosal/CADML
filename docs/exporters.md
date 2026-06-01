# Exporters — STL, 3MF & glTF

This document specifies the three mesh exporters in the reference
implementation: **binary STL** (for slicers / printers, lowest common
denominator), **3MF** (3D Manufacturing Format, the successor to STL
for slicer / printer handoff), and **glTF/glB** (for browsers, viewers,
click-to-source integration). All three consume a `FlatEvalResult`
from the evaluator.

---

## 1. Binary STL

### 1.1 Format

The binary STL format (de-facto standard, not formally specified):

```
[ 80 bytes ]   ─ header string. ASCII text, zero-padded.
                Convention: "CADML" or "CADML: <filename>".
[ 4 bytes ]    ─ uint32 little-endian triangle count.

For each triangle:
  [ 12 bytes ] ─ float[3] normal (face normal, not vertex normals).
  [ 12 bytes ] ─ float[3] vertex 0.
  [ 12 bytes ] ─ float[3] vertex 1.
  [ 12 bytes ] ─ float[3] vertex 2.
  [ 2 bytes ]  ─ uint16 attribute (unused; zero).
```

All floats are IEEE 754 single-precision, little-endian. All ints are
little-endian.

Total file size: `84 + 50 * triangle_count` bytes.

### 1.2 Header conventions

The 80-byte header is officially "arbitrary text". CADML writes:

- **Default**: `"CADML"` followed by 75 zero bytes.
- **With source file**: `"CADML: <basename>"` truncated/padded to 80
  bytes.

The `"solid "` prefix that ASCII STL uses **must not** appear at the
start of the header — some readers detect ASCII vs binary STL by
checking for that prefix. The reference implementation avoids it.

### 1.3 Per-face normals

STL stores **per-face** normals, not per-vertex. The reference
implementation **recomputes** each face normal from the geometry
rather than trusting the per-vertex normals carried in `FlatMesh`:

```cpp
Vec3 n = (v1 - v0).cross(v2 - v0).normalized();
```

Reasons:
- Per-vertex normals in `FlatMesh` may be averaged (for shading) and
  don't necessarily equal the face normal.
- Slicers care about the **geometric** face normal — they need to
  know which side of the triangle is "outside" the part.
- Per-face normals are cheap to compute (a single cross product per
  triangle).

### 1.4 Vertex precision

STL uses `float` (32-bit) for all coordinates. `FlatMesh` uses
`Vec3` of doubles internally; the exporter casts down at write time.
This loses precision for very large or very small parts, but most
slicers operate in `float` anyway.

### 1.5 Multi-part merging

`FlatEvalResult` can carry multiple `<part>`s. Binary STL has **no
concept of separate parts within a file** — the format is just a
flat triangle stream. The reference exporter merges all parts into
one stream:

```cpp
write_stl_binary(eval_result, path) {
  // Compute total triangle count across all parts.
  size_t total = 0;
  for (auto& p : eval_result.parts) total += p.mesh.triangle_count();

  // Write header + count.
  write_header(out, "CADML");
  write_uint32(out, total);

  // Per-part: write each triangle.
  for (auto& p : eval_result.parts) {
    for (auto& tri : p.mesh.triangles()) {
      write_triangle(out, tri);
    }
  }
}
```

Slicers receiving the file see one merged solid. They don't lose
anything semantically, because slicing operates on the geometry
boundary regardless of part attribution.

If you need per-part STL files (e.g., for multi-material printing),
export each `Part` to its own file:

```cpp
for (size_t i = 0; i < eval_result.parts.size(); ++i) {
  std::string fname = name + "_" + parts[i].name + ".stl";
  write_stl_binary_single_part(eval_result.parts[i], fname);
}
```

### 1.6 ASCII STL

ASCII STL is **not produced** by the reference implementation.
Reasons:
- Files are 5–10× larger.
- Slicers all accept binary STL; few production tools prefer ASCII.
- The locale-dependent decimal-point convention (`,` vs `.`) is a
  perennial source of import errors.

If you need ASCII STL for debugging or version control, write a
post-processor that reads binary STL and pretty-prints it.

---

## 2. 3MF (3D Manufacturing Format)

### 2.1 Format

3MF is the successor to STL for slicer / printer handoff,
maintained by the 3MF Consortium and supported natively by
PrusaSlicer, Bambu Studio, OrcaSlicer, Cura, and SuperSlicer. The
container is an [OPC](https://en.wikipedia.org/wiki/Open_Packaging_Conventions)
ZIP package; inside the ZIP, the geometry lives in
`3D/3dmodel.model` (an XML document conforming to the 3MF core spec).
Two siblings — `[Content_Types].xml` and `_rels/.rels` — are
boilerplate that every OPC reader expects.

```
my-part.3mf  (ZIP archive)
├── [Content_Types].xml        ← OPC content-type registry
├── _rels/.rels                ← OPC root relationships
└── 3D/3dmodel.model           ← the actual mesh XML
```

### 2.2 What 3MF gives you over STL

- **Per-part separation.** Each CADML `<part>` becomes its own
  `<object>` in the model; slicers see N distinct bodies, not one
  triangle soup. Multi-material printing, per-object slicer
  settings, and per-part assembly review all work without manual
  splitting.
- **Per-part color.** A part's authored `color` attribute is mapped
  onto a `<basematerials>` entry referenced by `pid` + `pindex`. The
  exporter deduplicates identical color strings so a 14-part assembly
  with three distinct colors emits three base materials, not
  fourteen.
- **Vertex sharing.** 3MF expects one entry per unique vertex
  (`<vertex x="..."/>`), unlike STL's per-triangle duplication. A
  typical part exports at ~⅓ the vertex count.
- **Document units.** The model's `unit="..."` attribute records the
  authored `units` setting from the entry file. CADML's
  `mm`/`cm`/`m`/`in`/`ft` map directly to 3MF's
  `millimeter`/`centimeter`/`meter`/`inch`/`foot`.
- **Metadata.** The `<metadata name="Title">` element carries a
  human-readable title (default: the entry file's stem); future
  revisions may add `Description`, `Author`, etc.

### 2.3 What's deferred

- **3MF Materials extension** (PBR, textures, finishing). Base colors
  only in 0.1.
- **3MF Production extension** (object UUIDs for replaceable parts).
- **Per-triangle material assignment.** Would require splitting a
  part along its `face_groups` boundaries; not justified for the v0.1
  per-part model.

### 2.4 CLI

```bash
cadml3mf <entry.cadml> -o <out.3mf> [options]
```

Options:
- `-o <path>` — 3MF output path (required).
- `--title <text>` — `<metadata name="Title">` value. Default: the
  entry filename's stem.
- `--units <unit>` — override the units recorded in the 3MF header
  (`mm`/`cm`/`m`/`in`/`ft`). Default: the authored `units` setting
  from the entry file.

### 2.5 Determinism scope

CADML promises byte-identical output for byte-identical input across
the bundler → engine → exporter pipeline. For 3MF that promise lives
on the **3D/3dmodel.model payload**, not on the ZIP container around
it.

The ZIP format records a per-entry modification timestamp in each
local-file-header and central-directory-header — by design of the
archive format. miniz (the ZIP writer we use) stamps these from
wall-clock time, so two adjacent runs of `cadml3mf` produce
`.3mf` files whose CONTAINER bytes differ in those fields. That
drift is intrinsic to ZIP and not a bug to scrub out.

Anyone hashing 3MF output for content addressing, build caching,
or supply-chain attestation should extract the
`3D/3dmodel.model` entry and hash that. The example regression
suite (`tests/examples/golden-3mf-hashes.txt`) does exactly this —
see `test_3mf_golden.cpp` for a worked example.

The OPC scaffolding parts (`[Content_Types].xml`, `_rels/.rels`)
are constant string literals; they're identical across runs.

### 2.6 Pipeline pseudocode

```
write_3mf(eval_result, opts) {
    1. validate every part's mesh
       (indices % 3 == 0, indices in range, finite coords)
    2. emit 3D/3dmodel.model XML:
         <model unit=opts.units>
           <metadata name="Title">opts.title</metadata>
           <resources>
             <basematerials id="1"> base entries for each unique color </basematerials>
             <object id="2..N+1" type="model" name="<part>" pid pindex>
               <mesh>
                 <vertices> one per unique vertex </vertices>
                 <triangles> one per face </triangles>
               </mesh>
             </object>
             ...
           </resources>
           <build>
             <item objectid="2"/> ... <item objectid="N+1"/>
           </build>
         </model>
    3. package via miniz heap-writer:
         [Content_Types].xml, _rels/.rels, 3D/3dmodel.model
    4. stream the ZIP bytes to out
}
```

Internal validators throw `std::runtime_error` on malformed input —
same fail-fast policy as STL.

---

## 4. glTF / glB

### 4.1 Format

glTF 2.0 is the Khronos Group's standard 3D-asset format. Two flavours:

- **glTF (`.gltf`)** — JSON + external `.bin` buffer files + external
  image files. Human-readable, easy to diff, multi-file.
- **glB (`.glb`)** — single binary file with embedded JSON +
  buffer + images. Easier to ship, harder to diff.

The reference implementation writes both via the same code path,
selected by file extension.

### 4.2 Structure

```
glTF document:
├── nodes: per-part nodes (transform = identity; geometry in mesh).
├── meshes: one mesh per part.
├── materials: per-part materials with the color from <part color="...">.
├── accessors: typed views into the buffer (vertex positions, indices,
│              and a NORMAL attribute when the engine produced one).
├── bufferViews: byte ranges within buffers.
└── buffers: raw binary data (vertex / index / image bytes).
```

Each part becomes one **mesh primitive**. The reference implementation
uses a single buffer (one binary blob) for the entire file.

### 4.3 Per-vertex normals

The exporter emits a `NORMAL` accessor only when the engine produced
non-zero per-vertex normals for the mesh. The 0.1 primitive
constructors leave `FlatMesh::normals` populated with zero vectors,
and the exporter detects that case and skips the NORMAL attribute
entirely — consumers then fall back to face normals (flat shading).
Re-enabling smooth shading is on the 0.2 roadmap; until then, viewers
that prefer smooth shading should compute the normals themselves from
the triangle data.

### 4.4 Materials (part color)

A `<part>`/group `color` attribute is exported as a glTF **material**.
Distinct colors are de-duplicated — one entry each in the top-level
`materials` array — and every primitive drawn from a colored part
references its material by index. The color (a CSS name like `red` or a
`#RGB`/`#RRGGBB` hex, resolved by the same `parse_color_rgba` the STL/3MF
paths use) is written as `pbrMetallicRoughness.baseColorFactor` (RGBA,
`0..1`). Since `baseColorFactor` is defined in **linear** space while
CADML colors are sRGB, the RGB channels are converted sRGB→linear on
export (alpha passes through). A part with no color, or a color that
doesn't resolve, gets no material and renders with the viewer's default.

### 4.5 Source attribution in `extras`

Each glTF node carries `extras` with the source location:

```json
{
  "name": "bolt",
  "mesh": 0,
  "extras": {
    "source": 0,        // file_id from the <sources> table
    "line": 17,         // line in the source file
    "node_index": 5     // index in the flat Document's nodes vector
  }
}
```

Per-triangle attribution is not exposed at the glTF level (glTF has
no per-triangle metadata), but per-part attribution is. Click-to-
source navigation works at the part level: pick any triangle in a
part, look up the part's `extras.source` + `extras.line`.

### 4.5 Color

Per-part colors from `<part color="...">` are emitted as glTF
materials. The default PBR shading model is used:

```json
{
  "pbrMetallicRoughness": {
    "baseColorFactor": [r, g, b, 1.0],
    "metallicFactor": 0.0,
    "roughnessFactor": 0.7
  }
}
```

CSS-named colors (`red`, `steelblue`, etc.) are resolved at export
time. `#RRGGBB` hex colors are converted to linear sRGB.

### 4.6 When to use which

| Need | Use |
|---|---|
| Slicing for 3D printing | STL |
| Loading in a browser viewer (Three.js, Babylon, model-viewer) | glB |
| Diffable in version control | glTF (multi-file) |
| Click-to-source in a custom editor | glTF or glB (both carry `extras`) |
| Shipping to a CAM toolchain | STL (universal), or STEP/IGES via a different tool |

---

## 5. Adding new exporters

If you want to add e.g. STEP, OBJ, or 3MF, the integration points are:

1. **Read `FlatEvalResult::parts`** — that's the canonical mesh
   source.
2. **Per part**: read `name`, `color`, `mesh.{vertices, normals,
   indices, triangle_node}`.
3. **If your format supports source attribution** (3MF does, via
   metadata): emit the `SourceMap`-relative file IDs + line numbers
   via `triangle_node` → `Document::nodes[i].source`.
4. **Write to either a `std::ostream` or a `std::filesystem::path`** —
   match the existing two-overload style.

The reference implementation's STL and glTF writers are good
templates; copy one and modify.

---

## 6. Reimplementation tips

- **Validate inputs early.** Check that `mesh.indices.size() % 3 == 0`
  before writing the first triangle. Check that every index is
  in-range. Better to error before you've written half a file.
- **Use little-endian explicitly.** Some platforms (rare today) are
  big-endian; assume nothing about host byte order. The reference
  implementation uses `memcpy` of pre-laid-out structs.
- **Test against a slicer.** The reference implementation has an
  integration test that runs `prusa-slicer --slice` on each example
  output. Without that, byte-level format compliance can pass while
  the result is geometrically nonsense.
- **Per-face normal direction matters.** STL doesn't carry "this is
  the outside" explicitly — the per-face normal IS the outside
  direction. If you compute normals as `(v1-v0).cross(v2-v0)` you
  rely on CCW winding being correct; the engine guarantees this via
  the winding-canonicalisation pass.
