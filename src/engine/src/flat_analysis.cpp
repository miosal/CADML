// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_analysis.hpp>

#include <cadml/engine/flat_color.hpp>
#include <cadml/expression.hpp>

#include <manifold/manifold.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cadml::engine {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// ── Local FlatMesh → manifold::MeshGL64 (mirrors flat_geometry.cpp) ──
manifold::MeshGL64 to_meshgl(const FlatMesh& mesh) {
    manifold::MeshGL64 gl;
    gl.numProp = 3;
    gl.vertProperties.reserve(mesh.vertices.size() * 3);
    for (const auto& v : mesh.vertices) {
        gl.vertProperties.push_back(v.x);
        gl.vertProperties.push_back(v.y);
        gl.vertProperties.push_back(v.z);
    }
    gl.triVerts.reserve(mesh.indices.size());
    for (const auto idx : mesh.indices) {
        gl.triVerts.push_back(static_cast<std::uint64_t>(idx));
    }
    gl.Merge();
    return gl;
}

// Build a 3x4 transform that maps world → local frame where the slicing
// plane is at z=0 with `plane_up` becoming +Y.
manifold::mat3x4 plane_to_xy(const std::array<double, 3>& point,
                             const std::array<double, 3>& normal,
                             const std::array<double, 3>& up) {
    auto norm3 = [](double x, double y, double z) {
        const double L = std::sqrt(x * x + y * y + z * z);
        return manifold::vec3(x / L, y / L, z / L);
    };
    manifold::vec3 Z = norm3(normal[0], normal[1], normal[2]);
    manifold::vec3 raw_up(up[0], up[1], up[2]);
    manifold::vec3 Y = raw_up - Z * linalg::dot(raw_up, Z);
    double ylen = linalg::length(Y);
    if (ylen < 1e-9) {
        manifold::vec3 fallback = (std::fabs(Z.x) < 0.9)
            ? manifold::vec3(1, 0, 0) : manifold::vec3(0, 1, 0);
        Y = fallback - Z * linalg::dot(fallback, Z);
        ylen = linalg::length(Y);
    }
    Y = Y / ylen;
    manifold::vec3 X = linalg::cross(Y, Z);

    manifold::vec3 P(point[0], point[1], point[2]);
    manifold::vec3 t(-linalg::dot(X, P),
                     -linalg::dot(Y, P),
                     -linalg::dot(Z, P));

    manifold::mat3x4 M;
    M[0] = { X.x, Y.x, Z.x };
    M[1] = { X.y, Y.y, Z.y };
    M[2] = { X.z, Y.z, Z.z };
    M[3] = t;
    return M;
}

// Tag string for a node. Built-ins go through libcadml's canonical
// helper; for `Instance` the actual element name lives in the node's
// InstanceAttrs::ref_name and we pull it from there at the call site.
std::string node_tag(NodeType t) {
    auto sv = builtin_name_from_node_type(t);
    if (!sv.empty()) return std::string(sv);
    if (t == NodeType::Instance) return "instance";
    return "unknown";
}

// Best-effort name accessor for any node type — variants that carry a
// human-meaningful name (Part, Def, Assembly, Port, Param, Instance)
// surface it; everything else returns empty.
std::string node_name(const Node& n) {
    return std::visit([](auto&& a) -> std::string {
        using T = std::decay_t<decltype(a)>;
        if constexpr (requires { a.name; })          return a.name;
        else if constexpr (requires { a.ref_name; }) return a.ref_name;
        else if constexpr (requires { a.id; }) {
            if constexpr (std::is_same_v<decltype(a.id), std::string>) return a.id;
            else                                                       return {};
        }
        else return std::string{};
    }, n.attrs);
}

}  // namespace

// ─── Mass properties ─────────────────────────────────────────────────
//
// Volume via signed tetrahedra: V_tri = (1/6) v0·(v1×v2). Sum over a
// closed mesh yields enclosed volume; the sign reflects winding.
//
// COM via signed-tetrahedra moment: integral of position × density
// over each tet is (1/24) (v0+v1+v2) ⊗ V_tri (component-wise on x/y/z)
// — actually the simpler form is r_centroid × V_tri = (1/4)(v0+v1+v2)
// × V_tri, but for the COM of a uniform-density solid bounded by a
// triangle mesh the formula reduces to:
//   V = sum 1/6 v0·(v1×v2)
//   M = sum (v0+v1+v2)/4 * 1/6 v0·(v1×v2)
//   COM = M / V
//
// Inertia tensor about origin via Mirtich '96 closed-form integration
// of the triangle-mesh contribution. Implemented using the 10-term
// per-triangle expansion (see mirtich.com or the canonical Eberly
// derivation). The raw integrals are pure mm⁵ geometric moments. When
// the caller supplies a non-zero density the tensors are multiplied
// by ρ (in kg/mm³) so the result becomes kg · mm².
MassProperties flat_mass_properties(const FlatMesh& mesh,
                                     double density_kg_per_m3,
                                     double unit_to_mm)
{
    MassProperties out;
    out.density_kg_per_m3 = density_kg_per_m3;
    if (mesh.vertices.empty() || mesh.indices.size() < 3) return out;

    // Mirtich integrals — the 10 we need for inertia. See:
    // https://people.csail.mit.edu/bkph/articles/Volume_Integration.pdf
    double integral_1   = 0;
    double integral_x   = 0,  integral_y  = 0,  integral_z  = 0;
    double integral_x2  = 0,  integral_y2 = 0,  integral_z2 = 0;
    double integral_xy  = 0,  integral_yz = 0,  integral_xz = 0;

    double surface_area  = 0;
    std::uint64_t degenerate = 0;
    const std::size_t ntri = mesh.indices.size() / 3;

    for (std::size_t t = 0; t < ntri; ++t) {
        const auto i0 = mesh.indices[t * 3 + 0];
        const auto i1 = mesh.indices[t * 3 + 1];
        const auto i2 = mesh.indices[t * 3 + 2];
        if (i0 >= mesh.vertices.size() ||
            i1 >= mesh.vertices.size() ||
            i2 >= mesh.vertices.size()) continue;
        const auto& v0 = mesh.vertices[i0];
        const auto& v1 = mesh.vertices[i1];
        const auto& v2 = mesh.vertices[i2];

        const double e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
        const double e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
        // n = e1 × e2 (un-normalised; magnitude = 2 * area)
        const double nx = e1y * e2z - e1z * e2y;
        const double ny = e1z * e2x - e1x * e2z;
        const double nz = e1x * e2y - e1y * e2x;
        const double area2 = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (area2 < 1e-18) { ++degenerate; continue; }
        surface_area += area2 * 0.5;

        // Per-vertex triple products. The Mirtich/Eberly form for the
        // 10 integrals reduces nicely to per-component sums of vertex
        // products. See "Polyhedral Mass Properties Revisited" (Eberly).
        auto sub = [](double x0, double x1, double x2,
                      double& f1, double& f2, double& f3,
                      double& g0, double& g1, double& g2) {
            const double t0 = x0 + x1;
            f1 = t0 + x2;
            const double t1 = x0 * x0;
            const double t2 = t1 + x1 * t0;
            f2 = t2 + x2 * f1;
            f3 = x0 * t1 + x1 * t2 + x2 * f2;
            g0 = f2 + x0 * (f1 + x0);
            g1 = f2 + x1 * (f1 + x1);
            g2 = f2 + x2 * (f1 + x2);
        };

        double f1x, f2x, f3x, g0x, g1x, g2x;
        double f1y, f2y, f3y, g0y, g1y, g2y;
        double f1z, f2z, f3z, g0z, g1z, g2z;
        sub(v0.x, v1.x, v2.x, f1x, f2x, f3x, g0x, g1x, g2x);
        sub(v0.y, v1.y, v2.y, f1y, f2y, f3y, g0y, g1y, g2y);
        sub(v0.z, v1.z, v2.z, f1z, f2z, f3z, g0z, g1z, g2z);

        // Per-triangle contribution to each integral.
        integral_1  += nx * f1x;
        integral_x  += nx * f2x;
        integral_y  += ny * f2y;
        integral_z  += nz * f2z;
        integral_x2 += nx * f3x;
        integral_y2 += ny * f3y;
        integral_z2 += nz * f3z;
        integral_xy += nx * (v0.y * g0x + v1.y * g1x + v2.y * g2x);
        integral_yz += ny * (v0.z * g0y + v1.z * g1y + v2.z * g2y);
        integral_xz += nz * (v0.x * g0z + v1.x * g1z + v2.x * g2z);
    }

    integral_1  *= 1.0 / 6.0;
    integral_x  *= 1.0 / 24.0;
    integral_y  *= 1.0 / 24.0;
    integral_z  *= 1.0 / 24.0;
    integral_x2 *= 1.0 / 60.0;
    integral_y2 *= 1.0 / 60.0;
    integral_z2 *= 1.0 / 60.0;
    integral_xy *= 1.0 / 120.0;
    integral_yz *= 1.0 / 120.0;
    integral_xz *= 1.0 / 120.0;

    // The integrals above are in DOCUMENT units (raw triangle coords).
    // Convert to mm via `unit_to_mm` so the *_mm3 / *_mm2 / kg·mm²
    // fields on the output actually carry their named units.
    const double s1 = unit_to_mm;
    const double s2 = s1 * s1;
    const double s3 = s2 * s1;
    const double s5 = s2 * s3;

    out.volume_mm3       = integral_1 * s3;
    out.surface_area_mm2 = surface_area * s2;
    out.degenerate_triangles = degenerate;

    // Reject the COM division when the volume is below a *relative*
    // threshold tied to the mesh's surface area. surface_area^1.5 is
    // volume-shaped and unit-correct (both scale with extent^3) so
    // the test works regardless of unit_to_mm. Below the threshold
    // the COM stays at (0, 0, 0).
    const double vol_ref = std::pow(std::max(surface_area, 1e-30), 1.5);
    if (std::fabs(integral_1) > 1e-12 * vol_ref) {
        out.center_of_mass = {
            (integral_x / integral_1) * s1,
            (integral_y / integral_1) * s1,
            (integral_z / integral_1) * s1,
        };
    }

    // Inertia tensor about origin (mass = 1):
    //   Ixx = integral_y2 + integral_z2
    //   Iyy = integral_x2 + integral_z2
    //   Izz = integral_x2 + integral_y2
    //   Ixy = -integral_xy   (off-diagonals are negated)
    //   Iyz = -integral_yz
    //   Ixz = -integral_xz
    //
    // The raw integrals are doc-unit⁵; multiply by s5 to land in mm⁵.
    const double Ixx =  (integral_y2 + integral_z2) * s5;
    const double Iyy =  (integral_x2 + integral_z2) * s5;
    const double Izz =  (integral_x2 + integral_y2) * s5;
    const double Ixy = -integral_xy * s5;
    const double Iyz = -integral_yz * s5;
    const double Ixz = -integral_xz * s5;
    out.inertia_origin = {
        Ixx, Ixy, Ixz,
        Ixy, Iyy, Iyz,
        Ixz, Iyz, Izz,
    };

    // Parallel-axis shift to the centre of mass:
    //   I_com = I_origin − M ⋅ (||c||² · I − c cᵀ)
    // with M = volume in mm³ (the implicit mass in the unit-density
    // integrals once unit-scaled). All quantities are now in mm-based
    // units, so the shift composes cleanly.
    const double cx = out.center_of_mass[0];
    const double cy = out.center_of_mass[1];
    const double cz = out.center_of_mass[2];
    const double M  = out.volume_mm3;
    const double Ixx_c = Ixx - M * (cy * cy + cz * cz);
    const double Iyy_c = Iyy - M * (cx * cx + cz * cz);
    const double Izz_c = Izz - M * (cx * cx + cy * cy);
    const double Ixy_c = Ixy + M * (cx * cy);
    const double Iyz_c = Iyz + M * (cy * cz);
    const double Ixz_c = Ixz + M * (cx * cz);
    out.inertia_com = {
        Ixx_c, Ixy_c, Ixz_c,
        Ixy_c, Iyy_c, Iyz_c,
        Ixz_c, Iyz_c, Izz_c,
    };

    out.is_watertight = (degenerate == 0 && ntri > 0);

    if (density_kg_per_m3 > 0) {
        // mm³ → m³: multiply by 1e-9.
        out.mass_kg = out.volume_mm3 * 1e-9 * density_kg_per_m3;
        // Fold density into both tensors so consumers report
        // results in kg·mm² directly.
        const double rho_per_mm3 = density_kg_per_m3 * 1e-9;  // kg/mm³
        for (auto& v : out.inertia_origin) v *= rho_per_mm3;
        for (auto& v : out.inertia_com)    v *= rho_per_mm3;
    }
    return out;
}

// ─── Bounding shapes ─────────────────────────────────────────────────
//
// AABB is trivial. OBB uses PCA on the vertex set: covariance matrix,
// 3x3 Jacobi eigendecomposition (we don't ship Eigen here, so a small
// in-file Jacobi is appropriate), eigenvectors → axes, project all
// vertices, take min/max per axis.
//
// Sphere uses Ritter's two-pass algorithm: pick a far pair, sphere
// around them, then expand for any vertex outside. ~10 % loose vs.
// the optimal Welzl sphere; sufficient for shipping-volume estimates.
//
// Bounding cylinder per axis: radius = max distance from the axis
// line to any vertex; height = AABB extent on that axis. Useful for
// "what stock would I clamp this to in a lathe".

namespace {

void jacobi_3x3(double m[3][3], double evec[3][3], double eval[3]) {
    // Symmetric Jacobi for a 3×3 covariance matrix.
    //
    // Convergence + skip thresholds are SCALE-RELATIVE (a fraction of
    // the input's Frobenius norm). The old code used absolute 1e-12 /
    // 1e-15 thresholds; for parts whose extents are ~1e-3 mm (μm
    // features) the covariance entries are O(1e-6) and the sweep
    // quit immediately with garbage eigenvectors.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) evec[i][j] = (i == j ? 1.0 : 0.0);
    }
    double frob_sq = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) frob_sq += m[i][j] * m[i][j];
    const double frob = std::sqrt(frob_sq);
    if (frob == 0.0) {
        for (int i = 0; i < 3; ++i) eval[i] = 0;
        return;
    }
    constexpr double kEpsRel = 1e-12;
    const double conv_thresh  = kEpsRel * frob;
    const double skip_thresh  = kEpsRel * frob;

    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j) off += std::fabs(m[i][j]);
        if (off < conv_thresh) break;

        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(m[p][q]) < skip_thresh) continue;
                const double app = m[p][p], aqq = m[q][q];
                const double theta = (aqq - app) / (2 * m[p][q]);
                double t;
                if (std::fabs(theta) > 1e15) t = 1.0 / (2 * theta);
                else {
                    const double s = (theta > 0 ? 1.0 : -1.0);
                    t = s / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                }
                const double c = 1.0 / std::sqrt(t * t + 1);
                const double s = t * c;
                m[p][p] = app - t * m[p][q];
                m[q][q] = aqq + t * m[p][q];
                m[p][q] = m[q][p] = 0;
                for (int r = 0; r < 3; ++r) {
                    if (r != p && r != q) {
                        const double mp = m[r][p], mq = m[r][q];
                        m[r][p] = m[p][r] = c * mp - s * mq;
                        m[r][q] = m[q][r] = s * mp + c * mq;
                    }
                    const double vp = evec[r][p], vq = evec[r][q];
                    evec[r][p] = c * vp - s * vq;
                    evec[r][q] = s * vp + c * vq;
                }
            }
        }
    }
    for (int i = 0; i < 3; ++i) eval[i] = m[i][i];

    // Sort eigenvalues largest-first and reorder eigenvector columns
    // to match — gives the OBB its longest extent on axis 0.
    int order[3] = { 0, 1, 2 };
    std::sort(order, order + 3,
              [&](int a, int b) { return eval[a] > eval[b]; });
    double sorted_eval[3];
    double sorted_evec[3][3];
    for (int k = 0; k < 3; ++k) {
        sorted_eval[k] = eval[order[k]];
        for (int r = 0; r < 3; ++r) sorted_evec[r][k] = evec[r][order[k]];
    }
    for (int k = 0; k < 3; ++k) {
        eval[k] = sorted_eval[k];
        for (int r = 0; r < 3; ++r) evec[r][k] = sorted_evec[r][k];
    }

    // Force right-handedness: flip the third column if the basis is
    // left-handed so callers don't have to special-case mirrored OBBs.
    const double det =
        evec[0][0] * (evec[1][1] * evec[2][2] - evec[1][2] * evec[2][1]) -
        evec[0][1] * (evec[1][0] * evec[2][2] - evec[1][2] * evec[2][0]) +
        evec[0][2] * (evec[1][0] * evec[2][1] - evec[1][1] * evec[2][0]);
    if (det < 0) {
        for (int r = 0; r < 3; ++r) evec[r][2] = -evec[r][2];
    }
}

}  // namespace

BoundingShapes flat_bounds(const FlatMesh& mesh) {
    BoundingShapes out;
    if (mesh.vertices.empty()) return out;

    out.aabb_min = {  kInf,  kInf,  kInf };
    out.aabb_max = { -kInf, -kInf, -kInf };

    double cx = 0, cy = 0, cz = 0;
    for (const auto& v : mesh.vertices) {
        out.aabb_min[0] = std::min(out.aabb_min[0], v.x);
        out.aabb_min[1] = std::min(out.aabb_min[1], v.y);
        out.aabb_min[2] = std::min(out.aabb_min[2], v.z);
        out.aabb_max[0] = std::max(out.aabb_max[0], v.x);
        out.aabb_max[1] = std::max(out.aabb_max[1], v.y);
        out.aabb_max[2] = std::max(out.aabb_max[2], v.z);
        cx += v.x; cy += v.y; cz += v.z;
    }
    const double n = static_cast<double>(mesh.vertices.size());
    cx /= n; cy /= n; cz /= n;

    // ── PCA OBB ───────────────────────────────────────────────────────
    // Covariance about the centroid.
    double cov[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
    for (const auto& v : mesh.vertices) {
        const double dx = v.x - cx, dy = v.y - cy, dz = v.z - cz;
        cov[0][0] += dx * dx;  cov[0][1] += dx * dy;  cov[0][2] += dx * dz;
        cov[1][1] += dy * dy;  cov[1][2] += dy * dz;
        cov[2][2] += dz * dz;
    }
    cov[1][0] = cov[0][1]; cov[2][0] = cov[0][2]; cov[2][1] = cov[1][2];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) cov[i][j] /= n;

    double evec[3][3], eval[3];
    jacobi_3x3(cov, evec, eval);

    // Project every vertex onto each principal axis, find extents.
    double pmin[3] = {  kInf,  kInf,  kInf };
    double pmax[3] = { -kInf, -kInf, -kInf };
    for (const auto& v : mesh.vertices) {
        const double dx = v.x - cx, dy = v.y - cy, dz = v.z - cz;
        for (int k = 0; k < 3; ++k) {
            const double dot = dx * evec[0][k] + dy * evec[1][k] + dz * evec[2][k];
            pmin[k] = std::min(pmin[k], dot);
            pmax[k] = std::max(pmax[k], dot);
        }
    }
    // Centre = centroid + Σ axis_k * (pmin+pmax)/2.
    out.principal_box_center = { cx, cy, cz };
    for (int k = 0; k < 3; ++k) {
        const double mid = (pmin[k] + pmax[k]) * 0.5;
        out.principal_box_center[0] += mid * evec[0][k];
        out.principal_box_center[1] += mid * evec[1][k];
        out.principal_box_center[2] += mid * evec[2][k];
        out.principal_box_extents[k] = (pmax[k] - pmin[k]) * 0.5;
    }
    // Pack axes row-major: row i = axis i.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.principal_box_axes[i * 3 + j] = evec[j][i];
    out.principal_box_volume_mm3 = 8.0 *
        out.principal_box_extents[0] * out.principal_box_extents[1] * out.principal_box_extents[2];

    // PCA-OBB ≠ minimum-bounding OBB. For asymmetric mass distributions
    // (e.g. an L-bracket where one diagonal carries more material than
    // the other) PCA finds max-variance axes that point ~45° off the
    // natural sides; the projected extents along those axes can exceed
    // the AABB. The min-OBB is bounded above by the AABB by definition,
    // so when PCA inflates we replace the OBB with the AABB. A proper
    // rotating-calipers / Klee min-OBB would be tighter than both, but
    // is a separate refactor.
    {
        const double aabb_w = out.aabb_max[0] - out.aabb_min[0];
        const double aabb_h = out.aabb_max[1] - out.aabb_min[1];
        const double aabb_d = out.aabb_max[2] - out.aabb_min[2];
        const double aabb_vol = aabb_w * aabb_h * aabb_d;
        if (out.principal_box_volume_mm3 > aabb_vol) {
            out.principal_box_axes    = { 1,0,0, 0,1,0, 0,0,1 };
            out.principal_box_extents = { aabb_w * 0.5, aabb_h * 0.5, aabb_d * 0.5 };
            out.principal_box_center  = {
                (out.aabb_max[0] + out.aabb_min[0]) * 0.5,
                (out.aabb_max[1] + out.aabb_min[1]) * 0.5,
                (out.aabb_max[2] + out.aabb_min[2]) * 0.5,
            };
            out.principal_box_volume_mm3 = aabb_vol;
        }
    }

    // ── Ritter sphere ─────────────────────────────────────────────────
    // Pass 1: pick the vertex farthest from a deterministic seed, then
    // the vertex farthest from that. Sphere = midpoint, radius = half
    // the distance.
    //
    // The seed is the lexicographically smallest (x, y, z) vertex —
    // independent of Manifold's `Merge()` which may permute vertices
    // between runs. Using `vertices[0]` directly produced sphere
    // centres that drifted across Manifold versions even when the
    // input geometry was identical.
    auto farthest_from = [&](const auto& origin) {
        double best = -1; std::size_t idx = 0;
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            const double d2 =
                (v.x - origin.x) * (v.x - origin.x) +
                (v.y - origin.y) * (v.y - origin.y) +
                (v.z - origin.z) * (v.z - origin.z);
            if (d2 > best) { best = d2; idx = i; }
        }
        return idx;
    };
    std::size_t seed = 0;
    for (std::size_t i = 1; i < mesh.vertices.size(); ++i) {
        const auto& vi = mesh.vertices[i];
        const auto& vs = mesh.vertices[seed];
        if (vi.x < vs.x ||
            (vi.x == vs.x && vi.y < vs.y) ||
            (vi.x == vs.x && vi.y == vs.y && vi.z < vs.z)) {
            seed = i;
        }
    }
    const std::size_t a = farthest_from(mesh.vertices[seed]);
    const std::size_t b = farthest_from(mesh.vertices[a]);
    const auto& va = mesh.vertices[a];
    const auto& vb = mesh.vertices[b];
    out.sphere_center = {
        (va.x + vb.x) * 0.5, (va.y + vb.y) * 0.5, (va.z + vb.z) * 0.5
    };
    out.sphere_radius = std::sqrt(
        (va.x - vb.x) * (va.x - vb.x) +
        (va.y - vb.y) * (va.y - vb.y) +
        (va.z - vb.z) * (va.z - vb.z)) * 0.5;
    // Pass 2: expand for any vertex outside.
    for (const auto& v : mesh.vertices) {
        const double dx = v.x - out.sphere_center[0];
        const double dy = v.y - out.sphere_center[1];
        const double dz = v.z - out.sphere_center[2];
        const double d  = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > out.sphere_radius) {
            const double new_r = (out.sphere_radius + d) * 0.5;
            const double k = (new_r - out.sphere_radius) / d;
            out.sphere_center[0] += dx * k;
            out.sphere_center[1] += dy * k;
            out.sphere_center[2] += dz * k;
            out.sphere_radius = new_r;
        }
    }

    // ── Bounding cylinder per AABB axis ───────────────────────────────
    // Axis k's cylinder is parallel to world axis k, runs through
    // the AABB midpoint (NOT the vertex centroid — the centroid is
    // pulled toward vertex-dense regions and would silently offset
    // the cylinder for asymmetric parts). The reported radius is the
    // max perpendicular distance from any vertex to that midline.
    const std::array<double, 3> aabb_mid{
        (out.aabb_min[0] + out.aabb_max[0]) * 0.5,
        (out.aabb_min[1] + out.aabb_max[1]) * 0.5,
        (out.aabb_min[2] + out.aabb_max[2]) * 0.5,
    };
    for (int k = 0; k < 3; ++k) {
        const int i = (k + 1) % 3;
        const int j = (k + 2) % 3;
        double max_r2 = 0;
        for (const auto& v : mesh.vertices) {
            const std::array<double, 3> dv{
                v.x - aabb_mid[0],
                v.y - aabb_mid[1],
                v.z - aabb_mid[2],
            };
            const double r2 = dv[i] * dv[i] + dv[j] * dv[j];
            if (r2 > max_r2) max_r2 = r2;
        }
        out.cyl_radius[k] = std::sqrt(max_r2);
        out.cyl_height[k] = out.aabb_max[k] - out.aabb_min[k];
        out.cyl_volume[k] =
            3.14159265358979323846 *
            out.cyl_radius[k] * out.cyl_radius[k] *
            out.cyl_height[k];
        // Axis-line anchor: the AABB midpoint with the k'th
        // coordinate cleared (the axis spans the full k-extent).
        out.cyl_axis_point[k] = aabb_mid;
        out.cyl_axis_point[k][k] = out.aabb_min[k];
    }
    return out;
}

// ─── Topology breakdown ──────────────────────────────────────────────
FlatTopology flat_topology(const FlatMesh& mesh, const Document& doc) {
    FlatTopology out;
    if (mesh.vertices.empty() || mesh.indices.empty()) return out;

    out.triangles = mesh.indices.size() / 3;
    out.vertices  = mesh.vertices.size();

    out.bbox_min = {  kInf,  kInf,  kInf };
    out.bbox_max = { -kInf, -kInf, -kInf };

    struct Acc {
        std::uint64_t triangles    = 0;
        double        volume_mm3   = 0;
        double        surface_area = 0;
        std::array<double, 3> bbox_min{  kInf,  kInf,  kInf };
        std::array<double, 3> bbox_max{ -kInf, -kInf, -kInf };
    };
    std::unordered_map<std::uint32_t, Acc> by_node;

    const bool have_attr =
        mesh.triangle_node.size() == out.triangles;

    for (std::size_t t = 0; t < out.triangles; ++t) {
        const auto i0 = mesh.indices[t * 3 + 0];
        const auto i1 = mesh.indices[t * 3 + 1];
        const auto i2 = mesh.indices[t * 3 + 2];
        if (i0 >= mesh.vertices.size() ||
            i1 >= mesh.vertices.size() ||
            i2 >= mesh.vertices.size()) continue;
        const auto& v0 = mesh.vertices[i0];
        const auto& v1 = mesh.vertices[i1];
        const auto& v2 = mesh.vertices[i2];

        // Signed-tet contribution (1/6 v0·(v1×v2)).
        const double cx = v1.y * v2.z - v1.z * v2.y;
        const double cy = v1.z * v2.x - v1.x * v2.z;
        const double cz = v1.x * v2.y - v1.y * v2.x;
        const double vol6 = v0.x * cx + v0.y * cy + v0.z * cz;

        const double e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
        const double e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
        const double area2 = std::sqrt(
            (e1y * e2z - e1z * e2y) * (e1y * e2z - e1z * e2y) +
            (e1z * e2x - e1x * e2z) * (e1z * e2x - e1x * e2z) +
            (e1x * e2y - e1y * e2x) * (e1x * e2y - e1y * e2x));

        out.volume_mm3       += vol6;
        out.surface_area_mm2 += area2;

        auto extend = [](std::array<double, 3>& lo,
                         std::array<double, 3>& hi,
                         double x, double y, double z) {
            lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x);
            lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
            lo[2] = std::min(lo[2], z); hi[2] = std::max(hi[2], z);
        };
        extend(out.bbox_min, out.bbox_max, v0.x, v0.y, v0.z);
        extend(out.bbox_min, out.bbox_max, v1.x, v1.y, v1.z);
        extend(out.bbox_min, out.bbox_max, v2.x, v2.y, v2.z);

        if (have_attr) {
            const auto nid = mesh.triangle_node[t];
            auto& a = by_node[nid];
            a.triangles    += 1;
            a.volume_mm3   += vol6;
            a.surface_area += area2;
            extend(a.bbox_min, a.bbox_max, v0.x, v0.y, v0.z);
            extend(a.bbox_min, a.bbox_max, v1.x, v1.y, v1.z);
            extend(a.bbox_min, a.bbox_max, v2.x, v2.y, v2.z);
        }
    }
    out.volume_mm3       /= 6.0;
    out.surface_area_mm2 *= 0.5;

    // Materialise per-element list, sorted by node_id.
    std::vector<std::uint32_t> ids;
    ids.reserve(by_node.size());
    for (const auto& kv : by_node) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());

    for (const auto id : ids) {
        const auto& a = by_node[id];
        FlatTopology::Element e;
        e.node_id      = id;
        e.triangles    = a.triangles;
        e.volume_mm3   = a.volume_mm3 / 6.0;
        e.surface_area_mm2 = a.surface_area * 0.5;
        e.bbox_min = a.bbox_min;
        e.bbox_max = a.bbox_max;
        if (id < doc.nodes.size()) {
            const auto& n = doc.nodes[id];
            e.tag  = node_tag(n.type);
            e.name = node_name(n);
        } else {
            e.tag = "unknown";
        }
        out.elements.push_back(std::move(e));
    }
    return out;
}

// ─── File-to-file diff ───────────────────────────────────────────────
DiffReport flat_diff(const FlatEvalResult& a, const FlatEvalResult& b) {
    DiffReport out;

    auto props = [](const FlatMesh& m) {
        // Cheap whole-mesh aggregates (volume, surface, COM). Cheaper
        // than calling flat_mass_properties since we don't need the
        // full inertia tensor.
        double vol6 = 0, area2 = 0;
        double mx = 0, my = 0, mz = 0;
        const std::size_t ntri = m.indices.size() / 3;
        for (std::size_t t = 0; t < ntri; ++t) {
            const auto i0 = m.indices[t * 3 + 0];
            const auto i1 = m.indices[t * 3 + 1];
            const auto i2 = m.indices[t * 3 + 2];
            if (i0 >= m.vertices.size() ||
                i1 >= m.vertices.size() ||
                i2 >= m.vertices.size()) continue;
            const auto& v0 = m.vertices[i0];
            const auto& v1 = m.vertices[i1];
            const auto& v2 = m.vertices[i2];
            const double cx = v1.y * v2.z - v1.z * v2.y;
            const double cy = v1.z * v2.x - v1.x * v2.z;
            const double cz = v1.x * v2.y - v1.y * v2.x;
            const double v6 = v0.x * cx + v0.y * cy + v0.z * cz;
            vol6 += v6;
            mx += (v0.x + v1.x + v2.x) * 0.25 * v6;
            my += (v0.y + v1.y + v2.y) * 0.25 * v6;
            mz += (v0.z + v1.z + v2.z) * 0.25 * v6;
            const double e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
            const double e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
            area2 += std::sqrt(
                (e1y * e2z - e1z * e2y) * (e1y * e2z - e1z * e2y) +
                (e1z * e2x - e1x * e2z) * (e1z * e2x - e1x * e2z) +
                (e1x * e2y - e1y * e2x) * (e1x * e2y - e1y * e2x));
        }
        const double vol = vol6 / 6.0;
        const double area = area2 * 0.5;
        std::array<double, 3> com{ 0, 0, 0 };
        if (std::fabs(vol) > 1e-12) {
            // M_x in the formula above is moment about origin without
            // the leading 1/6 factor; divide by vol6 (also un-1/6'd).
            com = { mx / vol6, my / vol6, mz / vol6 };
        }
        return std::tuple<double, double, std::array<double, 3>, std::uint64_t>{
            vol, area, com, ntri };
    };

    // Index parts by name.
    std::map<std::string, std::size_t> by_a, by_b;
    for (std::size_t i = 0; i < a.parts.size(); ++i) by_a[a.parts[i].name] = i;
    for (std::size_t i = 0; i < b.parts.size(); ++i) by_b[b.parts[i].name] = i;

    std::set<std::string> names;
    for (const auto& kv : by_a) names.insert(kv.first);
    for (const auto& kv : by_b) names.insert(kv.first);

    for (const auto& name : names) {
        DiffEntry e;
        e.part = name;
        std::array<double, 3> com_a{}, com_b{};
        if (auto it = by_a.find(name); it != by_a.end()) {
            e.in_a = true;
            const auto& m = a.parts[it->second].mesh;
            auto [vol, area, com, ntri] = props(m);
            e.volume_a    = vol;
            e.surface_a   = area;
            e.triangles_a = ntri;
            com_a = com;
            out.total_volume_a += vol;
        }
        if (auto it = by_b.find(name); it != by_b.end()) {
            e.in_b = true;
            const auto& m = b.parts[it->second].mesh;
            auto [vol, area, com, ntri] = props(m);
            e.volume_b    = vol;
            e.surface_b   = area;
            e.triangles_b = ntri;
            com_b = com;
            out.total_volume_b += vol;
        }
        if (e.in_a && e.in_b) {
            e.center_shift_mm = std::sqrt(
                (com_a[0] - com_b[0]) * (com_a[0] - com_b[0]) +
                (com_a[1] - com_b[1]) * (com_a[1] - com_b[1]) +
                (com_a[2] - com_b[2]) * (com_a[2] - com_b[2]));
        }
        out.entries.push_back(std::move(e));
    }
    return out;
}

// ─── Measurement probes ──────────────────────────────────────────────
namespace {

struct ElementVerts {
    std::vector<std::uint32_t> vertex_indices;
    std::array<double, 3> bbox_min{  kInf,  kInf,  kInf };
    std::array<double, 3> bbox_max{ -kInf, -kInf, -kInf };
    bool populated = false;
};

std::unordered_map<std::uint32_t, ElementVerts>
collect_verts_for(const FlatMesh& mesh,
                  const std::vector<std::uint32_t>& wanted)
{
    std::unordered_map<std::uint32_t, ElementVerts> out;
    if (mesh.triangle_node.size() != mesh.indices.size() / 3) return out;

    std::unordered_map<std::uint32_t,
                       std::unordered_map<std::uint32_t, std::uint8_t>> seen;
    std::unordered_map<std::uint32_t, std::uint8_t> wanted_set;
    for (auto id : wanted) {
        wanted_set[id] = 1;
        out[id];   // default-construct entry so size_check is correct
    }

    const std::size_t ntri = mesh.indices.size() / 3;
    for (std::size_t t = 0; t < ntri; ++t) {
        const auto eid = mesh.triangle_node[t];
        if (!wanted_set.count(eid)) continue;
        auto& ev = out[eid];
        auto& s = seen[eid];
        for (int c = 0; c < 3; ++c) {
            const auto vi = mesh.indices[t * 3 + c];
            if (vi >= mesh.vertices.size()) continue;
            if (s.emplace(vi, 1).second) {
                ev.vertex_indices.push_back(vi);
                const auto& v = mesh.vertices[vi];
                ev.bbox_min[0] = std::min(ev.bbox_min[0], v.x);
                ev.bbox_min[1] = std::min(ev.bbox_min[1], v.y);
                ev.bbox_min[2] = std::min(ev.bbox_min[2], v.z);
                ev.bbox_max[0] = std::max(ev.bbox_max[0], v.x);
                ev.bbox_max[1] = std::max(ev.bbox_max[1], v.y);
                ev.bbox_max[2] = std::max(ev.bbox_max[2], v.z);
            }
        }
        ev.populated = true;
    }
    return out;
}

const char* kind_name(MeasureKind k) {
    switch (k) {
        case MeasureKind::Bbox:         return "bbox";
        case MeasureKind::DistanceMin:  return "min-distance";
        case MeasureKind::DistanceMean: return "mean-distance";
        case MeasureKind::DistanceMax:  return "max-distance";
    }
    return "unknown";
}

}  // namespace

MeasureResult flat_measure(const FlatMesh& mesh,
                            const std::vector<MeasureProbe>& probes) {
    MeasureResult out;
    out.items.reserve(probes.size());

    std::vector<std::uint32_t> ids;
    ids.reserve(probes.size() * 2);
    for (const auto& p : probes) {
        ids.push_back(p.element_a);
        if (p.kind != MeasureKind::Bbox) ids.push_back(p.element_b);
    }
    auto verts = collect_verts_for(mesh, ids);

    for (const auto& p : probes) {
        MeasureItem it;
        it.kind      = kind_name(p.kind);
        it.element_a = p.element_a;
        it.element_b = p.element_b;

        const auto a_it = verts.find(p.element_a);
        if (a_it == verts.end() || !a_it->second.populated) {
            it.error = "element_a produced no triangles";
            out.items.push_back(std::move(it));
            continue;
        }
        const auto& ev_a = a_it->second;

        if (p.kind == MeasureKind::Bbox) {
            it.bbox_min = ev_a.bbox_min;
            it.bbox_max = ev_a.bbox_max;
            it.size = {
                ev_a.bbox_max[0] - ev_a.bbox_min[0],
                ev_a.bbox_max[1] - ev_a.bbox_min[1],
                ev_a.bbox_max[2] - ev_a.bbox_min[2],
            };
            it.ok = true;
            out.items.push_back(std::move(it));
            continue;
        }

        const auto b_it = verts.find(p.element_b);
        if (b_it == verts.end() || !b_it->second.populated) {
            it.error = "element_b produced no triangles";
            out.items.push_back(std::move(it));
            continue;
        }
        const auto& ev_b = b_it->second;

        // Single pass over the cross-product collects min/sum/max.
        // Brute-force O(|A|*|B|); fine for the per-element scale most
        // probes operate at (rarely more than a few hundred verts).
        double best = kInf;
        double worst = 0;
        double sum = 0;
        std::uint64_t count = 0;
        for (const auto ia : ev_a.vertex_indices) {
            const auto& va = mesh.vertices[ia];
            for (const auto ib : ev_b.vertex_indices) {
                const auto& vb = mesh.vertices[ib];
                const double dx = va.x - vb.x;
                const double dy = va.y - vb.y;
                const double dz = va.z - vb.z;
                const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < best)  best  = d;
                if (d > worst) worst = d;
                sum += d;
                ++count;
            }
        }
        it.pair_count = count;
        switch (p.kind) {
            case MeasureKind::DistanceMin:  it.distance = best; break;
            case MeasureKind::DistanceMax:  it.distance = worst; break;
            case MeasureKind::DistanceMean:
                it.distance = (count > 0) ? sum / static_cast<double>(count) : 0;
                break;
            default: break;   // Bbox handled above
        }
        it.ok = true;
        out.items.push_back(std::move(it));
    }
    return out;
}

// ─── Cylindrical-feature inventory ───────────────────────────────────
namespace {

// Find the nearest ancestor of node `idx` whose type is in `wanted`.
// Returns NO_NODE if no such ancestor exists. Walks via the parent
// chain and stops once we reach the document root.
std::uint32_t nearest_ancestor_of(const Document& doc,
                                  std::uint32_t idx,
                                  std::initializer_list<NodeType> wanted)
{
    std::uint32_t cur = doc.nodes[idx].parent;
    while (cur != NO_NODE) {
        const auto t = doc.nodes[cur].type;
        for (auto w : wanted) if (w == t) return cur;
        cur = doc.nodes[cur].parent;
    }
    return NO_NODE;
}

// Walk up from `idx` to find the enclosing <part>, returning its
// PartAttrs::name (or empty if no <part> ancestor — e.g. inside a
// <def>; the def name is reported instead).
std::string find_owner_label(const Document& doc, std::uint32_t idx) {
    std::uint32_t cur = doc.nodes[idx].parent;
    while (cur != NO_NODE) {
        const auto& n = doc.nodes[cur];
        if (n.type == NodeType::Part) {
            return std::get<PartAttrs>(n.attrs).name;
        }
        if (n.type == NodeType::Def) {
            return "<def:" + std::get<DefAttrs>(n.attrs).name + ">";
        }
        cur = n.parent;
    }
    return "";
}

// Determine the role of a circle-extruded-into-difference. The circle
// is "drilled" (subtracted from a body) when the enclosing <difference>
// has it as a non-first child. When the circle is the first child of
// the <difference>, it's the body the difference subtracts FROM —
// that's not really a hole, so we skip those.
//
// Returns "drilled" or "" when not a hole.
std::string classify_diff_role(const Document& doc, std::uint32_t circle_idx,
                                std::uint32_t diff_idx) {
    // Walk children of diff_idx in order; what's the position of the
    // ancestor that's a direct child of diff_idx?
    std::uint32_t up = circle_idx;
    while (doc.nodes[up].parent != diff_idx) {
        up = doc.nodes[up].parent;
        if (up == NO_NODE) return "";
    }
    // up is now the direct child of diff_idx that contains our circle.
    std::uint32_t child = doc.nodes[diff_idx].first_child;
    if (child == up) return "";   // circle is in the FIRST child = the body
    return "drilled";
}

// ── Direction-vector resolver for <extrude direction=…> + ancestor groups
//
// We deliberately do NOT lean on the engine's `parse_transform_string`
// helper (it lives in an anonymous namespace inside flat_evaluator.cpp).
// Holes only need the rotational part of each ancestor `<group transform>`;
// translation and scale don't change the extrude axis. So we parse the
// transform string ourselves and only handle `rotate(deg, ax, ay, az)`
// — every other op is a no-op for the direction we're tracking.

struct AxisVec {
    double x = 0, y = 0, z = 1;
};

AxisVec parse_local_direction(const std::string& s) {
    // Empty / default → +z. Single-axis aliases use SVG-ish notation.
    if (s.empty())                  return { 0, 0, 1 };
    if (s == "+x" || s == "x")      return { 1, 0, 0 };
    if (s == "-x")                  return { -1, 0, 0 };
    if (s == "+y" || s == "y")      return { 0, 1, 0 };
    if (s == "-y")                  return { 0, -1, 0 };
    if (s == "+z" || s == "z")      return { 0, 0, 1 };
    if (s == "-z")                  return { 0, 0, -1 };
    // Could be a "x y z" tuple. Split on whitespace / commas and parse
    // each component via parse_double_strict — the rest of the engine
    // standardised on the locale-neutral helper specifically so a host
    // with ',' as its locale's decimal separator (de_DE, fr_FR, …)
    // doesn't truncate the first '.' in `0.0 0.0 1.0`.
    AxisVec v{ 0, 0, 1 };
    std::vector<double> nums;
    std::size_t i = 0;
    while (i < s.size() && nums.size() < 3) {
        while (i < s.size() && (s[i] == ',' ||
               std::isspace(static_cast<unsigned char>(s[i])))) ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != ',' &&
               !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (start == i) break;
        auto parsed = ::cadml::parse_double_strict(s.substr(start, i - start));
        if (!parsed) return v;     // bad input → keep default +z
        nums.push_back(*parsed);
    }
    if (nums.size() == 3) {
        const double x = nums[0], y = nums[1], z = nums[2];
        const double L = std::sqrt(x * x + y * y + z * z);
        if (L > 1e-12) { v = { x / L, y / L, z / L }; }
    }
    return v;
}

AxisVec rotate_around(AxisVec v, double deg, double ax, double ay, double az) {
    const double L = std::sqrt(ax * ax + ay * ay + az * az);
    if (L < 1e-12) return v;
    ax /= L; ay /= L; az /= L;
    const double rad = deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad), k = 1.0 - c;
    // Rodrigues' rotation matrix applied to v.
    const double xx = c + ax * ax * k;
    const double xy = ax * ay * k - az * s;
    const double xz = ax * az * k + ay * s;
    const double yx = ay * ax * k + az * s;
    const double yy = c + ay * ay * k;
    const double yz = ay * az * k - ax * s;
    const double zx = az * ax * k - ay * s;
    const double zy = az * ay * k + ax * s;
    const double zz = c + az * az * k;
    return {
        xx * v.x + xy * v.y + xz * v.z,
        yx * v.x + yy * v.y + yz * v.z,
        zx * v.x + zy * v.y + zz * v.z,
    };
}

// Apply just the rotate(...) calls in a transform string to `v`. SVG
// transform composition is left-to-right in TEXT, but in MATRIX terms
// the leftmost op is the outermost (last applied to a local point).
// So for `rotate(A) rotate(B)`, the engine builds M = R_A * R_B and
// applies M to a local point: M * p = R_A * (R_B * p) — R_B fires
// FIRST. To stay in lock-step with the evaluator's
// parse_transform_string, we collect rotations left-to-right and
// apply them in REVERSE order to the axis vector.
AxisVec apply_transform_rotations(AxisVec v, const std::string& transform,
                                  ExpressionEvaluator& expr_ev) {
    // Collected rotation / mirror ops in text (left-to-right) order.
    // We apply them in reverse so the composition matches the
    // evaluator's matrix-chain semantics. `mirror(ax, ay, az)` flips
    // the axis vector across the plane normal to (ax, ay, az); the
    // hole-axis report must follow that flip or it disagrees with
    // the rendered geometry for any mirrored assembly.
    struct PendingOp {
        bool   is_mirror = false;
        double deg = 0, ax = 0, ay = 0, az = 0;
    };
    std::vector<PendingOp> rots;

    std::size_t pos = 0;
    auto skip_ws = [&]() { while (pos < transform.size() &&
                                  std::isspace(static_cast<unsigned char>(transform[pos]))) ++pos; };
    while (pos < transform.size()) {
        skip_ws();
        if (pos >= transform.size()) break;
        const std::size_t name_start = pos;
        while (pos < transform.size() &&
               std::isalpha(static_cast<unsigned char>(transform[pos]))) ++pos;
        const std::string name(
            transform.substr(name_start, pos - name_start));
        skip_ws();
        if (pos >= transform.size() || transform[pos] != '(') break;
        ++pos;
        const std::size_t args_start = pos;
        int depth = 1;
        while (pos < transform.size() && depth > 0) {
            if (transform[pos] == '(') ++depth;
            else if (transform[pos] == ')') --depth;
            if (depth > 0) ++pos;
        }
        const std::string args_text(
            transform.substr(args_start, pos - args_start));
        if (pos < transform.size()) ++pos;   // step past ')'
        if (name != "rotate" && name != "mirror")
            continue;                          // ignore translate / scale

        // Split args on ',' or whitespace, evaluate each as expression.
        std::vector<double> args;
        std::string buf;
        auto flush = [&]() {
            if (buf.empty()) return;
            std::vector<ExpressionError> errs;
            auto val = expr_ev.evaluate_number(buf, SourceRange{}, errs);
            if (val.has_value()) args.push_back(*val);
            buf.clear();
        };
        for (char c : args_text) {
            if (c == ',' || std::isspace(static_cast<unsigned char>(c))) flush();
            else buf.push_back(c);
        }
        flush();
        if (name == "rotate") {
            if (args.size() == 4) {
                rots.push_back({false, args[0], args[1], args[2], args[3]});
            } else if (args.size() == 1) {
                // SVG-style 2D rotate; CADML rotates about Z by convention.
                rots.push_back({false, args[0], 0, 0, 1});
            }
        } else if (name == "mirror" && args.size() == 3) {
            rots.push_back({true, 0, args[0], args[1], args[2]});
        }
    }
    // Apply in reverse: the rightmost text op is applied to the local
    // vector FIRST, then each leftward op wraps the previous result.
    for (auto it = rots.rbegin(); it != rots.rend(); ++it) {
        if (it->is_mirror) {
            // Householder reflection across the plane normal to
            // (ax, ay, az): v' = v - 2 (v·n) n / |n|².
            const double nx = it->ax, ny = it->ay, nz = it->az;
            const double n2 = nx * nx + ny * ny + nz * nz;
            if (n2 < 1e-30) continue;
            const double dot = (v.x * nx + v.y * ny + v.z * nz) / n2;
            v.x -= 2 * dot * nx;
            v.y -= 2 * dot * ny;
            v.z -= 2 * dot * nz;
        } else {
            v = rotate_around(v, it->deg, it->ax, it->ay, it->az);
        }
    }
    return v;
}

// Convert a unit direction vector to a human label. Dominant axis
// (component > 0.95) → "+x"/"-x"/.../etc. Otherwise emit a
// 2-decimal vector tuple as a stable, human-readable fallback.
std::string axis_label(AxisVec v) {
    const double L = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (L < 1e-12) return "+z";
    v.x /= L; v.y /= L; v.z /= L;
    constexpr double kDom = 0.95;
    if (std::fabs(v.x) >= kDom) return v.x > 0 ? "+x" : "-x";
    if (std::fabs(v.y) >= kDom) return v.y > 0 ? "+y" : "-y";
    if (std::fabs(v.z) >= kDom) return v.z > 0 ? "+z" : "-z";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%+.2f, %+.2f, %+.2f)", v.x, v.y, v.z);
    return buf;
}

// Walk parents from `extrude_idx` to the document root, accumulating
// any rotate() calls in ancestor `<group transform>` strings on top of
// the extrude's local direction. Returns a world-frame axis label.
std::string resolve_world_axis(const Document& doc,
                                std::uint32_t extrude_idx,
                                ExpressionEvaluator& expr_ev) {
    const auto& ext = std::get<ExtrudeAttrs>(doc.nodes[extrude_idx].attrs);
    AxisVec v = parse_local_direction(ext.direction_expr);

    // Walk parent chain inside-out (closest first) and apply each
    // ancestor's transform in that order: the extrude's direction is
    // expressed in its immediate parent's local frame, that parent's
    // transform takes it into the grandparent's frame, and so on up
    // to the world. Composing the OUTER transform first would give
    // the wrong basis (we already verified this on the engine's
    // pattern-unrolled head-bolt holes — outer-first put the four
    // instances at ±x/±y, the correct inner-first answer is all +y).
    std::uint32_t cur = doc.nodes[extrude_idx].parent;
    while (cur != NO_NODE) {
        const auto& n = doc.nodes[cur];
        if (n.type == NodeType::Group) {
            const auto& g = std::get<GroupAttrs>(n.attrs);
            if (!g.transform.empty()) {
                v = apply_transform_rotations(v, g.transform, expr_ev);
            }
        }
        cur = n.parent;
    }
    return axis_label(v);
}

}  // namespace

HoleReport flat_holes(const Document& doc) {
    HoleReport out;

    // Bind every numeric param into the evaluator. Two sources:
    //   * Document::params         — frontmatter-level
    //   * <param> Nodes in the body — per-part / per-def
    // The bundler folds frontmatter into per-part scope, so post-
    // bundling many params show up as Param nodes; pre-bundling some
    // are still in Document::params. Walking both keeps us robust.
    ExpressionEvaluator ev;
    auto bind_if_numeric = [&](const std::string& name,
                               const std::string& expr,
                               SourceRange src) {
        std::vector<ExpressionError> errs;
        auto v = ev.evaluate_number(expr, src, errs);
        if (v.has_value()) ev.set_param(name, *v);
    };
    for (const auto& p : doc.params) {
        bind_if_numeric(p.name, p.value_expr, p.source);
    }
    for (const auto& n : doc.nodes) {
        if (n.dead || n.type != NodeType::Param) continue;
        const auto& pa = std::get<ParamAttrs>(n.attrs);
        bind_if_numeric(pa.name, pa.value_expr, n.source);
    }

    auto any_ancestor_dead = [&](std::uint32_t idx) {
        std::uint32_t cur = doc.nodes[idx].parent;
        while (cur != NO_NODE) {
            if (doc.nodes[cur].dead) return true;
            cur = doc.nodes[cur].parent;
        }
        return false;
    };

    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        const auto& n = doc.nodes[i];
        if (n.dead || n.type != NodeType::Circle) continue;
        // Skip circles inside a retired subtree (e.g. the original
        // pattern's template that the unroller replaced with copies).
        if (any_ancestor_dead(i)) continue;

        // Must descend from an <extrude> AND a <difference> ancestor.
        const auto extrude_idx = nearest_ancestor_of(doc, i, { NodeType::Extrude });
        if (extrude_idx == NO_NODE) continue;
        const auto diff_idx = nearest_ancestor_of(doc, i, { NodeType::Difference });
        if (diff_idx == NO_NODE) continue;

        // The <extrude> must itself sit somewhere under the <difference>.
        // (Equivalently: the <difference> is an ancestor of the <extrude>.)
        bool ext_under_diff = false;
        std::uint32_t cur = doc.nodes[extrude_idx].parent;
        while (cur != NO_NODE) {
            if (cur == diff_idx) { ext_under_diff = true; break; }
            cur = doc.nodes[cur].parent;
        }
        if (!ext_under_diff) continue;

        const auto role = classify_diff_role(doc, i, diff_idx);
        if (role.empty()) continue;

        HoleEntry e;
        e.node_id   = i;
        e.role      = role;
        e.part_name = find_owner_label(doc, i);

        const auto& circle = std::get<CircleAttrs>(n.attrs);
        const auto& extrude = std::get<ExtrudeAttrs>(doc.nodes[extrude_idx].attrs);
        std::vector<ExpressionError> errs;

        if (auto r = ev.evaluate_number(circle.r_expr, n.source, errs)) {
            e.diameter_mm = *r * 2;
        } else {
            e.error_hint = "could not resolve r=" + circle.r_expr;
        }
        if (auto h = ev.evaluate_number(extrude.height_expr,
                                          doc.nodes[extrude_idx].source, errs)) {
            e.depth_mm = *h;
        } else if (e.error_hint.empty()) {
            e.error_hint = "could not resolve height=" + extrude.height_expr;
        }
        e.axis = resolve_world_axis(doc, extrude_idx, ev);

        out.entries.push_back(std::move(e));
    }
    return out;
}

// ─── Wall-thickness analysis ─────────────────────────────────────────
namespace {

// Ray-triangle intersection (Möller–Trumbore). Returns the ray-
// parameter t (distance along the unit-length ray) of the closest
// hit, or a negative number on miss. Backface hits are returned too —
// the caller decides whether to filter; we want them because the
// inward ray naturally hits the *back side* of the opposite wall.
double ray_triangle_distance(double rx, double ry, double rz,
                              double dx, double dy, double dz,
                              double v0x, double v0y, double v0z,
                              double v1x, double v1y, double v1z,
                              double v2x, double v2y, double v2z) {
    constexpr double kEps = 1e-9;
    const double e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
    const double e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;
    // h = d × e2
    const double hx = dy * e2z - dz * e2y;
    const double hy = dz * e2x - dx * e2z;
    const double hz = dx * e2y - dy * e2x;
    const double a  = e1x * hx + e1y * hy + e1z * hz;
    if (std::fabs(a) < kEps) return -1;
    const double f = 1.0 / a;
    const double sx = rx - v0x, sy = ry - v0y, sz = rz - v0z;
    const double u = f * (sx * hx + sy * hy + sz * hz);
    if (u < 0 || u > 1) return -1;
    const double qx = sy * e1z - sz * e1y;
    const double qy = sz * e1x - sx * e1z;
    const double qz = sx * e1y - sy * e1x;
    const double v = f * (dx * qx + dy * qy + dz * qz);
    if (v < 0 || u + v > 1) return -1;
    const double t = f * (e2x * qx + e2y * qy + e2z * qz);
    return t;   // negative iff hit is behind the ray origin
}

}  // namespace

WallThicknessReport flat_wall_thickness(const FlatMesh& mesh,
                                          const WallThicknessOptions& opts) {
    WallThicknessReport out;
    if (mesh.vertices.empty() || mesh.indices.empty()) return out;

    // Per-vertex normals — average of incident triangle normals.
    // Cheap to recompute here; the FlatMesh's stored normals are
    // sometimes flipped or smoothed differently for rendering.
    const std::size_t nv = mesh.vertices.size();
    std::vector<std::array<double, 3>> normals(nv, { 0, 0, 0 });
    const std::size_t ntri = mesh.indices.size() / 3;
    for (std::size_t t = 0; t < ntri; ++t) {
        const auto i0 = mesh.indices[t * 3 + 0];
        const auto i1 = mesh.indices[t * 3 + 1];
        const auto i2 = mesh.indices[t * 3 + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
        const auto& v0 = mesh.vertices[i0];
        const auto& v1 = mesh.vertices[i1];
        const auto& v2 = mesh.vertices[i2];
        const double e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
        const double e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
        const double nx = e1y * e2z - e1z * e2y;
        const double ny = e1z * e2x - e1x * e2z;
        const double nz = e1x * e2y - e1y * e2x;
        for (auto i : { i0, i1, i2 }) {
            normals[i][0] += nx;
            normals[i][1] += ny;
            normals[i][2] += nz;
        }
    }
    for (auto& n : normals) {
        const double L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (L > 1e-12) { n[0] /= L; n[1] /= L; n[2] /= L; }
    }

    std::vector<double> hits;
    hits.reserve(nv);
    // Per-hit sample data (vertex idx + thickness) — kept in the
    // same order as `hits`. Used to build the hotspots list at the
    // end. Skipped entirely when hotspot_count == 0 to avoid the
    // memory cost on the common case.
    std::vector<std::pair<std::uint32_t, double>> hit_idx;
    if (opts.hotspot_count > 0) hit_idx.reserve(nv);
    const std::uint32_t cap =
        (opts.max_samples == 0)
            ? static_cast<std::uint32_t>(nv)
            : std::min<std::uint32_t>(opts.max_samples,
                                      static_cast<std::uint32_t>(nv));

    // When sub-sampling, build a deterministic spatial-order index
    // first. Striding raw Manifold-merged indices gives a representative
    // sample only by luck — adjacent indices often correspond to nearby
    // vertices in the same face. Lex-sort by (x,y,z) so the stride
    // covers the full spatial extent regardless of Manifold's internal
    // vertex ordering, and so the same input mesh produces the same
    // sub-sample across Manifold versions.
    std::vector<std::uint32_t> spatial_order;
    if (opts.max_samples != 0 && nv > opts.max_samples) {
        spatial_order.resize(nv);
        for (std::uint32_t k = 0; k < nv; ++k) spatial_order[k] = k;
        // std::tie comparator — exact-float inequality on NaN
        // coordinates would violate strict weak ordering and trigger
        // UB inside std::sort. The tied tuples handle NaN by simply
        // being not-less-than each other (the std::sort still
        // terminates).
        std::sort(spatial_order.begin(), spatial_order.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      const auto& va = mesh.vertices[a];
                      const auto& vb = mesh.vertices[b];
                      return std::tie(va.x, va.y, va.z) <
                             std::tie(vb.x, vb.y, vb.z);
                  });
    }
    for (std::uint32_t vi = 0; vi < cap; ++vi) {
        const std::size_t idx = (opts.max_samples == 0 || nv <= opts.max_samples)
            ? vi
            : spatial_order[(static_cast<std::size_t>(vi) * nv) / cap];
        const auto& v = mesh.vertices[idx];
        const auto& n = normals[idx];
        const double L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (L < 1e-9) continue;
        ++out.samples;

        // Ray origin a hair inside (so the originating triangles
        // don't return t≈0 hits) along the negated normal.
        const double dx = -n[0], dy = -n[1], dz = -n[2];
        const double bias = opts.min_thickness_mm * 0.5;
        const double rx = v.x + dx * bias;
        const double ry = v.y + dy * bias;
        const double rz = v.z + dz * bias;

        double best = std::numeric_limits<double>::infinity();
        for (std::size_t t = 0; t < ntri; ++t) {
            const auto i0 = mesh.indices[t * 3 + 0];
            const auto i1 = mesh.indices[t * 3 + 1];
            const auto i2 = mesh.indices[t * 3 + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            // Skip triangles incident to the originating vertex —
            // those naturally return t≈0 (the source surface).
            if (i0 == idx || i1 == idx || i2 == idx) continue;
            const auto& v0 = mesh.vertices[i0];
            const auto& v1 = mesh.vertices[i1];
            const auto& v2 = mesh.vertices[i2];
            const double dist = ray_triangle_distance(
                rx, ry, rz, dx, dy, dz,
                v0.x, v0.y, v0.z,
                v1.x, v1.y, v1.z,
                v2.x, v2.y, v2.z);
            // `>=` so a wall whose true thickness equals the user's
            // `min_thickness_mm` floor still gets reported — the
            // intent is "warn me when walls are AT OR BELOW the
            // threshold", not "above".
            if (dist >= opts.min_thickness_mm && dist < best) best = dist;
        }
        if (best < std::numeric_limits<double>::infinity()) {
            hits.push_back(best + bias);   // include the bias we shifted
            if (opts.hotspot_count > 0) {
                hit_idx.emplace_back(
                    static_cast<std::uint32_t>(idx), best + bias);
            }
            ++out.samples_with_hit;
        } else {
            ++out.samples_with_no_hit;
        }
    }

    if (hits.empty()) return out;
    std::sort(hits.begin(), hits.end());
    auto pct = [&](double p) {
        const std::size_t i = static_cast<std::size_t>(
            std::clamp(p * (hits.size() - 1), 0.0,
                       static_cast<double>(hits.size() - 1)));
        return hits[i];
    };
    out.min_mm   = hits.front();
    out.p1_mm    = pct(0.01);
    out.p10_mm   = pct(0.10);
    out.median_mm = pct(0.50);
    out.max_mm   = hits.back();
    double sum = 0;
    for (double d : hits) sum += d;
    out.mean_mm = sum / static_cast<double>(hits.size());

    if (opts.hotspot_count > 0 && !hit_idx.empty()) {
        // Per-vertex node attribution: scan triangles, accumulate
        // each vertex's preferred triangle_node (first one wins —
        // boundary vertices between elements are rare).
        std::vector<std::uint32_t> vert_node(nv, NO_NODE);
        for (std::size_t t = 0; t < ntri; ++t) {
            const std::uint32_t nid =
                (t < mesh.triangle_node.size()) ? mesh.triangle_node[t] : NO_NODE;
            for (int k = 0; k < 3; ++k) {
                const auto vi = mesh.indices[t * 3 + k];
                if (vi < nv && vert_node[vi] == NO_NODE) vert_node[vi] = nid;
            }
        }
        std::sort(hit_idx.begin(), hit_idx.end(),
            [](const auto& l, const auto& r){ return l.second < r.second; });
        const std::size_t want = std::min<std::size_t>(
            opts.hotspot_count, hit_idx.size());
        out.hotspots.reserve(want);
        for (std::size_t i = 0; i < want; ++i) {
            const auto vi = hit_idx[i].first;
            const auto& v = mesh.vertices[vi];
            WallThicknessSample s;
            s.xyz = {v.x, v.y, v.z};
            s.thickness_mm = hit_idx[i].second;
            s.node_id = vi < vert_node.size() ? vert_node[vi] : NO_NODE;
            out.hotspots.push_back(s);
        }
    }
    return out;
}

TopologyAdjacency flat_topology_adjacency(const FlatMesh& mesh) {
    TopologyAdjacency out;
    if (mesh.indices.empty()) return out;

    // ── Edge classification ────────────────────────────────────────
    // Key edges by sorted (lo, hi) so opposite half-edge pairs hash
    // to the same slot. Count triangles incident to each edge.
    auto edge_key = [](std::uint32_t a, std::uint32_t b) {
        return (a < b) ? (std::uint64_t(a) << 32 | b)
                       : (std::uint64_t(b) << 32 | a);
    };
    std::unordered_map<std::uint64_t, std::uint32_t> edge_count;
    edge_count.reserve(mesh.indices.size());
    double min_len = std::numeric_limits<double>::infinity();
    double max_len = 0;
    const std::size_t tris = mesh.triangle_count();
    for (std::size_t t = 0; t < tris; ++t) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = mesh.indices[3 * t + k];
            const std::uint32_t b = mesh.indices[3 * t + (k + 1) % 3];
            ++edge_count[edge_key(a, b)];
            const auto& va = mesh.vertices[a];
            const auto& vb = mesh.vertices[b];
            const double dx = vb.x - va.x, dy = vb.y - va.y, dz = vb.z - va.z;
            const double L = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (L < min_len) min_len = L;
            if (L > max_len) max_len = L;
        }
    }
    std::uint64_t boundary = 0, non_manifold = 0, manifold = 0;
    for (const auto& [k, n] : edge_count) {
        if      (n == 1) ++boundary;
        else if (n == 2) ++manifold;
        else             ++non_manifold;
    }
    out.boundary_edges     = boundary;
    out.non_manifold_edges = non_manifold;
    out.min_edge_len_mm    = (min_len == std::numeric_limits<double>::infinity())
                              ? 0 : min_len;
    out.max_edge_len_mm    = max_len;
    out.watertight         = (boundary == 0 && non_manifold == 0);
    out.manifold           = (non_manifold == 0);

    // ── Shell count via union-find on triangles ────────────────────
    // Two triangles in the same shell iff they share an edge.
    std::vector<std::uint32_t> parent(tris);
    for (std::uint32_t i = 0; i < tris; ++i) parent[i] = i;
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](std::uint32_t a, std::uint32_t b) {
        const auto ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };
    // Build edge → triangle list, then unite all triangles sharing an edge.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edge_tris;
    edge_tris.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < tris; ++t) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = mesh.indices[3 * t + k];
            const std::uint32_t b = mesh.indices[3 * t + (k + 1) % 3];
            edge_tris[edge_key(a, b)].push_back(static_cast<std::uint32_t>(t));
        }
    }
    // Iterate edges in a stable key order so union-find produces
    // platform-independent root assignments (the unite() calls
    // collapse trees in iteration order, which affects which
    // representative each shell ends up with).
    {
        std::vector<std::uint64_t> sorted_edges;
        sorted_edges.reserve(edge_tris.size());
        for (const auto& kv : edge_tris) sorted_edges.push_back(kv.first);
        std::sort(sorted_edges.begin(), sorted_edges.end());
        for (const auto& k : sorted_edges) {
            const auto& ts = edge_tris[k];
            for (std::size_t i = 1; i < ts.size(); ++i) unite(ts[0], ts[i]);
        }
    }
    std::unordered_set<std::uint32_t> roots;
    for (std::uint32_t i = 0; i < tris; ++i) roots.insert(find(i));
    out.shell_count = static_cast<std::uint32_t>(roots.size());

    // ── Genus estimate (largest shell only, closed manifold) ──────
    // χ = V - E + F. For a closed orientable surface χ = 2 - 2g.
    if (out.watertight && out.manifold && out.shell_count > 0) {
        // Pick the largest shell. Break size ties by smallest root id
        // so the choice is platform-independent when two shells have
        // the same triangle count.
        std::unordered_map<std::uint32_t, std::uint32_t> shell_size;
        for (std::uint32_t i = 0; i < tris; ++i) ++shell_size[find(i)];
        std::uint32_t big_root = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t big_n = 0;
        for (const auto& [r, n] : shell_size) {
            if (n > big_n || (n == big_n && r < big_root)) {
                big_n = n; big_root = r;
            }
        }
        std::unordered_set<std::uint32_t> big_verts;
        std::unordered_set<std::uint64_t> big_edges;
        std::uint32_t F = 0;
        for (std::uint32_t t = 0; t < tris; ++t) {
            if (find(t) != big_root) continue;
            ++F;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = mesh.indices[3 * t + k];
                const std::uint32_t b = mesh.indices[3 * t + (k + 1) % 3];
                big_verts.insert(a); big_verts.insert(b);
                big_edges.insert(edge_key(a, b));
            }
        }
        const int chi = static_cast<int>(big_verts.size())
                      - static_cast<int>(big_edges.size())
                      + static_cast<int>(F);
        if ((2 - chi) % 2 == 0)
            out.genus_estimate = (2 - chi) / 2;
        else
            out.genus_estimate = -1;
    } else {
        out.genus_estimate = -1;
    }
    return out;
}

}  // namespace cadml::engine
