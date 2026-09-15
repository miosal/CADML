// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org


#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/engine/flat_stl.hpp>
#include <cadml/engine/flat_3mf.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

using namespace emscripten;

namespace {

// Plain result surfaced to JS as { ok, fcadml, errors, warnings }.
struct CompileOut {
    bool        ok = false;
    std::string fcadml;
    std::string errors;     // newline-joined
    std::string warnings;   // newline-joined
};

std::string join(const std::vector<cadml::compile::CompileError>& v) {
    std::string s;
    for (const auto& e : v) {
        if (!s.empty()) s += '\n';
        s += e.message;
    }
    return s;
}

CompileOut to_out(const cadml::compile::CompileResult& r) {
    CompileOut o;
    o.ok       = r.ok();
    o.fcadml   = r.ok() ? r.flat_text : std::string{};
    o.errors   = join(r.errors);
    o.warnings = join(r.warnings);
    return o;
}

// Copy raw bytes into a fresh JS Uint8Array (the `new Uint8Array(view)`
// copy happens before the local buffer is destroyed, so the returned
// array is safe — a typed_memory_view alone would dangle).
val to_uint8array(const std::string& bytes) {
    const val view = val(typed_memory_view(
        bytes.size(), reinterpret_cast<const std::uint8_t*>(bytes.data())));
    return val::global("Uint8Array").new_(view);
}

// ── Single-file API ───────────────────────────────────────────────────

CompileOut compileSource(const std::string& source) {
    return to_out(cadml::compile::compile_string(source));
}

val exportStlFromSource(const std::string& source) {
    auto cr = cadml::compile::compile_string(source);
    if (!cr.ok()) return val::null();
    auto er = cadml::engine::evaluate_flat(cr.document);
    if (!er.ok() || er.parts.empty()) return val::null();
    std::ostringstream os;
    cadml::engine::write_stl_binary(er, os);
    return to_uint8array(os.str());
}

val export3mfFromSource(const std::string& source) {
    auto cr = cadml::compile::compile_string(source);
    if (!cr.ok()) return val::null();
    auto er = cadml::engine::evaluate_flat(cr.document);
    if (!er.ok() || er.parts.empty()) return val::null();
    std::ostringstream os;
    cadml::engine::ThreeMfOptions opts;
    opts.units = cr.document.meta.units.empty() ? "mm" : cr.document.meta.units;
    cadml::engine::write_3mf(er, os, opts);
    return to_uint8array(os.str());
}

// ── Multi-file API (the InMemoryProvider path) ────────────────────────
//
// `jsFiles` is a JS array of { path, contents }. `entry` selects the
// entry within it. Imports resolve by lookup in the array — exactly the
// path a real WASM host (browser editor, playground) uses.

// A file's `contents` is a JS string (source text) or a Uint8Array
// (binary assets such as the PNG/JPEG a `<part texture="…">` names —
// spec 0.3). Bytes are copied into a std::string, which is what the
// in-memory provider hands the bundler for `texture-data` inlining.
std::string marshal_contents(const val& contents) {
    if (contents.isString()) return contents.as<std::string>();
    if (contents.instanceof(val::global("Uint8Array"))) {
        const std::size_t n = contents["length"].as<std::size_t>();
        std::string bytes(n, '\0');
        if (n > 0) {
            val view = val(typed_memory_view(
                n, reinterpret_cast<std::uint8_t*>(bytes.data())));
            view.call<void>("set", contents);
        }
        return bytes;
    }
    // Anything else (undefined, null, a number) is a caller bug; an
    // empty file makes the bundler report it rather than trapping here.
    return {};
}

std::vector<cadml::compile::InMemoryFile> marshal_files(const val& jsFiles) {
    std::vector<cadml::compile::InMemoryFile> files;
    const unsigned n = jsFiles["length"].as<unsigned>();
    files.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        const val f = jsFiles[i];
        files.push_back({ f["path"].as<std::string>(),
                          marshal_contents(f["contents"]) });
    }
    return files;
}

CompileOut compileProject(const val& jsFiles, const std::string& entry) {
    return to_out(cadml::compile::compile_in_memory(marshal_files(jsFiles), entry));
}

val exportStlFromProject(const val& jsFiles, const std::string& entry) {
    auto cr = cadml::compile::compile_in_memory(marshal_files(jsFiles), entry);
    if (!cr.ok()) return val::null();
    auto er = cadml::engine::evaluate_flat(cr.document);
    if (!er.ok() || er.parts.empty()) return val::null();
    std::ostringstream os;
    cadml::engine::write_stl_binary(er, os);
    return to_uint8array(os.str());
}

// ── Scene API ─────────────────────────────────────────────────────────
//
// One compile + one evaluation, surfaced per part so a renderer can
// give each top-level <part> its own colour and (spec 0.3) texture:
//
//   { ok, errors, warnings,
//     parts: [ { name, color, stl: Uint8Array,
//                texture: null | { mime, bytes: Uint8Array, scale } } ] }
//
// `stl` is that part alone, as a binary STL. `texture.bytes` is the
// image file as-is (PNG or JPEG); `scale` is the resolved world-space
// size of one tile — CADML meshes carry no UVs, so the consumer maps
// the image by projection (triplanar in the reference viewers). On a
// compile or evaluation failure `ok` is false, `errors` says why and
// `parts` is empty; warnings are reported either way.

std::string join_eval(const std::vector<cadml::engine::FlatEvalError>& v) {
    std::string s;
    for (const auto& e : v) {
        if (!s.empty()) s += '\n';
        s += e.message;
    }
    return s;
}

val scene_of(const cadml::compile::CompileResult& cr) {
    val out = val::object();
    val parts = val::array();
    std::string warnings = join(cr.warnings);
    if (!cr.ok()) {
        out.set("ok", false);
        out.set("errors", join(cr.errors));
        out.set("warnings", warnings);
        out.set("parts", parts);
        return out;
    }
    auto er = cadml::engine::evaluate_flat(cr.document);
    {
        const std::string w = join_eval(er.warnings);
        if (!w.empty()) warnings += (warnings.empty() ? "" : "\n") + w;
    }
    out.set("ok", er.ok());
    out.set("errors", join_eval(er.errors));
    out.set("warnings", warnings);
    if (er.ok()) {
        for (auto& part : er.parts) {
            val jp = val::object();
            jp.set("name", part.name);
            jp.set("color", part.color);
            if (part.texture) {
                val jt = val::object();
                jt.set("mime",  part.texture->mime);
                jt.set("bytes", to_uint8array(part.texture->bytes));
                jt.set("scale", part.texture->scale);
                jp.set("texture", jt);
            } else {
                jp.set("texture", val::null());
            }
            // write_stl_binary takes a whole result; give it this part
            // alone (moved, not copied — `er` is not used afterwards).
            cadml::engine::FlatEvalResult single;
            single.parts.push_back(std::move(part));
            std::ostringstream os;
            cadml::engine::write_stl_binary(single, os);
            jp.set("stl", to_uint8array(os.str()));
            parts.call<void>("push", jp);
        }
    }
    out.set("parts", parts);
    return out;
}

val sceneFromSource(const std::string& source) {
    return scene_of(cadml::compile::compile_string(source));
}

val sceneFromProject(const val& jsFiles, const std::string& entry) {
    return scene_of(cadml::compile::compile_in_memory(marshal_files(jsFiles), entry));
}

}  // namespace

EMSCRIPTEN_BINDINGS(cadml) {
    value_object<CompileOut>("CompileOut")
        .field("ok",       &CompileOut::ok)
        .field("fcadml",   &CompileOut::fcadml)
        .field("errors",   &CompileOut::errors)
        .field("warnings", &CompileOut::warnings);

    function("compileSource",        &compileSource);
    function("exportStlFromSource",  &exportStlFromSource);
    function("export3mfFromSource",  &export3mfFromSource);
    function("compileProject",       &compileProject);
    function("exportStlFromProject", &exportStlFromProject);
    function("sceneFromSource",      &sceneFromSource);
    function("sceneFromProject",     &sceneFromProject);
}
