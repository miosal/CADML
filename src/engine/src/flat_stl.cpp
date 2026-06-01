// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_stl.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

// Binary STL is a little-endian format. The writer below memcpy's raw
// IEEE-754 float bytes; that only matches the spec on a little-endian
// host. Catch the rare big-endian target at build time so a silent
// mis-export can't ship — the workaround (manual byte-swap on emit)
// is a small follow-up if a real consumer ever asks.
static_assert(std::endian::native == std::endian::little,
              "flat_stl writer assumes little-endian host; big-endian "
              "target needs a byte-swap on every f32/u32 emit.");

namespace cadml::engine {

namespace {

// Validate index range + multiple-of-3 invariant for a single part.
// Throws on any breach so the writer doesn't UB downstream.
void validate_mesh(const FlatMesh& mesh, const std::string& part_name) {
    if (mesh.indices.size() % 3 != 0) {
        throw std::runtime_error(
            "write_stl_binary: part `" + part_name +
            "` has indices.size() (" + std::to_string(mesh.indices.size()) +
            ") not a multiple of 3");
    }
    const std::size_t nverts = mesh.vertices.size();
    for (auto idx : mesh.indices) {
        if (idx >= nverts) {
            throw std::runtime_error(
                "write_stl_binary: part `" + part_name +
                "`: vertex index " + std::to_string(idx) +
                " out of range (vertices=" + std::to_string(nverts) + ")");
        }
    }
}

}  // namespace

void write_stl_binary(const FlatEvalResult& result,
                       std::ostream& out,
                       std::string_view header)
{
    // 80-byte header (binary STL spec). Truncate or zero-pad as needed.
    char hdr[80] = {};
    const std::size_t len = std::min(header.size(), std::size_t{ 79 });
    std::memcpy(hdr, header.data(), len);
    out.write(hdr, 80);

    // Validate every part up front and tally the total triangle count.
    std::uint64_t total_tris = 0;
    for (const auto& p : result.parts) {
        validate_mesh(p.mesh, p.name);
        total_tris += p.mesh.triangle_count();
    }
    if (total_tris > 0xFFFFFFFFull) {
        // Binary STL stores triangle count as uint32 — anything past
        // 4 billion overflows the format spec.
        throw std::runtime_error(
            "write_stl_binary: total triangles (" +
            std::to_string(total_tris) +
            ") exceeds binary STL's uint32 limit");
    }
    const std::uint32_t ntri = static_cast<std::uint32_t>(total_tris);
    out.write(reinterpret_cast<const char*>(&ntri), 4);

    // Per-triangle record: 3 normal floats + 9 vertex floats + 2-byte
    // attribute. Compute the face normal from geometry — STL is
    // per-face by spec, and per-vertex normals (when present) are
    // smoothed and don't represent the geometric face.
    auto write_float = [&out](double d) {
        const float f = static_cast<float>(d);
        out.write(reinterpret_cast<const char*>(&f), 4);
    };

    for (const auto& p : result.parts) {
        const auto& mesh = p.mesh;
        const auto tris = mesh.triangle_count();
        for (std::size_t i = 0; i < tris; ++i) {
            const auto i0 = mesh.indices[i * 3 + 0];
            const auto i1 = mesh.indices[i * 3 + 1];
            const auto i2 = mesh.indices[i * 3 + 2];

            const Vec3 v0 = mesh.vertices[i0];
            const Vec3 v1 = mesh.vertices[i1];
            const Vec3 v2 = mesh.vertices[i2];

            // Degenerate triangles (collinear vertices) yield a zero
            // cross product. `normalized()` returns zero in that case
            // — the STL still records (0,0,0) as the face normal,
            // which most readers tolerate. We don't filter out
            // degenerate triangles because they're geometrically
            // valid (just unhelpful for shading) and removing them
            // would change the mesh boundary.
            const Vec3 n = (v1 - v0).cross(v2 - v0).normalized();

            write_float(n.x);  write_float(n.y);  write_float(n.z);
            write_float(v0.x); write_float(v0.y); write_float(v0.z);
            write_float(v1.x); write_float(v1.y); write_float(v1.z);
            write_float(v2.x); write_float(v2.y); write_float(v2.z);

            const std::uint16_t attr = 0;
            out.write(reinterpret_cast<const char*>(&attr), 2);
        }
    }
}

void write_stl_binary(const FlatEvalResult& result,
                       const std::filesystem::path& path,
                       std::string_view header)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    write_stl_binary(result, f, header);
    // Detect partial writes: ofstream silently swallows disk-full /
    // permission-revoked errors at write() time; the failbit fires
    // when we explicitly close. Without this the user sees exit 0
    // and a truncated STL file.
    f.close();
    if (!f) {
        throw std::runtime_error("write to " + path.string() +
                                  " failed (disk full or permission?)");
    }
}

}  // namespace cadml::engine
