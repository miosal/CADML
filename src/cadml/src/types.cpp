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

SpecVersion spec_version_from_string(std::string_view version) {
    SpecVersion out = kSpecV01;
    int major = 0, minor = 0;
    std::size_t i = 0;
    auto read_int = [&](int& dst) {
        bool any = false;
        while (i < version.size() && version[i] >= '0' && version[i] <= '9') {
            dst = dst * 10 + (version[i] - '0');
            ++i;
            any = true;
        }
        return any;
    };
    if (!read_int(major)) return out;
    if (i >= version.size() || version[i] != '.') return out;
    ++i;
    if (!read_int(minor)) return out;
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
