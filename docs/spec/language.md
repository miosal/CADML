# CADML 0.1 Specification

**CAD Markup Language** — A declarative XML-based language for parametric solid modelling.

This document is the **normative source of truth** for valid and invalid
CADML 0.1 syntax, the compilation pipeline that lowers authoring files to the
flat language the renderer consumes, and the contracts each layer must uphold.

For information not in this document — exact tokenization rules, per-element
tessellation algorithms, the Lua sandbox specifics, CSG kernel choices, error
recovery — see the companion documents in the same directory:

| Topic | Document |
|---|---|
| End-to-end pipeline & dataflow | [`../architecture.md`](../architecture.md) |
| Parser & bundler internals | [`../compiler.md`](../compiler.md) |
| Per-element mesh-generation semantics | [`../evaluator.md`](../evaluator.md) |
| Flat-IR (`.fcadml`) format reference | [`./flat-ir.md`](./flat-ir.md) |
| Expression `{…}` mini-language | [`./expressions.md`](./expressions.md) |
| SVG path-`d` subset & arc semantics | [`./svg-subset.md`](./svg-subset.md) |
| Coordinate system, units, winding | [`./coordinate-system.md`](./coordinate-system.md) |
| Lua embedding & sandbox API | [`../lua-embedding.md`](../lua-embedding.md) |
| CSG model (Manifold) & manifoldness | [`../csg.md`](../csg.md) |
| STL / glTF exporter specifics | [`../exporters.md`](../exporters.md) |
| Error / warning taxonomy | [`../error-model.md`](../error-model.md) |
| Gotchas & known implementation limits | [`../implementation-notes.md`](../implementation-notes.md) |

If this document and a companion document disagree on any *normative*
question, this document wins and the companion is wrong. If they disagree
on an *implementation detail* (a default value, a tolerance, an algorithm
choice), the companion document is authoritative.

---

## 1. Overview

### 1.1 Two-tier model

CADML has two distinct file shapes:

- **Authoring CADML** (`.cadml`) — what humans write. May contain imports,
  assemblies, parametric instances, iteration, and Lua scripting.
- **Flat CADML** (`.fcadml`) — what renderers and analysis tools
  consume internally. Self-contained; no imports, no assembly
  composition, no iteration. One or more renderable parts plus the
  named definitions they reference.

A compiler (`cadml::compile`) lowers `.cadml` to `.fcadml`. Every CLI
tool in the reference implementation accepts a `.cadml` entry directly
and runs the lowering internally — `.fcadml` is the on-the-wire
intermediate, useful when you want to ship a self-contained artifact
to another machine or inspect the bundler's output. The split mirrors
HTML/JSX:

```
[ .cadml authoring ]   →   cadml_compile   →   [ .fcadml flat ]   →   evaluator
```

### 1.2 Design principles

1. **XML body, line-oriented frontmatter.** Geometry is expressed as XML
   elements; declarations (version, params, imports) are one per line.
2. **Exact dimensions.** `<fillet radius="2"/>` produces a 2 mm fillet or
   errors. No silent approximation.
3. **The engine consumes a small language.** Authoring constructs (imports,
   assemblies, iteration) are compiled away. The flat language has no
   composition primitives.
4. **Composition over generality.** When a modifier can't handle complex
   inputs, the answer is to decompose the geometry across files and assemble.
5. **Source-mapped.** Every node in the flat output traces back to a line
   in an authoring file.

### 1.3 File extensions

| Extension | Role | Read by |
|---|---|---|
| `.cadml` | Authoring source | Every CLI tool + editors + humans |
| `.fcadml` | Flat compiled output (the bundler's intermediate) | Engine / renderer / analysis tools when handed a pre-bundled artifact |
| `.lua` | Lua module (functions, helpers) | Compiler (via `import`) |

---

## 2. File Structure

### 2.1 Authoring file shape

A `.cadml` file consists of a frontmatter block followed by an XML body:

```
[ Frontmatter — line-oriented ]
version 0.1
units mm
description "..."
tags "..."

import "x.cadml"
import "y.lua" as y

param a = 10
param b = a * 2 (min=5, max=100)

[ Body — XML ]
<def name="..."> ... </def>
<script lang="lua"> ... </script>

<part>          OR          <assembly>
  ...                         ...
</part>                     </assembly>
```

### 2.2 Top-level exports

Each `.cadml` file exports **one or more** top-level `<part>` and/or
`<assembly>` elements. Each exported element must carry a unique
`name="..."` attribute; the file's own filename (without extension)
is **not** an implicit name.

The common case is a single export per file — it keeps imports
unambiguous (`import "bolt.cadml"` then `<bolt/>`). Multiple top-level
exports are supported for two situations the 0.1 toolchain handles
directly:

- A file that bundles a related family of parts (e.g.
  `examples/caster-wheel/caster-wheel.cadml`, which declares three
  top-level `<part>` elements wrapping imported fork / axle / wheel
  defs). Each part still gets its own STL / glTF / 3MF mesh under its
  declared `name`.
- An import surface that exposes several sub-parts under one filename;
  importers reference them by name, e.g.
  `import "bolts.cadml" as hw` then `<hw.m6/>`, `<hw.m8/>`.

A file may also contain:
- Zero or more `<def>` elements (file-private geometry helpers)
- Zero or more `<script>` elements (inline Lua)

A file with zero top-level exports (only `<def>`s and `<script>`s with
no `<part>`/`<assembly>`) is an error reported by the bundler — there
is nothing to import or render. The parser permits empty or partial
inputs to support incremental editing in tools.

### 2.3 Frontmatter terminates at first `<`

The first `<` character in the file (after stripping leading whitespace)
ends the frontmatter and begins the body. XML processing instructions
(`<?xml ... ?>`) are not permitted in CADML 0.1; they error.

XML comments (`<!-- ... -->`) are permitted inside the body. They are
parsed and discarded; the AST does not retain them and they do not appear
in `.fcadml`. Frontmatter uses `#` for comments (Section 3.6); XML-style
comments in frontmatter are not recognised and would terminate the
frontmatter at the leading `<`.

### 2.4 Synthetic root

The XML body is parsed as if wrapped in a single synthetic root element.
Multiple top-level siblings (e.g., a `<def>` and a `<part>`) are valid
without explicit XML wrapping. The synthetic root is invisible to the AST.

### 2.5 Encoding

Files are UTF-8 encoded. A leading byte-order mark (BOM, `EF BB BF`) is
permitted and stripped before parsing. Other encodings are rejected with a
parse error.

### 2.6 Line endings

Both `LF` (Unix) and `CRLF` (Windows) line endings are accepted. The parser
normalises to `LF` internally; line numbers in source maps refer to LF-
counted lines from the start of the file.

### 2.7 Coordinate system

CADML uses a **right-handed, Z-up** coordinate system, matching the standard
mechanical-engineering convention:

- `+X` → right
- `+Y` → forward (away from the viewer in a default orthographic front view)
- `+Z` → up
- Rotations follow the right-hand rule (positive angle = counter-clockwise
  when viewed from the positive end of the rotation axis, looking toward
  the origin)

The default `<sketch>` plane is `xy` (i.e., looking down the `+Z` axis). All
numeric coordinates and dimensions are in the units declared in frontmatter
(default `mm`). Angles are always in degrees.

### 2.8 Naming character set

Element names, import aliases, `<def>` names, and `<param>` names must
match the regex `[a-z][a-z0-9-]*` — lowercase ASCII, starting with a
letter, with hyphens as the kebab-case separator. Underscores, periods,
slashes, uppercase letters, and non-ASCII characters are not permitted in
identifier positions. (Periods serve as path separators in `at`/`port`
references; slashes serve as path separators in import strings.)

Lua identifiers are not subject to this restriction — Lua follows its own
naming rules, and the auto-translation between CADML kebab-case and Lua
snake_case bridges the gap (Section 8.2).

---

## 3. Frontmatter Grammar

### 3.1 Statement shapes

Three statement shapes share the frontmatter region:

| Shape | Form | Examples |
|---|---|---|
| **Setting** | `<keyword> <value>` | `version 0.1`, `units mm`, `description "..."` |
| **Directive** | `<keyword> <args>` | `import "x.cadml"`, `interference-tolerance 0.01mm³` |
| **Param binding** | `param <name> = <expression> [(constraints)]` | `param chord = 100`, `param d = 10 (min=3, max=30)` |

### 3.2 Order rules

Frontmatter sections appear in this fixed order, with optional comments and
blank lines between any two statements:

```
1. Settings
2. Imports
3. Params
```

Out-of-order statements (e.g. a `settings` line after `param`s) produce
a **parse warning** — the file still compiles, but tools that lint
style are encouraged to surface the warning to the user. The order is a
convention for readability, not a semantic constraint. Within each
section, ordering rules:

- **Settings**: any order.
- **Imports**: any order. Imports do not depend on other imports in 0.1.
- **Params**: top-to-bottom evaluation. A param may reference any
  previously-declared param or imported Lua function in its expression;
  forward references are an error.

### 3.3 Settings

| Keyword | Value | Default | Description |
|---|---|---|---|
| `version` | spec version (e.g. `0.1` or `0.1.0`) | required | Major.minor or major.minor.patch; both forms accepted |
| `units` | `mm` / `cm` / `m` / `in` / `ft` | `mm` | Document numeric unit |
| `description` | quoted string | empty | Human-readable description |
| `tags` | quoted string (space-separated) | empty | Searchable tags |
| `catalogue-version` | semver string (`major.minor.patch`) | empty | For catalogue parts only |
| `interference-tolerance` | volume quantity | `0mm³` | Threshold below which `cadml_check` ignores overlaps |

The `version` value accepts both short (`0.1`) and full (`0.1.0`) forms;
they are equivalent. Internally the compiler normalises to the
three-component form.

The `interference-tolerance` value is a number followed optionally by
a volume unit suffix (`mm³`, `cm³`, `m³`, `in³`, `ft³`). If no unit
suffix is given, the document's `units` setting applies (cubed). Examples:
`0.01mm³`, `1`, `0.001 cm³` (whitespace between number and unit is
permitted but not required).

Multiple values for the same key are an error.

### 3.4 Imports

```
import "<path>" [as <alias>]
```

| Path form | Resolution |
|---|---|
| `ctl/...` | `<entry-dir>/ctl/<rest>` (catalogue / standard library) |
| `./...` or `../...` | Relative to the importing file |
| Bare path (e.g., `shared/x.cadml`) | Relative to the importing file |

The default alias is the filename without extension and without leading
path components: `import "shared/bolt.cadml"` aliases as `bolt`. If a user
needs a different alias (collision, clarity), they must specify
`as <alias>`.

Dispatch is by file extension:

| Extension | Treated as |
|---|---|
| `.cadml` | Geometry module — exports the file's `<part>` or `<assembly>` |
| `.lua` | Lua module — exports top-level functions or `return`-ed table |

Cycles in `.cadml` import graphs are detected and reported as errors.
Self-imports (a file importing itself) are also errors. `.lua` files
cannot import other files in 0.1, so no `.lua` cycles are possible.

### 3.5 Params

```
param <name> = <expression> [(<constraint>=<value>, ...)]
```

The name must match the identifier rule from Section 2.8 — strictly
kebab-case (`[a-z][a-z0-9-]*`). Names with underscores, capitals, or
non-ASCII characters error at parse time. Names are auto-translated to
`snake_case` when bound into Lua scope (so `param max-thickness` is
visible in Lua as `max_thickness`).

The value is any CADML expression: a literal, an arithmetic expression
referencing earlier params, or a Lua function call. Because params are
evaluated after all imports have been resolved (per Section 3.2), a
default may invoke an imported Lua function:

```
import "ctl/aero/airfoils.lua" as airfoils
param chord = airfoils.default_chord()
```

Constraints (optional):

| Constraint | Meaning |
|---|---|
| `min` | Numeric lower bound (inclusive) |
| `max` | Numeric upper bound (inclusive) |

Constraints are validated:

1. At compile time against the param's declared default.
2. At compile time against `<X param-name="value"/>` instance overrides.
3. At runtime against API/programmatic param overrides.

A constraint violation at any stage produces a hard error.

### 3.6 Comments and whitespace

- `# ...` to end of line: comment.
- Blank lines: ignored.
- Indentation: not significant.

---

## 4. Body Grammar

### 4.1 Top-level structure

The body of a `.cadml` file consists of zero or more top-level
elements. At least one of them must be `<part>` or `<assembly>` (the
file's export(s)); others may be `<def>` or `<script>`. See §2.2 for
the multi-export semantics.

```
[any number of <def> and <script> elements]
[one or more <part> or <assembly> elements]
```

The order is conventional — most authors put `<def>`s and `<script>`s
before the exports — but the parser does not enforce ordering within
the body.

### 4.2 Element vocabulary

The set of valid element names in any single file is:

```
{ built-in element names }
  ∪ { import aliases }
  ∪ { local <def name="..."> declarations }
```

Any element whose name is outside this set is a parse error with a
helpful diagnostic listing nearby valid names.

### 4.3 Reserved built-in element names

For CADML 0.1, the following 30 names are reserved and cannot be used as
import aliases or `<def>` names:

**Structural (9):**
`part`, `def`, `assembly`, `connect`, `port`, `group`, `script`, `for`, `svg`

**2D primitives (4):**
`circle`, `rect`, `path`, `sketch`

**2D-to-3D (5):**
`extrude`, `revolve`, `sweep`, `loft`, `helix`

**Booleans + hull (4):**
`union`, `difference`, `intersect`, `hull`

**Modifiers (5):**
`fillet`, `chamfer`, `shell`, `cut`, `pattern`

**Flat-output (3):**
`param`, `sources`, `source`

**Implementation status of reserved built-ins**: every element name
in the table above is implemented and produces geometry today. Earlier
revisions of this spec carried a "reserved-but-deferred" note for
`<sweep>`, `<helix>`, and `<loft>`; all three have since landed.

The reserved set is pinned to the spec version declared in the file's
frontmatter. A future spec (e.g. 0.2) adding new built-ins does not
retroactively break files declaring `version 0.1` — those files keep
the 0.1 reserved set.

### 4.4 Collision rule

Within a single file, every name in `{built-ins} ∪ {imports} ∪ {<def>s}`
must be unique. Collisions produce a parse error pointing at both the
declaring locations.

### 4.5 Element instantiation

Any name in the file's vocabulary can be written as an XML element:

```xml
<extrude height="10"><circle r="5"/></extrude>     <!-- built-in -->
<bolt length="20"/>                                  <!-- import alias with param override -->
<blade/>                                             <!-- local def, used as inline geometry -->
```

The parser treats them uniformly. Resolution to either a built-in handler,
an imported file's `<part>`/`<assembly>`, or a local `<def>` happens at
compile time.

Attributes on instance elements (imports and defs) follow this rule:

- `id`, `at`, `port` are reserved structural attributes (Section 6).
- Any other attribute is a **param override**, applied to the imported
  part's `<param>`s — or, when the instance refers to a local `<def>`,
  to the def's `<param>` children (per §5.1, defs may declare params).
  Override names that don't match any declared param produce a
  compile-time error so typos like `<my-hole d="3"/>` against a def
  declaring `diameter` surface immediately instead of being silently
  ignored.

For example, `<bolt length="20" d="8"/>` overrides the imported bolt's
`length` and `d` params for this instance only.

### 4.6 Element attributes

All attribute values are strings in XML. They are parsed by type as
declared in the element's schema (Section 5):

- **Numeric** (`number`, `expr`): a literal or `{expression}`.
- **Vector** (`point2d`, `point3d`, `vector3d`): three space-separated
  numbers, OR a single axis alias (`+x`, `-z`), OR `{expression}`
  returning a vector.
- **String / enum**: literal text matching the spec.
- **Path data**: SVG path string OR `{expression}` returning a string.
- **Transform**: SVG-like transform chain.
- **Selector**: a CADML selector expression (Section 13).

---

## 5. Built-in Element Reference

Notation: `[attr]` = optional attribute. Default values shown after `=`.

### 5.1 Structural

#### `<part [name=""] [color=""]>`

A renderable solid body. Contains primitives, booleans, modifiers, transforms,
ports, and bare instances (named imports or local-def references without
`at`/`port` attributes — used for inline geometry composition).

Appears as a file's top-level export. A file may declare **one or more**
top-level `<part>` elements (§2.2); each becomes a separately-named output
part with its own colour. `<part>` never appears inside `<assembly>` —
assemblies contain instances, not parts.

The `color` attribute accepts:
- `#RRGGBB` or `#RGB` hex (case-insensitive; the short form doubles each nibble);
- a CSS Level 1 color name plus `orange` (case-insensitive): `aqua`, `black`,
  `blue`, `fuchsia`, `gray`, `green`, `lime`, `maroon`, `navy`, `olive`,
  `orange`, `purple`, `red`, `silver`, `teal`, `white`, `yellow`. Aliases:
  `cyan` = `aqua`, `magenta` = `fuchsia`, `grey` = `gray`.

An unrecognised name or malformed hex string is treated as "no color"; the
renderer falls back to its hash-palette default rather than hard-erroring.

Children:
- Any geometry-producing elements (primitives, booleans, modifiers,
  transforms via `<group>`).
- Zero or more `<port>` elements.
- Zero or more bare instance elements (`<X/>` for any X in the file's
  vocabulary, *without* `at`/`port` — used as inline geometry).

#### `<def name="">`

A named geometry block. Two distinct uses:

- **In authoring CADML (`.cadml`)**: a file-private helper. Cannot have a
  `<port>` child (anything with ports is a part — extract to its own file).
  Cannot appear inside another `<def>`. Used by `<X/>` (where `X = name`)
  within the same file only. May contain bare instance references
  (imported parts or other local defs without `at`/`port`) as inline
  geometry.

- **In flat CADML (`.fcadml`)**: any reusable geometry block. Imported
  `<part>` declarations from other files become `<def>`s after compilation,
  so flat-`<def>`s *do* carry `<port>` children when they originated from
  imported parts. The compiler hoists all defs (local helpers + imported
  parts) into a single namespace.

Children (authoring): any geometry-producing elements except `<port>`,
plus bare instance references (`<X/>` without `at`/`port`), plus
`<param>` declarations to give the def parameterised dimensions
(reachable from instance refs via `<my-def attr="value"/>` param
overrides — see `examples/mitered-frame/` for the canonical pattern).
Children (flat): any geometry-producing elements including `<port>`,
`<param>`, and bare instances.

#### `<assembly [name=""]>`

A composition of instances. The file's export when the file declares matings
between imported parts. An `<assembly>` may only appear as the file's
top-level export.

Children:

- Zero or more **instance elements**: `<X/>` where `X` names an imported
  `.cadml` file's `<part>`/`<assembly>`. Each instance may carry
  `at`/`port` attributes to establish a mate to its parent, an `id` for
  cross-referencing from `<connect>`, and any number of param-override
  attributes (Section 6.7).
- Zero or more `<connect>` elements for cross-cutting (graph) connections.

Local `<def>` references cannot appear as instances inside an `<assembly>`
because defs have no ports (they cannot mate). To compose multiple parts,
each must live in its own `.cadml` file and be imported.

Bare instances (without `at`/`port`) at the top level of an `<assembly>`
are allowed — they are placed at identity and form a forest of
unconnected sub-trees.

#### `<connect a="<id>.<port>" b="<id>.<port>" [allow-interference="true"]/>`

Cross-cutting mate constraint. Used inside `<assembly>` when nested
`at`/`port` syntax can't express a multi-edge graph (e.g., a bolt mating
both plates in a sandwich). Most assemblies don't need it.

`allow-interference="true"` opts the connected pair out of `cadml_check`
interference reports. Use for legitimate-overlap mates (a screw biting
into a plate, a press-fit pin in a hole). The flag accepts only the
literal string `"true"`; absent or any other value leaves the pair
subject to the default check.

#### `<port name="" position="" normal="" [up=""]>`

Connection-point metadata on a part. Survives compilation into `.fcadml`.
Has no geometric effect — purely declarative.

| Attr | Type | Default | Description |
|---|---|---|---|
| `name` | string | required | Unique within the part |
| `position` | point3d expr | required | Location in part-local coords |
| `normal` | vector3d expr | required | Outward-facing unit vector |
| `up` | vector3d expr | derived | Resolves rotational ambiguity |

#### `<group [transform=""] [color=""]>`

Grouping + coordinate transform inheritance. Children inherit the group's
transform (composed with their own) and color (unless overridden).

The `color` attribute (also valid on `<part>` and `<def>`) accepts:

- 6-digit hex with hash: `#RRGGBB` (e.g., `#4A4A4A`)
- 3-digit hex with hash: `#RGB` (shorthand for `#RRGGBB`, e.g., `#4AB` =
  `#44AABB`)

Other formats (named colors, RGBA, HSL) are **not** supported in 0.1.
Alpha is always 1.0; transparency is a renderer concern, not a CADML
attribute.

#### `<svg>`

Coordinate-frame wrapper for **pasted SVG content**. Inside `<svg>`, child
geometry is interpreted with SVG's y-down convention; the engine applies
`scale(1, -1, 1)` to descendant geometry so a snippet copied out of an SVG
file (Inkscape, Figma, hand-authored, generated) renders right-side-up in
CADML's y-up world.

Two mechanisms handle the y-down → y-up conversion:

1. **Engine-level winding canonicalisation** — every polygon entering an
   `<extrude>` or `<revolve>` (whether via `<path>`, `<rect>`, `<circle>`)
   is checked for orientation; CW polygons are reversed in place to CCW
   before the cap triangulator and side-face emitter run. SVG paths are
   typically CW in CADML's math coordinates (because the y-axis flip
   reverses winding direction), so this canonicalisation is what keeps
   SVG-pasted polygons valid even if their author thought CCW.
2. **Negative-determinant winding flip** in the transform application —
   when a transform reverses orientation (a y-flip qualifies, as do
   `scale(-1, 1, 1)` and `mirror(...)`), every triangle's winding is
   flipped post-transform so faces stay outward.

Together, an `<svg>` block keeps face winding consistent through the y-flip
without the author needing to think about it.

`<svg>` carries no authored attributes today. Future revisions may add
`viewBox` for SVG-coordinate-to-mm mapping and import-time attribute
mapping (e.g., `fill` → `<part color>`); the current minimal form is
forward-compatible with those additions. Nested `<svg>` composes naturally:
two y-flips compose to identity, so an `<svg>` inside another `<svg>`
renders in the original CADML orientation.

```xml
<part name="logo-plate" color="#888888">
  <difference>
    <extrude height="2"><rect width="100" height="60"/></extrude>
    <svg>
      <!-- Coordinates as you'd write them in an SVG file. -->
      <group transform="translate(0, 0, -1)">
        <extrude height="4">
          <path d="M 20,15 L 80,15 80,45 20,45 Z"/>
        </extrude>
      </group>
    </svg>
  </difference>
</part>
```

#### `<script lang="lua">`

Inline Lua source. May appear at any top-level position; multiple `<script>`
blocks share one Lua state per file.

Content is parsed as XML text. **If the script body contains `<`, `>`, or
`&` characters (e.g. Lua comparison operators), wrap it in a CDATA
section**:

```xml
<script lang="lua"><![CDATA[
  for i = 0, n do
    if i < 5 then ... end
  end
]]></script>
```

(Treating `<script>` content as raw text per HTML's convention is on the
0.2+ roadmap. Until then, the standard XML escaping rule applies.)

Attributes: `lang="lua"` (only `lua` supported in 0.1).

#### `<for>` (authoring-only; compiled away)

Iteration construct. Two forms:

```xml
<for var="i" from="0" to="10" steps="11">    <!-- uniform range, 11 values -->
  ...
</for>

<for var="c" values="nw ne sw se">           <!-- explicit values, 4 iterations -->
  ...
</for>
```

The loop variable is available as `{var}` in any expression inside the loop
body. The compiler unrolls each iteration into literal sibling elements
before assembly compilation.

The loop variable **must not shadow a frontmatter param** in the
enclosing document. Substitution into the loop body is textual, so a
`<for var="r">` inside a file that also declares `param r` would
silently capture the param. The bundler rejects this with a
`Composition` error pointing at both the loop and the shadowed param;
rename one of them.

### 5.2 2D primitives

#### `<circle [cx=0] [cy=0] r="" [segments=""]>`

Circle in the current sketch plane. `r` is required; `cx`, `cy` default to 0.

`segments` controls the polygonal tessellation. When omitted (the
common case), the engine picks an **adaptive** count: the chord-to-arc
sagitta stays below 0.005 document units, with the result clamped to `[8, 256]`.
That tolerance keeps cylindrical surfaces visually smooth at typical
viewer zoom levels (an M8 shank at r=4 mm gets ~64 segments) while
preventing huge flanges from saturating the budget (a 100 mm bore
tops out at the 256 clamp). Pass an explicit positive integer to
override — `segments="6"` makes a hexagon, `segments="32"` forces
the lighter faceting used in older revisions for reproducible tests.

#### `<rect [x=0] [y=0] width="" height="" [rx=0] [ry=rx]>`

Axis-aligned rectangle. `width` and `height` required.

#### `<path d="">`

Arbitrary 2D profile via SVG path data. Must close with `Z`. Supported
commands (see [`svg-subset.md`](svg-subset.md) for the full grammar):
`M` (move), `L` (line), `H` / `V` (horizontal / vertical line),
`C` / `S` (cubic Bezier with optional smooth continuation), `Q` / `T`
(quadratic Bezier with optional smooth continuation), `A` (arc),
`Z` (close). Lowercase variants of every command are supported as the
relative-coordinate form.

#### `<sketch [plane=xy] [origin="0 0 0"] [rotation=0] [normal=""]>`

A 2D drawing plane positioned in 3D space. Used to (a) wrap a 2D
primitive that needs explicit plane orientation (otherwise the
primitive defaults to the XY plane), and (b) supply each
cross-section to `<loft>` (see §5.3).

| Attr | Type | Default | Description |
|---|---|---|---|
| `plane` | enum | `xy` | One of `xy`, `xz`, `yz` |
| `origin` | point3d expr | `0 0 0` | Plane origin in world coords |
| `rotation` | number expr (deg) | `0` | Rotation around the plane normal |
| `normal` | vector3d expr | derived from `plane` | Override the plane normal explicitly |

When `normal` is given, it overrides the implicit normal from `plane`.
The sketch's local frame is computed as `right = up × normal`, where
`up` is derived as follows:

- If `plane="xy"`: `up` = `+y`.
- If `plane="xz"`: `up` = `+z`.
- If `plane="yz"`: `up` = `+z`.
- If `normal` is given (overriding `plane`): `up` is the projection of
  `+z` onto the plane perpendicular to `normal`. If `normal` is parallel
  to `+z` (within tolerance), `up` falls back to `+y`.

### 5.3 2D-to-3D

#### `<extrude height="" [scale=1] [draft=0] [symmetric=false] [direction="+z"]>`

Linearly extrude the contained 2D profile into 3D.

| Attr | Type | Default | Description |
|---|---|---|---|
| `height` | number expr | required | Distance to extrude |
| `symmetric` | bool | `false` | If true, extrude `±height/2` from sketch plane |

Children: a 2D primitive (`<rect>`, `<circle>`, `<path>`) or a `<sketch>`.

**Reserved attributes — rejected in 0.1.** The following attribute
names are reserved for a future release; the v0.1 bundler and
evaluator both reject them with a clear error so authoring tools
cannot quietly produce wrong geometry.

| Attr | Reserved for | Workaround in 0.1 |
|---|---|---|
| `scale` | End-cap scale factor (tapered extrude) | Use `<loft>` between two scaled profiles. |
| `draft` | Draft angle (sidewall taper) | Use `<loft>` between an offset profile pair. |
| `direction` | Extrusion direction overriding `+z` | Wrap the `<extrude>` in `<group transform="rotate(...)">`. |

#### `<revolve axis="z" [angle=360]>`

Rotate the contained 2D profile around an axis.

| Attr | Type | Default | Description |
|---|---|---|---|
| `axis` | axis alias (`x`/`y`/`z`/`+x`/...) | required | Rotation axis |
| `angle` | number expr (deg) | `360` | Rotation angle |

Children: exactly one of:
- A 2D primitive (`<rect>`, `<circle>`, `<path>`) — implicitly in the
  XY plane.
- A `<sketch>` containing a 2D primitive — for explicit plane control.

#### `<sweep>`

Translate a 2D profile along a guide curve. The 2D profile is placed at
each sample frame using the **axis-perpendicular** convention:
`profile.x` maps to the outward radial direction (away from the helix
axis) and `profile.y` maps to `+Z` (the helix axis). This is the
standard convention for thread modeling — the cross-section stays
parallel to the helix axis at every step, so a triangular thread
profile produces a clean spiral groove.

Children: exactly two, in any order:
1. A 2D primitive (`<rect>`, `<circle>`, `<path>`) or a `<sketch>`
   (the profile to sweep).
2. A `<helix>` (the guide curve).

#### `<loft>`

Polyhedral interpolation between cross-sections — ruled surfaces between
consecutive `<sketch>` sections, ear-clipped caps at the ends.

Children: two or more `<sketch>` elements (cross-sections), in order from
start to end. Each `<sketch>` contains exactly one 2D primitive. The
`plane`, `origin`, `rotation`, and `normal` attributes on each `<sketch>`
position the section in 3D space (see §5.2).

**Vertex-count constraint**: all sections must produce the same number of
2D-tessellated vertices. The ruled side surface pairs corresponding vertex
indices ring-to-ring, so a square at one section and a circle at another
would require either resampling (not currently implemented) or an explicit
matching mode. Mismatched counts produce a warning and an empty mesh.

For 2-section convex lofts (the most common case), `<hull>` is often
simpler — wrap two thin extrudes at different z heights and `<hull>` them.
Use `<loft>` when you need a faceted multi-section surface with controlled
cross-sections at intermediate stations (compressor blades, tapered
airfoils, etc.).

#### `<helix radius="" pitch="" turns="" [taper=0] [direction="ccw"]>`

A helical guide curve, only meaningful as a child of `<sweep>`. The
helix is parameterised in the engine as:

```
x(t) = (radius + taper * t) * cos(2π * t * dir)
y(t) = (radius + taper * t) * sin(2π * t * dir)
z(t) = pitch * t,           t ∈ [0, turns]
```

where `dir` is `+1` for `ccw` and `-1` for `cw`. A standalone
`<helix>` (not inside a `<sweep>`) produces no geometry; the engine
emits a warning.

| Attr | Type | Default | Description |
|---|---|---|---|
| `radius` | number expr | required | Helix radius (must be > 0) |
| `pitch` | number expr | required | Distance per turn along axis (cannot be 0; use `<revolve>` for a planar arc) |
| `turns` | number expr | required | Number of turns (must be > 0) |
| `taper` | number expr | `0` | Per-turn radius change (positive = expanding spiral) |
| `direction` | enum | `ccw` | `ccw` or `cw` |

### 5.4 Booleans

#### `<union>`

Merge children into a single solid. Children: two or more geometry-producing
elements.

#### `<difference>`

Subtract subsequent children from the first.

When using `<difference>`, the cutting geometry **must** extend beyond the
target's surface by at least 1 mm on each cut side. Coplanar faces cause
boolean failures. The convention is to declare `<param name="overshoot"
value="1"/>` and use it consistently in cutter geometry. Failure to
overshoot is a runtime CSG error, not a parse error.

#### `<intersect>`

Keep only the volume common to all children.

#### `<hull>`

Convex hull of one or more 3D-producing children. The classic use is to
express a 2-section convex loft (e.g. circular base blending into a square
top): wrap two thin `<extrude>`s positioned at different z heights and
`<hull>` them.

Children: one or more geometry-producing elements. With one child the hull
of that single solid is returned (useful for hulling around protrusions);
with multiple children the hull spans them all.

Implemented via the underlying CSG library's hull operation; non-manifold or
degenerate inputs produce a warning and an empty mesh.

### 5.5 Modifiers

#### `<fillet radius="" [select=all]>`

Round selected convex edges to the specified radius. Tier 1 only — input
must satisfy the modifier predicate (Section 12.1) or the modifier errors.

#### `<chamfer distance="" [angle=45] [select=all]>`

Bevel selected convex edges. Tier 1 only.

#### `<shell thickness="" [open=""]>`

Hollow a part by inset offset. Tier 1 only — input must be `<extrude>` of a
convex 2D profile or `<revolve>` of a profile curve. Other inputs error.

#### `<cut face="" type="" [angle=""] [miter=""] [bevel=""]>`

Manufacturing cut on stock. Resolved at evaluation time (see §12.5);
the element survives compilation unchanged.

| Attr | Type | Description |
|---|---|---|
| `face` | enum (`start`/`end`) | Required. Which face of the target |
| `type` | enum | Required. `miter`, `bevel`, `compound`, or empty for freeform |
| `angle` | number expr (deg) | Single-axis cuts (`miter` or `bevel`) |
| `miter` | number expr (deg) | Compound cuts only |
| `bevel` | number expr (deg) | Compound cuts only |

Children: target geometry (first child) + optional freeform cutting geometry
(remaining children).

The angle sign rule (in target's local coords):
- Positive `angle` keeps the **-X edge** at the face position (for miter).
- Negative `angle` keeps the **+X edge**.
- For bevel: same logic along Y instead of X.

#### `<pattern type="" count="" [axis=z] [spacing=""] [angle=360]>` (authoring-only; unrolled by compiler)

Replicate child geometry. Lowered to N literal `<group transform="...">`
siblings at compile time (Section 12.6).

| `type` | Required attrs | Behavior |
|---|---|---|
| `linear` | `count`, `axis`, `spacing` | Linear array along axis |
| `circular` | `count`, `axis`, `angle` | Circular array around axis |

---

## 6. Composition (Authoring-only)

These constructs appear only in `.cadml` files and are compiled away.

### 6.1 Imports

See Section 3.4. After import, the imported file's exported `<part>`/
`<assembly>` is available as an element named after the alias.

### 6.2 Where instance elements appear

Instance elements come in two flavors:

| Flavor | Where allowed | Effect |
|---|---|---|
| **Mating instance** (with `at`/`port`) | Inside `<assembly>` only | Establishes a binary mate to the parent via the connect-solver |
| **Bare instance** (no `at`/`port`) | Inside `<assembly>`, `<part>`, or `<def>` | Inlines the imported geometry at identity (or at the surrounding `<group transform>` if any) |

Mating instances require the parent to expose ports, which `<assembly>`
provides via its imports. Bare instances are geometry references; they
work anywhere geometry is allowed.

Examples:

```xml
<part>                                <!-- a part composed of imported sub-parts -->
  <bracket/>                            <!-- inlined at identity -->
  <group transform="translate(0, 0, 50)">
    <plate/>                            <!-- inlined inside a transform -->
  </group>
</part>

<def name="frame-corner">             <!-- a private helper using imports -->
  <strut/>
  <plate/>
</def>

<assembly>                            <!-- mating composition -->
  <plate>
    <bolt at="hole" port="head"/>     <!-- mating: bolt's head mates plate's hole -->
  </plate>
</assembly>
```

### 6.3 Assembly nesting

Inside `<assembly>`, instance elements may use `at` and `port` attributes
to declare a mate to the parent:

```xml
<assembly>
  <plate>
    <bolt at="hole" port="head" length="20"/>
  </plate>
</assembly>
```

| Attribute | Meaning |
|---|---|
| `at` | Port name on the parent element |
| `port` | Port name on this element |

The two attributes together define a single binary mate: this element's
`port` aligns face-to-face with the parent's `at` port. Each child of an
instance establishes one mate; multiple children of the same instance
establish multiple independent mates (star topology).

### 6.4 Sub-assembly port traversal

When the parent is an imported `<assembly>` (a sub-assembly), `at` may
reach into the sub-assembly's namespace:

```xml
<bip>
  <plate-top at="plate-bottom.top" port="bottom"/>
</bip>
```

`bip.plate-bottom.top` accesses the `top` port of `plate-bottom` *inside*
the imported `bip` assembly. Dotted paths navigate one level per dot.

### 6.5 `<connect>` for graph topology

When tree nesting cannot express a multi-edge constraint (e.g., a bolt
constrained by two plates), instances may carry `id` attributes and
`<connect>` siblings explicitly reference them:

```xml
<assembly>
  <plate-bottom>
    <bolt id="b1" at="hole" port="thread-end"/>
  </plate-bottom>
  <plate-top id="pt" at="plate-bottom.top" port="bottom"/>
  <connect a="b1.shaft" b="pt.hole"/>
</assembly>
```

`<connect>` is the escape hatch; nested `at`/`port` is the common path.

### 6.6 Iteration

`<for>` (Section 5.1) iterates an authoring-time loop. Compiler unrolls
the loop into literal siblings before any other compilation pass.

### 6.7 Param overrides on instance elements

Attributes on an instance element are categorised into reserved structural
attributes and param overrides:

| Reserved structural attributes | Behavior |
|---|---|
| `id` | Names the instance for cross-references in `<connect>` |
| `at` | Port name on the parent (mating only) |
| `port` | Port name on this instance (mating only) |

Any other attribute becomes a **param override**:

```xml
<bolt id="b1" at="hole" port="head" length="20" d="8"/>
        ^      ^        ^           ^           ^
        |      |        |           |           +---- param override
        |      |        |           +---------- param override
        |      |        +-------- structural
        |      +------- structural
        +------ structural
```

Param overrides are validated at compile time:
- The override's name must match a `<param>` declared in the target
  (the imported file's frontmatter `param`s for imported parts, or
  the def's `<param>` children for local def references — see §5.1
  for the def-param-declaration syntax).
- The override value must satisfy the matched param's `(min, max)`
  constraints.
- Overrides whose name doesn't match any declared param produce a
  compile-time error (catches typos like `<my-hole d="3"/>` against
  a def that declared `diameter`).

---

## 7. Expression Language

### 7.1 Syntax

CADML expressions appear inside `{...}` in attribute values.

```
{name}              parameter reference
{name * 2}          arithmetic
{naca(c, t, 40)}    Lua function call
{module.func(x)}    namespaced Lua function call (imported .lua module)
```

Bare numeric literals are also valid:
```
height="10"         literal
height="{a + b}"    expression
```

### 7.2 Operators

| Operator | Meaning | Precedence |
|---|---|---|
| `()` | Grouping | highest |
| `unary -` `unary +` | Sign | high |
| `*` `/` `%` | Multiplicative | |
| `+` `-` | Additive | |

The 0.1 expression grammar supports arithmetic (`+ - * / %`),
parentheses, parameter references, and dotted-path Lua / native
function calls. Comparison, logical, ternary, and exponentiation are
not part of 0.1 — use a Lua helper for conditional logic. Example:
`{my_module.choose(a, b, c)}` calling
`function choose(a, b, c) if a > 0 then return b else return c end end`.

### 7.3 References

| Reference | Resolution |
|---|---|
| `name` | Frontmatter `param` declaration, or top-level Lua function from inline `<script>`, or loop variable from enclosing `<for>` |
| `module.field` | Imported Lua module's exported function/value |
| `module` | A `.cadml` import alias — error in expression context (use as element `<module/>` instead) |
| `module.func()` | Lua module call (returns number/string/vector) |

### 7.4 Axis aliases

Axis aliases are pure parse-time sugar. They appear only in attribute values
typed as `vector3d`:

| Alias | Vector |
|---|---|
| `+x` / `x` | `(1, 0, 0)` |
| `-x` | `(-1, 0, 0)` |
| `+y` / `y` | `(0, 1, 0)` |
| `-y` | `(0, -1, 0)` |
| `+z` / `z` | `(0, 0, 1)` |
| `-z` | `(0, 0, -1)` |

Resolution happens at parse time; the AST stores `Vec3` directly (or an
expression node that resolves to a vector).

### 7.5 Negation outside braces

Both forms are valid:
```
height="-{a}"      literal minus a single brace expression
height="{-a}"      negation inside the expression
```

### 7.6 Whitespace inside braces

Spaces inside `{...}` are insignificant: `{a*2}`, `{a * 2}`, `{ a * 2 }` all
parse identically.

---

## 8. Lua API

### 8.1 Sandbox

Lua runs in a strict sandbox. Available:

- `math.*` — full standard math library
- `cadml.path(points)` — `{{x,y}, ...}` → SVG path string `"M x,y L ... Z"`
- `cadml.param(name)` — read a CADML param by name (string)

Disallowed (not in environment):

- `io`, `os`, `debug`, `package`, `require`, `dofile`, `loadfile`, `load`
- File I/O, system calls, dynamic code loading

### 8.2 Param access from Lua

CADML kebab-case names are auto-translated to snake_case in Lua scope:

```
# CADML
param max-thickness = 0.12
```

```lua
-- Lua sees:
max_thickness   -- equals 0.12
```

Both `max_thickness` (direct) and `cadml.param("max-thickness")` (string
lookup) work inside Lua; direct access is the preferred form.

The translation is **one-directional**: CADML kebab-case → Lua snake_case.
Lua-side identifiers (function names, exported table keys) keep their
original spelling when referenced from CADML expressions:

```lua
-- airfoils.lua
function naca_4digit(c, t, n) ... end       -- snake_case in Lua
```

```
import "ctl/aero/airfoils.lua" as airfoils

# Reference from CADML expression — Lua snake_case kept as-is:
<path d="{airfoils.naca_4digit(chord, 0.12, 40)}"/>
```

CADML expression scope therefore mixes kebab-case (CADML params) and
snake_case (Lua module fields). The dot separator (`.`) makes the source
of each identifier unambiguous.

**Why the bridge is one-way.** Lua identifiers cannot contain `-` (a
hyphen is the subtraction operator), so a CADML param like
`max-thickness` is not a legal Lua name and *must* be rewritten to
`max_thickness` to be reachable from a script — the kebab→snake
translation is what makes CADML params visible in Lua at all. There is
deliberately no reverse (snake→kebab) translation: a Lua-side name such
as `naca_4digit` is already a valid token inside a CADML `{…}`
expression (the expression grammar accepts `_` in identifiers), so it
can be referenced verbatim. Auto-translating Lua names back to kebab
would instead *break* those references and force authors to rename their
Lua code. Keeping the bridge one-directional means each side is written
in its own idiom and neither has to know the other's spelling rules.

### 8.3 Lua module files

A `.lua` file imported via `import "x.lua" as m` is wrapped in a per-import
scope. Two patterns are supported, dispatched on whether the file ends with
a `return` statement:

**Free-form (no terminating `return`):**
```lua
function naca(c, t, n) ... end
function clark_y(c, n) ... end
```
Top-level globals become `m.naca`, `m.clark_y`.

**Module-pattern (terminating `return <table>`):**
```lua
local M = {}
local function private_helper(x) ... end
function M.naca(c, t, n) ... end
return M
```
The returned table becomes `m`. Local functions remain private.

A `return` of any non-table value is an error.

### 8.4 Inline `<script>` blocks

Inline `<script>` shares the same Lua state as imported modules. Top-level
functions inside `<script>` are accessible directly by name (not namespaced):

```xml
<script lang="lua">
  function helper(x) return x * 2 end
</script>
```

```xml
<extrude height="{helper(5)}">  <!-- works: helper visible in expression scope -->
```

Inline `<script>` code may also call into imported Lua modules. Imported
module aliases live in the host file's expression scope (and therefore in
the inline-Lua global scope):

```
import "ctl/aero/airfoils.lua" as airfoils

<script lang="lua">
  function tip_chord(root_chord, taper)
    return airfoils.scale(naca_0012, root_chord * taper)
  end
</script>
```

The reverse — an imported `.lua` module reaching into the host file's
inline `<script>` — does not work, because each imported `.lua` file is
sealed in its own scope (Section 8.3).

### 8.5 Cross-script scope

Imported `.lua` files cannot access each other's exports in 0.1 (each is
sealed in its own scope). Cross-module calls must go through the
consuming `.cadml` file's expression scope. (Deferred to 0.2+.)

---

## 9. Compilation Semantics

The compiler (`cadml_compile`) lowers `.cadml` to `.fcadml`. The pipeline:

```
1. Parse entry file (frontmatter + body)
2. Resolve imports (recursive)
   - .cadml files: parse, register exports
   - .lua files: load, evaluate to namespaced scope
   - cycle detection
3. Build per-file element vocabulary
4. Validate name collisions
5. Run <for> and <pattern> unrollers (per file, per body)
6. Per-part: collect ports, evaluating port expressions against
   compile-time param snapshot
7. Per-assembly: walk nested instance tree, generate implicit <connect>s,
   solve constraint graph, emit <part> with <group transform> siblings
8. Hoist all <def>s (local + imported) into a single namespace
9. Validate min/max constraints against use-site overrides
10. Emit .fcadml (Section 10)

Note: `<cut>` is **not** lowered here. The element is preserved and
the evaluator handles it during mesh build — see §12.5.
```

### 9.1 What gets baked

The compiler bakes (resolves to literal values):

- **Assembly transforms.** Connect-solver math runs against a snapshot of
  param values, producing literal `translate(...)` and `rotate(...)`
  strings on `<group transform>` (the engine's transform-string
  parser supports `translate`, `rotate`, `scale`, and `mirror`).
- **`<for>` and `<pattern>` iterations.** Each iteration produces a
  literal `<group transform="...">` sibling with loop variable
  substituted in attribute expressions (for `<for>`) or transform
  computed from index and pattern parameters (for `<pattern>`).
- **`<cut>` wedges.** The cutting half-spaces (planar) are constructed
  from compile-time-resolved angles and emitted as primitive geometry
  inside `<difference>`.

### 9.2 What stays symbolic

The bundler distinguishes two classes of attribute expressions and
treats them differently. The split keeps the evaluator free of any Lua
runtime dependency without sacrificing parametric re-rendering.

**Param-reference expressions** stay symbolic in the flat output. These
are expressions that reference *only* frontmatter params, literals, and
the 0.1 arithmetic operators (`+`, `-`, `*`, `/`, `%`, parens — see
§7.2). The engine resolves them at evaluate time against the
in-document `<param>` table:

- `<extrude height="{plate-h}">` keeps `{plate-h}` symbolic.
- `<extrude height="{plate-h + 2*overshoot}">` keeps the whole
  expression symbolic.
- `<rect x="{-plate-w/2}">` keeps it symbolic.
- `<port position="0 0 {body-l}" ...>` keeps it symbolic.

**Lua-call expressions** are evaluated **eagerly at bundle time**. These
are expressions containing a function call: `{naca(c, t, n)}`,
`{airfoils.naca(chord, thickness, 40)}`, `{math.max(plate-t, 6)}`. The
result is substituted into the attribute as a literal string. The
`<script>` block or imported `.lua` module that defined the function is
still preserved in the flat output (for source-map provenance) but the
result of the call is baked in.

Rationale: param-ref expressions are cheap to evaluate (a hash lookup
plus arithmetic) and useful symbolic for parametric re-rendering with
overridden params at runtime. Lua-call expressions are expensive (full
Lua runtime evaluation) and usually produce large generated strings
(NACA airfoil path data, gear-tooth flank points) that would bloat the
`.fcadml` if kept symbolic. Eager evaluation keeps the evaluator free
of Lua and the flat document free of bulky path data.

**Other items that survive symbolic:**

- **`<param>` declarations.** Carried into the flat output as
  `<param name="..." value="..."/>` body elements (one per param scope).
  The `min`/`max` attributes are dropped after validation (already
  checked at compile time).

### 9.3 Param snapshot

When the compiler needs a literal value (assembly solve, `<for>` bounds,
`<cut>` angles), it uses a snapshot of current param values. The snapshot
is the param's declared default, possibly overridden by the chain of
`<X param-name="value"/>` instance overrides.

This means: changing a param's value at runtime (via API) updates geometry
expressions but **does not** update assembly transforms. Recompilation is
required for runtime param changes that should affect mating geometry.

### 9.4 Validation checks

Failures at any of these checks produce a hard error with source location:

- **Source loading**: parse errors, missing imports, import cycles,
  unknown elements.
- **Symbol resolution**: name collisions across `<def>`s, `<part>`s,
  `<assembly>`s, and `<import>` aliases.
- **`<for>` unrolling**: loop bounds not resolvable at compile time.
- **`<cut>` lowering**: inconsistent parameters (e.g. depth larger than
  the target bounding box, conflicting pivot specifications).
- **`<port>` resolution**: port expressions referencing undefined
  names.
- **Assembly composition**: `<connect>` references to nonexistent
  ports, over-constrained mates (multiple connects disagreeing within
  tolerance), under-constrained mates that cannot be resolved.
- **Param overrides**: instance-level `param-name=` values that fall
  outside the param's declared `(min, max)` bounds.

---

## 10. Flat Format (`.fcadml`)

### 10.1 Structure

```
[ Frontmatter — settings only ]
version 0.1
units mm
description "..."

[ Body — XML ]
<sources>
  <source id="0" path="..." hash="..."/>
  <source id="1" path="..." hash="..."/>
  ...
</sources>

<def name="..."> ... </def>     <!-- one per imported part + private helpers -->
<def name="..."> ... </def>

<script lang="lua"> ... </script>    <!-- merged from all source files -->

<part name="...">                    <!-- one or more exports (§2.2) -->
  ...
</part>
```

### 10.2 Frontmatter restrictions

`.fcadml` frontmatter contains only settings (`version`, `units`,
`description`). No imports, no params, no directives. All composition has
been resolved.

### 10.2.1 Where params land in `.fcadml`

Frontmatter `param` declarations from authoring files become XML `<param>`
elements in the flat output, placed according to which file they came from:

- **Entry file's params**: emitted as direct children of the entry file's
  exported `<part>` element, ahead of the geometry tree.
- **Imported file's params**: emitted as direct children of the
  corresponding `<def>` (the imported file's exported `<part>` is now a
  `<def>` in the host's namespace).

Each `<param>` element preserves the `name`, `value`, and any `min`/`max`
constraints from the source declaration. The `min` and `max` attributes
are **literal numbers only** in `.fcadml` (any expression in the source
is resolved at compile time against the param snapshot). The `value`
attribute may be a literal or an expression — expressions resolve at
runtime against current param state.

**Order is preserved from the source declaration**, both for entry-file
hoists and for imported-file hoists. When a `value` expression references
another param (a *derived param*, e.g. `param wh-z = {bk-bot-z + axle-inset}`),
the engine binds params left-to-right so each `value` evaluates against
the params declared before it. Forward references inside `value` are
unsupported and yield a "could not evaluate" warning at compile time.

Derived params behave identically whether the file is the compilation
entry or is imported as a subfile. A subfile may freely declare derived
params that reference its own earlier-declared params; the bundler
preserves their order when hoisting them onto the imported `<def>`.
Subfile derived params do **not** see the importing host's params — only
the subfile's own declared params (with values supplied by instance
overrides where the host passes them).

```xml
<def name="bolt">                          <!-- imported from bolt.cadml -->
  <param name="length" value="30" min="5" max="200"/>
  <param name="d" value="10" min="3" max="30"/>
  <extrude height="{length}">
    <circle r="{d/2}"/>
  </extrude>
  <port name="head" position="0 0 {length}" normal="-z" up="+x"/>
</def>

<part name="rig">                          <!-- entry file's part -->
  <param name="bolt-spacing" value="50"/>
  <bolt length="20"/>
  <group transform="translate({bolt-spacing}, 0, 0)">
    <bolt length="20"/>
  </group>
</part>
```

### 10.3 Body element ordering in `.fcadml`

The body of a `.fcadml` file appears in this order:

1. `<sources>` element (always first; required if any `src` attributes are
   present elsewhere).
2. All `<def>` elements (including hoisted imports and local helpers).
3. Optional merged `<script>` block (concatenated Lua from all source
   files).
4. One or more `<part>` elements (the file's deliverables — see §2.2).
   Each `<part>` becomes a separately-named output mesh.

### 10.4 `<sources>` table

A single `<sources>` element near the top of the body lists every source
file consulted during compilation, indexed by ID:

```xml
<sources>
  <source id="0" path="shared/bolt.cadml" hash="abc123..."/>
  <source id="1" path="plate.cadml"       hash="def456..."/>
  <source id="2" path="rig.cadml"         hash="789012..."/>
</sources>
```

Path resolution rules:

- `path` attributes are stored **relative to the entry `.cadml` file's
  directory**. This makes the `.fcadml` portable when the entire project
  tree (entry + imports) is moved together.
- Forward slashes (`/`) are used as path separators regardless of host
  OS. Implementations normalise on emission.
- The entry file is always `id="0"`. Subsequent IDs are assigned in
  bundler discovery order (depth-first walk over imports). See
  [`flat-ir.md`](flat-ir.md) §2 for the same rule stated more formally.
- Hashes are SHA-256 of file content (UTF-8 bytes after BOM stripping
  but before line-ending normalisation), encoded as lowercase hex.

Tools opening a `.fcadml` resolve `path` against the directory containing
the `.fcadml` file (or against an explicit project root if the tool
provides one). If the source file at the resolved location has a
different hash, the source map is reported as stale.

### 10.5 `src` attributes

Every meaningful node in the body carries a `src` attribute:

```
src="<file-id>:<line>:<col>"
```

Format: dotted-decimal where:

- `<file-id>` references the `<sources>` table.
- `<line>` is 1-based line number in the source file.
- `<col>` is 1-based column in the source file.

Generated nodes (synthesised by compiler passes such as `<for>`/`<pattern>`
unrolling or assembly compilation) inherit `src` from the originating
source element.

### 10.6 `<group id>` for assembler-visible structure

Groups generated by assembly compilation carry an `id` attribute:

```xml
<part name="rig">
  <group id="plate-bottom">
    <plate/>
  </group>
  <group id="bolt" transform="translate(0, 0, -14)">
    <bolt length="20"/>
  </group>
</part>
```

Sub-assemblies preserve their internal hierarchy via nested groups:

```xml
<part name="bolt-stack">
  <group id="bip">
    <group id="plate-bottom"><plate/></group>
    <group id="bolt"><bolt length="40"/></group>
  </group>
  <group id="plate-top" transform="translate(0, 0, 6)">
    <plate/>
  </group>
</part>
```

ID rules:

- **Compiler-synthesised groups** (those generated by the connect-solver
  from `at`/`port` mates or from `<connect>`) always carry `id`. Default
  ID is the source instance element name; user-supplied `id="..."` on the
  instance element overrides.
- **User-authored `<group>` elements** preserve their structure into
  `.fcadml`. Their `transform` and `color` attributes carry through.
  They receive an `id` only if the user explicitly wrote one. Otherwise
  they have no `id` and are not addressable by downstream tools as
  scene-graph nodes (they're just transform wrappers).
- IDs are unique within the immediate parent group. Collisions
  auto-discriminate: `bolt`, `bolt#2`, `bolt#3` (deterministic per-file
  ordering, stable across re-compiles).
- Designer-internal groups (inside `<def>` bodies, no user-supplied `id`)
  carry `transform` but no `id`.

### 10.7 Idempotence

`cadml_compile` is idempotent on flat input: running it on a `.fcadml`
file produces the same `.fcadml` file (modulo source paths). This lets
tools chain the compiler safely.

### 10.8 Strip mode

`cadml_compile --strip-sources` produces a `.fcadml` without the `<sources>`
table and without `src` attributes — for production deploys where source
mapping is not needed and minimum file size matters. Cuts ~20% of file
size in typical cases.

---

## 11. Source Mapping

### 11.1 Goals

Every node in `.fcadml` traces to a line in an authoring `.cadml` or
`.lua` file. Tools (viewers, error reporters, IDEs) use this for
click-to-source navigation, error pointing, and debugging.

### 11.2 What is tracked

Per node:
- File ID (index into `<sources>`)
- Line number (1-based)
- Column number (1-based)

The triple is encoded as `src="<id>:<line>:<col>"`.

### 11.3 Synthesised nodes

Compiler-generated nodes (groups from connect-solving, unrolled `<for>`
iterations) carry the source location of the *originating* element:

| Generated node | Source location |
|---|---|
| Assembly `<group>` from `at`/`port` mate | The instance element with `at`/`port` |
| Assembly `<group>` from `<connect>` | The `<connect>` element |
| Unrolled `<for>` iteration | The `<for>` element |
| Unrolled `<pattern>` iteration | The `<pattern>` element |

For unrolled iterations, the synthesised `<group>` may carry an
`iteration="<index>"` attribute (optional, for tooling) so that
click-to-source can highlight "iteration 2 of 4" in addition to pointing
at the source line.

### 11.4 Hash-stamping

`<source ... hash="..."/>` records SHA-256 of the source file at compile
time. Tools comparing `.fcadml` against current source files detect
staleness:

- If the current file's hash matches: source map is fresh.
- If it differs: line numbers may be misaligned; tools warn.

Tools should refuse to navigate to a stale source line; instead, prompt
the user to recompile.

---

## 12. Modifier Semantics

The exact-dimension guarantees in this section (`radius` matches in
§12.2, `distance` matches in §12.3, uniform `thickness` in §12.4)
describe the **v0.1 reference engine** — the flat evaluator under
`cadml::engine::evaluate_flat`, implemented in
`src/engine/src/flat_geometry.cpp`. This is the engine the
`cadmlstl`/`cadmlcheck`/`cadmlbuild` CLIs run, and the contract for
`cadml::engine`.

### 12.1 Tier 1 predicate

`<fillet>`, `<chamfer>`, and `<shell>` operate only on inputs satisfying
the **Tier 1 predicate**:

> *All convex edges of the input mesh selected by the modifier's `select`
> attribute are straight line segments, AND all faces adjacent to those
> selected edges are planar within float tolerance.*

Implementation:

1. Compute logical faces by grouping coplanar adjacent triangles. Two
   adjacent triangles are coplanar when their face normals agree within
   the **planar-tolerance** (recommended: `1e-4` deviation in unit-normal
   length, ≈ 0.006° angle; implementation may pick within `[1e-5, 1e-3]`).
2. Logical edges are boundaries between distinct logical faces.
3. For each selected logical edge: must consist of a single straight
   segment (all sub-segments collinear within the same tolerance).
4. For each face adjacent to a selected edge: must be a single logical
   face (already true by step 1, but verified).

The planar-tolerance is implementation-defined within the recommended
range. Tighter tolerances reject more inputs (more conservative); looser
tolerances accept inputs with subtle non-planarity (less conservative).
The recommended `1e-4` is a balance suitable for typical mesh outputs of
CADML primitives.

Failure produces a hard error with diagnostic listing failing edges.

### 12.2 `<fillet>` semantics

When the predicate is satisfied:

- Each selected edge is replaced with a quarter-cylinder of the requested
  radius.
- At corners where two or more selected edges meet at 90°, a spherical
  octant patch joins the cylinders.
- Adjacent face triangles are trimmed back by the requested radius.

Tier 1 only handles 90° corners. Other corner angles error.

The output mesh's actual fillet radius matches the requested `radius`
attribute exactly (within float tolerance).

### 12.3 `<chamfer>` semantics

When the predicate is satisfied:

- Each selected edge is replaced with a planar bevel face of width
  `distance * sec(angle/2)`, where `angle` is the dihedral angle at the
  edge.
- At corners, planar miter patches join the bevels.
- Adjacent face triangles are trimmed.

Tier 1 only handles 90° corners.

The bevel's measured width perpendicular to the original edge matches
`distance` exactly.

### 12.4 `<shell>` semantics

`<shell>` operates only on:
- `<extrude>` of a 2D primitive whose profile is convex.
- `<revolve>` of a profile curve (any profile shape).

Implementation: compute the 2D inset of the source profile by `thickness`
using polygon-offset algorithms. Re-extrude (or re-revolve) the inset.
Subtract inner from outer.

Wall thickness is exact and uniform.

For other inputs (`<sweep>`, `<loft>`, `<difference>` of primitives,
non-convex profiles), `<shell>` errors.

The `open` attribute removes cap faces from the resulting shell. For an
`<extrude>`, the removable faces are the two caps: the **start** cap
(outward normal `-z`, at `z = 0`) and the **end** cap (outward normal
`+z`, at `z = height`). `open` accepts either form:

- A **face selector** (Section 13) evaluated against the two cap faces.
  `open="face:normal=+z"` opens the end cap; `open="face:position.z=0"`
  opens the start cap. A selector matching neither cap is a warning
  (the shell stays closed).
- The convenience **keywords** `start`, `end`, or `start end` —
  equivalent to `face:normal=-z`, `face:normal=+z`, and both,
  respectively.

`open="all"` is invalid (it would remove every face and leave an empty
mesh) and is rejected at compile time. `open=""` (the default) yields a
closed shell.

### 12.5 `<cut>` semantics

`<cut>` survives compilation (does not lower to `<difference>` at compile
time). The engine resolves it at evaluation time when the target's
bounding box is available.

Rationale: the cutting wedge position depends on the target's actual
extent (the *stays* edge per the angle-sign rule passes through the
target's bbox edge). Without evaluating the target's geometry, the
compiler cannot know the bbox, and a heuristic constant would produce
incorrect cuts for most input sizes.

Engine semantics (per the existing implementation):

1. Evaluate the target's geometry; compute its bbox.
2. Resolve `face`, `type`, `angle`/`miter`/`bevel` against current params.
3. Compute the cutting half-space plane in the target's local frame:
   - Position: at the target's `start` (z = bbox.min.z) or `end`
     (z = bbox.max.z) face.
   - Normal: derived from `type` and angle sign.
   - Pivot: passes through the *stays* edge per the angle-sign rule
     (positive miter angle → -X edge stays; negative → +X edge stays).
4. Construct a slab sized from bbox extent (so the cutting volume fully
   covers the side to remove).
5. Bound the slab to one half of the stock so steep angles don't clip
   the opposite face.
6. For compound cuts: rotate around two axes (`miter` × `bevel`).
7. For freeform cuts: use the user-provided child geometry as the cutter.
8. Subtract: `target ∖ cutting_volume`.

The wedge plane must pass through the stays-edge (not the centroid).
This was a historical bug; the engine has a dedicated test suite for it.

### 12.6 `<pattern>` lowering

`<pattern>` is **unrolled at compile time**, consistent with `<for>`. The
flat output never contains `<pattern>` elements.

Each iteration becomes a literal `<group transform="...">` sibling
containing a copy of the pattern's children:

- `<pattern type="linear" count="N" axis="<axis>" spacing="S">` lowers to
  N groups, each with `transform="translate(i*S * axis-vec)"` for
  `i = 0 .. N-1`.
- `<pattern type="circular" count="N" axis="<axis>" angle="A">` lowers to
  N groups, each with `transform="rotate(i*A/N, axis-vec)"` for
  `i = 0 .. N-1`.

Each unrolled group's `src` attribute points at the originating
`<pattern>` element (Section 11.3).

Output size impact: the geometry inside each iteration is typically a
bare instance reference (`<bolt/>`, `<plate/>`) of an imported part
or local `<def>`, not inline geometry, so unrolling adds N
lightweight group wrappers — negligible compared to the geometry size.

This makes the engine's vocabulary smaller (one less iteration construct
to handle) and gives downstream tools a uniform scene-graph view (each
pattern instance is a regular group node).

---

## 13. Selector Grammar

The `select` attribute on `<fillet>`, `<chamfer>`, and `<shell open="...">`
selects a set of edges or faces.

### 13.1 Grammar

```
selector       ::= "all" | scoped-predicate
scoped-predicate ::= scope ":" predicate
scope          ::= "edge" | "face"
predicate      ::= field comparator value
field          ::= identifier ("." identifier)*
comparator     ::= "=" | ">" | "<" | ">=" | "<="
value          ::= number | axis-alias | vector3d
```

Whitespace inside the predicate is allowed but not required:
`edge:dihedral>90` and `edge:dihedral > 90` are equivalent.

### 13.2 Edge fields and operators

| Field | Type | Comparators | Example |
|---|---|---|---|
| `along` | axis-alias | `=` | `edge:along=+x` |
| `dihedral` | number (deg) | `>`, `<`, `>=`, `<=`, `=` | `edge:dihedral>90` |
| `position.x`, `position.y`, `position.z` | number | `=`, `>`, `<`, `>=`, `<=` | `edge:position.z=0` |
| `length` | number | `>`, `<`, `>=`, `<=`, `=` | `edge:length>10` |

Equality (`=`) tests on numeric fields use a scale-relative
tolerance: `|a − b| < max(1e-9, 1e-6 · max(|a|, |b|))`. Authors
selecting on values whose magnitudes vary by orders of magnitude
within a single document don't need to worry about a single absolute
tolerance falling out of step.

`along` treats edges as **undirected** — a selector `along=+x`
matches edges whose direction is parallel to either `+x` or `-x`.
Authors who need signed orientation can use `dihedral` or
`position.*` selectors instead. The matching angle tolerance is
fixed at `1°` (`kSelectorAngleTolDeg`).

### 13.3 Face fields and operators

| Field | Type | Comparators | Example |
|---|---|---|---|
| `normal` | vector3d (or axis-alias) | `=` | `face:normal=+z` |
| `position.x`, `position.y`, `position.z` | number | `=`, `>`, `<`, `>=`, `<=` | `face:position.z=10` |
| `area` | number | `>`, `<`, `>=`, `<=`, `=` | `face:area>100` |

`face:normal=` matches when the angle between the face's outward
normal and the given vector is below the `1°` angle tolerance.
Faces are treated as **undirected**, matching the `edge:along=`
convention — `face:normal=+z` matches both `+z`-facing and
`-z`-facing faces. Use `position.*` for signed selection when
needed.

### 13.4 Combinators

Selectors are single predicates in 0.1 — there are no logical
combinators (`and`, `or`, `not`). Complex selection is achieved
through geometric decomposition (Section 12 — composition over
generality).

### 13.5 Selector errors

- **Malformed syntax** — an unknown scope or field, an illegal
  comparator for the field (`along`/`normal` accept only `=`), or a
  value that doesn't parse for the field's type — is a **compile-time
  error** reported by the bundler with the modifier's source location.
- **Wrong scope for the modifier** is also a compile-time error:
  `<fillet>`/`<chamfer>` `select` expect an edge selector (`edge:…`) or
  `all`; `<shell>` `open` expects a face selector (`face:…`) or the
  `start`/`end` keywords.
- A well-formed selector that **matches no edges/faces** produces a
  **warning**, not an error — the modifier leaves the mesh unchanged.
  (It does *not* silently fall back to "match everything".)
- A selector that matches edges failing the Tier 1 predicate
  (Section 12.1) produces an error.

---

## 14. Errors and Diagnostics

### 14.1 Error categories

Bundler categories (`CompileError::Category`):

| Category | When |
|---|---|
| `Parse` | Malformed XML or frontmatter |
| `Schema` | Valid syntax, but invalid attributes / structure |
| `Vocabulary` | Unknown element name, or local name collides with a reserved built-in |
| `Import` | Cannot resolve an import, import cycle, oversized source, or extension-dispatch failure |
| `Validation` | Param `min`/`max` violation, instance override out of range |
| `Composition` | `<connect>` references a nonexistent port, over- or under-constrained mate, `<for>` / `<pattern>` lowering failure, `<cut>` parameter inconsistency |
| `Lua` | Sandbox violation, runtime Lua error, missing global, type mismatch in a Lua call |
| `Internal` | Bundler-bug indicator — file a report if you see one |

Engine warnings / errors are emitted as `FlatEvalError` with a plain
text `message` and a `SourceRange`. The most common payloads:

| Payload prefix | Meaning |
|---|---|
| `division by zero in expression` / `modulo by zero in expression` | Promoted to an error in the result (hard) |
| `could not evaluate <attr> expression` | Surfaced as a warning; geometry continues with a 0 fallback |
| CSG / modifier failures | Surface as warnings; the affected mesh is empty |

### 14.2 Diagnostic format

Every error includes:
- Category
- Source file + line + column (where known)
- Brief explanatory message
- Suggestion when one applies

Example:
```
error[vocabulary]: bolt-stack.cadml:8 — unknown element <bolts>
  did you mean <bolt>?
  in scope: bolt, plate. built-ins: extrude, circle, rect, sketch,
                                    group, part, def, ...
```

### 14.3 Warnings

| Source | Reason |
|---|---|
| `cadml_check` interference | Two parts overlap above tolerance |
| `cadml_check` under-constrained | Instance has free DOF |
| Selector matches no edges/faces | The selector is dead code |
| Stale source map (in tools) | `.fcadml` references a `.cadml` whose hash has changed |
| Unrecognized frontmatter setting | Forward-compatible reading; ignore |

Warnings do not block compilation or rendering by default. CLI tools may
escalate warnings to errors with `--strict`.

---

## 15. Versioning

### 15.1 Spec versions

CADML evolves incrementally. Each spec version (this document is `0.1`) is
identified by the string in `version` declarations.

- **Patch (e.g., `0.1.1`)**: bug fixes only. No new elements, no schema
  changes. All `0.1.0` files parse identically under `0.1.1`.
- **Minor (e.g., `0.1`)**: additive — new elements, new attributes, new
  modifiers, new selectors. Existing files continue parsing under their
  declared version.
- **Major (e.g., `1.0`)**: breaking changes. Old files require migration.

### 15.2 Version pinning

The `version` declaration in each file determines which built-in element
set is reserved (Section 4.3). A file declaring `version 0.1` is validated
against 0.1's reserved set; future spec versions cannot break it by
adding new built-ins to that file's namespace.

The compiler must support reading and validating files for any spec
version it claims to implement. A `version` newer than the compiler
recognizes is an error.

### 15.3 Compatibility table

A 0.1.x compiler accepts files that declare `version 0.1`. Any other
literal version string (`0.2`, `1.0`, anything else) is rejected with
an unrecognized-spec-version error. The compiler does not heuristically
"degrade" newer files to 0.1 semantics.

---

## Appendix A — Pipeline summary

```
.cadml files                                     [authoring source]
    ├── frontmatter (settings + imports + params)
    └── body (XML: <def>s, <script>s, <part> or <assembly>)
        │
        ▼
    cadml_compile                                [bundler]
        ├── parse all imports recursively
        ├── build per-file vocabulary
        ├── unroll <for> and <pattern>
        ├── port collection per <part>
        ├── assembly compile (connect-solving)
        ├── hoist <def>s into flat namespace
        └── emit .fcadml
        │
        ▼
.fcadml file                                     [flat compiled]
    ├── settings frontmatter
    ├── <sources> table
    ├── <def>s (geometry definitions)
    ├── <script> (merged Lua)
    └── one or more <part>s (each a deliverable mesh, scene-graph hierarchy via <group id>)
        │
        ▼
    flat engine                                  [evaluator]
        ├── parse .fcadml
        ├── reject authoring constructs (defense in depth)
        ├── evaluate primitives, booleans, modifiers
        ├── apply transforms, resolve <param> expressions
        └── emit mesh / glTF / STL / 3MF
```

---

## Appendix B — Element reference summary

**Authoring-only (compiled away):**
`<assembly>`, `<connect>`, `<for>`, `<pattern>`, mating instance
elements (named imports with `at`/`port`).

**Survive into `.fcadml`:**
`<part>`, `<def>`, `<group>`, `<svg>`, `<port>`, `<param>`,
`<script>`, `<sources>`, `<source>`, all primitives (`<circle>`,
`<rect>`, `<path>`, `<sketch>`), all 2D-to-3D operations
(`<extrude>`, `<revolve>`, `<sweep>`, `<loft>`, `<helix>`), all
booleans (`<union>`, `<difference>`, `<intersect>`, `<hull>`),
`<fillet>`, `<chamfer>`, `<shell>`, `<cut>` (engine resolves with
bbox), plus bare named-element instantiation (`<bolt/>`,
`<plate/>`, resolved to their `<def>` lookup).

**Total reserved built-in names (0.1):** 30 (including the
flat-output trio `param`, `sources`, `source` that the compiler
emits but the user never authors directly).

---

*End of CADML 0.1 specification.*
