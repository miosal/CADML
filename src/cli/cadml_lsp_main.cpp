// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>
#include <cadml/types.hpp>

#include "cli_panic.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

namespace rj = rapidjson;

struct SpecEntry {
    const char* name;
    const char* detail;
    const char* documentation;
    const char* snippet;
};

constexpr SpecEntry kElements[] = {
    {"part", "CADML part", "Top-level export (a deliverable part). A file may declare one or more.", "part name=\"$1\">\n  $0\n</part>"},
    {"def", "CADML definition", "File-private reusable geometry helper.", "def name=\"$1\">\n  $0\n</def>"},
    {"assembly", "CADML assembly", "Top-level composition of imported parts using instances and connections.", "assembly name=\"$1\">\n  $0\n</assembly>"},
    {"connect", "CADML connection", "Cross-cutting mate constraint inside an assembly.", "connect a=\"$1\" b=\"$2\"/>"},
    {"port", "CADML port", "Named connection point on a part.", "port name=\"$1\" position=\"$2\" normal=\"$3\"/>"},
    {"group", "CADML group", "Applies transform and presentation attributes to child geometry.", "group transform=\"$1\">\n  $0\n</group>"},
    {"script", "CADML Lua script", "Inline Lua helper code for parametric expressions.", "script lang=\"lua\">\n$0\n</script>"},
    {"for", "CADML loop", "Authoring-time iteration lowered by the compiler.", "for var=\"$1\" from=\"$2\" to=\"$3\" steps=\"$4\">\n  $0\n</for>"},
    {"svg", "CADML SVG wrapper", "Coordinate-frame wrapper for pasted SVG-like 2D content.", "svg>\n  $0\n</svg>"},
    {"circle", "2D circle", "Circle in the current sketch plane. Attribute r is required.", "circle r=\"$1\"/>"},
    {"rect", "2D rectangle", "Axis-aligned rectangle. Attributes width and height are required.", "rect width=\"$1\" height=\"$2\"/>"},
    {"path", "2D SVG path", "SVG path data used as a 2D profile.", "path d=\"$1\"/>"},
    {"sketch", "2D sketch plane", "Places child 2D primitives on a named plane or custom normal.", "sketch plane=\"$1\">\n  $0\n</sketch>"},
    {"extrude", "3D extrude", "Extrudes a 2D profile along a direction.", "extrude height=\"$1\">\n  $0\n</extrude>"},
    {"revolve", "3D revolve", "Revolves a 2D profile around an axis.", "revolve axis=\"$1\" angle=\"$2\">\n  $0\n</revolve>"},
    {"sweep", "3D sweep", "Sweeps a profile along a path.", "sweep>\n  $0\n</sweep>"},
    {"loft", "3D loft", "Lofts between ordered profiles.", "loft>\n  $0\n</loft>"},
    {"helix", "3D helix", "Creates a helical solid/path primitive.", "helix radius=\"$1\" pitch=\"$2\" turns=\"$3\"/>"},
    {"stl", "STL mesh import", "Imports a triangle mesh from an STL file (src=) or embedded base64 (data=). Requires version 0.2.", "stl src=\"$1\"/>"},
    {"union", "Boolean union", "Combines child solids.", "union>\n  $0\n</union>"},
    {"difference", "Boolean difference", "Subtracts later child solids from the first child solid.", "difference>\n  $0\n</difference>"},
    {"intersect", "Boolean intersection", "Keeps the shared volume of child solids.", "intersect>\n  $0\n</intersect>"},
    {"hull", "Convex hull", "Creates the convex hull around child geometry.", "hull>\n  $0\n</hull>"},
    {"fillet", "Modifier fillet", "Rounds selected edges.", "fillet radius=\"$1\"/>"},
    {"chamfer", "Modifier chamfer", "Bevels selected edges.", "chamfer distance=\"$1\"/>"},
    {"shell", "Modifier shell", "Hollows a solid by offsetting faces.", "shell thickness=\"$1\"/>"},
    {"cut", "Authoring cut", "Authoring-only tube/profile cut lowered by the compiler.", "cut face=\"$1\" angle=\"$2\"/>"},
    {"pattern", "Authoring pattern", "Authoring-only linear or circular repetition.", "pattern type=\"$1\" count=\"$2\">\n  $0\n</pattern>"},
};

constexpr SpecEntry kFrontmatter[] = {
    {"version", "CADML setting", "Spec version: 0.1 or 0.2 (patch forms like 0.2.0 accepted).", "version 0.2"},
    {"units", "CADML setting", "Document units: mm, cm, m, in, or ft.", "units mm"},
    {"description", "CADML setting", "Quoted human-readable document description.", "description \"$1\""},
    {"tags", "CADML setting", "Quoted space-separated searchable tags.", "tags \"$1\""},
    {"catalogue-version", "CADML setting", "Semantic version for catalogue parts.", "catalogue-version 0.1.0"},
    {"interference-tolerance", "CADML directive", "Volume threshold below which checks ignore overlaps.", "interference-tolerance 0mm3"},
    {"import", "CADML directive", "Imports a CADML or Lua file, optionally with an alias.", "import \"$1\""},
    {"param", "CADML parameter", "Declares a top-level param expression with optional min/max constraints.", "param $1 = $2"},
};

constexpr SpecEntry kAttributes[] = {
    {"name", "CADML attribute", "Names a part, assembly, def, port, or param.", "name=\"$1\""},
    {"color", "CADML attribute", "Presentation color as #RGB or #RRGGBB.", "color=\"$1\""},
    {"id", "CADML attribute", "Names an instance or group for references.", "id=\"$1\""},
    {"at", "CADML attribute", "Parent port used for mating an instance.", "at=\"$1\""},
    {"port", "CADML attribute", "Own port used for mating an instance.", "port=\"$1\""},
    {"transform", "CADML attribute", "SVG-like transform chain.", "transform=\"$1\""},
    {"lang", "CADML attribute", "Script language. CADML supports lua.", "lang=\"lua\""},
    {"var", "CADML attribute", "Loop variable name for a for element.", "var=\"$1\""},
    {"from", "CADML attribute", "Start expression for a uniform for loop.", "from=\"$1\""},
    {"to", "CADML attribute", "End expression for a uniform for loop.", "to=\"$1\""},
    {"steps", "CADML attribute", "Step count expression for a uniform for loop.", "steps=\"$1\""},
    {"values", "CADML attribute", "Space-separated explicit values for a for loop.", "values=\"$1\""},
    {"cx", "CADML attribute", "Circle center X expression.", "cx=\"$1\""},
    {"cy", "CADML attribute", "Circle center Y expression.", "cy=\"$1\""},
    {"r", "CADML attribute", "Circle radius expression.", "r=\"$1\""},
    {"segments", "CADML attribute", "Optional circle tessellation segment count.", "segments=\"$1\""},
    {"x", "CADML attribute", "Rectangle X expression.", "x=\"$1\""},
    {"y", "CADML attribute", "Rectangle Y expression.", "y=\"$1\""},
    {"width", "CADML attribute", "Rectangle width expression.", "width=\"$1\""},
    {"height", "CADML attribute", "Extrude height or rectangle height expression.", "height=\"$1\""},
    {"rx", "CADML attribute", "Rectangle corner radius X expression.", "rx=\"$1\""},
    {"ry", "CADML attribute", "Rectangle corner radius Y expression.", "ry=\"$1\""},
    {"d", "CADML attribute", "SVG path data.", "d=\"$1\""},
    {"plane", "CADML attribute", "Sketch plane: xy, xz, or yz.", "plane=\"$1\""},
    {"origin", "CADML attribute", "Sketch origin point expression.", "origin=\"$1\""},
    {"rotation", "CADML attribute", "Sketch rotation expression.", "rotation=\"$1\""},
    {"normal", "CADML attribute", "Port or sketch normal vector expression.", "normal=\"$1\""},
    {"up", "CADML attribute", "Port up vector expression.", "up=\"$1\""},
    // <extrude scale|draft|direction> are reserved attribute names
    // that the 0.1 bundler rejects with a schema error (see
    // docs/spec/language.md §5.3 — Reserved attributes). Don't surface
    // them as completions; the user would accept the completion and
    // then hit a compile failure with no editor-side hint.
    {"symmetric", "CADML attribute", "Whether an extrude is centered around its profile.", "symmetric=\"true\""},
    {"axis", "CADML attribute", "Axis alias such as x, y, z, +x, or -z.", "axis=\"$1\""},
    {"angle", "CADML attribute", "Angle expression in degrees.", "angle=\"$1\""},
    {"radius", "CADML attribute", "Radius expression.", "radius=\"$1\""},
    {"pitch", "CADML attribute", "Helix pitch expression.", "pitch=\"$1\""},
    {"turns", "CADML attribute", "Helix turns expression.", "turns=\"$1\""},
    {"taper", "CADML attribute", "Helix taper expression.", "taper=\"$1\""},
    {"select", "CADML attribute", "Edge/face selector expression.", "select=\"$1\""},
    {"distance", "CADML attribute", "Chamfer distance expression.", "distance=\"$1\""},
    {"thickness", "CADML attribute", "Shell thickness expression.", "thickness=\"$1\""},
    {"open", "CADML attribute", "Shell open-face selector.", "open=\"$1\""},
    {"face", "CADML attribute", "Cut face selector such as start or end.", "face=\"$1\""},
    {"type", "CADML attribute", "Pattern/cut type.", "type=\"$1\""},
    {"count", "CADML attribute", "Pattern count expression.", "count=\"$1\""},
    {"spacing", "CADML attribute", "Linear pattern spacing expression.", "spacing=\"$1\""},
    {"allow-interference", "CADML attribute", "Allows intentional overlap for a connect mate.", "allow-interference=\"true\""},
};

struct TextDocument {
    std::string text;
};

class Server {
public:
    int run() {
        set_binary_stdio();

        std::string payload;
        while (read_message(payload)) {
            handle(payload);
            if (exit_requested_) {
                break;
            }
        }
        // Per LSP §3.16.2: exit code 0 only when the server received
        // `shutdown` before `exit`; otherwise exit code 1 to signal
        // an abnormal termination (the editor restarts on non-zero
        // exit). If stdin closes without an `exit` request at all,
        // also report abnormal.
        if (!exit_requested_)     return 1;     // stream closed unexpectedly
        if (!shutdown_requested_) return 1;     // exit without prior shutdown
        return 0;
    }

private:
    std::unordered_map<std::string, TextDocument> documents_;
    bool shutdown_requested_ = false;
    bool exit_requested_ = false;

    static void set_binary_stdio() {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    }

    static bool read_message(std::string& payload) {
        payload.clear();

        std::string header;
        char ch = 0;
        while (std::cin.get(ch)) {
            header.push_back(ch);
            const auto n = header.size();
            if (n >= 4 && header.compare(n - 4, 4, "\r\n\r\n") == 0) {
                break;
            }
        }
        if (header.empty()) {
            return false;
        }
        if (header.size() < 4 || header.substr(header.size() - 4) != "\r\n\r\n") {
            return false;
        }

        constexpr std::string_view key = "Content-Length:";
        std::size_t content_length = 0;
        std::size_t start = 0;
        while (start < header.size()) {
            const auto end = header.find("\r\n", start);
            if (end == std::string::npos || end == start) {
                break;
            }
            const auto line = std::string_view(header).substr(start, end - start);
            if (line.size() >= key.size()
                && std::equal(key.begin(), key.end(), line.begin(),
                              [](char a, char b) {
                                  return std::tolower(static_cast<unsigned char>(a))
                                      == std::tolower(static_cast<unsigned char>(b));
                              })) {
                auto value = line.substr(key.size());
                while (!value.empty() && value.front() == ' ') {
                    value.remove_prefix(1);
                }
                content_length = static_cast<std::size_t>(std::strtoull(
                    std::string(value).c_str(), nullptr, 10));
                break;
            }
            start = end + 2;
        }

        if (content_length == 0) {
            return false;
        }
        payload.resize(content_length);
        std::cin.read(payload.data(), static_cast<std::streamsize>(content_length));
        return static_cast<std::size_t>(std::cin.gcount()) == content_length;
    }

    static void write_json(const rj::Value& value) {
        rj::StringBuffer sb;
        rj::Writer<rj::StringBuffer> writer(sb);
        value.Accept(writer);
        std::cout << "Content-Length: " << sb.GetSize() << "\r\n\r\n";
        std::cout.write(sb.GetString(), static_cast<std::streamsize>(sb.GetSize()));
        std::cout.flush();
    }

    static bool has_id(const rj::Document& msg) {
        return msg.HasMember("id")
            && (msg["id"].IsString() || msg["id"].IsInt() || msg["id"].IsUint()
                || msg["id"].IsNull());
    }

    static rj::Value copy_id(const rj::Document& msg, rj::Document::AllocatorType& alloc) {
        rj::Value id;
        if (msg.HasMember("id")) {
            id.CopyFrom(msg["id"], alloc);
        } else {
            id.SetNull();
        }
        return id;
    }

    static void add_string(rj::Value& obj, const char* key, std::string_view value,
                           rj::Document::AllocatorType& alloc) {
        obj.AddMember(rj::Value(key, alloc).Move(),
                      rj::Value(value.data(), static_cast<rj::SizeType>(value.size()), alloc).Move(),
                      alloc);
    }

    void send_response(const rj::Document& request, const rj::Value& result_value) {
        rj::Document out;
        out.SetObject();
        auto& alloc = out.GetAllocator();
        out.AddMember("jsonrpc", "2.0", alloc);
        out.AddMember("id", copy_id(request, alloc), alloc);
        rj::Value result;
        result.CopyFrom(result_value, alloc);
        out.AddMember("result", result, alloc);
        write_json(out);
    }

    void send_error(const rj::Document& request, int code, const char* message) {
        rj::Document out;
        out.SetObject();
        auto& alloc = out.GetAllocator();
        out.AddMember("jsonrpc", "2.0", alloc);
        out.AddMember("id", copy_id(request, alloc), alloc);
        rj::Value error(rj::kObjectType);
        error.AddMember("code", code, alloc);
        error.AddMember("message", rj::Value(message, alloc).Move(), alloc);
        out.AddMember("error", error, alloc);
        write_json(out);
    }

    void send_notification(const char* method, const rj::Value& params_value) {
        rj::Document out;
        out.SetObject();
        auto& alloc = out.GetAllocator();
        out.AddMember("jsonrpc", "2.0", alloc);
        out.AddMember("method", rj::Value(method, alloc).Move(), alloc);
        rj::Value params;
        params.CopyFrom(params_value, alloc);
        out.AddMember("params", params, alloc);
        write_json(out);
    }

    void handle(std::string_view payload) {
        rj::Document msg;
        msg.Parse(payload.data(), payload.size());
        if (msg.HasParseError() || !msg.IsObject() || !msg.HasMember("method")
            || !msg["method"].IsString()) {
            return;
        }

        const std::string method = msg["method"].GetString();

        // Per LSP spec: after a `shutdown` request, every method
        // except `exit` must return an InvalidRequest (-32600) error.
        // `exit` is the only follow-up the server may honour.
        // JSON-RPC §4.1 also forbids replying to notifications
        // (messages without an `id` field), so the error reply is
        // gated on the message being a request.
        if (shutdown_requested_ && method != "exit") {
            if (msg.HasMember("id")) {
                send_error(msg, -32600,
                    "invalid request: server is shut down — only `exit` "
                    "may follow a `shutdown` request");
            }
            return;
        }

        if (method == "initialize") {
            handle_initialize(msg);
        } else if (method == "shutdown") {
            shutdown_requested_ = true;
            rj::Document result;
            result.SetNull();
            send_response(msg, result);
        } else if (method == "exit") {
            exit_requested_ = true;
        } else if (method == "textDocument/didOpen") {
            handle_did_open(msg);
        } else if (method == "textDocument/didChange") {
            handle_did_change(msg);
        } else if (method == "textDocument/didSave") {
            handle_did_save(msg);
        } else if (method == "textDocument/completion") {
            handle_completion(msg);
        } else if (method == "textDocument/hover") {
            handle_hover(msg);
        } else if (has_id(msg)) {
            send_error(msg, -32601, "Method not found");
        }
    }

    void handle_initialize(const rj::Document& request) {
        rj::Document result;
        result.SetObject();
        auto& alloc = result.GetAllocator();

        rj::Value server_info(rj::kObjectType);
        server_info.AddMember("name", "cadmllsp", alloc);
        server_info.AddMember("version", "0.1.0", alloc);
        result.AddMember("serverInfo", server_info, alloc);

        rj::Value capabilities(rj::kObjectType);
        capabilities.AddMember("textDocumentSync", 1, alloc);
        capabilities.AddMember("hoverProvider", true, alloc);

        rj::Value completion(rj::kObjectType);
        rj::Value triggers(rj::kArrayType);
        triggers.PushBack("<", alloc).PushBack(" ", alloc).PushBack("\"", alloc);
        completion.AddMember("triggerCharacters", triggers, alloc);
        completion.AddMember("resolveProvider", false, alloc);
        capabilities.AddMember("completionProvider", completion, alloc);

        result.AddMember("capabilities", capabilities, alloc);
        send_response(request, result);
    }

    void handle_did_open(const rj::Document& msg) {
        const auto* td = text_document_param(msg, "textDocument");
        if (td == nullptr || !td->HasMember("uri") || !(*td)["uri"].IsString()
            || !td->HasMember("text") || !(*td)["text"].IsString()) {
            return;
        }
        const std::string uri = (*td)["uri"].GetString();
        documents_[uri].text = (*td)["text"].GetString();
        publish_diagnostics(uri);
    }

    void handle_did_change(const rj::Document& msg) {
        const auto* td = text_document_param(msg, "textDocument");
        if (td == nullptr || !td->HasMember("uri") || !(*td)["uri"].IsString()) {
            return;
        }
        if (!msg.HasMember("params") || !msg["params"].IsObject()
            || !msg["params"].HasMember("contentChanges")
            || !msg["params"]["contentChanges"].IsArray()) {
            return;
        }
        const auto& changes = msg["params"]["contentChanges"];
        if (changes.Empty()) {
            return;
        }
        const auto& last = changes[changes.Size() - 1];
        if (!last.IsObject() || !last.HasMember("text") || !last["text"].IsString()) {
            return;
        }
        const std::string uri = (*td)["uri"].GetString();
        documents_[uri].text = last["text"].GetString();
        publish_diagnostics(uri);
    }

    void handle_did_save(const rj::Document& msg) {
        const auto* td = text_document_param(msg, "textDocument");
        if (td == nullptr || !td->HasMember("uri") || !(*td)["uri"].IsString()) {
            return;
        }
        const std::string uri = (*td)["uri"].GetString();
        if (msg.HasMember("params") && msg["params"].IsObject()
            && msg["params"].HasMember("text") && msg["params"]["text"].IsString()) {
            documents_[uri].text = msg["params"]["text"].GetString();
        }
        publish_diagnostics(uri);
    }

    static const rj::Value* text_document_param(const rj::Document& msg, const char* name) {
        if (!msg.HasMember("params") || !msg["params"].IsObject()
            || !msg["params"].HasMember(name) || !msg["params"][name].IsObject()) {
            return nullptr;
        }
        return &msg["params"][name];
    }

    void publish_diagnostics(const std::string& uri) {
        const auto it = documents_.find(uri);
        if (it == documents_.end()) {
            return;
        }

        const auto parsed = cadml::parse(it->second.text, 0);

        rj::Document params;
        params.SetObject();
        auto& alloc = params.GetAllocator();
        params.AddMember("uri", rj::Value(uri.c_str(), alloc).Move(), alloc);

        rj::Value diagnostics(rj::kArrayType);
        for (const auto& error : parsed.errors) {
            diagnostics.PushBack(make_diagnostic(error, 1, alloc), alloc);
        }
        for (const auto& warning : parsed.warnings) {
            diagnostics.PushBack(make_diagnostic(warning, 2, alloc), alloc);
        }

        params.AddMember("diagnostics", diagnostics, alloc);
        send_notification("textDocument/publishDiagnostics", params);
    }

    static rj::Value make_diagnostic(const cadml::ParseError& error, int severity,
                                     rj::Document::AllocatorType& alloc) {
        rj::Value diagnostic(rj::kObjectType);
        rj::Value range(rj::kObjectType);
        const auto line = error.source.line > 0 ? error.source.line - 1 : 0;
        const auto col = error.source.column > 0 ? error.source.column - 1 : 0;
        const auto len = error.source.length > 0 ? error.source.length : 1;

        rj::Value start(rj::kObjectType);
        start.AddMember("line", line, alloc);
        start.AddMember("character", col, alloc);
        rj::Value end(rj::kObjectType);
        end.AddMember("line", line, alloc);
        end.AddMember("character", col + len, alloc);
        range.AddMember("start", start, alloc);
        range.AddMember("end", end, alloc);

        diagnostic.AddMember("range", range, alloc);
        diagnostic.AddMember("severity", severity, alloc);
        diagnostic.AddMember("source", "cadml", alloc);
        add_string(diagnostic, "message",
                   std::string(category_name(error.category)) + ": " + error.message,
                   alloc);
        return diagnostic;
    }

    static const char* category_name(cadml::ParseError::Category category) {
        switch (category) {
            case cadml::ParseError::Parse: return "parse";
            case cadml::ParseError::Schema: return "schema";
            case cadml::ParseError::Vocabulary: return "vocabulary";
            case cadml::ParseError::Expression: return "expression";
            case cadml::ParseError::Validation: return "validation";
        }
        return "cadml";
    }

    void handle_completion(const rj::Document& msg) {
        rj::Document result;
        result.SetArray();
        auto& alloc = result.GetAllocator();

        CompletionContext ctx = completion_context(msg);
        if (ctx.in_opening_tag) {
            for (const auto& entry : kElements) {
                result.PushBack(completion_item(entry, 7, alloc), alloc);
            }
        } else if (ctx.in_tag) {
            for (const auto& entry : kAttributes) {
                result.PushBack(completion_item(entry, 10, alloc), alloc);
            }
        } else {
            for (const auto& entry : kFrontmatter) {
                result.PushBack(completion_item(entry, 14, alloc), alloc);
            }
            for (const auto& entry : kElements) {
                result.PushBack(completion_item(entry, 7, alloc), alloc);
            }
        }

        send_response(msg, result);
    }

    struct CompletionContext {
        bool in_opening_tag = false;
        bool in_tag = false;
    };

    CompletionContext completion_context(const rj::Document& msg) const {
        CompletionContext ctx;
        const auto [uri, line, character] = uri_position(msg);
        const auto it = documents_.find(uri);
        if (it == documents_.end()) {
            return ctx;
        }
        const auto offset = offset_at(it->second.text, line, character);
        const auto before = it->second.text.substr(0, offset);
        const auto lt = before.rfind('<');
        const auto gt = before.rfind('>');
        if (lt != std::string::npos && (gt == std::string::npos || lt > gt)) {
            ctx.in_tag = true;
            const auto tag_head = before.substr(lt + 1);
            ctx.in_opening_tag = tag_head.find_first_of(" \t\r\n/") == std::string::npos;
        }
        return ctx;
    }

    static rj::Value completion_item(const SpecEntry& entry, int kind,
                                     rj::Document::AllocatorType& alloc) {
        rj::Value item(rj::kObjectType);
        item.AddMember("label", rj::Value(entry.name, alloc).Move(), alloc);
        item.AddMember("kind", kind, alloc);
        item.AddMember("detail", rj::Value(entry.detail, alloc).Move(), alloc);
        item.AddMember("documentation", rj::Value(entry.documentation, alloc).Move(), alloc);
        item.AddMember("insertText", rj::Value(entry.snippet, alloc).Move(), alloc);
        item.AddMember("insertTextFormat", 2, alloc);
        return item;
    }

    void handle_hover(const rj::Document& msg) {
        const auto [uri, line, character] = uri_position(msg);
        const auto it = documents_.find(uri);
        const SpecEntry* entry = nullptr;
        if (it != documents_.end()) {
            const auto word = word_at(it->second.text, line, character);
            entry = find_entry(word);
        }

        rj::Document result;
        auto& alloc = result.GetAllocator();
        if (entry == nullptr) {
            result.SetNull();
        } else {
            result.SetObject();
            rj::Value contents(rj::kObjectType);
            contents.AddMember("kind", "markdown", alloc);
            const std::string markdown = std::string("**") + entry->name + "**\n\n"
                + entry->documentation + "\n\n`" + entry->snippet + "`";
            contents.AddMember("value", rj::Value(markdown.c_str(), alloc).Move(), alloc);
            result.AddMember("contents", contents, alloc);
        }
        send_response(msg, result);
    }

    static const SpecEntry* find_entry(std::string_view name) {
        for (const auto& entry : kElements) {
            if (name == entry.name) {
                return &entry;
            }
        }
        for (const auto& entry : kFrontmatter) {
            if (name == entry.name) {
                return &entry;
            }
        }
        for (const auto& entry : kAttributes) {
            if (name == entry.name) {
                return &entry;
            }
        }
        return nullptr;
    }

    static std::tuple<std::string, std::uint32_t, std::uint32_t>
    uri_position(const rj::Document& msg) {
        std::string uri;
        std::uint32_t line = 0;
        std::uint32_t character = 0;
        const auto* td = text_document_param(msg, "textDocument");
        if (td != nullptr && td->HasMember("uri") && (*td)["uri"].IsString()) {
            uri = (*td)["uri"].GetString();
        }
        if (msg.HasMember("params") && msg["params"].IsObject()
            && msg["params"].HasMember("position") && msg["params"]["position"].IsObject()) {
            const auto& pos = msg["params"]["position"];
            if (pos.HasMember("line") && pos["line"].IsUint()) {
                line = pos["line"].GetUint();
            }
            if (pos.HasMember("character") && pos["character"].IsUint()) {
                character = pos["character"].GetUint();
            }
        }
        return {uri, line, character};
    }

    static std::size_t offset_at(const std::string& text, std::uint32_t line,
                                 std::uint32_t character) {
        std::size_t offset = 0;
        std::uint32_t current_line = 0;
        while (offset < text.size() && current_line < line) {
            if (text[offset] == '\n') {
                ++current_line;
            }
            ++offset;
        }
        return std::min(text.size(), offset + character);
    }

    static bool is_word_char(char c) {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '-' || c == '_';
    }

    static std::string_view word_at(const std::string& text, std::uint32_t line,
                                    std::uint32_t character) {
        if (text.empty()) {
            return {};
        }
        auto offset = offset_at(text, line, character);
        if (offset == text.size() && offset > 0) {
            --offset;
        }
        if (offset < text.size() && !is_word_char(text[offset]) && offset > 0
            && is_word_char(text[offset - 1])) {
            --offset;
        }
        if (offset >= text.size() || !is_word_char(text[offset])) {
            return {};
        }
        auto start = offset;
        while (start > 0 && is_word_char(text[start - 1])) {
            --start;
        }
        auto end = offset;
        while (end < text.size() && is_word_char(text[end])) {
            ++end;
        }
        return std::string_view(text).substr(start, end - start);
    }
};

}  // namespace

int main() {
    ::cadml::cli::install_panic_handler();
    return Server().run();
}
