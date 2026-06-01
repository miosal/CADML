# Error Model

CADML separates **errors** (block compilation / rendering) from
**warnings** (don't block, but flag a probable issue). Every diagnostic
carries a `SourceRange` so tooling can quote back to the user's
editor.

---

## 1. Per-stage error types

Each pipeline stage has its own error type:

| Stage | Type | Categories |
|---|---|---|
| Parser | `ParseError` | `Parse`, `Schema`, `Vocabulary`, `Expression`, `Validation` |
| Bundler | `CompileError` | `Parse`, `Schema`, `Vocabulary`, `Import`, `Validation`, `Composition`, `Lua`, `Internal` |
| Evaluator | `FlatEvalError` | (no sub-categorisation — message string + source) |
| Exporters | `std::runtime_error` (thrown) | format-specific |

All three result types (`ParseResult`, `CompileResult`,
`FlatEvalResult`) carry both an `errors` vector and a `warnings`
vector, and an `ok()` accessor that returns true iff `errors.empty()`.

---

## 2. Errors vs. warnings

| Property | Errors | Warnings |
|---|---|---|
| Affects `ok()` | yes | no |
| Pipeline continues to next stage | no | yes |
| Affected geometry produced | no (empty mesh) | possibly partial |
| Should tools display | always, prominently | yes, secondary |

The rule for picking: if a downstream consumer can still do its job
sensibly, it's a warning. If not, it's an error.

**Example.** `<revolve>` with no profile child:
- The revolve produces no geometry (it has nothing to revolve).
- But the *containing part* may have other elements that still produce
  geometry — the rest of the part is fine, just this one operation is
  empty.
- → warning.

**Example.** `<revolve>` with a profile that crosses the rotation axis:
- The revolve operation is mathematically ill-defined (the profile
  would intersect itself when rotated).
- The engine can't produce *any* sensible mesh for this revolve.
- The user's intent is almost certainly a bug.
- → error.

---

## 3. Source-range propagation

Every node in every `Document` carries a `SourceRange { file_id, line,
col }`. Diagnostics inherit this. So a warning emitted by the evaluator
on a node produced by `<for>` unrolling in the bundler points back to
the original `<for>` element in the user's source file — not at the
synthesised intermediate node.

The invariant: a diagnostic's source range always points at a line
the user can open in their editor and fix.

When you generate a new node in a compiler pass, copy the source range
from the **originating** source element (the user-authored element
that caused the new node to exist). Don't make up new locations; don't
leave it zero.

---

## 4. Parser error categories

| Category | Cause | Recovery |
|---|---|---|
| `Parse` | Malformed XML, malformed frontmatter, I/O error, encoding error | Fix the source. The parser stops at the first unrecoverable point. |
| `Schema` | Valid XML, invalid attributes (missing required, wrong type) | Fix the attribute. Most schema errors are local. |
| `Vocabulary` | Unknown element name (single-file vocab only — full check is in the bundler) | Check spelling; the full check after imports may resolve it. |
| `Expression` | Malformed `{…}` syntax | Fix the expression. Often locally diagnosable (unbalanced parens, etc.). |
| `Validation` | Param violates declared `min`/`max` | Adjust the param value or the constraint. |

**Recovery semantics.** The parser is **recovering** — it accumulates
errors rather than aborting on the first one. Editor integrations
use this to keep showing syntax highlighting and outline for the
file even when there are errors. A `ParseResult` with any errors is
not safe to bundle, but it's safe to read.

---

## 5. Bundler error categories

| Category | Cause | Recovery |
|---|---|---|
| `Parse` | Forwarded from parser | Same — fix the source. |
| `Schema` | Forwarded from parser | Same. |
| `Vocabulary` | Element name unknown even after imports | Check imports + def names. |
| `Import` | Missing file, cyclic import, Lua load failure | Fix the path; resolve the cycle. |
| `Validation` | Param min/max violated, including via instance overrides | Adjust value or constraint. |
| `Composition` | Assembly mate failure, unsupported `<cut>` config, malformed `<connect>` | Fix the assembly (often a port-name typo). |
| `Lua` | `<script>` runtime error, Lua function call from `{…}` failed | Fix the Lua code. |
| `Internal` | The bundler hit a state it didn't expect | File a bug. Should never fire in normal use. |

---

## 6. Evaluator diagnostics (warnings)

Common warning messages:

| Message | Cause | Fix |
|---|---|---|
| `extrude has no 2D profile child` | `<extrude>` with no `<rect>`/`<circle>`/`<path>` child (often `<sketch>` wrap — not supported) | Use the primitive directly. |
| `revolve has no 2D profile child` | Same as above for `<revolve>`. | Same. |
| `sweep: missing 2D profile child` | `<sweep>` with only a `<helix>` and no profile. | Add the 2D primitive. |
| `loft: section N has X vertices but section 0 has Y` | `<loft>` sections produced different vertex counts (e.g., `<circle segments="32">` vs `<circle segments="64">`) | Match `segments` across sections, or pass equally-sampled `cadml.path` outputs. |
| `extrude/revolve/sweep: profile polygon is wound clockwise` | CW input (often pasted from an SVG file) | Wrap in `<svg>` or reverse the polygon. |
| `path: tessellated to fewer than 3 points` | `<path>` with no real geometry (e.g., `M 0,0 Z`) | Add `L` commands. |
| `boolean produced empty mesh` | Coplanar faces or non-manifold input | Add overshoot; check input meshes. |
| `revolve.segments out of range [3, 4096)` | `<revolve segments="2">` or huge value | Pick a value in range. |
| `hull: degenerate input (<4 non-coplanar vertices)` | Hull of a single point, two points, or collinear points | Add more vertices. |

Warnings do not stop evaluation. The affected element may produce
empty geometry; subsequent siblings still evaluate.

---

## 7. Evaluator diagnostics (errors)

| Message | Cause |
|---|---|
| `revolve profile crosses axis` | Profile has `x < 0` for `axis="z"`. |
| `boolean kernel failure` | Manifold returned empty due to non-manifold input. |
| `fillet: corner angle X° not in [85°, 95°]` | Tier-1 fillet limit (only handles ~90° corners). |
| `internal: unresolved instance <name>` | Bundler bug — instance reference survived bundling. |

---

## 8. Stale source maps

A `.fcadml` flat document carries source paths + SHA-256 hashes
in `<sources>`. When a tool opens the flat document later, it can
compare hashes against the current on-disk files:

- **Hash matches** → source is fresh; line numbers are reliable.
- **Hash mismatches** → source has been edited since bundling; the
  flat document's line numbers are stale.

Tools should not blindly navigate to stale line numbers; they should
either:
- Re-bundle and use the fresh source map.
- Warn the user and let them rebundle.
- Refuse to navigate and tell the user to rebundle.

A tool that can bundle on demand should prefer option (1).

---

## 9. Staleness in long-lived integrations (caller pattern)

Not an error the engine or any CLI here produces — a pattern for
callers that wrap the compiler / evaluator behind a longer-lived API
(an editor, a server). The `<sources>` hash machinery (§8) is the
mechanism such callers should use to detect when a previously-derived
result has been invalidated by an edit.

When a caller hands a client a result derived from some source file and
the user later edits that file, any subsequent operation that references
the now-stale content should be refused rather than silently acting on
outdated line numbers or geometry. The natural mechanism is the same
SHA-256 the source map already records: stamp the hash (or mtime) when
the source is first read, and on a later operation compare it against the
current file. A representative shape for such a "stale reference" signal:

```json
{
  "error": "stale_reference",
  "message": "source hash changed since it was last read; re-read or re-bundle",
  "current_sha256": "abc...123"
}
```

The engine gives you the building block (content hashes in `<sources>`);
the staleness policy itself lives in the caller.

---

## 10. CLI conventions

The reference CLI tools (`cadmlc`, `cadmlstl`, `cadmlcheck`, …)
follow standard Unix conventions:

- **Exit code 0** if `ok()` returned true on every stage.
- **Exit code 1** otherwise.
- **Errors written to stderr** with file:line prefix.
- **Warnings written to stderr** with file:line prefix and `warning:`
  marker.
- **Successful output written to stdout** (or to `--output` file).
- **`--strict` flag**: escalates warnings to errors (exit 1 if any
  warning fired).

Example output:

```
$ cadmlstl bolt.cadml --output bolt.stl
bolt.cadml:23: warning: extrude: profile polygon is wound clockwise.
[stl] wrote 4862 triangles to bolt.stl
$ echo $?
0

$ cadmlstl bolt.cadml --output bolt.stl --strict
bolt.cadml:23: warning: extrude: profile polygon is wound clockwise.
error: 1 warning(s) escalated to errors (--strict)
$ echo $?
1
```

---

## 11. Reimplementation tips

- **Don't share an error type across stages.** Each stage's errors
  have different semantics (parser errors are local; bundler errors
  may involve multiple files). Sharing a type tempts you to throw
  away the per-stage category info.
- **Always carry a SourceRange.** "Error at line 0" is useless.
  Investigate: which compilation pass dropped the source attribution?
- **Don't abort on the first error.** Tools want a list. Even the
  parser keeps recovering.
- **Warnings are returned, not logged.** Treat them as a separate
  vector on the result with `ok()` ignoring them — not as stderr
  messages tools may or may not surface.
