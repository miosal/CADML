# CADML Compiler

This document specifies the parser and bundler — everything that runs
between `.cadml` source on disk and a `.fcadml` flat document ready for
the evaluator. If you are reimplementing CADML, this is the document
you'll spend the most time with.

Background reading: [`architecture.md`](architecture.md) for the
pipeline overview; [`spec/language.md`](spec/language.md) for what valid
`.cadml` looks like.

---

## 1. Parser

The parser has two halves that run in sequence on a single source string:

1. **Frontmatter parser** (`parse_frontmatter`) — line-oriented. Reads
   `version`, `units`, `description`, `tags`, `param`, `import` until
   it hits the first `<` after leading whitespace.
2. **Body parser** (`parse_body`) — XML. Wraps the body region in a
   synthetic root element so multiple top-level siblings (e.g., one
   `<def>` and one `<part>`) parse without explicit wrapping.

### 1.1 Frontmatter tokenization

The frontmatter is a stream of newline-terminated statements. Each
statement matches one of:

| Statement | Form | Example |
|---|---|---|
| Setting | `<key> <value>` | `version 0.1` |
| Description | `description "<string>"` | `description "M8 hex bolt"` |
| Tags | `tags "<string>"` | `tags "fastener,bolt"` |
| Import | `import "<path>" [as <alias>]` | `import "x.lua" as x` |
| Param | `param <name> = <expression> [(<constraint>=<value>, ...)]` | `param d = 10 (min=5)` |
| Comment | `# <text>` to end of line | `# inducer-side` |
| Blank | empty or whitespace-only line | |

**Whitespace rules:**

- Tabs and spaces are equivalent; both count as inter-token whitespace.
- Leading whitespace on a line is ignored. Frontmatter does not have
  significant indentation.
- Trailing whitespace is stripped before tokenization.
- Blank lines are permitted anywhere and have no semantic meaning.

**Comment rules:**

- `# ...` to end of line is a comment. Comments may stand alone or
  follow a statement: `version 0.1 # bumped from 0.1`.
- Comments do **not** continue across lines. There is no `/* ... */`
  block-comment form in frontmatter.
- A line starting with `#` is a pure comment line.
- XML-style comments (`<!-- ... -->`) are **not** recognised in
  frontmatter; a `<` character terminates the frontmatter region.

**Statement ordering** is enforced per spec §3.2: settings first, then
imports, then params. Statements out of order produce a parse error
with the line of the offender.

**Frontmatter termination.** The parser scans byte-by-byte until it
encounters `<` after consuming any leading whitespace on the current
line. The byte offset of that `<` is returned as `body_offset`. XML
declarations (`<?xml ...?>`) and XML comments (`<!-- ... -->`) at the
start of the body region are rejected with parse errors — the only
thing that's allowed to begin the body is a real XML element.

### 1.2 Body parser

The body is parsed as XML using pugixml. A synthetic root element wraps
the body string before pugixml sees it, so multiple top-level siblings
parse as a tree. The synthetic root is then unwrapped — `Document::nodes`
holds the children of the synthetic root, not the root itself.

**Allowed elements** are the reserved built-ins of the file's declared
spec version (31 as of 0.2 — see spec §4.3 and §15.2) plus
any import alias or local `<def>` name. Unknown element names produce
a `Vocabulary` parse error, *except* during initial parsing where the
single-file vocabulary check is deferred — the full check runs in the
bundler after imports are resolved.

**Attribute parsing** follows standard XML rules:

- Attribute values must be quoted with `"` or `'`.
- Entity references are decoded: `&lt; → <`, `&gt; → >`, `&amp; → &`,
  `&quot; → "`, `&apos; → '`, `&#NNN;` (decimal) and `&#xNNNN;` (hex)
  for Unicode codepoints.
- Whitespace inside attribute values is preserved verbatim.

**CDATA sections** (`<![CDATA[ ... ]]>`) are preserved verbatim. They
are primarily used inside `<script>` blocks to allow Lua code containing
`<`, `>`, `&` without XML-escaping.

**Synthetic root convention.** The body XML is parsed as if wrapped in a
single synthetic root element. This means the file:

```xml
<def name="hole">...</def>
<part name="plate">...</part>
```

is shape-equivalent to a hypothetical `<root><def/><part/></root>`. The
synthetic root is invisible to the AST — `Document::nodes` contains the
two top-level entries directly. Implementations are free to use a
sentinel root index (e.g., `-1`) or store the top-level set explicitly.

### 1.3 Source mapping

Every `Node` carries a `SourceRange { file_id, line, col }`. Conventions:

- `file_id` is `0` for the entry file. Imports get IDs `1, 2, ...` in
  bundler discovery order.
- `line` is 1-indexed. The line counter is normalised: CR, LF, and
  CRLF all advance the counter by exactly one.
- `col` is 1-indexed. The column counter resets to 1 after each
  newline.
- `SourceRange` records the **start** position of the element only.
  The closing tag's position is not recorded; tooling that needs the
  range walks to the end of the element's children.

The `SourceMap` (one per `Document`) is a parallel `vector<SourceFile>`
keyed by `file_id`, recording the file's `path` and the SHA-256 hash of
its byte content. Tooling that opens the corresponding `.fcadml` after
the source has been edited compares hashes to detect staleness.

### 1.4 Recoverable errors

The parser does **not** abort on the first error. Instead it accumulates
errors in `ParseResult::errors` and returns a partial `Document` that
editor integrations can use to provide partial syntax highlighting,
outline views, and autocomplete on the parseable prefix.

A `Document` with `errors.size() > 0` is **not safe** to pass to the
bundler. Tools that want to compile must check `ok()` first.

---

## 2. Bundler

The bundler turns a parsed `Document` (the *entry file*) plus its
filesystem context into a single self-contained `Document` (the
*flat* document). It runs eleven passes in fixed order. Earlier passes'
output is the input to later passes; there is no fixed-point iteration.

```
Entry .cadml
  │
  ▼
[1] Parse entry file ── libcadml
  │
  ▼
[2] Resolve imports ── recursive, cycle-detect
  │
  ▼
[3] Build per-file vocabulary ── element name → kind
  │
  ▼
[4] Validate cross-file name collisions
  │
  ▼
[5] Unroll <for> + <pattern>
  │
  ▼
[6] Collect ports (parametric position resolution)
  │
  ▼
[7] Lower assemblies (nested instances, connect graph)
  │
  ▼
[8] Hoist <def>s into single namespace
  │
  ▼
[9] Validate param min/max against instance overrides
  │
  ▼
[10] Emit .fcadml
```

### 2.1 Import resolution

Imports are resolved depth-first, starting from the entry file's
frontmatter. For each `import "path" [as alias]`:

1. **Resolve the path** to an absolute filesystem path:
   - `./x` or `../x` — relative to the importing file's directory.
   - `ctl/...` — relative to a standard-library root (the catalogue;
     not yet shipped — currently identical to "bare").
   - Bare paths — relative to the importing file's directory.
2. **Detect cycles.** Keep a stack of files currently being imported.
   If the resolved path is already on the stack, emit an `Import` error
   ("cyclic import") and skip.
3. **Detect already-seen** (non-cyclic). If the resolved path was
   imported earlier (different stack), reuse the existing
   `SourceFileId` — don't reparse.
4. **Recurse** — parse the new file, run frontmatter checks, then
   process its imports before its body.
5. **Assign alias.** If `as alias` was given, use it. Otherwise default
   to the filename without extension and without leading path
   components: `import "shared/lib/bolt.cadml"` → alias `bolt`.

**Symlinks** are followed via filesystem canonicalisation. A symlink
that creates a cycle is caught by step 2.

**Import order in the frontmatter is not significant.** All imports
form a DAG; the bundler walks it depth-first but the result is
order-independent.

**`.lua` imports** are not parsed as CADML. They are loaded into the
shared Lua state at evaluation time (see
[`lua-embedding.md`](lua-embedding.md)). They do contribute a
`SourceFileId` to the source map (so Lua errors can quote back to the
right file).

### 2.2 Per-file vocabulary

For each file in the import set, build a map of `element_name → kind`
where `kind` is one of:

- `BuiltIn` — one of the reserved names of the file's declared spec version.
- `Def` — a local `<def name="...">` declared in this file.
- `ImportAlias` — an `import "..." as <name>` declaration.

The vocabulary is used by the body re-parser to validate every element
name. Names not in the vocabulary produce a `Vocabulary` error with
the offending element's source location.

### 2.3 Cross-file collision check

For each pair of `Def` and `ImportAlias` names in a single file,
collisions are errors. Collisions across files are allowed (different
files have different vocabularies). The rule per spec:

- Two `<def>`s with the same name in the same file → error.
- A `<def name="X">` and an `import "..." as X` in the same file →
  error.
- Built-in names cannot be shadowed: `<def name="extrude">` is an
  error.

### 2.4 `<for>` and `<pattern>` unrolling

`<for>` loops are unrolled by *cloning* their child subtree once per
iteration value, wrapping each clone in a `<group iteration="N">` (a
transparent group with no transform — the `iteration` attribute is
metadata only). Loop-variable substitution is performed on the cloned
subtree before insertion: every `{i}` reference is replaced with the
literal value for that iteration.

**`steps` semantics.** `<for var="i" from="A" to="B" steps="N">` produces
`N` iterations with values evenly spaced from `A` to `B` *inclusive*:

```
values[k] = A + (B - A) * k / (N - 1)   for k = 0, 1, ..., N - 1
```

So `from="0" to="10" steps="11"` produces `0, 1, 2, ..., 10` (11
values). `steps="1"` produces just `A` (degenerate); `steps="2"`
produces `A, B`.

**`values` semantics.** `<for var="c" values="nw ne sw se">` iterates
over space-separated strings. The strings are not validated as
identifiers — they may contain digits, underscores, or arbitrary
characters (excluding whitespace). Substitution replaces `{c}`
verbatim.

**`<pattern>`** is sugar for a circular `<for>`:

```xml
<pattern type="circular" count="6" axis="z" angle="360">
  <child/>
</pattern>
```

unrolls to 6 clones of `<child/>`, each wrapped in
`<group transform="rotate(θ, 0, 0, 1)">` with `θ = k * 360 / 6` for
`k = 0..5`. The `axis` may be `x`, `y`, or `z`; `angle` may be less
than 360 to produce a partial arc.

After unrolling, `<for>` and `<pattern>` elements no longer appear in
the flat document — they have been replaced by their expanded children.

### 2.5 `<cut>` is NOT lowered

Earlier revisions of this document said the bundler lowered `<cut>` to
`<difference>` plus a wedge cutter. That is not what the 0.1 toolchain
does: `<cut>` survives compilation as a `<cut>` node in the `.fcadml`,
and the evaluator (`flat_evaluator.cpp`, `NodeType::Cut`) handles it
during mesh build instead.

The spec-level definition lives in
[`spec/language.md`](spec/language.md) §12.5. The reason for keeping
the operator alive past the bundler is that the lowering requires
geometric analysis of the cuttee (bbox, axis, edge positions) which is
cleaner to do once the evaluator already has the mesh in hand.

### 2.6 Port collection

For each `<part>` and `<assembly>`, scan for `<port>` children and
record them in a per-part port table:

```cpp
struct PortDecl {
    std::string name;
    Vec3 position;     // evaluated against this part's params
    Vec3 normal;
    Vec3 up;
};
```

Port positions are typically expressed parametrically:
`<port name="shaft-out" position="0 0 {body-l}" normal="0 0 1" up="1 0 0"/>`.
These expressions are evaluated *now*, against the part's param values,
so downstream assembly-compilation has concrete world-space points.

If `up` is missing, the bundler emits an error: a port with only
`position` and `normal` leaves one rotational DOF unconstrained, and
that's never what the user wants in an assembly context.

### 2.7 Assembly compilation

For each `<assembly>` body element:

1. **Enumerate instances** — every immediate child that isn't `<port>`,
   `<connect>`, or `<group>` is treated as an instance of either a
   local `<def>` or an imported file's exported part.
2. **Classify instances**:
   - *Bare* — no `at` or `port` attribute. The instance's geometry is
     inlined at identity (or at the enclosing `<group transform>`).
   - *Mating* — has `at="<src>"` and `port="<dst>"`. The instance is
     positioned by computing the transform that aligns its `<dst>` port
     against the named `<src>` port on a previously-placed instance.
3. **Resolve port references.** `at="a.port-name"` refers to instance
   `a`'s port `port-name`. Dotted paths (`at="a.b.port-name"`) traverse
   sub-assemblies: instance `a` must be a bare instance of an assembly
   containing instance `b`, which has port `port-name`.
4. **Compute alignment.** Given a source port `(p_s, n_s, u_s)` and a
   destination port `(p_d, n_d, u_d)`, compute the transform `T` such
   that after applying `T`:
   - The destination port's position lands on the source port's position.
   - The destination port's normal is anti-parallel to the source's
     normal (mating surfaces face each other).
   - The destination port's `up` aligns with the source's `up`
     (rotational lock).
5. **Inline geometry.** Once `T` is computed, the instance's part body
   is cloned into the assembly with `T` applied to its root group.

`<connect a="X.port1" b="Y.port2"/>` is an alternative to inline
`at`/`port` for multi-edge mate networks. Each connect adds a
constraint to a graph; the bundler solves the graph by depth-first
placement (first instance is placed at identity; each subsequent is
placed against an already-placed neighbour).

If the connect graph is over-constrained (a cycle of mates that don't
agree) or under-constrained (a floating subgraph), the bundler emits
a `Composition` error.

### 2.8 Def hoisting

After all imports are inlined, all `<def>`s from all files are collected
into a single document-level def table. Name collisions across files
are resolved by prefixing the file's alias: `bolt::hole`, `pulley::hole`.
This prefix lookup is only used internally — the original source name
is preserved for diagnostics.

### 2.9 Param validation

For each param in each part, validate the bundled value against any
declared constraints:

- `(min=N)` — value must be ≥ N (exclusive comparison: numeric strict
  greater-than-or-equal, no floating-point tolerance).
- `(max=N)` — value must be ≤ N.
- `(min=A, max=B)` — both.

For instance param overrides (e.g., `<m6-hole r="5"/>` overriding the
def's `r` param), the overridden value is also validated against the
def's constraints. Overriding a param the def doesn't declare is an
error.

**Param evaluation order.** Params are evaluated in *declaration order*
(top to bottom in frontmatter). Forward references are errors:

```
param a = 10           # OK: literal
param b = {a + 5}      # OK: a is already declared
param c = {d - 1}      # ERROR: d not yet declared
param d = 7
```

### 2.10 Expression substitution

After all unrolling and assembly compilation, the bundler walks every
attribute value and processes any `{...}` expressions it finds. There
are two cases, treated differently:

**Param-reference expressions** (the expression body is composed only
of param names, numeric literals, and arithmetic operators) are *left
symbolic*. The bundler validates that every name resolves to a declared
param, but does not substitute. The expression travels into the
`.fcadml` as-is. The evaluator resolves it at render time against the
in-document `<param>` table.

**Lua-call expressions** (the expression contains a function call —
either a top-level function from a `<script>` block or a qualified
call into an imported `.lua` module) are *evaluated eagerly*. The
bundler invokes the Lua interpreter, gets the return value as a
string, and substitutes it into the attribute. The result appears as
a literal in the `.fcadml`. The originating `<script>` is still
preserved in the flat output, but its functions are no longer called
from any attribute.

Both cases run *after* `<for>` unrolling so loop variables (which
become param-ref-style identifiers after unrolling) are already
resolved.

Rationale: param-refs are cheap to resolve (a hash lookup + arithmetic)
and useful as symbolic for runtime param overrides; Lua calls are
expensive (full Lua runtime invocation) and typically produce large
generated strings that would bloat the `.fcadml`. The split keeps the
evaluator independent of Lua while preserving the parametric story
for the cheap case.

A surviving `{...}` after substitution (e.g., one that referenced an
unknown identifier, or a Lua call that failed) is a compile error.

### 2.11 Flat-format emission

Serialisation rules:

- The flat document is emitted as UTF-8 XML, with the original
  frontmatter (`version`, `units`, `description`, surviving `param`s).
- Attribute order is unspecified; emitters typically emit in
  declaration order.
- Standard XML entity escaping applies to attribute values and text.
- A synthetic `<sources>` block (top of body) records the file ID
  table:
  ```xml
  <sources>
    <source id="0" path="bolt.cadml" hash="..."/>
    <source id="1" path="thread.lua"   hash="..."/>
    <source id="2" path="../shared/hex.cadml" hash="..."/>
  </sources>
  ```
- Each Node carries `src="<file_id>:<line>"` (or `src="<file_id>:<line>:<col>"`
  when col is significant) so the evaluator can attribute warnings to
  the right source line.
- Paths in `<source path="...">` are relative to the entry file's
  directory and use forward slashes regardless of host OS.
- Path normalisation (collapsing `./` and `../`) is recommended for
  readability but not required.

To strip the source map for production payloads (e.g., shipping a
`.fcadml` without the original `.cadml` paths), pass
`CompileOptions::include_source_map = false`. The `<sources>` block
and all `src=` attributes are omitted; diagnostics from evaluation
will still report line numbers but not file paths.

---

## 3. Lua sub-system in the bundler

`<script>` blocks and `import "x.lua"` directives interact with the
bundler in three places:

1. **At import resolution.** `.lua` files are recorded in the source
   map (so Lua errors carry file IDs) but their bodies are not parsed
   as CADML.
2. **At expression substitution.** When an attribute contains
   `{module.func(args)}`, the bundler invokes the Lua interpreter with
   the appropriate module loaded.
3. **At `<script>` evaluation.** `<script lang="lua">...</script>`
   blocks are executed once, in source order, before any expressions
   that reference symbols they define.

The Lua interpreter state is shared across the whole bundle — modules
loaded from one file's `import` are visible to expressions in any
file. This makes a `.lua` helper module (e.g., `airfoils.lua`)
reusable across multiple CADML files in the same project.

See [`lua-embedding.md`](lua-embedding.md) for the full sandbox
specification.

---

## 4. Determinism

The bundler is **deterministic given the same input bytes**. Specifically:

- Iteration order over filesystem directories (for catalogue resolution)
  is the order returned by the OS; we sort lexicographically before use.
- Hash maps that iterate during emission (e.g., the def table) are
  iterated in insertion order.
- Lua does not have access to time, randomness, or filesystem I/O
  (sandboxed; see [`lua-embedding.md`](lua-embedding.md)).
- Floating-point evaluation in expressions uses IEEE 754 doubles with
  no fast-math flags — reproducible across compilers and platforms.

The same `.cadml` input + the same set of imported files produces a
byte-identical `.fcadml`. The CI build verifies this for the example
corpus.

---

## 5. Error model

The bundler returns a `CompileResult` with `errors` and `warnings`. The
categories (per `CompileError::Category`):

| Category | When it fires | Recovery |
|---|---|---|
| `Parse` | Forwarded from parser (malformed XML/frontmatter) | None — fix the source. |
| `Schema` | Valid syntax, invalid element structure | Fix the structure (typically wrong child element). |
| `Vocabulary` | Element name unknown after all imports resolved | Check spelling; check import alias. |
| `Import` | Missing file / cycle / load failure | Fix the import path; resolve the cycle. |
| `Validation` | Param min/max violated | Adjust param default or constraint. |
| `Composition` | Assembly mate failure / unsupported `<cut>` configuration | Read the message; the bundler tries to be specific. |
| `Lua` | `<script>` runtime error / module load failure | Fix the Lua code. |
| `Internal` | Bundler bug indicator | Report it. |

See [`error-model.md`](error-model.md) for the full diagnostic model,
including how warnings propagate through the pipeline.

---

## 6. Reimplementation tips

If you're writing a bundler from scratch:

- **Start with parser + identity bundler.** Get to "I can read a .cadml
  with no imports and emit a .fcadml that looks identical" first. The
  identity bundler is just `Document → Document` with the source map
  preserved.
- **Add imports next.** Without unrolling or assembly. Most simple
  CADML files only use imports for `.lua` helpers — get those working
  before tackling cross-file `<def>` references.
- **Then unrolling.** `<for>` first, `<pattern>` second (it's just
  sugar). Test against `examples/mitered-frame/` which uses both.
- **Then expressions.** Build the expression evaluator before tackling
  assemblies — assemblies use it for port positions.
- **Then assemblies.** Single-level first (one `<assembly>` with bare
  instances), then mating with `at`/`port`, then nested.
- `<cut>` is not lowered in 0.1 — the evaluator handles it. If you
  ever do add a compiler-side lowering pass, save it for last; the
  edge-selection algorithm is the hairiest in the codebase.

Each milestone should pass the corresponding subset of the test suite.
See `CADML/tests/` for the per-stage test corpus.
