// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <string>

namespace cadml {

struct SerializeOptions {
    // Indent string for nested elements (default: two spaces).
    std::string indent = "  ";
    // If true, emit the <sources> table and src= attributes (.fcadml form).
    // If false, emit authoring form (frontmatter only, no sources block).
    bool include_source_map = false;
};

// Serialize a Document to a CADML source string.
//
// For authoring-form output (`include_source_map = false`):
//   * Frontmatter declarations come first (settings, imports, params),
//     in the spec-required order.
//   * Body XML is emitted with synthetic-root unwrapping.
//   * `src` attributes and `<sources>` are omitted.
//
// For flat-form output (`include_source_map = true`):
//   * Frontmatter contains only settings (no imports, no params — those
//     became XML in the body).
//   * `<sources>` block emitted at top of body.
//   * `src` attribute on every node.
//   * `<param>` elements emitted inside their host `<def>`/`<part>`.
std::string serialize(const Document& doc, const SerializeOptions& opts = {});

}  // namespace cadml
