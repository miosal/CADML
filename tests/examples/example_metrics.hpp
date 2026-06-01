// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace cadml::examples {

struct ExampleMetrics {
    std::size_t parts = 0;
    double      volume_mm3 = 0;   // summed over all parts (signed)
    double      dx = 0, dy = 0, dz = 0;  // union AABB extents
};

// Aggregate the per-part meshes of an evaluated document into one set of
// whole-document invariants. `unit_to_mm` rescales raw doc-unit volume
// into mm³ (pass the result of `cadml::units_to_mm_scale(doc.meta.units)`)
// so the `volume_mm3` field actually contains millimetres-cubed regardless
// of whether the source `.cadml` declared `units mm`, `in`, `m`, etc.
inline ExampleMetrics compute_metrics(const cadml::engine::FlatEvalResult& er,
                                      double unit_to_mm = 1.0) {
    ExampleMetrics m;
    m.parts = er.parts.size();
    double lo[3] = {  1e30,  1e30,  1e30 };
    double hi[3] = { -1e30, -1e30, -1e30 };
    bool any = false;
    for (const auto& part : er.parts) {
        const auto mp = cadml::engine::flat_mass_properties(part.mesh, /*density=*/0.0, unit_to_mm);
        const auto bb = cadml::engine::flat_bounds(part.mesh);
        m.volume_mm3 += mp.volume_mm3;
        if (part.mesh.triangle_count() > 0) {
            any = true;
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], bb.aabb_min[i]);
                hi[i] = std::max(hi[i], bb.aabb_max[i]);
            }
        }
    }
    if (any) { m.dx = hi[0] - lo[0]; m.dy = hi[1] - lo[1]; m.dz = hi[2] - lo[2]; }
    return m;
}

// Relative tolerance for volume + bbox extents. 0.2 % is comfortably
// wider than the cross-platform / cross-config drift observed for
// Manifold boolean output, yet tight enough to catch a real geometry
// regression (which moves these by whole percent or more).
inline constexpr double kRelTol = 2e-3;
// Absolute floor so a near-zero extent (a flat part) doesn't make the
// relative check degenerate.
inline constexpr double kAbsFloor = 1e-3;

inline bool within_tol(double got, double expected) {
    const double allow = std::max(kAbsFloor, std::abs(expected) * kRelTol);
    return std::abs(got - expected) <= allow;
}

}  // namespace cadml::examples
