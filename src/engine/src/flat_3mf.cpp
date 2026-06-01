// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_3mf.hpp>
#include <cadml/engine/flat_color.hpp>

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace cadml::engine {

namespace {

// ─── Per-mesh validation ──────────────────────────────────────────────
//
// Same shape as flat_stl.cpp::validate_mesh — refuse malformed inputs
// at the API boundary rather than UBing downstream. The flat engine is
// supposed to produce well-formed meshes; this is defense in depth in
// case a hand-crafted FlatEvalResult is passed in.
void validate_mesh(const FlatMesh& mesh, const std::string& part_name) {
    if (mesh.indices.size() % 3 != 0) {
        throw std::runtime_error(
            "write_3mf: part `" + part_name +
            "` has indices.size() (" + std::to_string(mesh.indices.size()) +
            ") not a multiple of 3");
    }
    const std::size_t nverts = mesh.vertices.size();
    for (auto idx : mesh.indices) {
        if (idx >= nverts) {
            throw std::runtime_error(
                "write_3mf: part `" + part_name +
                "`: vertex index " + std::to_string(idx) +
                " out of range (vertices=" + std::to_string(nverts) + ")");
        }
    }
    for (const auto& v : mesh.vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
            throw std::runtime_error(
                "write_3mf: part `" + part_name +
                "` contains a non-finite vertex (NaN/inf)");
        }
    }
}

// ─── Unit-suffix mapping ──────────────────────────────────────────────
//
// 3MF schema accepts a fixed enum: micron, millimeter, centimeter, inch,
// foot, meter (per the core spec §3.3). CADML's frontmatter `units` uses
// the colloquial short form (mm/cm/m/in/ft). Translate, reject anything
// else loudly.
std::string map_units(std::string_view u) {
    if (u.empty() || u == "mm") return "millimeter";
    if (u == "cm")              return "centimeter";
    if (u == "m")               return "meter";
    if (u == "in")              return "inch";
    if (u == "ft")              return "foot";
    throw std::runtime_error(
        "write_3mf: unsupported units `" + std::string(u) +
        "` (expected mm, cm, m, in, or ft)");
}

// ─── XML escaping ─────────────────────────────────────────────────────
//
// Escape an attribute value for XML emission. Same surface as
// serializer.cpp::xml_escape_attr but local to the exporter — the 3MF
// XML is a leaf concern of this file and we don't want a public-API
// dependency on the parser library here.
std::string xml_escape_attr(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out.push_back(ch);
        }
    }
    return out;
}

std::string xml_escape_text(std::string_view value) {
    // Element-text escaping needs <, >, &; quotes are fine in text.
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out.push_back(ch);
        }
    }
    return out;
}

// ─── Number formatting ────────────────────────────────────────────────
//
// 3MF stores numbers as decimal strings. Use %.17g so a double round-
// trips exactly; the slicer parses to float anyway, but determinism of
// the emitted file (for golden-hash tests) depends on the formatter
// being independent of locale and rounding mode. Route through
// cadml::format_double_canonical so the radix character is always `.`
// regardless of the host's LC_NUMERIC.
std::string fmt_double(double v) {
    // Normalise -0 → 0 so two semantically-identical inputs hash the same.
    if (v == 0.0) v = 0.0;
    return ::cadml::format_double_canonical(v, 17);
}

// ─── 3dmodel.model emission ───────────────────────────────────────────
//
// The schema is fixed:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <model unit="millimeter" xml:lang="en-US"
//          xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
//     <metadata name="Title">…</metadata>
//     <resources>
//       <basematerials id="1">
//         <base name="…" displaycolor="#AABBCCFF"/>
//         …
//       </basematerials>
//       <object id="2" type="model" name="…" pid="1" pindex="0">
//         <mesh>
//           <vertices>
//             <vertex x="…" y="…" z="…"/> …
//           </vertices>
//           <triangles>
//             <triangle v1="…" v2="…" v3="…"/> …
//           </triangles>
//         </mesh>
//       </object>
//       …
//     </resources>
//     <build>
//       <item objectid="2"/> …
//     </build>
//   </model>
//
// Object IDs start at 2 because basematerials always takes id="1" when
// we emit one (kept stable whether or not parts have colors — makes the
// reading-it-back code simpler).

// Resolve a CADML color — a CSS name ("red", "blue", …) OR "#RGB" /
// "#RRGGBB" — to the 3MF displaycolor form "#RRGGBBAA". Routes through
// the engine's shared parse_color_rgba so named colors and hex stay in
// lockstep with the rest of the engine. Returns empty for anything it
// can't resolve: signal to the caller to skip the basematerials entry.
std::string color_to_3mf_hex(std::string_view c) {
    const std::uint32_t rgba = parse_color_rgba(c);
    if (rgba == 0) return {};   // empty / invalid / unrecognised name
    // parse_color_rgba's pack() layout is R=bits0-7, G=8-15, B=16-23,
    // A=24-31; a resolved color always has A=0xFF, so 0 unambiguously
    // means "none".
    const unsigned r =  rgba        & 0xFFu;
    const unsigned g = (rgba >> 8)  & 0xFFu;
    const unsigned b = (rgba >> 16) & 0xFFu;
    const unsigned a = (rgba >> 24) & 0xFFu;
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", r, g, b, a);
    return std::string(buf);
}

// Build the entire 3dmodel.model body as a string.
std::string emit_model_xml(const FlatEvalResult& result,
                            const ThreeMfOptions& opts)
{
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<model unit=\"" << map_units(opts.units) << "\""
        << " xml:lang=\"en-US\""
        << " xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";

    if (!opts.title.empty()) {
        xml << "  <metadata name=\"Title\">"
            << xml_escape_text(opts.title) << "</metadata>\n";
    }

    // ── Resources block ───────────────────────────────────────────────
    xml << "  <resources>\n";

    // Collect distinct colors and assign each part a pindex into the
    // single shared basematerials group. Parts without color don't get
    // a pid attribute (slicer falls back to its default material).
    //
    // Dedup is cheap and reduces .3mf size on documents with many parts
    // sharing one color — common in multi-instance assemblies.
    std::vector<std::string> base_colors;       // 3MF "#RRGGBBAA" form
    std::vector<std::string> base_names;        // CADML's authored color
    std::unordered_map<std::string, std::size_t> color_index;
    for (const auto& p : result.parts) {
        if (p.color.empty()) continue;
        const auto hex = color_to_3mf_hex(p.color);
        if (hex.empty()) continue;          // unrecognised — skip
        if (color_index.count(p.color)) continue;
        color_index[p.color] = base_colors.size();
        base_colors.push_back(hex);
        base_names.push_back(p.color);
    }

    const bool has_materials = !base_colors.empty();
    constexpr int kBaseMaterialsId = 1;
    if (has_materials) {
        xml << "    <basematerials id=\"" << kBaseMaterialsId << "\">\n";
        for (std::size_t i = 0; i < base_colors.size(); ++i) {
            xml << "      <base name=\""
                << xml_escape_attr(base_names[i])
                << "\" displaycolor=\""
                << xml_escape_attr(base_colors[i])
                << "\"/>\n";
        }
        xml << "    </basematerials>\n";
    }

    // ── Per-part objects ──────────────────────────────────────────────
    // Object IDs start at 2 so material id 1 is unambiguous even when
    // we don't emit any materials. (3MF schema requires unique resource
    // IDs across the doc; the shift keeps that invariant.)
    constexpr int kObjectIdBase = 2;
    std::vector<int> object_ids;
    object_ids.reserve(result.parts.size());

    for (std::size_t i = 0; i < result.parts.size(); ++i) {
        const auto& part = result.parts[i];
        if (part.mesh.triangle_count() == 0) {
            // Skip empty parts — emitting an object with no triangles
            // is technically legal but most slicers reject it. Matches
            // flat_stl.cpp's "skip what nothing's there" instinct.
            object_ids.push_back(-1);
            continue;
        }

        const int id = static_cast<int>(kObjectIdBase + i);
        object_ids.push_back(id);

        xml << "    <object id=\"" << id << "\" type=\"model\"";
        if (!part.name.empty()) {
            xml << " name=\"" << xml_escape_attr(part.name) << "\"";
        }

        // Material assignment via pid+pindex.
        if (has_materials && !part.color.empty()) {
            auto it = color_index.find(part.color);
            if (it != color_index.end()) {
                xml << " pid=\"" << kBaseMaterialsId
                    << "\" pindex=\"" << it->second << "\"";
            }
        }
        xml << ">\n";

        // ── Mesh ───────────────────────────────────────────────────────
        xml << "      <mesh>\n";
        xml << "        <vertices>\n";
        for (const auto& v : part.mesh.vertices) {
            xml << "          <vertex x=\"" << fmt_double(v.x)
                << "\" y=\"" << fmt_double(v.y)
                << "\" z=\"" << fmt_double(v.z) << "\"/>\n";
        }
        xml << "        </vertices>\n";

        xml << "        <triangles>\n";
        const auto& idx = part.mesh.indices;
        for (std::size_t t = 0; t < idx.size(); t += 3) {
            xml << "          <triangle v1=\"" << idx[t]
                << "\" v2=\"" << idx[t + 1]
                << "\" v3=\"" << idx[t + 2] << "\"/>\n";
        }
        xml << "        </triangles>\n";
        xml << "      </mesh>\n";
        xml << "    </object>\n";
    }
    xml << "  </resources>\n";

    // ── Build items ───────────────────────────────────────────────────
    // One <item> per non-empty object at identity. CADML's flat engine
    // already produces world-space coordinates (assemblies + groups
    // are baked into each part's vertices), so a transform here would
    // double-apply. Identity is the right default; future per-build
    // positioning lives in tooling above this layer.
    xml << "  <build>\n";
    for (int id : object_ids) {
        if (id < 0) continue;
        xml << "    <item objectid=\"" << id << "\"/>\n";
    }
    xml << "  </build>\n";
    xml << "</model>\n";

    return xml.str();
}

// ─── OPC boilerplate parts ────────────────────────────────────────────
//
// Every 3MF package contains these two parts at fixed paths. They're
// constants — values come from the OPC + 3MF specs.

constexpr const char* kContentTypesXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
    "  <Default Extension=\"rels\""
        " ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
    "  <Default Extension=\"model\""
        " ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
    "</Types>\n";

constexpr const char* kRootRelsXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    "  <Relationship Id=\"rel0\""
        " Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\""
        " Target=\"/3D/3dmodel.model\"/>\n"
    "</Relationships>\n";

// ─── ZIP packaging via miniz ──────────────────────────────────────────
//
// miniz's heap-based archive API writes a complete ZIP into a malloc'd
// buffer. We collect it then stream into `out`. The package layout
// (filenames, order) is fixed by the 3MF spec.
//
// Compression: miniz default level is fine. STORE (no compression) is
// allowed by 3MF but consumer slicers handle DEFLATE universally and
// the size win is significant for the model.xml (which is dominated by
// repeated tag characters).
//
// **Determinism scope:** the geometry payload (3D/3dmodel.model) is
// byte-deterministic for byte-identical input — that's the property
// the spec promises and the property the engine and emit_model_xml()
// enforce. The ZIP container around that payload includes per-entry
// modification timestamps that miniz stamps from wall-clock time;
// those vary across runs. That's intentional: an archive format
// recording when its entries were packaged is not a bug to scrub out.
// Tests that need to compare 3MF output for equivalence extract the
// inner XML before hashing — see tests/examples/test_3mf_golden.cpp.
void package_zip(std::ostream& out,
                  std::string_view model_xml)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

    if (!mz_zip_writer_init_heap(&zip, /*size_to_reserve_at_beginning=*/0,
                                  /*initial_allocation_size=*/64 * 1024)) {
        throw std::runtime_error("write_3mf: miniz heap init failed");
    }

    auto add = [&](const char* archive_name,
                    std::string_view content) {
        // miniz stamps the per-entry modification time from wall-clock;
        // we accept that — the determinism contract lives on the
        // payload (3D/3dmodel.model), not the ZIP container around it
        // (see comment block above on the determinism scope).
        if (!mz_zip_writer_add_mem(
                &zip, archive_name,
                content.data(), content.size(),
                MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            throw std::runtime_error(
                "write_3mf: miniz failed adding `" +
                std::string(archive_name) + "` to archive");
        }
    };

    // The 3MF spec doesn't mandate a file order inside the ZIP, but
    // emitting [Content_Types].xml first matches how every other
    // OPC-style format (docx, xlsx, pptx) does it — keeps the package
    // friendly to anyone inspecting it with a generic ZIP reader.
    add("[Content_Types].xml", kContentTypesXml);
    add("_rels/.rels",          kRootRelsXml);
    add("3D/3dmodel.model",     model_xml);

    void*  heap_buf  = nullptr;
    std::size_t heap_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &heap_buf, &heap_size)) {
        mz_zip_writer_end(&zip);
        throw std::runtime_error(
            "write_3mf: miniz failed finalising archive");
    }
    mz_zip_writer_end(&zip);

    if (heap_buf == nullptr || heap_size == 0) {
        if (heap_buf) mz_free(heap_buf);
        throw std::runtime_error(
            "write_3mf: miniz produced an empty archive");
    }

    out.write(static_cast<const char*>(heap_buf),
              static_cast<std::streamsize>(heap_size));
    mz_free(heap_buf);
}

}  // namespace

void write_3mf(const FlatEvalResult& result,
                std::ostream& out,
                const ThreeMfOptions& opts)
{
    // Validate every part up front. Same fail-fast policy as flat_stl.
    for (const auto& p : result.parts) {
        validate_mesh(p.mesh, p.name);
    }

    const std::string model_xml = emit_model_xml(result, opts);
    package_zip(out, model_xml);
}

void write_3mf(const FlatEvalResult& result,
                const std::filesystem::path& path,
                const ThreeMfOptions& opts)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    write_3mf(result, f, opts);
    // Mirror flat_stl's partial-write detection: ofstream silently
    // swallows disk-full / permission errors at write() time; failbit
    // fires only on explicit close.
    f.close();
    if (!f) {
        throw std::runtime_error("write to " + path.string() +
                                  " failed (disk full or permission?)");
    }
}

}  // namespace cadml::engine
