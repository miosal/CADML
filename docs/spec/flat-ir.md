# Flat IR — `.fcadml` Format

This document specifies the **flat intermediate representation** —
the format that comes out of the bundler and goes into the evaluator.
It is the binary contract between the two halves of the toolchain.

If you only ever ship CADML as a unified library (parser + bundler +
evaluator linked together), you can skip this document. The flat IR
matters when you want to:

- Compile on one machine and evaluate on another (CI, server-side
  rendering).
- Inspect or diff the compiler's output (debugging the bundler).
- Re-evaluate a part without re-parsing source files.
- Bundle a `.fcadml` as a production asset alongside a `.cadml` source
  it was generated from (the source map then enables tooling like
  jump-to-definition without shipping the source).

---

## 1. File shape

A `.fcadml` is **the same syntactic shape as a `.cadml`** — a line-
oriented frontmatter region followed by an XML body — but the bundler
has applied a sequence of lowering passes. After bundling:

1. **No frontmatter `import`s.** All transitive imports are inlined,
   either as `<def>` blocks merged into the host's namespace or, for
   `.lua` modules, as synthesised `<script>` blocks (see §5 below).
2. **No `<for>` or `<pattern>`.** Loop and pattern bodies are unrolled
   into `<group iteration="k">` wrappers in document order.
3. **No `<assembly>`.** Each `<assembly>` is compiled to a `<part>`
   whose children are the placed instances with their mating
   transforms already baked into `<group transform="...">` wrappers.
4. **No `<connect>` or `at=`/`port=` mating attributes** on instances.
   They are consumed during assembly compilation and disappear.
5. **No Lua-function-call expressions** of the form `{module.fn(...)}`.
   These are evaluated at bundle time and replaced with literal
   results (numeric, string, or `<path d="...">` payloads).
6. **A synthetic `<sources>` block** appears at the top of the body,
   recording the file IDs the source map references.

What *survives* lowering — by design — is described in §5.

Everything else (frontmatter syntax, body XML rules, element vocab)
is identical to authoring CADML. The same parser produces a
`Document` from both; the bundler is what guarantees the lowered
shape.

```
version 0.1
units mm
description "..."
param shank-l = 50            ← surviving frontmatter params (immutable)

<sources>
  <source id="0" path="bolt.cadml"        hash="abc...123"/>
  <source id="1" path="thread.lua"        hash="def...456"/>
  <source id="2" path="../shared/hex.cadml" hash="789...0ab"/>
</sources>

<part name="bolt" src="0:5">
  <difference src="0:8">
    <extrude src="0:9" height="50">
      <circle src="0:10" r="4"/>
    </extrude>
    <group src="0:14" transform="rotate(15, 0, 0, 1)">
      <sweep src="2:23">
        <helix src="2:24" radius="3.7" pitch="1.25" turns="38.4"/>
        <path src="2:30" d="M -1.083, 0 L 0.1, -0.625 0.1, 0.625 Z"/>
      </sweep>
    </group>
  </difference>
</part>
```

---

## 2. The `<sources>` block

```xml
<sources>
  <source id="0" path="entry.cadml"  hash="..."/>
  <source id="1" path="imported.cadml" hash="..."/>
  <source id="2" path="helper.lua"   hash="..."/>
</sources>
```

- `id` is a non-negative integer, monotonic from 0, dense (no gaps).
- `path` is **relative to the entry file's directory**, with forward
  slashes regardless of host OS. Path normalisation (collapsing `./`
  and `../`) is recommended but not required.
- `hash` is the lower-case hex of the file's SHA-256 byte content
  at the time of bundling. Tools comparing this hash against the
  current file on disk can detect staleness. (The attribute is named
  `hash` for brevity; SHA-256 is the only algorithm the 0.1 bundler
  emits.)

The entry file is always `id="0"`. Other IDs are assigned in
bundler discovery order (depth-first walk over imports).

---

## 3. The `src=` attribute

Every authored node carries a `src="<file_id>[:<line>[:<col>]]"`
attribute. Conventions:

- `src="0:5"` — file 0, line 5 (column unspecified).
- `src="0:5:12"` — file 0, line 5, column 12.
- The `<sources>` block itself has no `src=`. The implicit synthetic
  root has no `src=`.

When the bundler synthesises a node that didn't exist in the source
(e.g., the wrapper `<group iteration="3">` around a `<for>`-unroll), the
attribute is inherited from the **originating element** — for the loop
example, the `<for>` node's source. The rule: `src=` always points
at the user-authored source line that *caused* the node to exist.

---

## 4. Surviving `<param>` elements

Frontmatter `param` declarations survive into the flat file as
`<param>` XML elements inside the body — placed as direct children
of the entry's `<part>`s (for entry-file params) or of each imported
`<def>` (for that file's hoisted params). The flat file's frontmatter
itself contains only settings (no `param` lines, no `import` lines).
See `language.md` §10.2.1 for the placement rules.

```xml
<def name="bolt">
  <param name="length" value="30" min="5" max="200"/>
  <param name="d"      value="10" min="3" max="30"/>
  ...
</def>
```

Param **default expressions** are evaluated eagerly. Any `{…}` that
calls an imported Lua function is resolved at bundle time; bare
param references and native arithmetic stay symbolic so they re-
resolve against the param table the evaluator sees at runtime (which
may differ from the declared defaults if instance overrides are
applied).

Param **constraints** (`(min=N, max=N)`) are **preserved** — the
bundler has already validated them against the declared defaults, but
consumers of the `.fcadml` (analysis tools, editors, downstream
re-bundlers) may want to re-check them against new overrides, so they
survive into the flat output:

```
param shank-l = 50 (min=10, max=150)
```

---

## 5. What survives, what's removed, what's added

Compared to source `.cadml`:

### Removed by lowering

| Removed | Reason |
|---|---|
| `<for>` | Unrolled in place. Each iteration is wrapped in `<group iteration="k">`. |
| `<pattern>` | Lowered to `<for>` then unrolled. |
| `<assembly>` | Compiled to a `<part>` whose children carry mating transforms as `<group transform="...">`. |
| `<connect>`, `at=`, `port=` | Consumed during assembly compilation; never appear in the flat output. |
| `<import>` (frontmatter) | All transitive imports are inlined; the directive is removed. |
| `{module.fn(...)}` expressions | Evaluated eagerly at bundle time and replaced with literal values (number, string, or `<path>` payload). |

### Preserved into the flat document

| Kept | Notes |
|---|---|
| `<script>` blocks | **Survive.** Author-written `<script>` elements remain in place, and the bundler additionally synthesises a `<script lang="lua">` at the top of the body for every `import "x.lua"` in the frontmatter — wrapped so the import alias is bound as a global. The flat engine re-loads them on evaluation and exposes their functions to runtime expressions. |
| `<cut>` | **Survives.** Pivot-edge positioning needs the target's evaluated bounding box, so the element is left for the evaluator to resolve at mesh-build time (see `flat_evaluator.cpp`, `NodeType::Cut`). |
| `<def>` blocks | Hoisted into a single per-document namespace (no longer scoped to their source file). |
| Instance refs (`<my-def/>`) | Preserved; resolved at evaluation time against the hoisted def table. |
| `{param-ref}` expressions | **Survive.** A `{name}` whose `name` resolves to a frontmatter param is left symbolic. The engine substitutes the param's current value when it evaluates the node. |
| `{native-builtin(...)}` expressions | **Survive.** Arithmetic over param references and native helpers stays symbolic for the engine to evaluate. Only Lua-function calls (`{alias.fn(...)}`) are resolved at bundle time. |
| Param constraints (`min`/`max`) | Preserved on `<param>` declarations so consumers of the `.fcadml` can still re-validate. |

### Added by the bundler

| Added | Reason |
|---|---|
| `<sources>` block | Records the file ID table for the source map. |
| `src=` attributes on every authored node | Per-node source attribution. |
| `iteration="k"` on `<group>` wrappers from `<for>` | Loop variable trace. |
| Synthesised `<script>` blocks for each imported `.lua` | Make the import alias's functions available to runtime expressions on the engine side. |

---

## 6. Stripping the source map

For production payloads where the source map is dead weight (the
end consumer is a slicer or a renderer, not an editor), the bundler
can omit the `<sources>` block and all `src=` attributes. Pass:

```cpp
CompileOptions opts;
opts.include_source_map = false;
auto result = compile_file("entry.cadml", opts);
```

The resulting `.fcadml` is valid and evaluable; diagnostics from the
evaluator will still report line numbers (from any surviving
`src=`-less metadata) but not file paths.

---

## 7. Round-tripping

A `.fcadml` is itself valid `.cadml` — the parser accepts it without
modification. This enables round-tripping: parse a `.fcadml`, edit it,
re-serialise. The evaluator is happy to consume a re-emitted `.fcadml`
that was modified in this way (provided the modifications don't
re-introduce authoring constructs like `<for>` or `{…}` — those would
need re-bundling).

The compiler emits flat documents with attribute order, indentation,
and whitespace normalised so that two semantically-equivalent
bundlings produce byte-identical output. CI tests can assert against
a checked-in `.fcadml` for regression detection.

---

## 8. Stability

The `.fcadml` format is **versioned by the same `version`** as the
authoring language. A flat file declares `version 0.1` and is
interpreted by a 0.1 evaluator. Cross-version flat files are not
supported in 0.1.

When a future CADML (e.g. 0.2) lands, the flat format may gain new
elements (for deferred features like the selector grammar in §13 of the
language spec). A 0.1 evaluator reading a 0.2 flat file will reject it at
the version check; a 0.2 evaluator reading a 0.1 flat file will
work (backward-compatible).
