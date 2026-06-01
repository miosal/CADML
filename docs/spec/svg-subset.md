# SVG Path Subset

CADML's `<path d="...">` attribute uses a strict subset of SVG path
syntax for the `d` data string. This document specifies that subset.

CADML's `d` grammar is a strict subset of SVG. Arbitrary transforms,
presentation attributes, and multiple sub-paths in one `d` string are
not supported. A `.cadml` parser does not need an SVG library.

---

## 1. Supported commands

| Cmd | Form | Meaning |
|---|---|---|
| `M` | `M x,y` | Move to absolute `(x, y)`. Begins a sub-path. |
| `L` | `L x,y` | Line to absolute `(x, y)`. |
| `H` | `H x` | Horizontal line to absolute `x` (`y` unchanged). |
| `V` | `V y` | Vertical line to absolute `y` (`x` unchanged). |
| `A` | `A rx ry x-axis-rot large-arc-flag sweep-flag x,y` | Arc to absolute `(x, y)`. |
| `C` | `C x1,y1 x2,y2 x,y` | Cubic Bezier to absolute `(x, y)` with control points `(x1, y1)` and `(x2, y2)`. |
| `S` | `S x2,y2 x,y` | Smooth cubic Bezier — first control point is the reflection of the previous segment's second control point (see §6). |
| `Q` | `Q x1,y1 x,y` | Quadratic Bezier to absolute `(x, y)` with control point `(x1, y1)`. |
| `T` | `T x,y` | Smooth quadratic Bezier — control point is the reflection of the previous segment's control point (see §6). |
| `Z` | `Z` (no args) | Close the current sub-path back to its last `M`. |

Lowercase variants — `m`, `l`, `h`, `v`, `a`, `c`, `s`, `q`, `t`, `z` —
use **relative** coordinates: each coordinate is added to the current
cursor position.

**Not supported in 0.1:**

| Feature | Reason |
|---|---|
| Multiple sub-paths in one `d` | Each `<path>` produces a single closed profile; use multiple `<path>` elements. |
| Presentation attributes, transforms, `<style>` | CADML `<path>` is geometry only; use `<group transform=…>` for placement. |

---

## 2. Repeated command argument lists

Per SVG convention, a command followed by multiple coordinate sets
implicitly repeats the command:

```
M 0,0 L 10,0 20,0 30,0 Z
```

is equivalent to

```
M 0,0 L 10,0 L 20,0 L 30,0 Z
```

For `M` specifically, the second and subsequent coordinate pairs are
treated as `L` commands (matching SVG).

This shortform is convenient for revolve profiles, which are typically
just a sequence of line segments:

```xml
<path d="M 0,0
         L {tip-r}, 0
           {thread-d/2}, {tip-chamfer-l}
           0, {tip-chamfer-l}
         Z"/>
```

---

## 3. Coordinate format

Coordinate pairs may be separated by `,` or by whitespace. Commands
may be separated from their arguments by whitespace or directly
juxtaposed (`M0,0` is valid).

Numbers follow standard floating-point format:

```
0       # integer
1.5     # decimal
-2.3    # negative
1e-3    # scientific
```

The decimal separator is **always** `.` (locale-independent).

---

## 4. Closing paths

The `Z` (or `z`) command closes the current sub-path: it draws a line
from the cursor's current position back to the position of the most
recent `M`. After closing, the cursor remains at the `M` position
(not at the original "before close" position).

Paths in CADML **should always close** with `Z`. Open paths are
accepted by the parser but the engine warns and may produce
unexpected geometry — `<extrude>` of an open polygon produces a
shell-without-cap, which is rarely what the user wants.

---

## 5. Arc semantics

```
A rx ry x-axis-rotation large-arc-flag sweep-flag x,y
```

- `rx`, `ry` — ellipse radii.
- `x-axis-rotation` — angle (degrees) of the ellipse's x-axis relative
  to the parent's x-axis.
- `large-arc-flag` — `0` for the smaller of the two possible arcs,
  `1` for the larger.
- `sweep-flag` — `0` for the arc swept counter-clockwise, `1` for
  clockwise.
- `x, y` — absolute endpoint (relative for `a`).

The arc semantics follow the SVG 1.1 spec verbatim. Out-of-range
parameters (degenerate ellipse, endpoints coincident) are handled per
SVG spec: a degenerate arc becomes a line.

Tessellation: arcs are sampled to keep chord error below `0.005`
document units — the same sagitta target the `<circle>` and Bezier
flatteners use, so all curve primitives in the same file get matching
facet density. Document units are the unit the file declares in
its `units` setting.

---

## 6. Bezier semantics

Cubic Bezier `C x1,y1 x2,y2 x,y`:

```
P(t) = (1-t)³ P₀ + 3(1-t)²t P₁ + 3(1-t)t² P₂ + t³ P₃
       where P₀ = cursor, P₁ = (x1,y1), P₂ = (x2,y2), P₃ = (x,y)
```

Quadratic Bezier `Q x1,y1 x,y`:

```
P(t) = (1-t)² P₀ + 2(1-t)t P₁ + t² P₂
       where P₀ = cursor, P₁ = (x1,y1), P₂ = (x,y)
```

Tessellation is adaptive: the curve is subdivided until the chord
error falls below 0.005 document units. The implementation uses recursive
midpoint subdivision with flatness test.

### 6.1 Smooth continuations (`S`, `T`)

`S` and `T` are shorthand for a `C` / `Q` whose leading control point is
inferred by reflection, following SVG 1.1:

- `S x2,y2 x,y` is a cubic Bezier whose **first** control point `P₁` is
  the reflection of the **previous** segment's second control point
  about the current cursor — *but only if the previous command was a
  cubic (`C`/`c`/`S`/`s`)*. Otherwise `P₁` coincides with the cursor.
  `(x2,y2)` is `P₂` and `(x,y)` is the endpoint.
- `T x,y` is a quadratic Bezier whose control point is the reflection of
  the **previous** segment's control point about the current cursor —
  *only if the previous command was a quadratic (`Q`/`q`/`T`/`t`)*.
  Otherwise the control point coincides with the cursor (the segment
  degenerates to a straight line).

The reflection rule lets you chain C¹-continuous curves without
restating the shared tangent. Mixing scopes (e.g. `T` after a `C`)
resets the inferred control point to the cursor, exactly as SVG
specifies.

---

## 7. Coordinate system

By default, path `d` data uses CADML's coordinate convention:

- `x` axis: right.
- `y` axis: up.
- CCW winding (right-hand rule, viewed from outside the solid).

Paths that are clockwise in CADML coordinates are accepted but produce
a warning. The engine silently reverses the polygon to maintain CCW
winding (so the extrude/revolve still works), but the warning surfaces
the unusual case — typically a path copy-pasted from an SVG file
without a `<svg>` wrapper to flip Y.

Inside a `<svg>` wrapper, the Y axis is flipped (`scale(1, -1, 1)`)
to match SVG-tool output, and the CW warning is suppressed (because
CW in y-down is CCW in y-up after the flip).

See [`coordinate-system.md`](coordinate-system.md) for the full
treatment.

---

## 8. Examples

A square:

```
M 0,0 L 10,0 L 10,10 L 0,10 Z
```

Same square with implicit repeat:

```
M 0,0 L 10,0 10,10 0,10 Z
```

A rectangle with rounded corners:

```
M 5,0
L 95,0
A 5,5 0 0,1 100,5
L 100,95
A 5,5 0 0,1 95,100
L 5,100
A 5,5 0 0,1 0,95
L 0,5
A 5,5 0 0,1 5,0
Z
```

A revolve profile (e.g., a stepped washer):

```
M {id/2}, 0
L {od/2}, 0
  {od/2}, {t}
  {(id+od)/4}, {t}
  {(id+od)/4}, {t/2}
  {id/2}, {t/2}
Z
```

(That last is in the `(radius, axial)` plane for `<revolve axis="z">`.)
