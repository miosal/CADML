// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/types.hpp>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <clocale>        // setlocale, locale_t (POSIX)
#include <cmath>
#include <cstdlib>        // strtod (host's), _strtod_l on Windows
#include <string>
#include <system_error>

namespace cadml {

// ─── Vec3 ─────────────────────────────────────────────────────────────

double Vec3::length() const {
    return std::sqrt(length_sq());
}

Vec3 Vec3::normalized() const {
    const double len = length();
    if (len < 1e-12) return { 0, 0, 0 };
    return { x / len, y / len, z / len };
}

// ─── Mat4 ─────────────────────────────────────────────────────────────

Mat4 Mat4::identity() {
    return Mat4{};   // default-constructed = identity
}

Mat4 Mat4::translation(double tx, double ty, double tz) {
    Mat4 r = identity();
    r.m[12] = tx;
    r.m[13] = ty;
    r.m[14] = tz;
    return r;
}

Mat4 Mat4::scaling(double sx, double sy, double sz) {
    Mat4 r{};
    r.m[0]  = sx;
    r.m[5]  = sy;
    r.m[10] = sz;
    r.m[15] = 1;
    return r;
}

Mat4 Mat4::rotation(double angle_deg, double ax, double ay, double az) {
    // Rodrigues' rotation formula in column-major form.
    constexpr double pi = 3.14159265358979323846;
    const double angle = angle_deg * pi / 180.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double t = 1.0 - c;

    // Normalise axis.
    const double len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-12) return identity();
    const double nx = ax / len, ny = ay / len, nz = az / len;

    Mat4 r{};
    // column 0
    r.m[0]  = c + nx * nx * t;
    r.m[1]  = ny * nx * t + nz * s;
    r.m[2]  = nz * nx * t - ny * s;
    r.m[3]  = 0;
    // column 1
    r.m[4]  = nx * ny * t - nz * s;
    r.m[5]  = c + ny * ny * t;
    r.m[6]  = nz * ny * t + nx * s;
    r.m[7]  = 0;
    // column 2
    r.m[8]  = nx * nz * t + ny * s;
    r.m[9]  = ny * nz * t - nx * s;
    r.m[10] = c + nz * nz * t;
    r.m[11] = 0;
    // column 3
    r.m[12] = 0;
    r.m[13] = 0;
    r.m[14] = 0;
    r.m[15] = 1;
    return r;
}

Mat4 Mat4::mirror(double nx, double ny, double nz) {
    // Reflection through plane with normal (nx,ny,nz) through origin:
    //   I - 2 * n * n^T
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12) return identity();
    const double ux = nx / len, uy = ny / len, uz = nz / len;

    Mat4 r{};
    r.m[0]  = 1 - 2 * ux * ux;
    r.m[1]  = -2 * uy * ux;
    r.m[2]  = -2 * uz * ux;
    r.m[3]  = 0;

    r.m[4]  = -2 * ux * uy;
    r.m[5]  = 1 - 2 * uy * uy;
    r.m[6]  = -2 * uz * uy;
    r.m[7]  = 0;

    r.m[8]  = -2 * ux * uz;
    r.m[9]  = -2 * uy * uz;
    r.m[10] = 1 - 2 * uz * uz;
    r.m[11] = 0;

    r.m[12] = 0; r.m[13] = 0; r.m[14] = 0; r.m[15] = 1;
    return r;
}

Mat4 Mat4::operator*(const Mat4& other) const {
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += m[k * 4 + row] * other.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

Vec3 Mat4::transform_point(Vec3 p) const {
    const double x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
    const double y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
    const double z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    const double w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (std::abs(w) < 1e-12) return { x, y, z };
    return { x / w, y / w, z / w };
}

Vec3 Mat4::transform_direction(Vec3 d) const {
    const double x = m[0] * d.x + m[4] * d.y + m[8]  * d.z;
    const double y = m[1] * d.x + m[5] * d.y + m[9]  * d.z;
    const double z = m[2] * d.x + m[6] * d.y + m[10] * d.z;
    return { x, y, z };
}

// ─── Port alignment (assembly compiler) ───────────────────────────────

namespace {

// Build a 4x4 from three column vectors (right, up, normal).
Mat4 frame_to_mat4(Vec3 right, Vec3 up, Vec3 normal) {
    Mat4 m{};
    m.m[0]  = right.x;  m.m[1]  = right.y;  m.m[2]  = right.z;  m.m[3]  = 0;
    m.m[4]  = up.x;     m.m[5]  = up.y;     m.m[6]  = up.z;     m.m[7]  = 0;
    m.m[8]  = normal.x; m.m[9]  = normal.y; m.m[10] = normal.z; m.m[11] = 0;
    m.m[12] = 0;        m.m[13] = 0;        m.m[14] = 0;        m.m[15] = 1;
    return m;
}

// Transpose of the 3x3 rotation block (== inverse for orthonormal frames).
Mat4 transpose_rotation(const Mat4& m) {
    Mat4 r{};
    r.m[0] = m.m[0]; r.m[1] = m.m[4]; r.m[2] = m.m[8];
    r.m[4] = m.m[1]; r.m[5] = m.m[5]; r.m[6] = m.m[9];
    r.m[8] = m.m[2]; r.m[9] = m.m[6]; r.m[10] = m.m[10];
    r.m[15] = 1;
    return r;
}

// Diag(1, 1, -1) — flips the +Z axis. Inserted between port frames so
// the two ports' normals end up anti-parallel after alignment.
Mat4 flip_z() {
    Mat4 m{};
    m.m[0] = 1; m.m[5] = 1; m.m[10] = -1; m.m[15] = 1;
    return m;
}

}  // namespace

Mat4 compute_port_alignment(Vec3 a_pos, Vec3 a_normal, Vec3 a_up,
                             Vec3 b_pos, Vec3 b_normal, Vec3 b_up,
                             const Mat4& transform_b) {
    // Build orthonormal frames from each port's (normal, up) basis.
    // `up` is Gram-Schmidt'd against `normal` so the result is strictly
    // orthonormal even if the source vectors weren't perfectly so.
    Vec3 nA = a_normal.normalized();
    Vec3 uA = a_up;
    uA = (uA - nA * uA.dot(nA)).normalized();
    Vec3 rA = uA.cross(nA).normalized();

    Vec3 nB = b_normal.normalized();
    Vec3 uB = b_up;
    uB = (uB - nB * uB.dot(nB)).normalized();
    Vec3 rB = uB.cross(nB).normalized();

    const Mat4 frame_A = frame_to_mat4(rA, uA, nA);
    const Mat4 frame_B = frame_to_mat4(rB, uB, nB);

    // R = frame_B * flip_Z * frame_A^T.
    const Mat4 R = frame_B * flip_z() * transpose_rotation(frame_A);

    const Vec3 world_pB    = transform_b.transform_point(b_pos);
    const Vec3 rotated_pA  = R.transform_point(a_pos);
    const Vec3 t = world_pB - rotated_pA;

    return Mat4::translation(t.x, t.y, t.z) * R;
}

// ─── Mat4 → transform string ─────────────────────────────────────────
//
// KNOWN LIMITATION: emits axis-angle (rotate) form which can't represent
// improper rotations (det == -1, equivalent to mirror + rotation). For
// inputs from compute_port_alignment with flipped chirality, the
// extracted rotation has zero axis components, producing the degenerate
// "rotate(angle, 0, 0, 0)" string. The engine parses zero-axis rotation
// as identity — visually correct for parts symmetric across the mirror
// plane (bolts, washers, plates) but wrong for asymmetric ones.
// TODO(0.3): emit matrix(...) form when det(R) < 0 and extend the
// engine's transform parser to handle it.

std::string mat4_to_transform_string(const Mat4& m) {
    constexpr double kPiLocal = 3.14159265358979323846;
    const double tx = m.m[12];
    const double ty = m.m[13];
    const double tz = m.m[14];

    const bool has_rotation =
        std::abs(m.m[0] - 1) > 1e-6 ||
        std::abs(m.m[5] - 1) > 1e-6 ||
        std::abs(m.m[10] - 1) > 1e-6 ||
        std::abs(m.m[1])     > 1e-6 ||
        std::abs(m.m[2])     > 1e-6 ||
        std::abs(m.m[4])     > 1e-6;

    const bool has_translation =
        std::abs(tx) > 1e-6 || std::abs(ty) > 1e-6 || std::abs(tz) > 1e-6;

    // Format helpers — locale-pinned so commas only ever appear as
    // argument separators, never as decimal points (which would
    // collide with the comma separator and break the parser).
    auto fmt = [](double v) {
        return format_double_canonical(v, 6);
    };

    std::string result;
    if (has_translation) {
        result = "translate(" + fmt(tx) + ", " + fmt(ty) + ", " +
                 fmt(tz) + ")";
    }

    if (has_rotation) {
        const double trace = m.m[0] + m.m[5] + m.m[10];
        const double clamped = std::max(-1.0, std::min(1.0, (trace - 1.0) / 2.0));
        const double angle_rad = std::acos(clamped);
        const double angle_deg = angle_rad * 180.0 / kPiLocal;

        if (std::abs(angle_deg) > 0.01) {
            const double s = 2.0 * std::sin(angle_rad);
            double ax = 0, ay = 0, az = 1;
            if (std::abs(s) > 1e-6) {
                ax = (m.m[6] - m.m[9]) / s;
                ay = (m.m[8] - m.m[2]) / s;
                az = (m.m[1] - m.m[4]) / s;
            }
            result += " rotate(" + fmt(angle_deg) + ", " + fmt(ax) +
                      ", " + fmt(ay) + ", " + fmt(az) + ")";
        }
    }

    if (!result.empty() && result.front() == ' ') result.erase(0, 1);
    return result;
}

// ─── Axis aliases (per spec §7.4) ──────────────────────────────────────

namespace {

// Trim leading and trailing whitespace, then return the trimmed view.
std::string_view trim_view(std::string_view s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

}  // namespace

std::optional<Vec3> parse_axis_alias(std::string_view s) {
    const auto t = trim_view(s);
    // Spec §7.4: +x, -x, +y, -y, +z, -z, x, y, z.
    if (t == "+x" || t == "x") return Vec3{ 1, 0, 0 };
    if (t == "-x")             return Vec3{ -1, 0, 0 };
    if (t == "+y" || t == "y") return Vec3{ 0, 1, 0 };
    if (t == "-y")             return Vec3{ 0, -1, 0 };
    if (t == "+z" || t == "z") return Vec3{ 0, 0, 1 };
    if (t == "-z")             return Vec3{ 0, 0, -1 };
    return std::nullopt;
}

namespace {

// Linear scale factor from `unit` to mm. Returns nullopt on unknown
// units. Used internally by parse_interference_tolerance and
// re-exported under cadml::units_to_mm_scale for CLI tools that need
// to convert document-unit geometry to real units before applying
// density / weight / etc.
std::optional<double> linear_to_mm(std::string_view unit) {
    if (unit == "mm") return 1.0;
    if (unit == "cm") return 10.0;
    if (unit == "m")  return 1000.0;
    if (unit == "in") return 25.4;
    if (unit == "ft") return 304.8;
    return std::nullopt;
}

}  // namespace

std::optional<double> units_to_mm_scale(std::string_view unit) {
    return linear_to_mm(unit);
}

std::string format_double_canonical(double v, int precision) {
    // Use std::to_chars(general) when the standard library ships it
    // — it's guaranteed locale-independent and produces a `.` for
    // the decimal point. Fall back to a C-locale-pinned snprintf via
    // the platform's _l() API so cross-locale builds stay
    // bit-stable.
    char buf[64];
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && \
    (!defined(_LIBCPP_VERSION) || _LIBCPP_VERSION >= 200000) && \
    (!defined(_GLIBCXX_RELEASE) || _GLIBCXX_RELEASE >= 11)
    auto res = std::to_chars(buf, buf + sizeof(buf), v,
                              std::chars_format::general, precision);
    if (res.ec != std::errc{}) {
        // Catastrophic — buffer too small for the formatted value.
        // Fall through to the snprintf path below (it'll either fit
        // or truncate identically to what to_chars would have).
    } else {
        return std::string(buf, res.ptr);
    }
#endif
#if defined(_WIN32)
    static _locale_t c_loc = _create_locale(LC_ALL, "C");
    const int n = _snprintf_s_l(buf, sizeof(buf), _TRUNCATE,
                                 "%.*g", c_loc, precision, v);
#else
    static locale_t c_loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    // Swap locale on this thread for the duration of the snprintf;
    // uselocale is per-thread, so this doesn't affect other threads.
    locale_t prev = uselocale(c_loc);
    const int n = std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
    uselocale(prev);
#endif
    if (n <= 0) return {};
    const std::size_t len = std::min<std::size_t>(
        static_cast<std::size_t>(n), sizeof(buf) - 1);
    return std::string(buf, len);
}

std::optional<double> parse_interference_tolerance(std::string_view text,
                                                     std::string_view doc_units)
{
    const auto t = trim_view(text);
    if (t.empty()) return 0.0;

    // Find where the trailing unit suffix begins. The suffix is a
    // letter (or `³`/`3`); the number portion is everything before
    // that. Walk from the start past the optional sign, digits,
    // decimal point, and exponent.
    std::size_t i = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) ++i;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
    }
    if (i == 0) return std::nullopt;     // no number found

    const auto number_part = trim_view(t.substr(0, i));
    const auto suffix_part = trim_view(t.substr(i));

    double value = 0;
    {
        // `std::stod` is permissive — it parses "1.2.3" as 1.2 and
        // returns silently. Use the `pos` out-parameter to confirm
        // it consumed the *entire* number-part our scanner found,
        // otherwise reject. Also reject infinities and NaN (overflow
        // on large cubed conversions can produce HUGE_VAL).
        std::size_t consumed = 0;
        try {
            value = std::stod(std::string(number_part), &consumed);
        } catch (...) {
            return std::nullopt;
        }
        if (consumed != number_part.size()) return std::nullopt;
        if (!std::isfinite(value))           return std::nullopt;
    }
    if (value < 0) return std::nullopt;   // negative tolerance is nonsense

    // No suffix → interpret as document-unit cubed (no conversion).
    if (suffix_part.empty()) return value;

    // Strip trailing `³` (UTF-8 0xC2 0xB3) or `3`. Anything else is
    // the linear unit (mm/cm/m/in/ft).
    std::string unit_str(suffix_part);
    if (unit_str.size() >= 2 &&
        static_cast<unsigned char>(unit_str[unit_str.size() - 2]) == 0xC2 &&
        static_cast<unsigned char>(unit_str[unit_str.size() - 1]) == 0xB3) {
        unit_str.resize(unit_str.size() - 2);
    } else if (!unit_str.empty() && unit_str.back() == '3') {
        unit_str.pop_back();
    } else {
        return std::nullopt;             // suffix without cubed marker
    }
    // Trim again: tolerate "mm ³" by trimming after stripping.
    std::string_view linear_unit = trim_view(unit_str);

    auto scale_unit = linear_to_mm(linear_unit);
    auto scale_doc  = linear_to_mm(doc_units);
    if (!scale_unit || !scale_doc) return std::nullopt;

    // Convert from `unit` cubed to mm³, then to doc-unit cubed.
    // Guard against the multiplication / division overflowing to
    // infinity for absurd inputs like "1e200m³".
    const double mm3       = value * (*scale_unit) * (*scale_unit) * (*scale_unit);
    const double doc_cubed = mm3 / ((*scale_doc) * (*scale_doc) * (*scale_doc));
    if (!std::isfinite(doc_cubed)) return std::nullopt;
    return doc_cubed;
}

// ─── parse_double_strict ───────────────────────────────────────────────

std::optional<double> parse_double_strict(std::string_view s) {
    // Drop leading + trailing ASCII whitespace so attribute values
    // like ` 1.5 ` parse the same way as `1.5`. Neither std::from_chars
    // nor std::strtod accept leading whitespace at all; matching pure
    // CADML grammar would forbid it, but every other numeric site
    // already tolerates it — so do the same here for consistency.
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back()))  s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    // Strip a leading `+`. Both std::from_chars(double) and std::strtod
    // accept a leading `-` natively; std::from_chars rejects bare `+`
    // per [charconv], while std::strtod accepts it. SVG path data,
    // transform args, and CLI flags all legally write `+0.5` so we
    // normalise here so both backends see the same canonical form.
    if (s.front() == '+') {
        s.remove_prefix(1);
        if (s.empty()) return std::nullopt;
    }

    double value = 0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && \
    (!defined(_LIBCPP_VERSION) || _LIBCPP_VERSION >= 200000) && \
    (!defined(_GLIBCXX_RELEASE) || _GLIBCXX_RELEASE >= 11)
    // libc++ < LLVM 20 and libstdc++ < 11 advertise <charconv> but
    // omit the floating-point overloads (GCC 10.x's __GLIBCXX__
    // date-stamp doesn't reliably predict the FP-overload landing,
    // so we gate on _GLIBCXX_RELEASE instead). Use std::from_chars
    // only when the standard library actually ships them; otherwise
    // fall through to the C-locale strtod path below.
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    const auto res = std::from_chars(first, last, value);
    if (res.ec == std::errc::invalid_argument)    return std::nullopt;
    if (res.ec == std::errc::result_out_of_range) return std::nullopt;
    if (res.ptr != last) return std::nullopt;     // trailing junk
#else
    // Fallback: strtod pinned to the C locale via the platform's
    // _l() / locale_t API. Plain std::strtod is LC_NUMERIC-sensitive
    // and would silently truncate `60.0` to `60` on a host running a
    // comma-decimal locale (de_DE, fr_FR, …) — exactly the
    // determinism hazard we're guarding against.
    std::string buf(s);
    char* end = nullptr;
#  if defined(_WIN32)
    static _locale_t c_loc = _create_locale(LC_ALL, "C");
    value = _strtod_l(buf.c_str(), &end, c_loc);
#  else
    static locale_t c_loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    value = strtod_l(buf.c_str(), &end, c_loc);
#  endif
    if (end == buf.c_str())                       return std::nullopt; // invalid
    if (static_cast<std::size_t>(end - buf.c_str()) != buf.size())
        return std::nullopt;                                            // trailing junk
#endif
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

}  // namespace cadml
