// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cstdint>

namespace cadml {

// ── Mathematical constants ────────────────────────────────────────────

inline constexpr double kPi  = 3.14159265358979323846;
inline constexpr double kTau = 6.28318530717958647693;     // 2π

// ── Tessellation defaults ─────────────────────────────────────────────
//
// Per spec/evaluator.md §3.2. These are the policy values consumed by
// the flat engine's primitive tessellators. Explicit `segments="N"`
// on a primitive bypasses the [min, max] clamp.
//
// **Unit caveat (0.1):** the sagitta / chord-error thresholds below
// are interpreted in document units, not millimetres. A file declared
// with `units in` therefore samples to 0.005 inches sagitta; one
// declared with `units m` samples to 0.005 metres. Authors mixing
// units across files in the same project see proportionally different
// mesh quality. Unifying these to a unit-aware tolerance is on the
// 0.2 roadmap.

inline constexpr double      kCircleSagitta     = 0.005;   // doc units
inline constexpr int         kCircleSegmentsMin = 8;
inline constexpr int         kCircleSegmentsMax = 256;

// kPathTolerance is the canonical sagitta target for path/bezier
// flattening; it MUST equal kCircleSagitta so authors get the same
// facet density on `<circle>` and on `<path>` curves of the same
// scale. flat_geometry.cpp shadowed this with 0.005 (correct) while
// the header advertised 0.05 (10× coarser); aligning the two
// removes the drift hazard.
inline constexpr double      kPathTolerance     = kCircleSagitta;
inline constexpr int         kBezierMaxSubdivDepth = 18;

inline constexpr int         kRevolveSegmentsDefault = 32;
inline constexpr int         kRevolveSegmentsMin     = 3;
inline constexpr int         kRevolveSegmentsMax     = 4096;

inline constexpr int         kSweepSegmentsPerTurn  = 32;

// ── Safety / DoS budgets ──────────────────────────────────────────────
//
// Enforced upstream of the engine to refuse pathological inputs from
// untrusted CADML sources.

inline constexpr std::size_t kMaxSourceBytes      = 64ull * 1024 * 1024;
inline constexpr int         kMaxXmlDepth         = 256;
inline constexpr int         kMaxExpressionDepth  = 256;
inline constexpr int         kMaxForSteps         = 100000;
inline constexpr int         kMaxPatternCount     = 10000;
inline constexpr int         kMaxInstanceDepth    = 64;

// ── Modifier tolerances ───────────────────────────────────────────────

// Threshold (radians) at which a dihedral angle is considered "sharp"
// for Tier-1 modifier acceptance (chamfer/fillet predicate). Wider
// than ±5° to allow 12-segment cylinders (30°/segment) through.
inline constexpr double      kSharpDihedralRad   = 2.6179938779914944;  // π − π/6

}  // namespace cadml
