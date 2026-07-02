// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/types.hpp>

#include <array>
#include <string_view>

namespace cadml {

namespace {

// One row of the built-in vocabulary table: {tag-name, NodeType}.
// Ordered alphabetically for readability — lookup is linear (28 names),
// fast enough not to need a hash map.
struct BuiltinEntry {
    std::string_view name;
    NodeType         type;
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
    { "stl",        NodeType::Stl      },
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

NodeType node_type_from_builtin_name(std::string_view name) {
    for (const auto& entry : kBuiltins) {
        if (entry.name == name) return entry.type;
    }
    return NodeType::Unknown;
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
