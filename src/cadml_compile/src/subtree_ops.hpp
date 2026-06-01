// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cadml::compile::detail {

// Recursively collect all node indices in a subtree, in depth-first
// pre-order. Useful for both clone and substitute passes.
std::vector<std::uint32_t> collect_subtree(const Document& doc,
                                            std::uint32_t root_idx);

// Deep-copy `root_idx`'s subtree. New nodes are appended to `doc.nodes`.
// The returned new root has `parent = new_parent_idx` and `next_sibling =
// NO_NODE`. All internal parent/first_child/next_sibling links are
// re-mapped to point within the cloned region.
//
// `iteration` (if not UINT32_MAX) is stamped on the new ROOT node only,
// useful for marking unrolled-loop wrappers.
std::uint32_t clone_subtree(Document& doc, std::uint32_t root_idx,
                             std::uint32_t new_parent_idx,
                             std::uint32_t iteration = UINT32_MAX);

// Replace whole-identifier occurrences of `var` in `s` with `value`.
// Identifier per spec §2.8: [a-z][a-z0-9-]*. The match must be a full
// identifier — `var` does not match inside `var-suffix` or `prefix-var`.
//
// Used for textual substitution of loop variables in attribute values.
std::string substitute_var_in_string(std::string_view s,
                                      std::string_view var,
                                      std::string_view value);

// Walk every string attribute of every node in the subtree rooted at
// `root_idx` and apply substitute_var_in_string. Mutates `doc` in place.
void substitute_var_in_subtree(Document& doc, std::uint32_t root_idx,
                                std::string_view var,
                                std::string_view value);

// Remove `node_idx` from its parent's children list (or from the
// top-level sibling chain if parent == NO_NODE). The node's storage in
// doc.nodes is NOT freed; only the parent → child link is severed.
//
// Returns the previous sibling index (or NO_NODE if the node was first).
// Caller can use this as an anchor for splicing in replacement nodes.
std::uint32_t unlink_from_parent(
    Document& doc,
    std::uint32_t node_idx,
    // Top-level chain — provide the head pointer storage for fixup when
    // unlinking a top-level sibling. Pass nullptr if `node_idx` has a
    // parent; we'll fix the parent's first_child instead.
    std::uint32_t* top_level_head = nullptr);

// Splice a chain of new sibling nodes (linked via next_sibling) into
// the sibling chain. The chain starts at `chain_first` and ends at
// `chain_last` (caller computes both).
//
// `parent_idx` is the parent (NO_NODE for top-level).
// `top_level_head` is the head pointer for top-level chains (only
// consulted when parent_idx == NO_NODE).
// `anchor_idx` is the previous sibling to insert after (NO_NODE means
// insert at the head).
// `following_idx` is what should come after the spliced chain
// (whatever was originally after the anchor — typically the unlinked
// node's next_sibling).
void splice_after(Document& doc,
                   std::uint32_t parent_idx,
                   std::uint32_t* top_level_head,
                   std::uint32_t anchor_idx,
                   std::uint32_t chain_first,
                   std::uint32_t chain_last,
                   std::uint32_t following_idx);

}  // namespace cadml::compile::detail
