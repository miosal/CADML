# engine

The CADML geometry engine.

## Library

**`cadml_engine`** (alias `cadml::engine`) — the v0.1 engine.
Consumes a flat `Document` (post-bundler `.fcadml`) and produces a
`FlatEvalResult` (per-part meshes + diagnostics), plus the exporters
(STL / 3MF / glTF) and analysis primitives (mass, bounds, topology,
interference, wall thickness, hole inventory). Every CLI and the WASM
module link this.

## Layout

```
src/engine/
├── include/cadml/engine/   ─ public headers (flat_*.hpp)
├── src/                    ─ flat_*.cpp
└── tests/                  ─ GoogleTest binaries
```

The CLI mains live at `src/cli/`, not under this subtree.

## Tests

`tests/` — `test_flat_evaluator.cpp` (geometry + pipeline),
`test_flat_analysis.cpp` (mass / bounds / topology / measure / holes /
walls), and `test_flat_color.cpp`.
