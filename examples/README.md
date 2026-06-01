# Examples

A curated set of `.cadml` files exercising different CADML features.

Build any of them with the CLI:

```bash
cadmlc hex-bolt/hex-bolt.cadml -o hex-bolt.fcadml      # compile to flat
cadmlstl hex-bolt/hex-bolt.cadml -o hex-bolt.stl       # full pipeline → STL
```

## Index

| Example | Demonstrates |
|---|---|
| [`demo-l-bracket/`](demo-l-bracket/) | Simple L-bracket: extruded shape + holes. The smallest "real geometry" file. |
| [`angle-bracket/`](angle-bracket/) | Basic extrude + difference for a sheet-metal-style bracket. |
| [`flange/`](flange/) | Stacked-extrude flange disc with a `<pattern type="circular">` bolt-hole pattern. |
| [`de-laval-nozzle/`](de-laval-nozzle/) | Pure revolve of a complex `<path>` profile. |
| [`v-belt-pulley/`](v-belt-pulley/) | Revolve + grooved profile (V-belt cross-section). |
| [`hex-bolt/`](hex-bolt/) | Union of extrudes + helical thread via `<sweep>` over `<helix>`. Exercises most of the 0.1 language surface in one file. |
| [`hex-nut/`](hex-nut/) | Pair to hex-bolt: helical internal thread with the cutter apex pointing outward. |
| [`compression-spring/`](compression-spring/) | `<sweep>` of a `<circle>` along a `<helix>` — the simplest non-trivial swept geometry. |
| [`compressor/`](compressor/) | Lua-driven centrifugal compressor wheel: bell-shaped revolve + backswept blades via `<for>` + `<extrude>` of generated `<path>` data. |
| [`gear/`](gear/) | Involute spur gear teeth driven by a Lua module computing per-tooth flank points. |
| [`mitered-frame/`](mitered-frame/) | `<cut>` operator to miter four extruded bars into a closed picture-frame. |
| [`peg-in-hole/`](peg-in-hole/) | Two top-level `<part>`s positioned by arithmetic transforms — peg sits in a square cavity. `cadmlcheck` confirms the clearance gap (no interference). |
| [`demo-sweep/`](demo-sweep/) | Minimal `<sweep>` over a hand-authored helix. |
| [`demo-star/`](demo-star/) | Concave 10-vertex star traced with relative-coordinate `<path>` (lowercase `l` + `z`). |
| [`demo-svg-curves/`](demo-svg-curves/) | SVG-path subset features (`C`, `Q`, `A` curves). |
| [`demo-svg-import/`](demo-svg-import/) | The `<svg>` wrapper element with a y-flipped path paste-in. |
| [`showcase-cut-rx-symmetric/`](showcase-cut-rx-symmetric/) | `<rect>` with `rx` (rounded corners) inside a `<cut>` chain. |
| [`showcase-hull/`](showcase-hull/) | `<hull>` of two extruded sections — easier than `<loft>` for convex transitions. |
| [`showcase-path/`](showcase-path/) | Path-attribute features (multi-segment, sub-paths, line-to repetition). |
| [`showcase-fillet-chamfer/`](showcase-fillet-chamfer/) | Six side-by-side blocks for `<fillet>`, `<chamfer>`, and `<shell>` — all-edge, selector-driven, composed, and the open-cap shell. One modifier element per block. |
| [`propeller/`](propeller/) | 5-inch drone propeller: a 12-station polyhedral `<loft>` of Lua-computed Clark-Y airfoil sections, hub + bore, three blades via `<pattern>`. |
| [`caster-wheel/`](caster-wheel/) | Multi-file assembly: a fork, axle, and wheel imported from their own files and emitted as three separately-coloured top-level `<part>`s. Demonstrates multi-part 3MF export with declared `<port>` attachment points on every piece. |
| [`bolt-on-plate/`](bolt-on-plate/) | Canonical `<assembly>` + nested `at`/`port` mating model: a bolt's `head-seat` port mates a plate's `hole` port and the bundler solves the resulting transform. Assembly output is a single fused `<part>` — choose this pattern over the caster-wheel pattern when you want compiler-solved kinematics over per-part colours. |

## Recommended reading order

If you're new to CADML and reading the source to learn:

1. `demo-l-bracket/` — see the shape of a CADML file: your first real
   geometry (an extrude with holes).
2. `flange/` — see `<pattern type="circular">` for the bolt circle.
3. `de-laval-nozzle/` and `v-belt-pulley/` — see revolve.
4. `compression-spring/` and `demo-sweep/` — see sweep + helix.
5. `hex-bolt/hex-bolt.cadml` — the full-feature reference (~100 lines,
   touches everything except `<loft>`).
6. `showcase-fillet-chamfer/` — see all three Tier-1 modifiers
   (`<fillet>`, `<chamfer>`, `<shell>`) in one file.
7. `compressor/` and `gear/` — see Lua-driven parametric helpers.
8. `propeller/` — see `<loft>` of Lua-generated airfoil cross-sections
   (the most feature-dense single part).
9. `peg-in-hole/` — see a two-part export with interference checking.
10. `caster-wheel/` — see a multi-file import graph emitted as a
    three-part assembly with per-part colours and declared ports.

Each example folder contains a `.cadml` (and sometimes a `.lua` helper)
plus, in a few cases, a `session.md` chronicling how the example came
together. The session logs are kept as case studies, not as
authoritative docs — for normative material, see `../docs/`.
