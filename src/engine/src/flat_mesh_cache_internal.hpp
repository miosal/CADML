// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>

#include <map>
#include <string>

namespace cadml::engine {

class FlatMeshCacheAccess {
public:
    // Returns a pointer to the cached mesh, or nullptr on miss. Each
    // call updates the hit/miss counters.
    static const FlatMesh* lookup(
        FlatMeshCache& cache,
        const std::string& def_name,
        const std::map<std::string, double>& overrides);

    static void store(
        FlatMeshCache& cache,
        const std::string& def_name,
        const std::map<std::string, double>& overrides,
        FlatMesh mesh);
};

}  // namespace cadml::engine
