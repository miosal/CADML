// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_check.hpp>

#include "flat_geometry.hpp"

namespace cadml::engine {

namespace {

// True if `(name_a, name_b)` (in either order) appears in `pairs`.
// Linear scan — `pairs` is expected to be small (one entry per
// allow-interference connect in the document).
bool pair_is_allowed(const std::vector<std::pair<std::string, std::string>>& pairs,
                      const std::string& name_a,
                      const std::string& name_b) {
    for (const auto& p : pairs) {
        if ((p.first == name_a && p.second == name_b) ||
            (p.first == name_b && p.second == name_a)) {
            return true;
        }
    }
    return false;
}

}  // namespace

InterferenceResult check_interference(const FlatEvalResult& result,
                                        const InterferenceOptions& opts)
{
    InterferenceResult out;
    const auto& parts = result.parts;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
            if (pair_is_allowed(opts.allow_pairs,
                                  parts[i].name, parts[j].name)) {
                continue;
            }
            auto vr = detail::intersect_volume(parts[i].mesh, parts[j].mesh);
            if (!vr.ok) {
                out.errors.push_back({
                    "intersect_volume(" + parts[i].name + ", " +
                    parts[j].name + "): " + vr.error,
                    SourceRange{} });
                continue;
            }
            if (vr.volume > opts.tolerance) {
                out.reports.push_back({ parts[i].name, parts[j].name,
                                          vr.volume });
            }
        }
    }
    return out;
}

}  // namespace cadml::engine
