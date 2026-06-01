# Implementation Notes

Gotchas, known limits, spec-vs-engine deltas, and "things an
implementer needs to know that aren't in the formal spec". Read this
*after* the language spec, the architecture overview, and the compiler
+ evaluator docs — it assumes you know the shape of the pipeline.

This document is **descriptive of the reference implementation**, not
normative for CADML the language. A from-scratch implementation may
choose differently on any of these; this is the place to record the
choices the reference made and why.

---

## 1. Spec-vs-engine deltas

These are places where the formal language spec aspires to more than
the reference implementation currently delivers. If you're
reimplementing, you can choose to match the spec (close the gap) or
the engine (keep parity).

### 1.1 `<sketch>` is only honoured inside `<loft>`

The spec ([language.md](spec/language.md) §5.3) describes `<extrude>`,
`<revolve>`, and `<sweep>` as accepting either a 2D primitive
**or** a `<sketch>` wrapping a 2D primitive (for explicit plane
control).

**Engine reality:** the reference evaluator's `first_2d_child`
helper only recognises `<rect>`, `<circle>`, and `<path>` as
profile children. A `<sketch>`-wrapped profile is invisible to it,
so the op produces no geometry and emits:

```
warning: revolve has no 2D profile child
```

**Workaround for authors:** use the primitive directly. For
`<revolve axis="z">`, the profile is already interpreted in the
(radial, axial) plane — no `<sketch>` plane wrapper is needed.

**Fix for implementers:** add a `<sketch>` case to the 2D-child
finder in `eval_3d` for `<extrude>` / `<revolve>` / `<sweep>`. It
needs to evaluate the sketch and return both the 2D polygon and the
3D frame; today only `<loft>`'s code path does this.

### 1.2 Cut lowering has a "stays-edge" sign convention

The spec ([language.md](spec/language.md) §12.5) describes `<cut>`
ergonomically but defers the geometric details to the engine. The
engine's lowering algorithm has a sign-convention rule:

> For `face="end"` with positive angle, the stays-edge is the edge
> nearest `+face`. Negative angle flips this.

This is the #1 source of "the miter is the wrong way around" bugs.
When in doubt, render in wireframe (`set_render_mode wireframe=true`)
and visually verify the edge that survived.

### 1.3 Loft section vertex-count must match exactly

The spec ([language.md](spec/language.md) §5.3) says loft sections
"must have matching vertex counts" with a footnote about future
resampling support. Today there is no resampling — mismatched counts
produce a warning and empty mesh.

If you generate sections from Lua, sample at the same `n` for every
section:

```lua
function blade.section(t, n)        -- n is the shared vertex count
  ...
end
```

### 1.4 Modifier inputs are Tier 1 only

The spec ([language.md](spec/language.md) §12.1) describes a
"Tier 1 / Tier 2" distinction: Tier 1 modifiers (`<fillet>`,
`<chamfer>`, `<shell>`, `<cut>`) only accept restricted inputs —
`<extrude>` of a convex profile, `<revolve>` of a profile curve, or
a `<union>` of either. Tier 2 (modifiers on arbitrary CSG results)
is deferred.

**Practical impact:** apply modifiers *before* booleans, not after.
A `<fillet>` of a `<difference>` errors today; the workaround is
to fillet the inputs and difference the result.

### 1.5 `<shell open="...">` selector grammar deferred

The spec ([language.md](spec/language.md) §13) describes a rich
selector grammar (`select="convex.top"`, `select="all.outer"`)
for modifiers. Today only `select="all"` and `open="start"` /
`open="end"` are recognised.

---

## 2. Boolean overshoot is not optional

`<difference>` with the cutter ending exactly at the cuttee's face
produces coplanar surfaces the CSG kernel can't classify. Symptoms:
the part renders fine *most* of the time, then mysteriously becomes
empty when you reload, or generates phantom non-manifold edges.

**Rule:** every `<difference>` cutter extends past the target by at
least 1 mm on each face. Declare `param overshoot = 1` in the
frontmatter and use it consistently. See [`csg.md`](csg.md) §3 for
the full story.

---

## 3. Units are millimetres unless declared

`units mm` (or `units in`, `units cm`, …) in the frontmatter is a
setting. There is no `units` attribute on any element. Pick once in
frontmatter, stick with it for the whole file. Lua expressions inside
`{…}` use the unit as-is — no automatic conversion.

---

## 4. Lua and the XML parser

`if x < y then …` inside an inline `<script lang="lua">` block breaks
the XML parser because `<y` looks like an opening tag. Wrap script
bodies in CDATA whenever they contain `<`, `>`, or `&`:

```xml
<script lang="lua"><![CDATA[
  if x < y then ... end
]]></script>
```

Better still: put Lua in its own `.lua` file and `import "x.lua" as x`
in the frontmatter. External modules don't have the CDATA trap and are
easier to test + reuse.

---

## 5. There is no `<defs>` wrapper

`<def>` and `<script>` are top-level body siblings, not children of a
`<defs>` element. Just write them at the top of the body:

```xml
<def name="m6-hole">…</def>
<script lang="lua"><![CDATA[ … ]]></script>

<part name="plate">…</part>
```

`<defs>` is **not a CADML element**. Writing `<defs>…</defs>` is a
parse error.

---

## 6. Defs are referenced by writing their name, not via `<use>`

`<def name="m6-hole">` is invoked as `<m6-hole/>`, not
`<use ref="m6-hole"/>`. There is no `<use>` element. The def's name
*becomes* a valid element name inside the same file (and across files
once imported).

```xml
<def name="m6-hole">
  <extrude height="{plate-t + 2}">
    <circle r="3.3"/>
  </extrude>
</def>
<part name="plate">
  <difference>
    <extrude height="{plate-t}">…</extrude>
    <group transform="translate( 30,  30, -1)"><m6-hole/></group>
    <group transform="translate(-30,  30, -1)"><m6-hole/></group>
  </difference>
</part>
```

---

## 7. Negative attribute values: two equivalent forms

`<rect x="{-w/2}" .../>` and `<rect x="-{w/2}" .../>` both work. Pick
one and stick with it within a file.

---

## 8. Cut + rotation = sign flip

The `<cut>` element operates in the child's local frame. When you
`rotate(90, 0, 1, 0)` to lay a tube along X, local +X maps to world
-Z. A positive `angle="45"` keeps the local -X edge (= world +Z, the
top). If you wanted to keep the bottom edge you need *negative* 45.

When in doubt, render in wireframe and visually verify.

---

## 9. Sketch normals matter for `<loft>` blades

A `<loft>` between sketches on a curved hub computes `right = up ×
normal`. The blade span extends along `right`. For a hub meridional
curve, set `<sketch normal="{-tx} {-ty} {-tz}">` (negated tangent
components) so the blade extends outward away from the hub, not
into it.

---

## 10. Revolves default to 32 segments

Looks blocky on screen for parts with visible curvature.
`<revolve axis="z" angle="360" segments="180">` is usually enough for
CAD work; 360 if curvature still shows up faceted at the viewing
scale. Cost is linear in segment count.

---

## 11. Assembly ports need both `normal` AND `up`

`<port>` with only `position` + `normal` gives the engine a degree of
rotational freedom (rotation around the normal axis). Add `up` to
fully constrain orientation:

```xml
<port name="shaft-out"
      position="0 0 {body-l}"
      normal="0 0 1"
      up="1 0 0"/>
```

Missing `up` is an error in the bundler (it surfaces as a
`Composition` failure when the port is referenced from a mating
instance).

---

## 12. Def-instance param overrides

When a def is parameterised and you instantiate it with attributes:

```xml
<def name="circular-hole">
  <param name="r" default="3"/>
  <extrude height="{plate-t + 2}">
    <circle r="{r}"/>
  </extrude>
</def>
<part name="plate">
  <difference>
    …
    <circular-hole r="5"/>     <!-- override default for this instance -->
  </difference>
</part>
```

The `r="5"` attribute on the instance overrides the def's default
`r` for that instance only. Other instances still see the default.

---

## 13. File paths are relative to the file doing the `import`

`import "b.cadml"` from `parts/a.cadml` looks for `parts/b.cadml`,
not `b.cadml` from the workspace root.

| Path shape | Resolves against |
|---|---|
| `ctl/aero/airfoils.lua` | `<entry-dir>/ctl/…` (catalogue / stdlib) |
| `./helper.cadml` or `../shared/x.cadml` | The importing file's dir |
| Bare `shared/x.cadml` | The importing file's dir |

---

## 14. Boolean order matters

`<difference>` subtracts the 2nd, 3rd, … children from the 1st.
`<union>` and `<intersect>` are commutative *value-wise* but the
output ordering of triangles still depends on input order (visible in
`triangle_node` attribution and in any per-triangle property like
color).

---

## 15. `<for>` inside `<loft>` expands sketches

```xml
<loft>
  <for var="i" from="0" to="3" steps="4">
    <sketch plane="xy" origin="0 0 {i*25}">…</sketch>
  </for>
</loft>
```

works — the `<for>` becomes four `<sketch>` siblings at z = 0, 25,
50, 75 before flat-eval runs. `<for>` is compiled away; it doesn't
appear in `.fcadml`.

---

## 16. Loft cross-sections must have matching vertex counts

(Same as §1.3 — repeated here because users hit it via different
paths.)

`<loft>` builds ruled side faces by pairing corresponding vertex
indices between successive sections. Sections with different vertex
counts produce a warning and an empty mesh.

For 2-section convex transitions, `<hull>` of two thin extrudes is
usually simpler than authoring a `<loft>`.

---

## 17. Manifold-ness can fail silently

The mesh renders; the slicer chokes. Run a topology analysis
(`MeshStats::is_watertight`) and check `is_manifold` before declaring
victory on a print-bound part.

---

## 18. Mass needs an explicit density

Mass computation requires `density_kg_per_m3`. Common defaults:

- PLA: 1240
- PETG: 1270
- ABS: 1040
- Aluminium 6061: 2700
- Steel: 7850
- Brass: 8500

Don't guess — ask the user if material isn't obvious from the file.

---

## 19. Self-write file-watcher loops

A naive file-watcher reload triggers on every save — including saves
a tool itself made (e.g. a programmatic write-back of a `.cadml` from
an editor or automation layer). Without a self-write suppression
window, every such save triggers a duplicate compile + flat-eval, and
two tools writing the same file can ping-pong.

A workable mechanism is a short post-write suppression window
(~250 ms) keyed by file path: a change noticed within that window is
attributed to the tool's own write and skipped. Any watch-based UI
built over the engine will need something similar.

---

## 20. Don't use Lua when builtins suffice

Lua is the escape hatch for things builtins *can't* express:

- Involute gear teeth (need parametric curve evaluation).
- NACA airfoils (formula-driven cross-section).
- Backswept blade plan views (closed-form trig).
- Cam profiles (angle → radius function).

Lua is the **wrong** tool for:

- Composing static primitives — use `<for>` + builtins.
- Setting up a transform — use `<group transform="...">`.
- Conditional inclusion — use a param + multiple branches; CADML
  has no `<if>`, but a part with `param show-bore = 1` can
  multiplied by 0/1 to no-op a `<difference>`.

When a file accumulates `<script>` blocks for stuff `<extrude>` + a
`<for>` could do, it's a smell — refactor to drop the Lua.
