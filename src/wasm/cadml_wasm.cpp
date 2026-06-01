// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org


#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/engine/flat_stl.hpp>
#include <cadml/engine/flat_3mf.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
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

std::vector<cadml::compile::InMemoryFile> marshal_files(const val& jsFiles) {
    std::vector<cadml::compile::InMemoryFile> files;
    const unsigned n = jsFiles["length"].as<unsigned>();
    files.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        const val f = jsFiles[i];
        files.push_back({ f["path"].as<std::string>(),
                          f["contents"].as<std::string>() });
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
}
