# Coordinate System & Units

This document specifies CADML's coordinate convention, unit handling,
and winding rules. These are the conventions every other doc in the
corpus relies on.

---

## 1. Axes

CADML uses a **right-handed, Z-up** coordinate system, matching the
standard mechanical-engineering convention:

```
        +Z (up)
         │
         │
         │___________ +X (right)
        /
       /
      /
     +Y (forward, away from viewer)
```

- `+X` — to the right.
- `+Y` — forward (away from the viewer in a default orthographic
  "front" view).
- `+Z` — up.

Rotations follow the **right-hand rule**: curling the right hand's
fingers around the axis in the direction of rotation, the thumb points
in the positive axis direction. So a positive rotation around `+Z` is
counter-clockwise when viewed from above.

---

## 2. Default `<sketch>` plane

The default plane for a `<sketch>` (when no `plane` or `normal`
attribute is given) is `xy`:

- Sketch normal = `+Z`.
- Sketch "up" = `+Y`.
- Sketch "right" = `+X`.

This means a default sketch lies flat on the ground plane, with the
profile drawn from a top-down view (looking down `-Z` at the `xy`
plane).

Other plane choices:

| `plane=` | Normal | Up | Right |
|---|---|---|---|
| `xy` | `+Z` | `+Y` | `+X` |
| `xz` | `+Y` | `+Z` | `+X` |
| `yz` | `+X` | `+Z` | `+Y` |

See [`evaluator.md`](../evaluator.md) §4 for the full sketch-frame
derivation rules (including explicit-normal override and the `up`
fallback when normal is parallel to `+Z`).

---

## 3. Units

The document's `units` setting (frontmatter line) declares the unit
for **all numeric attributes** in the file:

```
units mm
units cm
units in
units m
units ft
```

Default is `mm`. Once set, all numbers in the body (attribute values,
expression results, path coordinates) are interpreted in that unit.

There is **no per-element units override**. Mixing units within a
file is not supported — you'd have to do unit conversion in Lua and
pre-multiply.

Angles are **always in degrees**, independent of the `units` setting.

---

## 4. Winding convention

2D profiles (used by `<extrude>`, `<revolve>`, `<sweep>`, and inside
`<sketch>`) should be wound **counter-clockwise** (CCW) when viewed
from the normal-positive side:

- For a default `xy`-plane sketch viewed from `+Z` (looking down at
  the ground), CCW means the polygon turns "leftward" as you walk
  along its edge in winding order.
- Equivalently: by the right-hand rule, if you curl your fingers in
  the polygon's winding direction, your thumb points in the normal
  direction (`+Z` for the default plane).

**CW input is accepted with a warning.** The engine detects CW
polygons in profile inputs and silently reverses them so the operation
still succeeds. A warning is emitted with `Category::Warning`:

```
extrude: profile polygon is wound clockwise. CADML's <path>/<rect>
/<circle> coordinates are math y-up; an SVG-style (y-down) CCW polygon
comes out CW in this frame. The engine will silently reverse it so the
extrude succeeds, but if you pasted this from an SVG file, wrapping
the content in <svg>...</svg> will both flip Y and silence this
warning.
```

This is typical when paths are pasted from an SVG file (which uses
y-down); the right fix is to wrap the paste in `<svg>` (see §6).

---

## 5. Revolve profile plane

`<revolve>` evaluates the 2D profile in the **(radial, axial)** plane:

- The profile's `x` coordinate maps to **radial** distance from the
  rotation axis.
- The profile's `y` coordinate maps to **axial** distance along the
  axis.

For `<revolve axis="z">`, that's `(r, z)`. For `<revolve axis="x">`,
that's `(r, x)` — radial outward from the X axis, axial along X.

**Profile constraint:** the profile must not cross the rotation axis.
For `axis="z"`, all profile `x` coordinates must be ≥ 0. A profile
with negative `x` is an error.

No `<sketch>` wrapper is needed (or accepted — see
[`implementation-notes.md`](../implementation-notes.md)) — the
orientation is fully determined by the axis.

---

## 6. The `<svg>` wrapper

```xml
<svg>
  <path d="..."/>
</svg>
```

Inside an `<svg>` element, descendant 2D geometry is implicitly
transformed by `scale(1, -1, 1)` — the Y axis is flipped. This serves
two purposes:

1. **SVG paste compatibility.** SVG editors use y-down coordinates; a
   path copy-pasted from one renders right-side-up in CADML when
   wrapped in `<svg>`.
2. **Silences the CW warning.** Inside `<svg>`, polygons are *expected*
   to be CW (because y-down CCW is CW in y-up after the flip), so the
   warning is suppressed.

The Y flip is applied at the `<svg>` boundary; the engine then
post-corrects triangle winding (because the negative-determinant
transform inverts winding).

---

## 7. Group transforms

`<group transform="...">` composes transforms in **left-to-right**
order. Reading

```xml
<group transform="translate(10, 0, 0) rotate(90, 0, 0, 1)">
```

as "first rotate the child by 90° around Z, then translate by 10 along
X" is the correct mental model.

Equivalently, in matrix form:

```
M_world = M_translate × M_rotate × (vertex of child)
```

The leftmost matrix is the **last** transformation applied to the
vertex, in the convention where matrices left-multiply column vectors.

---

## 8. The right-hand rule, applied

Rotation axes follow the right-hand rule. Specifically:

- Positive rotation around `+Z`: counter-clockwise when viewed from
  above (looking down `-Z`).
- Positive rotation around `+X`: counter-clockwise when viewed from
  the right (looking down `-X`).
- Positive rotation around `+Y`: counter-clockwise when viewed from
  the front (looking down `-Y`).

This matters when:
- Composing `<group>` rotations.
- Setting up `<port>` normals + ups.
- Authoring helical sweeps (`<helix direction="ccw">` vs `"cw"`).
- Interpreting `<for>` loop values for circular patterns.

---

## 9. World vs local frames

Most operations work in the **local frame** of the containing
`<group>`. The evaluator accumulates the world transform top-down as
it walks the document; the per-node world transforms are surfaced in
`FlatEvalResult::node_world_transforms` for tooling.

`<port>` positions are typically expressed in the local frame of the
`<part>` they belong to. When the part is instantiated in an
assembly, the assembly's transform composes with the port's local
position to give the world-space port location.

---

## 10. Unit-less computations in Lua

Lua expressions don't have unit awareness. Numbers passed in and out
of Lua functions are bare doubles. It's the author's responsibility
to maintain unit consistency:

```lua
function compressor.hub_z(t)
  -- t is a parametric 0..1
  -- returns mm because the caller is using mm
  return H * (1 - t^0.65)
end
```

If you change `units mm` to `units in`, you have to also adjust any
constants embedded in Lua helpers (or pull the constants out as
CADML params, which *are* unit-aware).
