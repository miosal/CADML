// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/engine/flat_color.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cadml::engine {

namespace {

constexpr std::uint32_t kGltfFloat       = 5126;
constexpr std::uint32_t kGltfUnsignedInt = 5125;

constexpr std::uint32_t kGltfArrayBuffer        = 34962;
constexpr std::uint32_t kGltfElementArrayBuffer = 34963;

// True iff any normal in `m` is non-zero. The primitives currently
// emit zero normals (filled by a future pass), so most parts skip
// the normal accessor entirely.
bool has_real_normals(const FlatMesh& m) {
    for (const auto& n : m.normals) {
        if (n.x != 0 || n.y != 0 || n.z != 0) return true;
    }
    return false;
}

// sRGB component (0..1) → linear (0..1). glTF's
// pbrMetallicRoughness.baseColorFactor is defined in LINEAR space, but
// CADML colors (CSS names / #hex, via parse_color_rgba) are sRGB — so
// convert rather than dump sRGB values into a linear field. Standard IEC
// 61966-2-1 transfer. Alpha is linear by definition and not converted.
float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Append `n` raw bytes from `src` to `out`. Used to build the binary
// buffer in glTF's host-endian little-endian convention. (Windows /
// Linux on x86_64 / arm64 are all little-endian, matching glTF.)
void append_bytes(std::vector<std::uint8_t>& out,
                   const void* src, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(src);
    out.insert(out.end(), p, p + n);
}

void append_float(std::vector<std::uint8_t>& out, double d) {
    const float f = static_cast<float>(d);
    append_bytes(out, &f, sizeof(f));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t u) {
    append_bytes(out, &u, sizeof(u));
}

// Pad `out` so its length is a multiple of `alignment`. glTF buffer
// views have alignment requirements (4 bytes for indices is common).
void pad_to(std::vector<std::uint8_t>& out, std::size_t alignment) {
    while (out.size() % alignment) out.push_back(0);
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                  (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                  static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >>  6) & 0x3F]);
        out.push_back(alphabet[ v        & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        const auto rem = data.size() - i;
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (rem == 2) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(rem == 2 ? alphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// Find the originating <part> Node for `part` (by name). Returns
// nullptr if no top-level Part with that name exists.
//
// O(n_nodes) per call: a glTF export with N parts therefore costs
// O(N × n_nodes). In practice N is usually 1 per .fcadml so this is
// fine; many-part exports can build a `name → Node*` index once at
// the top of `export_gltf`.
const Node* find_part_node(const Document& doc, const std::string& name) {
    for (const auto& n : doc.nodes) {
        if (n.dead) continue;
        if (n.type != NodeType::Part) continue;
        if (n.parent != NO_NODE) continue;
        if (std::get<PartAttrs>(n.attrs).name == name) return &n;
    }
    return nullptr;
}

// SourceFile id → filename via doc.source_files.
std::string source_filename(const Document& doc, SourceFileId id) {
    for (const auto& sf : doc.source_files) {
        if (sf.id == id) return sf.path;
    }
    return {};
}

}  // namespace

std::string export_gltf(const FlatEvalResult& result,
                         const Document& doc,
                         const GltfExportOptions& opts)
{
    // Don't `using namespace rapidjson;` — rapidjson::Document collides
    // with the cadml::Document parameter type. Qualify explicitly.
    using rj = rapidjson::Document;
    using Value = rapidjson::Value;
    constexpr auto kArrayType  = rapidjson::kArrayType;
    constexpr auto kObjectType = rapidjson::kObjectType;

    rj json;
    json.SetObject();
    auto& alloc = json.GetAllocator();

    // asset
    {
        Value asset(kObjectType);
        asset.AddMember("version", "2.0", alloc);
        asset.AddMember("generator", "cadml-flat-engine", alloc);
        json.AddMember("asset", asset, alloc);
    }

    // Build a single buffer concatenating every part's positions
    // (+ optional normals) + indices, with bufferView/accessor entries
    // tracking offsets.
    std::vector<std::uint8_t> buffer;
    Value buffer_views(kArrayType);
    Value accessors(kArrayType);
    Value meshes(kArrayType);
    Value nodes(kArrayType);
    Value scene_nodes(kArrayType);

    auto add_buffer_view = [&](std::size_t offset, std::size_t length,
                                std::uint32_t target) -> std::uint32_t {
        Value bv(kObjectType);
        bv.AddMember("buffer", 0u, alloc);
        bv.AddMember("byteOffset", static_cast<std::uint64_t>(offset), alloc);
        bv.AddMember("byteLength", static_cast<std::uint64_t>(length), alloc);
        bv.AddMember("target", target, alloc);
        const auto idx = static_cast<std::uint32_t>(buffer_views.Size());
        buffer_views.PushBack(bv, alloc);
        return idx;
    };
    auto add_accessor = [&](std::uint32_t buffer_view,
                              std::uint32_t component_type,
                              std::size_t count,
                              const char* type) -> std::uint32_t {
        Value a(kObjectType);
        a.AddMember("bufferView", buffer_view, alloc);
        a.AddMember("componentType", component_type, alloc);
        a.AddMember("count", static_cast<std::uint64_t>(count), alloc);
        Value tval(type, alloc);
        a.AddMember("type", tval, alloc);
        const auto idx = static_cast<std::uint32_t>(accessors.Size());
        accessors.PushBack(a, alloc);
        return idx;
    };

    // Materials: one per distinct part color, created lazily. Colors are
    // resolved through the engine's shared parse_color_rgba (CSS names +
    // #hex — the same resolver the STL/3MF paths use) and emitted as a
    // glTF pbrMetallicRoughness.baseColorFactor (linear RGBA, 0..1). A
    // part with no color or an unresolved color gets no material (the
    // viewer falls back to its default).
    Value materials(kArrayType);
    std::unordered_map<std::string, std::uint32_t> color_to_material;
    auto material_for =
        [&](const std::string& color) -> std::optional<std::uint32_t> {
        if (color.empty()) return std::nullopt;
        if (auto it = color_to_material.find(color);
            it != color_to_material.end()) {
            return it->second;
        }
        const std::uint32_t rgba = parse_color_rgba(color);
        if (rgba == 0) return std::nullopt;   // unresolved name / invalid
        const float r = srgb_to_linear((rgba         & 0xFFu) / 255.0f);
        const float g = srgb_to_linear(((rgba >> 8)   & 0xFFu) / 255.0f);
        const float b = srgb_to_linear(((rgba >> 16)  & 0xFFu) / 255.0f);
        const float a =                ((rgba >> 24)  & 0xFFu) / 255.0f;
        Value bcf(kArrayType);
        bcf.PushBack(r, alloc); bcf.PushBack(g, alloc);
        bcf.PushBack(b, alloc); bcf.PushBack(a, alloc);
        Value pbr(kObjectType);
        pbr.AddMember("baseColorFactor", bcf, alloc);
        Value mat(kObjectType);
        mat.AddMember("pbrMetallicRoughness", pbr, alloc);
        Value mname(color.c_str(), alloc);
        mat.AddMember("name", mname, alloc);
        const auto mi = static_cast<std::uint32_t>(materials.Size());
        materials.PushBack(mat, alloc);
        color_to_material.emplace(color, mi);
        return mi;
    };

    for (const auto& part : result.parts) {
        const auto& m = part.mesh;
        const std::size_t vert_count = m.vertices.size();
        const std::size_t index_count = m.indices.size();
        if (vert_count == 0 || index_count == 0) continue;

        // ── positions ──────────────────────────────────────────────
        const auto pos_offset = buffer.size();
        Vec3 vmin{ std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max() };
        Vec3 vmax{ std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest() };
        for (const auto& v : m.vertices) {
            append_float(buffer, v.x);
            append_float(buffer, v.y);
            append_float(buffer, v.z);
            vmin.x = std::min(vmin.x, v.x); vmax.x = std::max(vmax.x, v.x);
            vmin.y = std::min(vmin.y, v.y); vmax.y = std::max(vmax.y, v.y);
            vmin.z = std::min(vmin.z, v.z); vmax.z = std::max(vmax.z, v.z);
        }
        const auto pos_length = buffer.size() - pos_offset;
        const auto pos_bv = add_buffer_view(pos_offset, pos_length,
                                              kGltfArrayBuffer);
        // POSITION accessor — glTF requires min/max for the bounding box.
        Value pos_accessor(kObjectType);
        pos_accessor.AddMember("bufferView", pos_bv, alloc);
        pos_accessor.AddMember("componentType", kGltfFloat, alloc);
        pos_accessor.AddMember("count",
            static_cast<std::uint64_t>(vert_count), alloc);
        pos_accessor.AddMember("type", "VEC3", alloc);
        Value vmin_arr(kArrayType);
        vmin_arr.PushBack(static_cast<float>(vmin.x), alloc);
        vmin_arr.PushBack(static_cast<float>(vmin.y), alloc);
        vmin_arr.PushBack(static_cast<float>(vmin.z), alloc);
        Value vmax_arr(kArrayType);
        vmax_arr.PushBack(static_cast<float>(vmax.x), alloc);
        vmax_arr.PushBack(static_cast<float>(vmax.y), alloc);
        vmax_arr.PushBack(static_cast<float>(vmax.z), alloc);
        pos_accessor.AddMember("min", vmin_arr, alloc);
        pos_accessor.AddMember("max", vmax_arr, alloc);
        const auto pos_acc_idx = static_cast<std::uint32_t>(accessors.Size());
        accessors.PushBack(pos_accessor, alloc);

        // ── optional normals ───────────────────────────────────────
        std::optional<std::uint32_t> normal_acc_idx;
        if (has_real_normals(m)) {
            const auto n_offset = buffer.size();
            for (const auto& n : m.normals) {
                append_float(buffer, n.x);
                append_float(buffer, n.y);
                append_float(buffer, n.z);
            }
            const auto n_length = buffer.size() - n_offset;
            const auto n_bv = add_buffer_view(n_offset, n_length,
                                                 kGltfArrayBuffer);
            normal_acc_idx = add_accessor(n_bv, kGltfFloat, vert_count, "VEC3");
        }

        // ── indices (4-byte aligned) ───────────────────────────────
        pad_to(buffer, 4);
        const auto idx_offset = buffer.size();
        for (auto i : m.indices) append_u32(buffer, i);
        const auto idx_length = buffer.size() - idx_offset;
        const auto idx_bv = add_buffer_view(idx_offset, idx_length,
                                              kGltfElementArrayBuffer);
        const auto idx_acc_idx = add_accessor(idx_bv, kGltfUnsignedInt,
                                                index_count, "SCALAR");

        // ── primitive ──────────────────────────────────────────────
        Value primitive(kObjectType);
        Value attrs(kObjectType);
        attrs.AddMember("POSITION", pos_acc_idx, alloc);
        if (normal_acc_idx) attrs.AddMember("NORMAL", *normal_acc_idx, alloc);
        primitive.AddMember("attributes", attrs, alloc);
        primitive.AddMember("indices", idx_acc_idx, alloc);
        if (auto mat = material_for(part.color)) {
            primitive.AddMember("material", *mat, alloc);
        }

        Value primitives(kArrayType);
        primitives.PushBack(primitive, alloc);

        Value mesh_obj(kObjectType);
        mesh_obj.AddMember("primitives", primitives, alloc);
        Value name_val(part.name.c_str(), alloc);
        mesh_obj.AddMember("name", name_val, alloc);
        const auto mesh_idx = static_cast<std::uint32_t>(meshes.Size());
        meshes.PushBack(mesh_obj, alloc);

        // ── node ───────────────────────────────────────────────────
        Value node(kObjectType);
        node.AddMember("mesh", mesh_idx, alloc);
        Value node_name(part.name.c_str(), alloc);
        node.AddMember("name", node_name, alloc);

        if (opts.include_source_extras) {
            // Trace the part name back to the originating <part> node
            // and pull its SourceRange. file_id → filename via
            // doc.source_files; line is the source range's first line.
            //
            // Only emit `extras` when the source is genuinely valid
            // (file != NO_FILE && line > 0) — otherwise we'd produce
            // `extras: { source: "", line: 0 }` which the viewer can't
            // navigate to and which adds noise to the JSON.
            const Node* part_node = find_part_node(doc, part.name);
            if (part_node && part_node->source.valid()) {
                const auto src_file = source_filename(doc, part_node->source.file);
                if (!src_file.empty()) {
                    Value extras(kObjectType);
                    Value src_path(src_file.c_str(), alloc);
                    extras.AddMember("source", src_path, alloc);
                    extras.AddMember("line",
                        static_cast<std::uint32_t>(part_node->source.line),
                        alloc);
                    node.AddMember("extras", extras, alloc);
                }
            }
        }

        const auto node_idx = static_cast<std::uint32_t>(nodes.Size());
        nodes.PushBack(node, alloc);
        scene_nodes.PushBack(node_idx, alloc);
    }

    Value scene(kObjectType);
    scene.AddMember("nodes", scene_nodes, alloc);
    Value scenes(kArrayType);
    scenes.PushBack(scene, alloc);

    json.AddMember("scene", 0u, alloc);
    json.AddMember("scenes", scenes, alloc);
    json.AddMember("nodes", nodes, alloc);
    json.AddMember("meshes", meshes, alloc);
    json.AddMember("accessors", accessors, alloc);
    json.AddMember("bufferViews", buffer_views, alloc);
    // Only emit `materials` when at least one part resolved a color —
    // an empty array is legal but noise.
    if (!materials.Empty()) {
        json.AddMember("materials", materials, alloc);
    }

    // Buffers: omit entirely when there's nothing to emit. Embedding
    // a zero-byte data URI (`data:application/octet-stream;base64,`)
    // is technically valid per RFC 2397 but some glTF validators
    // reject it; producing no buffers at all sidesteps the question.
    if (!buffer.empty()) {
        Value buffer_obj(kObjectType);
        buffer_obj.AddMember("byteLength",
            static_cast<std::uint64_t>(buffer.size()), alloc);
        const std::string uri =
            "data:application/octet-stream;base64," + base64_encode(buffer);
        Value uri_val(uri.c_str(), alloc);
        buffer_obj.AddMember("uri", uri_val, alloc);
        Value buffers(kArrayType);
        buffers.PushBack(buffer_obj, alloc);
        json.AddMember("buffers", buffers, alloc);
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    json.Accept(writer);
    return sb.GetString();
}

}  // namespace cadml::engine
