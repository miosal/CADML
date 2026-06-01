# CADML cookbook

Common CADML patterns and the canonical way to write them.

The cookbook assumes you already know:

- Frontmatter is line-oriented (`version 0.1`, `units mm`,
  `param name = value`), **terminates at the first `<`**.
- The body has one or more top-level `<part>`/`<assembly>` exports
  plus optional `<def>`s and `<script>`s. The common case is one
  export per file; bundling multiple related parts in a single file
  is supported (see the caster-wheel example for three top-level
  `<part>` wrappers around three imported defs). There is
  **no `<cadml>` root** and **no `<defs>` wrapper**.
- Identifier names are kebab-case (`thread-d`, never `thread_d`
  or `threadD`).
- Cuts (`<difference>`) need overshoot on the cutter (see
  [implementation-notes.md](implementation-notes.md) §2).

For the full canonical file shape and a complete anti-patterns
checklist, see [implementation-notes.md](implementation-notes.md).

The full grammar lives in [`spec/language.md`](spec/language.md). When in
doubt about an attribute name, default value, or which children an
element accepts, the spec is normative; this cookbook is curated
recipes on top.

---

## The 30 reserved built-in elements

```
Structural (9):       part def assembly connect port group script for svg
2D primitives (4):    circle rect path sketch
2D → 3D (5):          extrude revolve sweep loft helix
Booleans + hull (4):  union difference intersect hull
Modifiers (5):        fillet chamfer shell cut pattern
Flat-output (3):      param sources source     (compiler-emitted)
```

That's the full set. There is no `<box>` — `<extrude>` a `<rect>`.
There is no `<cylinder>` — `<extrude>` a `<circle>`. There is no
`<thread>` — `<sweep>` a triangular `<path>` along a `<helix>`. There
is no `<cadml>`, `<defs>`, `<use>`, or `<paramref>` — see
[implementation-notes.md](implementation-notes.md) §§5–6 if you
reached for one of those.

---

## Recipe 1 — Plate with two holes (canonical full file)

The smallest non-trivial part. Frontmatter declares every dimension as
a `param`, body uses one `<difference>` of an extruded rect minus two
overshooting cylinders.

```
version 0.1
units mm
description "Mounting plate with two through-holes."

param plate-w   = 80
param plate-h   = 40
param plate-t   = 6
param hole-r    = 3
param hole-x    = 30           # ± from plate centre
param overshoot = 1            # >0; see Recipe 6

<part name="plate">
  <difference>
    <extrude height="{plate-t}">
      <rect x="{-plate-w/2}" y="{-plate-h/2}"
            width="{plate-w}" height="{plate-h}" rx="3"/>
    </extrude>
    <group transform="translate({ hole-x}, 0, {-overshoot})">
      <extrude height="{plate-t + 2*overshoot}">
        <circle r="{hole-r}"/>
      </extrude>
    </group>
    <group transform="translate({-hole-x}, 0, {-overshoot})">
      <extrude height="{plate-t + 2*overshoot}">
        <circle r="{hole-r}"/>
      </extrude>
    </group>
  </difference>
</part>
```

The shape to remember: a `<difference>` of one solid minus N cutters,
each cutter overshoots both faces. This pattern covers most parts
with holes.

---

## Recipe 2 — Cylinder (extruded circle)

There is no `<cylinder>` primitive. Use `<extrude>` over `<circle>`.

```
param l = 50
param r = 4

<part name="dowel">
  <extrude height="{l}">
    <circle r="{r}"/>
  </extrude>
</part>
```

`<circle>` has an optional `segments` attribute. Omit it (the engine
picks an adaptive value that keeps the chord error below 0.005 document
units, clamped to 8..256). Pass `segments="6"` to force a hexagonal prism,
`segments="32"` for the lighter faceting some tests expect.

---

## Recipe 3 — Box (extruded rect)

```
param w = 60
param d = 30
param h = 12

<part name="block">
  <extrude height="{h}">
    <rect x="{-w/2}" y="{-d/2}" width="{w}" height="{d}"/>
  </extrude>
</part>
```

`<rect>` has `rx`/`ry` for rounded corners — much cheaper than running
`<fillet>` after the fact, because rounded `<rect>` is a single
tessellation pass.

---

## Recipe 4 — Revolve (washer, gasket, bowl, anything circularly symmetric)

```
param id = 8                # inner diameter
param od = 18               # outer
param t  = 2                # thickness

<part name="washer">
  <revolve axis="z" angle="360">
    <path d="M {id/2}, 0
             L {od/2}, 0
               {od/2}, {t}
               {id/2}, {t}
             Z"/>
  </revolve>
</part>
```

`<revolve>` takes the 2D profile **directly** as a child — a `<path>`,
`<circle>`, or `<rect>`. The profile lives in the **(radial, axial)**
plane: `x` is the radius from the axis and `y` is the axial coordinate.
For `axis="z"`, that's `(r, z)`. Do **not** wrap the profile in
`<sketch>` — `<sketch>` is for `<loft>` cross-sections; `<extrude>` /
`<revolve>` / `<sweep>` expect the primitive raw. See the "Sketch
wrapping" gotcha for the engine reality vs spec aspiration.

For a swept hub or bell-shaped profile, generate the `d=` string from
Lua and inline it as `<path d="{my_module.hub_profile(...)}"/>` — that
is how `examples/v0.1/compressor` builds its bell.

For a partial revolve (an arc, a fan): `angle="180"`.

For smoother curvature: `<revolve axis="z" angle="360" segments="180">`
— default is 64. Cost is linear in segment count; only bump it if the
curvature shows up faceted.

---

## Recipe 5 — Threaded shank (sweep + helix)

The hex-bolt thread pattern: a 60° V-cutter swept along a helix, then
boolean-differenced from the cylindrical shank. This is the reference
example for `<sweep>` + `<helix>`.

```
param thread-d         = 10        # major diameter
param thread-pitch     = 1.25      # axial distance per turn
param shank-l          = 40
param tri-h            = {0.866025 * thread-pitch}   # ISO H
param thread-overshoot = 0.1
param thread-turns     = {(shank-l - 1) / thread-pitch}

<part name="threaded-shank">
  <difference>
    <extrude height="{shank-l}">
      <circle r="{thread-d / 2}"/>
    </extrude>
    <sweep>
      <helix radius="{thread-d / 2}"
             pitch="{thread-pitch}"
             turns="{thread-turns}"/>
      <path d="M {-tri-h}, 0
               L  {thread-overshoot}, {-thread-pitch/2}
                  {thread-overshoot},  {thread-pitch/2}
               Z"/>
    </sweep>
  </difference>
</part>
```

`<sweep>` takes exactly two children, in any order: a 2D profile
(`<rect>`/`<circle>`/`<path>` — directly, NOT wrapped in `<sketch>`)
and a `<helix>`. The profile
is placed at each sample frame using the **axis-perpendicular**
convention: `profile.x` maps to the outward radial direction (away
from the helix axis), `profile.y` maps to `+Z`. So in the path above,
`x = -tri-h` is the apex (deepest cut into the cylinder) and `y = ±
pitch/2` are the axial extents (so adjacent turns butt at the OD,
producing sharp crests).

`<helix>` alone (not inside `<sweep>`) emits a warning and produces
no geometry — it has no rendering meaning by itself.

---

## Recipe 6 — Cuts must overshoot

When a `<difference>` cutter ends *exactly* at the cuttee's face, the
CSG kernel produces a coplanar face it can't classify cleanly — the
operation fails or produces a zero-thickness sliver. Push the cutter
through both faces by at least 1 mm.

Convention: declare `param overshoot = 1` in frontmatter and use it
consistently:

```
<difference>
  <extrude height="{t}">…</extrude>                       <!-- target -->
  <group transform="translate(x, y, {-overshoot})">       <!-- cutter -->
    <extrude height="{t + 2*overshoot}">                  <!-- 1mm past each face -->
      <circle r="{hole-r}"/>
    </extrude>
  </group>
</difference>
```

For a cutter that only needs to break ONE face (a blind hole, a
through-cut along an axis where one end is open air), overshoot only
that face — but most cuts need both.

---

## Recipe 7 — Parametric curves via Lua (NACA airfoils, gear teeth, cams)

When the shape requires real math — `math.sin`, iteration, table-driven
sampling — author the curve in Lua and pull it into a `<path>` via an
expression in `d="…"`.

**File: `naca.lua`** (one curve generator)

```lua
-- NACA-4 airfoil cross-section as an SVG-compatible path string.
-- Returns a string ready for <path d="...">.
function naca_4(thickness_pct, chord, n)
  local t = thickness_pct / 100
  local pts_up, pts_dn = {}, {}
  for i = 0, n do
    local xc = i / n
    local x  = xc * chord
    local yt = 5*t*chord * (
        0.2969*math.sqrt(xc) - 0.1260*xc - 0.3516*xc^2
      + 0.2843*xc^3       - 0.1015*xc^4)
    table.insert(pts_up, string.format("%g,%g", x,  yt))
    table.insert(pts_dn, 1, string.format("%g,%g", x, -yt))
  end
  return "M " .. pts_up[1] ..
         " L " .. table.concat(pts_up, " ", 2) ..
         " "   .. table.concat(pts_dn, " ") .. " Z"
end
```

**File: `wing.cadml`** (the part that consumes it)

```
version 0.1
units mm
description "Trapezoidal wing: extruded NACA-2412 cross-section."

import "naca.lua" as naca       # exposes naca.naca_4(...)

param chord     = 100
param span      = 250
param thickness = 12             # percent

<part name="wing">
  <extrude height="{span}">
    <path d="{naca.naca_4(thickness, chord, 40)}"/>
  </extrude>
</part>
```

**Rules of thumb for Lua:**

- Prefer **`import "x.lua" as alias`** in frontmatter over inline
  `<script lang="lua">…</script>`. External `.lua` files are easier to
  test, version, and reuse.
- The CADML→Lua bridge auto-translates: `param max-thickness` in
  frontmatter is visible in Lua as `max_thickness`. Kebab in CADML,
  snake in Lua.
- Use Lua **only when the geometry requires it** — math functions,
  iteration, table lookups. Don't reach for it to write a plain rect or
  circle.
- If you must inline a `<script>`, wrap its body in `<![CDATA[ … ]]>`
  whenever it contains `<`, `>`, or `&` (Lua comparison operators!):
  ```xml
  <script lang="lua"><![CDATA[
    if x < 5 then ... end
  ]]></script>
  ```

---

## Recipe 8 — Repeated geometry via `<def>` + bare instance

Reusable geometry block + N call sites, all in one file. The `<def>`
**is not** wrapped in `<defs>`. Calling a def is just writing its name
as an element — there is **no `<use>` tag**.

```
version 0.1
units mm

param plate-t = 6
param hole-r  = 3.3              # M6 clearance

<def name="m6-clearance-hole">
  <extrude height="{plate-t + 2}">
    <circle r="{hole-r}"/>
  </extrude>
</def>

<part name="plate">
  <difference>
    <extrude height="{plate-t}">
      <rect x="-40" y="-40" width="80" height="80"/>
    </extrude>
    <group transform="translate( 30,  30, -1)"><m6-clearance-hole/></group>
    <group transform="translate(-30,  30, -1)"><m6-clearance-hole/></group>
    <group transform="translate( 30, -30, -1)"><m6-clearance-hole/></group>
    <group transform="translate(-30, -30, -1)"><m6-clearance-hole/></group>
  </difference>
</part>
```

The def is referenced as `<m6-clearance-hole/>`. Names are kebab-case,
same as everything else.

When you want many copies in a uniform pattern, `<pattern>` is even
cleaner — see Recipe 9.

---

## Recipe 9 — Patterns (linear arrays, bolt circles)

`<pattern>` is unrolled at compile time into N `<group transform>`
siblings — same geometry as writing them out by hand, just much
shorter to author. Two flavours:

```xml
<!-- Bolt circle: 6 holes equally spaced around Z, pitch-circle r=30 -->
<difference>
  <extrude height="{t}"><rect x="-50" y="-50" width="100" height="100"/></extrude>
  <pattern type="circular" count="6" axis="z" angle="360">
    <group transform="translate(30, 0, -1)">
      <extrude height="{t + 2}"><circle r="{hole-r}"/></extrude>
    </group>
  </pattern>
</difference>

<!-- Linear array: 5 ribs along +Y, 12 mm apart -->
<pattern type="linear" count="5" axis="y" spacing="12">
  <extrude height="{rib-h}"><rect x="-20" y="0" width="40" height="2"/></extrude>
</pattern>
```

`type` is required (`linear` or `circular`). `count` is the total
number of instances (including the original at offset 0). For
`circular`, `angle="360"` evenly spaces a closed ring;
`angle="180"` would space them across a semicircle.

---

## Recipe 10 — Loops via `<for>`

When the per-iteration value can't be expressed as a uniform
translation/rotation (e.g., positions read from a Lua-computed table,
non-uniform spacing, per-step variation), `<for>` unrolls iterations
into siblings:

```xml
<!-- 5 increasing-radius disks along +Z -->
<for var="i" from="0" to="4" steps="5">
  <group transform="translate(0, 0, {i * 5})">
    <extrude height="2">
      <circle r="{4 + i * 1.5}"/>
    </extrude>
  </group>
</for>

<!-- Explicit value list -->
<for var="c" values="nw ne sw se">
  <!-- the {c} value is available in any expression inside -->
</for>
```

`{var}` is available in every expression inside the loop body. The
compiler unrolls each iteration into literal sibling elements before
flat-eval runs — `<for>` doesn't appear in `.fcadml` output.

---

## Recipe 11 — Convex blends via `<hull>`

For a tapered transition between two profiles (a circular base
blending into a square top, or two offset disks blending into a
capsule), `<hull>` of two thin extrudes is the easiest tool — much
simpler than authoring a `<loft>`.

```xml
<part name="capsule">
  <hull>
    <group transform="translate(-15, 0, 0)">
      <extrude height="2"><circle r="6"/></extrude>
    </group>
    <group transform="translate( 15, 0, 0)">
      <extrude height="2"><circle r="6"/></extrude>
    </group>
  </hull>
</part>
```

The hull operation spans every input solid; degenerate or non-manifold
inputs produce a warning and an empty mesh. Use it for convex
transitions; for non-convex multi-section interpolation, see `<loft>`
in [`spec/language.md`](spec/language.md) §5.3.

---

## Recipe 12 — Coordinate-frame discipline

CADML uses a **right-handed, Z-up** coordinate system. `+X` right,
`+Y` forward, `+Z` up. Angles are degrees. Rotations follow the
right-hand rule.

Convention for parts:
- Build the geometry with the **part's natural "down" face on the XY
  plane** (z=0). An extruded plate sits with its bottom at z=0 and top
  at z=t. A revolved bowl sits with its base at z=0.
- The part's "front" faces +Y. The part's "right" faces +X.
- When assemblies stack parts, the parent's `<port>` provides the
  position + normal + up vector that the child mates against.

`<group transform="translate(x, y, z) rotate(deg, axis-x, axis-y,
axis-z) scale(sx, sy, sz)">` is the standard form. Reading text
left-to-right, the LEFTMOST op is applied LAST to a local point —
i.e. the engine builds `M = T · R · S` and computes
`M · p_local = T · (R · (S · p_local))`. So in the example above
the child is scaled first, then rotated, then translated. See
[`spec/coordinate-system.md`](spec/coordinate-system.md) §7 for
the canonical statement.

---

## Recipe 13 — Assemblies (multi-file mating)

When parts come from different files and need to mate, the file
becomes an `<assembly>`, instances reference imports, and `<port>`s
on each imported `<part>` define the mating frames.

**File: `motor.cadml`**

```
version 0.1
units mm

param body-l = 50
param body-r = 20

<part name="motor">
  <extrude height="{body-l}">
    <circle r="{body-r}"/>
  </extrude>
  <port name="shaft-out" position="0 0 {body-l}"
                          normal="0 0 1" up="1 0 0"/>
</part>
```

**File: `drivetrain.cadml`**

```
version 0.1
units mm

import "motor.cadml" as motor
import "gear.cadml"  as gear

<assembly name="drivetrain">
  <motor id="m1"/>
  <gear  id="g1" at="m1.shaft-out" port="bore"/>
</assembly>
```

The engine resolves the rigid transform that takes `gear.bore`'s frame
onto `m1.shaft-out`'s frame. For multi-edge graphs (one bolt mating
both plates), use `<connect a="m1.shaft-out" b="g1.bore"/>` inside the
assembly.

Bare `<motor/>` (no `at`/`port`) at top of an assembly = placed at
identity, ungrouped — useful for "this gearbox sits next to the motor
but isn't rigidly mated."

---

## Recipe 14 — SVG paste (`<svg>` wrapper)

SVG editors author in Y-down; CADML is Y-up. The `<svg>` element is
a coordinate-frame wrapper that applies `scale(1, -1, 1)` so a path
copied out of Inkscape/Figma renders right-side-up:

```xml
<part name="logo-plate">
  <difference>
    <extrude height="2"><rect width="100" height="60"/></extrude>
    <svg>
      <!-- Coordinates here are in SVG's y-down convention. -->
      <group transform="translate(0, 0, -1)">
        <extrude height="4">
          <path d="M 20,15 L 80,15 80,45 20,45 Z"/>
        </extrude>
      </group>
    </svg>
  </difference>
</part>
```

This is **not** a generic SVG-file importer. It's a wrapper that
makes pasted SVG path strings work without the author having to flip
every Y by hand. To pull in a separate `.svg` file as an importable
geometry asset, see the spec's §6 on imports.

---

## Recipe 15 — Modifiers (fillet, chamfer, shell)

```xml
<part name="rounded-box">
  <fillet radius="2">
    <extrude height="20"><rect width="60" height="30"/></extrude>
  </fillet>
</part>

<part name="bevelled-box">
  <chamfer distance="1.5">
    <extrude height="20"><rect width="60" height="30"/></extrude>
  </chamfer>
</part>

<part name="hollow-cup">
  <shell thickness="2">
    <extrude height="30"><circle r="20"/></extrude>
  </shell>
</part>
```

These are **Tier 1** modifiers — they accept restricted inputs:
`<extrude>` of a convex 2D profile, `<revolve>` of a profile curve, or
a `<union>` of either. They don't (yet) accept arbitrary CSG
results — apply them BEFORE booleans, not after. If you fillet a
`<difference>` and it errors, the answer is to fillet the input and
then difference.

`<select>` defaults to `all`; future versions will accept selectors
(e.g., `select="convex.top"`) to limit which edges get rounded.

---

## Naming params

- **Semantic role, not abbreviation.** `thread-d` (thread diameter)
  beats `d1`; `wall-t` (wall thickness) beats `t`; `bolt-circle-r`
  beats `bcr`.
- **Singular for one, plural for counts.** `tooth-count`,
  `spoke-count` — not `n` / `n2`.
- **Suffix when the unit isn't the document default.**
  `pressure-angle-deg`, `pitch-mm`, `coil-pitch`. Avoids ambiguity
  when later edits read "set the angle to 20."
- **Sibling consistency.** If you have `head-h` and `head-w`, the
  third is `head-d` — not `head_d` or `headD`. Pick a convention
  per file and stick to it.

---

## What to do when a recipe doesn't render

1. Open the spec at [`spec/language.md`](spec/language.md) and find the
   primitive you used. Confirm every attribute name + default + child
   shape.
2. Compile with `cadmlcheck path/to/file.cadml` (or `cadmlbuild`); the
   stderr output gives the engine's error diagnostic — line + column
   + which primitive failed.
3. If the failure is "boolean produced empty mesh," the most likely
   cause is missing overshoot (Recipe 6) or a non-manifold input
   (e.g., a `<hull>` of a single degenerate solid).
4. If the failure is "unknown element name," you wrote something
   that's not in the 30 reserved set (top of this file) and not an
   import alias or local `<def>`. Re-read the anti-patterns block
   in [implementation-notes.md](implementation-notes.md).
5. Compare against an existing example — `hex-bolt.cadml` exercises
   `<union>` + `<extrude>` + `<revolve>` + `<difference>` + `<sweep>`
   + `<helix>` + `<group transform>` + `param` math expressions in
   one ~100-line file.
