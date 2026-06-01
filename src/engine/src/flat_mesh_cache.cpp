// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "flat_mesh_cache_internal.hpp"

#include <cadml/engine/flat_evaluator.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

namespace cadml::engine {

namespace {

// Canonical key.
//
// Components are length-prefixed so the `|` delimiter is unambiguous
// even if the def name or a param name contains `|`, `=`, or any other
// printable byte. Each field is encoded as `<byte-count>:<bytes>`. The
// numeric value is rendered with `%.17g` so binary-equal doubles map
// to the same byte sequence.
//
// CADML's identifier grammar never produces `|` or `=`, but the cache
// sits below the parser and ought not depend on grammar invariants —
// any future relaxation (qualified Lua names, embedded specials)
// would otherwise alias distinct evaluations onto one mesh.
std::string make_key(const std::string& def_name,
                      const std::map<std::string, double>& sorted_overrides)
{
    auto append_field = [](std::string& out, const std::string& bytes) {
        out += std::to_string(bytes.size());
        out += ':';
        out += bytes;
    };
    std::string out;
    append_field(out, def_name);
    for (const auto& [name, value] : sorted_overrides) {
        out += '|';
        append_field(out, name);
        out += '=';
        // Locale-independent — a de_DE host must hash to the same key
        // as a C-locale host or cache hit/miss counts diverge across
        // builds.
        append_field(out, ::cadml::format_double_canonical(value, 17));
    }
    return out;
}

}  // namespace

struct FlatMeshCache::Impl {
    std::unordered_map<std::string, FlatMesh> entries;
    std::size_t hit_count = 0;
    std::size_t miss_count = 0;
};

FlatMeshCache::FlatMeshCache()  : impl_(std::make_unique<Impl>()) {}
FlatMeshCache::~FlatMeshCache() = default;

void FlatMeshCache::reset() {
    impl_->entries.clear();
    impl_->hit_count = 0;
    impl_->miss_count = 0;
}

std::size_t FlatMeshCache::size()   const { return impl_->entries.size(); }
std::size_t FlatMeshCache::hits()   const { return impl_->hit_count;     }
std::size_t FlatMeshCache::misses() const { return impl_->miss_count;    }

// ─── Internal access (used by eval_instance) ────────────────────────

const FlatMesh* FlatMeshCacheAccess::lookup(
    FlatMeshCache& cache,
    const std::string& def_name,
    const std::map<std::string, double>& overrides)
{
    const auto key = make_key(def_name, overrides);
    auto it = cache.impl_->entries.find(key);
    if (it == cache.impl_->entries.end()) {
        ++cache.impl_->miss_count;
        return nullptr;
    }
    ++cache.impl_->hit_count;
    return &it->second;
}

void FlatMeshCacheAccess::store(
    FlatMeshCache& cache,
    const std::string& def_name,
    const std::map<std::string, double>& overrides,
    FlatMesh mesh)
{
    const auto key = make_key(def_name, overrides);
    cache.impl_->entries.emplace(key, std::move(mesh));
}

}  // namespace cadml::engine
