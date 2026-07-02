// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/types.hpp>

#include <array>
#include <string_view>

namespace cadml {

namespace {

// One row of the built-in vocabulary table: {tag-name, NodeType, the
// spec version that introduced the name}. Ordered alphabetically within
// each group for readability — lookup is linear over a few dozen names,
// fast enough not to need a hash map. `since` implements the §15.2
// pinning rule: a document only "sees" the names its declared spec
// version reserves, so adding a built-in here can never break the
// namespace of files written against an older version.
struct BuiltinEntry {
    std::string_view name;
    NodeType         type;
    SpecVersion      since = kSpecV01;
};

constexpr std::array<BuiltinEntry, 31> kBuiltins = {{
    // Structural (9)
    { "assembly",   NodeType::Assembly },
    { "connect",    NodeType::Connect  },
    { "def",        NodeType::Def      },
    { "for",        NodeType::For      },
    { "group",      NodeType::Group    },
    { "part",       NodeType::Part     },
    { "port",       NodeType::Port     },
    { "script",     NodeType::Script   },
    { "svg",        NodeType::Svg      },
    // 2D primitives (4)
    { "circle",     NodeType::Circle   },
    { "path",       NodeType::Path     },
    { "rect",       NodeType::Rect     },
    { "sketch",     NodeType::Sketch   },
    // 2D-to-3D (5)
    { "extrude",    NodeType::Extrude  },
    { "helix",      NodeType::Helix    },
    { "loft",       NodeType::Loft     },
    { "revolve",    NodeType::Revolve  },
    { "sweep",      NodeType::Sweep    },
    // Mesh import (1)
    { "stl",        NodeType::Stl,       kSpecV02 },
    // Booleans (3) + convex hull
    { "difference", NodeType::Difference },
    { "hull",       NodeType::Hull       },
    { "intersect",  NodeType::Intersect  },
    { "union",      NodeType::Union      },
    // Modifiers (5)
    { "chamfer",    NodeType::Chamfer  },
    { "cut",        NodeType::Cut      },
    { "fillet",     NodeType::Fillet   },
    { "pattern",    NodeType::Pattern  },
    { "shell",      NodeType::Shell    },
    // Flat-output (3)
    { "param",      NodeType::Param    },
    { "source",     NodeType::Source   },
    { "sources",    NodeType::Sources  },
}};

}  // namespace

namespace {

// Ceiling for a parsed version component: far above any real spec
// version, low enough that the accumulate step cannot overflow int. A
// hostile `version 99999999999999999999` must yield a (rejectable)
// saturated value, not signed-overflow UB.
constexpr int kSpecComponentCap = 1'000'000;

// Read a saturating decimal run at `i`. Returns whether any digit was
// consumed; `i` always advances past the whole run.
bool read_spec_component(std::string_view s, std::size_t& i, int& dst) {
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (dst < kSpecComponentCap) dst = dst * 10 + (s[i] - '0');
        if (dst > kSpecComponentCap) dst = kSpecComponentCap;
        ++i;
        any = true;
    }
    return any;
}

}  // namespace

std::string to_string(SpecVersion v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor);
}

SpecVersion spec_version_from_string(std::string_view version) {
    SpecVersion out = kSpecV01;
    int major = 0, minor = 0;
    std::size_t i = 0;
    if (!read_spec_component(version, i, major)) return out;
    if (i >= version.size() || version[i] != '.') return out;
    ++i;
    if (!read_spec_component(version, i, minor)) return out;
    return SpecVersion{major, minor};
}

std::optional<SpecVersion> spec_version_parse_strict(std::string_view version) {
    int major = 0, minor = 0, patch = 0;
    std::size_t i = 0;
    if (!read_spec_component(version, i, major)) return std::nullopt;
    if (i >= version.size() || version[i] != '.') return std::nullopt;
    ++i;
    if (!read_spec_component(version, i, minor)) return std::nullopt;
    if (i < version.size()) {  // optional ".patch", digits only
        if (version[i] != '.') return std::nullopt;
        ++i;
        if (!read_spec_component(version, i, patch)) return std::nullopt;
    }
    if (i != version.size()) return std::nullopt;
    (void)patch;  // validated (digits only) but never affects vocabulary
    return SpecVersion{major, minor};
}

NodeType node_type_from_builtin_name(std::string_view name, SpecVersion spec) {
    for (const auto& entry : kBuiltins) {
        if (entry.name == name) {
            return entry.since <= spec ? entry.type : NodeType::Unknown;
        }
    }
    return NodeType::Unknown;
}

NodeType node_type_from_builtin_name(std::string_view name) {
    return node_type_from_builtin_name(name, kSpecLatest);
}

std::optional<SpecVersion> builtin_since(std::string_view name) {
    for (const auto& entry : kBuiltins) {
        if (entry.name == name) return entry.since;
    }
    return std::nullopt;
}

std::string_view builtin_name_from_node_type(NodeType type) {
    for (const auto& entry : kBuiltins) {
        if (entry.type == type) return entry.name;
    }
    return {};
}

bool is_builtin(NodeType type) {
    switch (type) {
        case NodeType::Instance:
        case NodeType::Unknown:
            return false;
        default:
            return true;
    }
}

}  // namespace cadml
