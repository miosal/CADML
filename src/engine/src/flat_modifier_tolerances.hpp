// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/constants.hpp>

#include <cmath>

namespace cadml::engine::detail {

// ─── Edge classification (flat_edge_topology.cpp) ─────────────────────

// Cross-product magnitude (squared) below which two adjacent triangles
// are treated as coplanar (Flat). 1e-12 chosen because:
//   - Unit-normalized normals: |n1|=|n2|=1, so |cross|² = sin²θ.
//   - sin²(1e-6 rad) ≈ 1e-12, so this catches sub-microradian noise.
//   - Larger values (e.g. 1e-9) would treat 0.03° bends as flat,
//     which would fail the 12-segment-cylinder rejection at the
//     boundary of the predicate.
inline constexpr double kFlatCrossSqEps = 1e-12;

// ─── Tier 1 predicate (flat_edge_topology.cpp) ────────────────────────

// "Sharp dihedral" threshold: convex edges with dihedral ≥ this value
// are treated as curve-approximation segments and rejected. π − π/6
// = 150°, equivalently bend ≤ 30°.
//
// Rationale: a 12-segment cylinder has 30° bends and is the coarsest
// "obviously curved" geometry we want to reject; a 32-segment cylinder
// has ~11° bends → comfortably rejected; a cube has 90° bends →
// comfortably accepted.
//
// The value is shared with `cadml::kSharpDihedralRad` in the public
// constants header so callers outside the engine can reference the
// same threshold; the two MUST stay equal.
inline constexpr double kSharpDihedral = ::cadml::kSharpDihedralRad;

// ─── Chamfer (flat_geometry.cpp) ──────────────────────────────────────

// (No knobs — chamfer prism geometry is parameter-free given width.)

// ─── Fillet (flat_geometry.cpp) ───────────────────────────────────────

// Number of segments per fillet's quarter-cylinder. 16 for a full
// circumference → 4 visible segments per edge (a quarter-arc). Higher
// values smooth the visible curve at cost of triangle count;
// triangle count scales linearly.
//
// `tessellate_circle` (used by `<circle>` profile evaluation) defaults
// to 32 segments while fillet cylinders default to 16. The numbers
// are tuned for different roles — profile circles need to read as
// smooth on their own, fillet cylinders only show a quarter-arc per
// edge. A future user-controllable detail level could unify them.
inline constexpr int kFilletCylinderSegments = 16;

// Padding applied to fillet prism corners (in the −t1, −t2 directions)
// so the original mesh corner is STRICTLY inside the cutter union.
// Without this, Manifold leaves stray vertices at the original
// cube-corner positions because the prism apex coincides with the
// corner. 1e-4 is small enough not to affect visible geometry at
// typical CAD scales (mm to m) but large enough to escape Manifold's
// inside/boundary fuzz.
inline constexpr double kFilletPrismPad = 1e-4;

// ─── Shell (flat_geometry.cpp) ────────────────────────────────────────

// Z-overshoot applied past an open cap when extruding the inner
// cavity, so Manifold's subtract removes the original cap fully
// rather than leaving a paper-thin lid. 1e-3 is conservative for
// CAD scales.
inline constexpr double kShellCapPad = 1e-3;

}  // namespace cadml::engine::detail
