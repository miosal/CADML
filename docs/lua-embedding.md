# Lua Embedding

CADML embeds Lua 5.4 as the escape hatch for parametric geometry
helpers — anything where a static `<path d="...">` would be too rigid:
NACA airfoils, gear teeth, blade sections, splines, parametric curves.

This document specifies the Lua sandbox, the API CADML exposes, and the
interop conventions between CADML and Lua identifiers.

---

## 1. When to use Lua

What to write in CADML vs. what to write in Lua:

| Task | Tool |
|---|---|
| Parametric extrude of a circle | `<extrude><circle r="..."/></extrude>` — no Lua needed. |
| Box with rounded corners | `<extrude><rect rx="..."/></extrude>` — no Lua. |
| Polygon hole pattern | `<for><circle/></for>` — no Lua. |
| NACA airfoil cross-section | Lua: returns a `<path d="...">` string. |
| Involute gear teeth | Lua: per-tooth angles + flank points. |
| Cam profile | Lua: angle → radius function. |
| Spline with arbitrary control points | Lua: tessellate to path. |

Use builtins for static composition; Lua is only justified when the
geometry requires arithmetic the expression mini-language can't
express.

---

## 2. The sandbox

CADML loads Lua 5.4 in a **restricted sandbox**. The following
standard library tables are exposed in full:

| Library | Purpose |
|---|---|
| `math.*` | Trig, sqrt, floor, ceil, pi, etc. |
| `string.*` | format, sub, gsub, len, etc. |
| `table.*` | insert, remove, concat, sort, etc. |

The following are **deliberately not exposed**:

| Library | Reason |
|---|---|
| `io` | No filesystem access. |
| `os` | No `os.exit`, `os.execute`, `os.getenv`, `os.time`. |
| `debug` | No introspection that could escape the sandbox. |
| `package` | No `require`, no `dofile`, no `loadfile`. |
| `load`, `loadstring`, `loadfile`, `dofile` | No dynamic code loading. |

The sandbox blocks filesystem, network, and process-spawn primitives.
The implementation has not been security-audited, so do not rely on it
as a sole defence against hostile input.

---

## 3. The `cadml.*` API

CADML installs a `cadml` table in the Lua global scope with a small set
of helpers:

### 3.1 `cadml.param(name)`

```lua
local r = cadml.param("hole-r")
```

Looks up a CADML param by name. Returns its evaluated value (number,
or string for `description`/`tags`).

If the param doesn't exist, raises a Lua error. Unknown params are
never silently `nil`.

The lookup respects the current scope — a Lua helper called from
inside a `<def>` body sees the def's params; a helper called from a
top-level expression sees the file's params.

Note that the kebab-to-snake mapping (§4 below) applies *only* to
identifiers in `{…}` expressions. `cadml.param("hole-r")` accepts
the kebab-cased name verbatim because it's a string literal.

### 3.2 `cadml.path(points)`

```lua
local path = cadml.path({
  {0, 0},
  {10, 0},
  {10, 10},
  {0, 10},
})
-- returns "M 0,0 L 10,0 L 10,10 L 0,10 Z"
```

Convenience that builds an SVG path-`d` string from a Lua table of
2-element tables. The path is **implicitly closed** with `Z`.

Used for the common case of "I computed N points in Lua, I want a
closed polygon":

```xml
<extrude height="{h}">
  <path d="{my-lua.compute_outline(n)}"/>
</extrude>
```

---

## 4. Identifier mapping

CADML uses **kebab-case** (`hub-tip-d`); Lua uses **snake_case**
(`hub_tip_d`). The bundler maps between them automatically when
expressions cross the boundary.

### 4.1 In `{…}` expressions

When a CADML expression calls a Lua function:

```xml
<path d="{compressor.hub_profile(hub-tip-d, wheel-od, hub-h)}"/>
```

The arguments `hub-tip-d`, `wheel-od`, `hub-h` are CADML param
references. The bundler resolves them to their values, then calls the
Lua function with positional arguments:

```lua
compressor.hub_profile(16.0, 80.0, 22.0)
```

Inside the Lua function, you use snake_case names:

```lua
function M.hub_profile(hub_tip_d, wheel_od, hub_h)
  -- hub_tip_d = 16.0
end
```

### 4.2 In `cadml.param()` calls

```lua
-- inside Lua: pass the kebab-case name as a string
local r = cadml.param("hub-tip-d")
```

String lookup keys preserve the kebab format because they're not
identifiers (the Lua parser doesn't see them).

---

## 5. Module structure

A `.lua` file imported by CADML follows the standard module pattern:

```lua
-- compressor.lua
local M = {}

function M.hub_profile(r_top, r_base, h_total, h_lip, n)
  local pts = {}
  table.insert(pts, "0,0")
  table.insert(pts, string.format("%g,0", r_base))
  -- ...
  return "M " .. table.concat(pts, " L ") .. " Z"
end

function M.blade_plan(r_in, r_out, sweep_deg, thickness, n)
  -- ...
end

return M
```

The module returns its function table. CADML caches the returned
table under the alias name from the import:

```
import "compressor.lua" as compressor
```

After this, `compressor.hub_profile(...)` is callable from `{…}`
expressions in the same CADML file.

**When Lua calls run.** Lua-call expressions in attribute values are
evaluated **eagerly at bundle time**, not at render time. The bundler
invokes the function, gets the return value, and substitutes it as a
literal into the `.fcadml`. The evaluator then sees only the substituted
result — no Lua runtime needed downstream. (Pure param-arithmetic
expressions like `{plate-h + 2}` are different — they stay symbolic. See
[`../compiler.md`](../compiler.md) §2.10.)

This has a few consequences:

- Lua functions can be expensive (compute NACA airfoils with 400
  samples, etc.) because they only run once per bundle, not per render.
- Changing a Lua function and re-rendering the existing `.fcadml`
  has *no effect* — you need to re-bundle to pick up the new function.
- The `<script>` block or imported `.lua` module is still preserved in
  the bundled output for source-map provenance, but its functions are
  no longer called from any attribute.

**Module return convention:**

- A module that `return`s a table → table is bound to the alias.
  Functions are namespaced as `<alias>.func(...)`. Recommended for
  any module with more than one function.
- A module that has no `return` → free-form style. Top-level globals
  defined by the script are exposed in **both** namespaces: the alias
  table (`<alias>.func(...)`) AND the bare global (`func(...)`).
  Useful for one-helper-per-file:

  ```lua
  -- helper.lua
  function compute_height(t)
    return t * 10
  end
  ```

  Then in CADML:
  ```xml
  <import "helper.lua"/>
  <extrude height="{compute_height(layer-count)}">
  ```

  (You could equally have written `{helper.compute_height(...)}` —
  both forms resolve to the same function.)

  **Why the dual exposure?** Free-form modules in the wild typically
  call their own helpers (e.g. `compressor.lua`'s `hub_tx` calling
  `hub_r`). If those bare calls were stripped from `_G`, intra-module
  calls would break. Trade-off: two free-form modules defining the
  same function name will collide in `_G`. Use the module-pattern
  (with `return M`) when you need strict namespacing.

- A module that `return`s anything other than a table or nil — error
  at load time.

---

## 6. `<script>` blocks

For tiny inline helpers, `<script>` is sometimes more readable than a
separate `.lua` file:

```xml
<script lang="lua"><![CDATA[
  function bevel_angle(t)
    return 45 - 30 * t
  end
]]></script>

<for var="i" from="0" to="10" steps="11">
  <group transform="rotate({bevel_angle(i/10)}, 1, 0, 0)">
    ...
  </group>
</for>
```

**CDATA wrapping** (`<![CDATA[ ... ]]>`) is required when the Lua code
contains `<`, `>`, or `&` characters (which it usually does, given Lua's
comparison operators). Without CDATA, the XML parser would mis-tokenise
`x < y` as the start of an element.

Better-still: put Lua in a `.lua` file. External modules don't have
the CDATA trap, can be unit-tested separately, and are reusable across
multiple CADML files.

---

## 7. Determinism

The Lua sandbox is **deterministic by construction**:

- No `os.time()`, `os.clock()`, `math.random()` (well — `math.random`
  is exposed, but seeded to a fixed value at sandbox creation; for
  random geometry, seed it explicitly).
- No filesystem, no network, no environment variables.
- IEEE 754 doubles throughout.

So the same `.cadml` input produces the same `.fcadml` output, byte
for byte, on every machine.

If you need pseudo-random for an example file:

```lua
math.randomseed(42)        -- explicit seed → reproducible
local r = math.random() * 10
```

---

## 8. Error handling

Lua runtime errors bubble up to the bundler as `Category::Lua`
errors. The error message includes the Lua stack trace and the source
location of the calling CADML expression.

A division-by-zero in Lua produces `inf` (Lua's behavior), which then
propagates into the expression — typically caught by the downstream
attribute parser as "value out of range". For numeric robustness,
guard divisions in Lua:

```lua
if denom == 0 then error("denom must be non-zero") end
return num / denom
```

---

## 9. Reimplementation tips

If you're embedding Lua in a CADML reimplementation:

- **Use sol2 (or your language's equivalent)**, not raw Lua C API.
  The boilerplate savings are huge and the sandboxing patterns are
  well-trodden.
- **Build the sandbox in two passes**: load Lua's `_G`, then null out
  the disallowed tables, then expose `cadml.*` as the last step. Don't
  start from an empty table and try to add things back.
- **Cache loaded modules.** The same `.lua` file imported by two
  CADML files (transitively, via different entry files in a server
  context) should be loaded once and shared.
- **Surface Lua errors with source attribution.** The Lua stack trace
  refers to `.lua` line numbers; map those back to the source file
  via the bundler's source map.
- **Test the sandbox.** A `<script>` containing `io.open("/etc/passwd")`
  should be a runtime error, not a successful read. Write tests that
  assert each forbidden table is genuinely unavailable.
