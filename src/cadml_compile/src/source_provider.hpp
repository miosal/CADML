// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/compile/bundler.hpp>   // InMemoryFile
#include <cadml/constants.hpp>          // kMaxSourceBytes

#include <concepts>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace cadml::compile {

// Result type returned by every SourceProvider::read.
//   - `contents` is set when the file was found and is within the
//     bundler's size limit (`cadml::kMaxSourceBytes`, 64 MiB).
//   - `too_large` is true when the file exists but exceeds the cap,
//     so the caller can surface a distinct "file too large" Import
//     error instead of the generic "cannot find imported file".
struct ReadResult {
    std::optional<std::string> contents;
    bool                        too_large = false;
};

// Normalize a relative path into a canonical lookup key: lexically
// collapse `.`/`..`, force forward slashes, drop a leading `./`. Pure
// string math — never touches the filesystem. A key that escapes the
// root keeps its leading `../` so the resolver can reject it.
inline std::string normalize_key(std::string_view p) {
    std::filesystem::path fp{ std::string(p) };
    std::string n = fp.lexically_normal().generic_string();
    if (n.rfind("./", 0) == 0) n.erase(0, 2);
    if (n == ".") n.clear();
    // lexically_normal can leave a trailing "/" on dir-like inputs; the
    // resolver only ever passes file paths, but be defensive.
    if (n.size() > 1 && n.back() == '/') n.pop_back();
    return n;
}

// The provider contract. A type models SourceProvider if it offers
//   ReadResult read(std::string_view) const
// returning the file's bytes (or a too_large=true marker / blank
// result if the key isn't available).
template <class P>
concept SourceProvider = requires(const P& p, std::string_view key) {
    { p.read(key) } -> std::same_as<ReadResult>;
};

// ─── FilesystemProvider (native; backs compile_file/compile_string) ───
class FilesystemProvider {
public:
    explicit FilesystemProvider(std::filesystem::path root)
        : root_(canonicalize(std::move(root))) {}

    ReadResult read(std::string_view key) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path candidate = fs::weakly_canonical(root_ / std::string(key), ec);
        if (ec) candidate = (root_ / std::string(key)).lexically_normal();

        // SECURITY: refuse anything that resolves outside the root —
        // defence in depth against `../` escapes and symlink redirection
        // (the resolver also rejects `../`-escaping keys lexically, but
        // weakly_canonical here additionally collapses real symlinks).
        if (!inside(candidate)) return {};
        if (!fs::is_regular_file(candidate, ec) || ec) return {};

        // Cap the read at kMaxSourceBytes BEFORE allocating the buffer
        // so a 4 GiB hostile file can't OOM the process between size
        // check and parse.
        const auto sz = fs::file_size(candidate, ec);
        if (ec) return {};
        if (sz > cadml::kMaxSourceBytes) {
            ReadResult r;
            r.too_large = true;
            return r;
        }

        std::ifstream f(candidate, std::ios::binary);
        if (!f) return {};
        std::string out(sz, '\0');
        f.read(out.data(), static_cast<std::streamsize>(sz));
        // gcount() should equal sz on success; treat short reads as
        // failure rather than truncating silently.
        if (f.gcount() != static_cast<std::streamsize>(sz)) return {};
        ReadResult r;
        r.contents = std::move(out);
        return r;
    }

private:
    std::filesystem::path root_;

    static std::filesystem::path canonicalize(std::filesystem::path p) {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : c;
    }

    bool inside(const std::filesystem::path& candidate) const {
        const auto c = candidate.lexically_normal().generic_string();
        const auto r = root_.lexically_normal().generic_string();
        if (r.empty()) return true;
        if (c.size() < r.size()) return false;
        if (c.compare(0, r.size(), r) != 0) return false;
        if (c.size() == r.size()) return true;
        const char nx = c[r.size()];
        return nx == '/' || nx == '\\';
    }
};
static_assert(SourceProvider<FilesystemProvider>);

// ─── InMemoryProvider (backs compile_in_memory; WASM/sandbox entry) ───
class InMemoryProvider {
public:
    explicit InMemoryProvider(const std::vector<InMemoryFile>& files) {
        files_.reserve(files.size());
        for (const auto& f : files) {
            // Last write wins on duplicate keys — deterministic given the
            // caller's vector order.
            files_[normalize_key(f.path)] = f.contents;
        }
    }

    ReadResult read(std::string_view key) const {
        const auto it = files_.find(normalize_key(key));
        if (it == files_.end()) return {};
        if (it->second.size() > cadml::kMaxSourceBytes) {
            ReadResult r;
            r.too_large = true;
            return r;
        }
        ReadResult r;
        r.contents = it->second;
        return r;
    }

private:
    std::unordered_map<std::string, std::string> files_;
};
static_assert(SourceProvider<InMemoryProvider>);

}  // namespace cadml::compile
