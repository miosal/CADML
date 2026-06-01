# Expression Mini-Language

This document specifies the `{…}` expression language embedded in CADML
attribute values and frontmatter `param` declarations.

Expressions are a small typed evaluator over numbers, vectors, and
strings, with an escape hatch into the Lua interpreter for anything
that can't be expressed concisely inline.

---

## 1. Where expressions appear

Expressions are delimited by `{` and `}` and can appear in three
contexts:

| Context | Example |
|---|---|
| Frontmatter `param` RHS | `param x = {a * 2 + 1}` |
| Element attribute value | `<extrude height="{plate-t + 2*overshoot}">` |
| Path / transform string | `<path d="M {bore-r}, 0 L {od/2}, 0 Z"/>` |

Multiple expressions may appear in a single attribute, interleaved
with literal text:

```xml
<group transform="translate({hole-x}, 0, {-overshoot})">
```

The attribute value is parsed left-to-right; each `{…}` is evaluated
and substituted into the surrounding text. The result is then handed
to the element-specific attribute parser as if the user had written
it inline.

---

## 2. Identifiers

Identifiers are **kebab-case**:

```
[a-z][a-z0-9-]*
```

This matches element names, param names, `<def>` names, and import
aliases.

Inside an expression, an identifier resolves in this order:

1. **Frontmatter param** declared in the same file.
2. **Loop variable** from an enclosing `<for>`.
3. **Lua module alias** from `import "..." as <alias>`.
4. **Function call** if followed by `(...)` — looks up in Lua's
   top-level functions.

A name with no resolution is a compile error: `{undefined-thing}`
fails the bundle.

### 2.1 Kebab-to-snake mapping for Lua

Lua identifiers can't contain `-` (Lua treats `-` as subtraction). When
a kebab-case CADML name is passed into a Lua call, the bundler maps
hyphens to underscores:

| CADML name | Lua name |
|---|---|
| `thread-d` | `thread_d` |
| `max-blade-width` | `max_blade_width` |
| `hub-r` | `hub_r` |

The mapping is purely lexical: every `-` becomes `_`, no other
transformation. So when you write
`{cw.hub_profile(hub-tip-d, wheel-od)}` in CADML, Lua sees the call
as `cw.hub_profile(hub_tip_d, wheel_od)`.

Your Lua module must use `_` not `-`:

```lua
-- in compressor.lua
function M.hub_profile(hub_tip_d, wheel_od)
  ...
end
```

---

## 3. Numeric literals

```
123        # integer
3.14       # decimal
1.5e-3     # scientific
-7.2       # negative
```

All numbers are IEEE 754 doubles. No suffixes for units — the document's
`units` setting applies at attribute-parsing time after the expression
has been substituted.

---

## 4. String literals

```
"hello"    # double-quoted
```

Strings appear rarely in CADML expressions — mostly in `description`
frontmatter and `<part name="...">` attributes, both of which don't
need expressions. But they're valid inside `{…}`:

```
{my-module.format_name(thread-d)}    # Lua returns a string
```

The string is substituted *unquoted* into the attribute value, which
matters for path data:

```
<path d="{cw.compute_path()}"/>     ← string becomes the d= value
```

---

## 5. Arithmetic

Binary operators (in precedence order, lowest to highest):

| Operator | Meaning |
|---|---|
| `+`, `-` | Add, subtract |
| `*`, `/`, `%` | Multiply, divide, modulo (`std::fmod` semantics) |
| `(…)` | Grouping |

Unary `-` (negation) binds tighter than `*`/`/`: `-x*y` parses as
`(-x)*y`. Use parens if ambiguous.

Exponentiation (`^`) is not part of the 0.1 grammar; the parser
rejects `^`. Use `pow(x, y)` instead (see §7).

Integer division is **not** distinguished — `5/2` produces `2.5`. To
floor-divide, use Lua: `{math.floor(5/2)}`.

---

## 6. Vectors

Three-component vectors are written as space-separated values inside
attributes that expect them — *not* with explicit notation in
expressions:

```xml
<port position="{ox} {oy} {oz}" normal="0 0 1"/>
```

The three `{…}` evaluate independently to scalars; the surrounding
attribute parser interprets the space-separated triple as a vector.

There is no `{ox, oy, oz}` tuple syntax — vectors are *attributes*, not
expression values.

---

## 7. Function calls

A function call is resolved in two stages. First the evaluator checks
its table of **built-in functions** (§7.1); an unqualified name that
matches a built-in with the right arity is computed natively in C++ and
never reaches Lua. Every other call — qualified names like `math.max`,
module functions, or unrecognised unqualified names — is **routed to
Lua**. The bundler maintains a shared Lua state preloaded with all
imported `.lua` modules; expression evaluation calls into that state.

Because built-ins are tried first, an unqualified `sin`/`cos`/`max`/… in
a CADML expression always means the native built-in, even if a Lua
module happens to define a global of the same name. To call a Lua
function unambiguously, qualify it (`math.sin`, `airfoils.naca`).

```xml
<path d="{airfoils.naca(2412, chord, 40)}"/>
<extrude height="{math.max(plate-t, 6)}">
```

The grammar:

```
function-call := qualified-name "(" arg-list ")"
qualified-name := identifier ( "." identifier )*
arg-list := ( expression ( "," expression )* )?
```

`math.*` (Lua's standard math library) is always available; module
identifiers come from `import "..." as alias` declarations. All
arguments are positional in 0.1 — the call site has no `name=value`
keyword-argument syntax. Pass a table from Lua-side helpers if you
need named parameters.

### 7.1 Built-in functions

These functions are evaluated natively (no Lua round-trip) and are
always available, regardless of whether any `.lua` module is imported.
They are matched by exact name **and** argument count.

| Function | Args | Returns | Notes |
|---|---|---|---|
| `sin(deg)` | 1 | number | argument in **degrees** |
| `cos(deg)` | 1 | number | argument in **degrees** |
| `tan(deg)` | 1 | number | argument in **degrees** |
| `asin(x)` | 1 | **degrees** | inverse sine; result in degrees |
| `acos(x)` | 1 | **degrees** | inverse cosine; result in degrees |
| `atan2(y, x)` | 2 | **degrees** | two-argument arctangent; result in degrees; argument order is `(y, x)` |
| `sqrt(x)` | 1 | number | square root |
| `abs(x)` | 1 | number | absolute value |
| `min(a, b)` | 2 | number | smaller of two values (binary only) |
| `max(a, b)` | 2 | number | larger of two values (binary only) |
| `pow(base, exp)` | 2 | number | exponentiation — the canonical form (the `^` operator is not part of the 0.1 grammar; see §5) |
| `floor(x)` | 1 | number | round toward −∞ |
| `ceil(x)` | 1 | number | round toward +∞ |
| `round(x)` | 1 | number | round to nearest integer |

**Trigonometry is in degrees, not radians.** This matches CADML's
degrees-everywhere convention for angles (`rotate`, `<helix>`, etc.),
but it differs from Lua's `math.sin`/`math.cos`, which take radians. A
file that mixes a CADML expression `{sin(30)}` (→ 0.5) with a Lua call
`math.sin(30)` (→ −0.988…) is mixing degree and radian conventions —
use `math.rad`/`math.deg` on the Lua side if you need to bridge them.

`min`/`max` are strictly binary; nest them for more arguments
(`max(a, max(b, c))`) or use Lua's variadic `math.max`.

#### Built-in constants

Two constants are pre-seeded into every expression scope:

| Constant | Value |
|---|---|
| `pi` | 3.14159265358979323846 |
| `tau` | 6.28318530717958647692 (= 2·pi) |

A `<param>` named `pi` or `tau` shadows the constant within its scope;
avoid those names unless that is intended.

---

## 8. When expressions get evaluated

The bundler treats two kinds of attribute expression differently:

- **Param-reference expressions** (only param names, literals,
  arithmetic ops) stay **symbolic** in the `.fcadml`. The evaluator
  resolves them at render time against the in-document `<param>`
  table. Example: `<extrude height="{plate-h + 2*overshoot}">` keeps
  the whole expression intact in the flat output.

- **Lua-call expressions** (any expression containing a function call)
  are **evaluated eagerly** at bundle time. The Lua return value is
  substituted into the attribute as a literal string. Example:
  `<path d="{naca(c, t, n)}">` becomes
  `<path d="M 0,0 L 2.5,2.61 ... Z">` in the `.fcadml`.

This is a deliberate split. Param-refs are cheap and useful symbolic
(for parametric re-rendering); Lua calls are expensive and would bloat
the flat output if kept symbolic. See [`../compiler.md`](../compiler.md)
§2.10 for the rationale and [`../lua-embedding.md`](../lua-embedding.md)
§2 for the Lua sandbox specifics.

Frontmatter `param` declarations themselves are *always* evaluated at
bundle time (so derived params land in the `.fcadml` as literals).

## 8.1 Evaluation order within frontmatter

Expressions in `param` declarations are evaluated in **declaration
order** (top to bottom in frontmatter):

```
param a = 10
param b = {a + 5}     ← OK, a is already declared
param c = {d - 1}     ← ERROR, d not yet declared
param d = 7
```

Forward references are compile errors. The bundler does not perform
fixed-point iteration; the order is strict.

Expressions in body attributes are evaluated **after** all params have
been resolved and all `<for>` loops have been unrolled. So
loop-variable references in attribute expressions see the correct
per-iteration value:

```xml
<for var="i" from="0" to="3" steps="4">
  <group transform="translate({i*10}, 0, 0)">
    ...
  </group>
</for>
```

After unrolling, the four group transforms are
`translate(0, 0, 0)`, `translate(10, 0, 0)`, `translate(20, 0, 0)`,
`translate(30, 0, 0)`.

---

## 9. Errors

Expression errors are reported with `Category::Expression` and carry
the source line of the offending attribute:

| Error | Cause |
|---|---|
| Undefined identifier | A name was referenced that doesn't exist in any scope. |
| Syntax error | Malformed expression (unbalanced parens, unexpected character). |
| Type mismatch | An operator received an unexpected type (e.g., string + number). |
| Lua error | The Lua call itself raised an error (caught and re-reported). |
| Division by zero | Self-explanatory. Produces `inf` in some implementations; CADML treats it as an error to avoid silent NaN propagation. |

A `{…}` that survives bundling is an error — every expression must
evaluate to a substitutable value. The error frame includes both the
attribute name and the position within the attribute where the
substitution failed.

---

## 10. Reimplementation tips

If you're writing the expression evaluator from scratch:

- **Parse to an AST first**, evaluate second. Don't try to combine the
  two — it makes diagnostics much worse.
- **Tokenise carefully around `-`.** In a CADML kebab-name context,
  `hub-r` is one token. In an expression context, `5-3` is three
  tokens. The disambiguator is whether you're at the start of an
  expression vs. inside an identifier.
- **Validate eagerly, evaluate lazily.** Validate that an identifier
  resolves at parse time of the expression — but defer Lua calls
  until evaluation time, since they can have side effects.
- **Cache evaluated params.** Once `param a = {expensive-call()}` has
  been computed, store the result; don't re-evaluate every time `a`
  appears.
