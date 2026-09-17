#pragma once
// CodeIntel: Tree-sitter based symbol extraction
//
// Extracts functions, classes, methods from source files.
// Supports: C/C++, Python, JavaScript/TypeScript, Go, Rust, Java, Ruby, C#, Swift,
// and heading-scoped Markdown documents.

#include <tree_sitter/api.h>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

// External tree-sitter language functions (from tree-sitter-parsers library)
extern "C" {
    TSLanguage* tree_sitter_cpp();
    TSLanguage* tree_sitter_python();
    TSLanguage* tree_sitter_javascript();
    TSLanguage* tree_sitter_typescript();
    TSLanguage* tree_sitter_go();
    TSLanguage* tree_sitter_rust();
    TSLanguage* tree_sitter_java();
    TSLanguage* tree_sitter_ruby();
    TSLanguage* tree_sitter_c_sharp();
    TSLanguage* tree_sitter_swift();
    TSLanguage* tree_sitter_lua();
}

namespace chitta {

// Derived source knowledge. The sidecar contains registered roots only; chunks
// are rebuilt from the filesystem and hashes are checked on every source query.
// It deliberately shares recall's result rows rather than adding an RPC/lane.
class RepositoryIndex {
public:
    static bool repository_question(const std::string& query);
    static std::string content_hash(const std::string& bytes);
    static std::string file_hash(const std::string& path);
    static std::string repository_root(const std::string& path);
    static nlohmann::json source_anchor(const std::string& content, const nlohmann::json& supplied);
    void open(const std::string& sidecar, const std::string& known_files);
    void index(const std::string& path, const std::string& realm);
    nlohmann::json search(const std::string& query, const std::string& realm, size_t limit);
private:
    struct Chunk {
        std::string identity, name, text, kind;
        size_t line = 1;
        std::unordered_map<std::string, size_t> terms;
        size_t token_count = 0;
    };
    struct File {
        std::string root, realm, path, hash;
        std::vector<Chunk> chunks;
    };
    std::mutex mutex_;
    std::string sidecar_;
    std::unordered_map<std::string, std::string> roots_; // realm -> active checkout
    // Last full walk per realm: a walk reads and hashes every source file, so it
    // runs at most every CHITTA_SOURCE_REFRESH_S seconds (default 30), not per recall.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_refresh_;
    std::unordered_map<std::string, File> files_;
    void refresh(const std::string& realm);
};

// Extracted symbol with location info
struct ExtractedSymbol {
    std::string kind;       // function, class, method, variable
    std::string name;
    std::string signature;  // For functions: return type + params
    std::string file_path;
    int32_t line_start = 0;
    int32_t line_end = 0;
    std::string parent;     // Parent class/module name (if method)
};

// ═══════════════════════════════════════════════════════════════════════════
// Callsite Extraction (Phase 1: Code Intelligence)
// ═══════════════════════════════════════════════════════════════════════════

enum class CallKind {
    Call,        // foo(...)
    MemberCall,  // obj.method(...), ptr->method(...)
    Qualified,   // ns::foo(...), Class::staticMethod(...)
    New,         // new Foo(...)
    Ctor,        // Foo x(...), Foo{...}, Foo(...) temporaries
    Indirect,    // (*fp)(...), fp(...), unknown callee
    LambdaCall,  // []{}(...)
    Channel      // Nextflow producer output routed to a consumer
};

inline std::string call_kind_to_string(CallKind kind) {
    switch (kind) {
        case CallKind::Call: return "call";
        case CallKind::MemberCall: return "member_call";
        case CallKind::Qualified: return "qualified";
        case CallKind::New: return "new";
        case CallKind::Ctor: return "ctor";
        case CallKind::Indirect: return "indirect";
        case CallKind::LambdaCall: return "lambda_call";
        case CallKind::Channel: return "channel";
    }
    return "unknown";
}

// Extracted callsite with full context
struct Callsite {
    // Identity (cpp:callsite:file:start:end)
    std::string file_path;
    uint32_t start_byte = 0;
    uint32_t end_byte = 0;

    // Location
    uint32_t line = 0;       // 1-based
    uint32_t column = 0;     // 1-based

    // Caller context
    std::string caller_symbol;  // Symbol ID of containing function/method

    // Callee surface form (always present)
    CallKind kind = CallKind::Call;
    std::string callee_text;     // Exact source slice for callee expr
    std::string callee_leaf;     // Just the name: "foo", "method"
    std::string scope_text;      // For qualified: "ns::Class"

    // Member-call detail
    std::string receiver_text;   // "obj", "ptr", "this"
    std::string member_op;       // ".", "->", "::"

    // Args
    uint32_t arg_count = 0;

    // Template info
    std::string template_args;   // "<int, T>" if present

    // Resolution (populated by resolver, not extraction)
    std::string resolved_symbol; // Target symbol ID when resolved
    float resolve_confidence = 0.0f;

    // Generate callsite ID
    std::string id() const {
        return "cpp:callsite:" + file_path + ":" +
               std::to_string(start_byte) + ":" + std::to_string(end_byte);
    }

    // Generate location string
    std::string location() const {
        return file_path + ":" + std::to_string(line) + ":" + std::to_string(column);
    }
};

// Type hierarchy relationship (extends/implements)
struct TypeRelationship {
    std::string derived_name;   // The class/type that inherits
    std::string base_name;      // The class/interface being inherited from
    std::string relationship;   // "extends", "implements", "embeds"
    std::string file_path;
    uint32_t line = 0;
};

// Import statement tracking
struct ImportStatement {
    std::string file_path;      // File containing the import
    std::string import_path;    // The module/file being imported
    std::string alias;          // Optional alias (e.g., "import foo as bar")
    std::vector<std::string> names;  // Specific names imported (e.g., "from foo import a, b")
    uint32_t line = 0;
};

struct CodeReference {
    std::string file_path;
    std::string name;
    uint32_t line;
    bool file_reference = false;
};

// Combined extraction result
struct ExtractionResult {
    std::vector<ExtractedSymbol> symbols;
    std::vector<Callsite> callsites;
    std::vector<TypeRelationship> type_relationships;
    std::vector<ImportStatement> imports;
    std::vector<CodeReference> references;
};

// Language detection and parser management
class CodeIntel {
public:
    CodeIntel() {
        // Initialize parsers for each language
        parsers_["cpp"] = ts_parser_new();
        ts_parser_set_language(parsers_["cpp"], tree_sitter_cpp());

        parsers_["python"] = ts_parser_new();
        ts_parser_set_language(parsers_["python"], tree_sitter_python());

        parsers_["javascript"] = ts_parser_new();
        ts_parser_set_language(parsers_["javascript"], tree_sitter_javascript());

        parsers_["typescript"] = ts_parser_new();
        ts_parser_set_language(parsers_["typescript"], tree_sitter_typescript());

        parsers_["go"] = ts_parser_new();
        ts_parser_set_language(parsers_["go"], tree_sitter_go());

        parsers_["rust"] = ts_parser_new();
        ts_parser_set_language(parsers_["rust"], tree_sitter_rust());

        parsers_["java"] = ts_parser_new();
        ts_parser_set_language(parsers_["java"], tree_sitter_java());

        parsers_["ruby"] = ts_parser_new();
        ts_parser_set_language(parsers_["ruby"], tree_sitter_ruby());

        parsers_["csharp"] = ts_parser_new();
        ts_parser_set_language(parsers_["csharp"], tree_sitter_c_sharp());

        parsers_["swift"] = ts_parser_new();
        ts_parser_set_language(parsers_["swift"], tree_sitter_swift());

        parsers_["lua"] = ts_parser_new();
        ts_parser_set_language(parsers_["lua"], tree_sitter_lua());
        initialize_extended_parsers();
    }

    ~CodeIntel() {
        for (auto& [_, parser] : parsers_) {
            ts_parser_delete(parser);
        }
    }

    // Detect language from file extension
    static std::string detect_language(const std::string& path) {
        std::filesystem::path p(path);
        std::string ext = p.extension().string();

        if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" ||
            ext == ".cc" || ext == ".cxx" || ext == ".hxx") {
            return "cpp";
        }
        if (ext == ".py" || ext == ".pyw") return "python";
        if (ext == ".js" || ext == ".jsx" || ext == ".mjs") return "javascript";
        if (ext == ".ts" || ext == ".tsx") return "typescript";
        if (ext == ".go") return "go";
        if (ext == ".rs") return "rust";
        if (ext == ".java") return "java";
        if (ext == ".rb") return "ruby";
        if (ext == ".cs") return "csharp";
        if (ext == ".swift") return "swift";
        if (ext == ".lua") return "lua";
        if (ext == ".md" || ext == ".markdown" || ext == ".mdown") return "markdown";

        return detect_extended_language(path);
    }

    static const TSLanguage* extended_grammar(const std::string& language);

    // Each chunk ends before the next heading. Hierarchical names distinguish
    // repeated child headings; occurrence suffixes distinguish repeated siblings.
    // Fenced examples are content, never document structure. Keeping extraction
    // here shares exactly the same index path as source symbols (no new codec).
    static std::vector<ExtractedSymbol> extract_markdown(const std::string& path) {
        std::ifstream file(path);
        if (!file) return {};
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
        struct Heading { size_t start; size_t body; size_t level; std::string text; };
        std::vector<Heading> headings;
        char fence = 0;
        size_t fence_width = 0;
        auto trim = [](const std::string& s) {
            auto first = s.find_first_not_of(" \t");
            if (first == std::string::npos) return std::string{};
            return s.substr(first, s.find_last_not_of(" \t") - first + 1);
        };
        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            size_t start = line.find_first_not_of(' ');
            if (start == std::string::npos || start > 3) continue;
            char c = line[start];
            size_t run = 0;
            while (start + run < line.size() && line[start + run] == c) ++run;
            if (fence) {
                if (c == fence && run >= fence_width && trim(line.substr(start + run)).empty()) fence = 0;
                continue;
            }
            if ((c == '`' || c == '~') && run >= 3) {
                fence = c;
                fence_width = run;
                continue;
            }
            if (c == '#' && run <= 6 &&
                (start + run == line.size() || line[start + run] == ' ' || line[start + run] == '\t')) {
                auto title = trim(line.substr(start + run));
                auto hashes = title.find_last_not_of('#');
                if (hashes != std::string::npos && hashes + 1 < title.size() &&
                    (title[hashes] == ' ' || title[hashes] == '\t')) title = trim(title.substr(0, hashes + 1));
                headings.push_back({i, i + 1, run, title});
            } else if ((c == '=' || c == '-') && trim(line.substr(start + run)).empty() && i > 0 &&
                       !trim(lines[i - 1]).empty() &&
                       (headings.empty() || headings.back().body < i)) {
                headings.push_back({i - 1, i + 1, c == '=' ? 1u : 2u, trim(lines[i - 1])});
            }
        }
        if (lines.empty()) return {};
        if (headings.empty() || headings.front().start > 0)
            headings.insert(headings.begin(), {0, 0, 0, "(preamble)"});
        std::vector<ExtractedSymbol> out;
        std::vector<std::pair<size_t, std::string>> parents;
        std::unordered_map<std::string, size_t> occurrences;
        for (size_t i = 0; i < headings.size(); ++i) {
            const auto& h = headings[i];
            while (!parents.empty() && parents.back().first >= h.level) parents.pop_back();
            std::string name;
            for (const auto& parent : parents) name += parent.second + " / ";
            name += h.text;
            auto occurrence = ++occurrences[name];
            std::string component = h.text;
            if (occurrence > 1) {
                component += " [" + std::to_string(occurrence) + "]";
                name += " [" + std::to_string(occurrence) + "]";
            }
            if (h.level > 0) parents.emplace_back(h.level, component);
            size_t end = i + 1 < headings.size() ? headings[i + 1].start : lines.size();
            std::string text;
            for (size_t j = h.start; j < end; ++j) text += lines[j] + "\n";
            out.push_back({"heading", name, text, path,
                           static_cast<int32_t>(h.start + 1), static_cast<int32_t>(end), ""});
        }
        return out;
    }

    // Extract symbols from a single file
    std::vector<ExtractedSymbol> extract_file(const std::string& path) {
        std::vector<ExtractedSymbol> symbols;

        std::string lang = detect_language(path);
        if (lang.empty()) return symbols;
        if (lang == "markdown") return extract_markdown(path);
        if (extended_grammar(lang)) return extract_file_full(path).symbols;

        auto it = parsers_.find(lang);
        if (it == parsers_.end()) return symbols;

        // Read file content
        std::ifstream file(path);
        if (!file) return symbols;

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // Parse
        TSTree* tree = ts_parser_parse_string(
            it->second, nullptr,
            source.c_str(), source.length()
        );
        if (!tree) return symbols;

        TSNode root = ts_tree_root_node(tree);

        // Extract symbols based on language
        if (lang == "cpp") {
            extract_cpp(root, source, path, symbols);
        } else if (lang == "python") {
            extract_python(root, source, path, symbols);
        } else if (lang == "javascript" || lang == "typescript") {
            extract_js(root, source, path, symbols);
        } else if (lang == "go") {
            extract_go(root, source, path, symbols);
        } else if (lang == "rust") {
            extract_rust(root, source, path, symbols);
        } else if (lang == "java") {
            extract_java(root, source, path, symbols);
        } else if (lang == "ruby") {
            extract_ruby(root, source, path, symbols);
        } else if (lang == "csharp") {
            extract_csharp(root, source, path, symbols);
        } else if (lang == "swift") {
            extract_swift(root, source, path, symbols);
        } else if (lang == "lua") {
            extract_lua(root, source, path, symbols);
        }

        ts_tree_delete(tree);
        return symbols;
    }

    // Extract symbols AND callsites from a single file (Phase 1 Code Intelligence)
    ExtractionResult extract_file_full(const std::string& path) {
        ExtractionResult result;

        std::string lang = detect_language(path);
        if (lang.empty()) return result;
        if (lang == "markdown") {
            result.symbols = extract_markdown(path);
            return result;
        }

        auto it = parsers_.find(lang);
        if (it == parsers_.end()) return result;

        // Read file content
        std::ifstream file(path);
        if (!file) return result;

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // Parse
        TSTree* tree = ts_parser_parse_string(
            it->second, nullptr,
            source.c_str(), source.length()
        );
        if (!tree) return result;

        TSNode root = ts_tree_root_node(tree);

        // Extract symbols, callsites, type relationships, and imports based on language
        if (lang == "cpp") {
            extract_cpp_full(root, source, path, result.symbols, result.callsites,
                           result.type_relationships, result.imports);
        } else if (lang == "python") {
            extract_python_full(root, source, path, result.symbols, result.callsites,
                              result.type_relationships, result.imports);
        } else if (lang == "javascript" || lang == "typescript") {
            extract_js_full(root, source, path, result.symbols, result.callsites,
                          result.type_relationships, result.imports);
        } else if (lang == "go") {
            extract_go_full(root, source, path, result.symbols, result.callsites,
                          result.type_relationships, result.imports);
        } else if (lang == "rust") {
            extract_rust_full(root, source, path, result.symbols, result.callsites,
                            result.type_relationships, result.imports);
        } else if (lang == "swift") {
            extract_swift_full(root, source, path, result.symbols, result.callsites,
                              result.type_relationships, result.imports);
        } else if (lang == "lua") {
            extract_lua_full(root, source, path, result.symbols, result.callsites,
                            result.type_relationships, result.imports);
        } else if (extended_grammar(lang)) {
            extract_extended(root, source, path, lang, result);
        } else {
            // Languages without full extraction yet - symbols only
            if (lang == "java") {
                extract_java(root, source, path, result.symbols);
            } else if (lang == "ruby") {
                extract_ruby(root, source, path, result.symbols);
            } else if (lang == "csharp") {
                extract_csharp(root, source, path, result.symbols);
            }
        }

        // Complete navigation metadata from the defining AST node. Basic
        // extraction historically left signatures and out-of-class C++ parents
        // empty; those omissions must not invent global name bindings.
        std::unordered_map<uint64_t, std::vector<ExtractedSymbol*>> definitions;
        for (auto& symbol : result.symbols)
            definitions[(uint64_t(symbol.line_start) << 32) | uint32_t(symbol.line_end)].push_back(&symbol);
        std::vector<TSNode> pending{root};
        while (!pending.empty()) {
            auto node = pending.back(); pending.pop_back();
            auto body = find_child_by_field(node, "body");
            auto key = (uint64_t(node_line(node)) << 32) | uint32_t(node_end_line(node));
            auto found = definitions.find(key);
            if (!ts_node_is_null(body) && found != definitions.end()) {
                auto start = ts_node_start_byte(node), end = ts_node_start_byte(body);
                auto header = source.substr(start, end - start);
                while (!header.empty() && std::isspace(static_cast<unsigned char>(header.back()))) header.pop_back();
                for (auto* symbol : found->second) {
                    if (header.find(symbol->name) == std::string::npos) continue;
                    if (symbol->signature.empty()) symbol->signature = header;
                    if (lang != "cpp" || !symbol->parent.empty()) continue;
                    auto declarator = find_child_by_field(node, "declarator");
                    std::vector<TSNode> names;
                    if (!ts_node_is_null(declarator)) names.push_back(declarator);
                    while (!names.empty()) {
                        auto name_node = names.back(); names.pop_back();
                        if (std::strcmp(ts_node_type(name_node), "qualified_identifier") == 0 &&
                            extract_declarator_name(name_node, source) == symbol->name) {
                            auto scope = find_child_by_field(name_node, "scope");
                            if (!ts_node_is_null(scope)) symbol->parent = node_text(scope, source);
                            break;
                        }
                        for (uint32_t n = 0; n < ts_node_named_child_count(name_node); ++n)
                            names.push_back(ts_node_named_child(name_node, n));
                    }
                }
            }
            std::string type = ts_node_type(node);
            if (type == "identifier" || type == "type_identifier" || type == "field_identifier")
                result.references.push_back({path, node_text(node, source), static_cast<uint32_t>(node_line(node))});
            extract_file_reference(node, source, path, result);
            for (uint32_t n = 0; n < ts_node_named_child_count(node); ++n)
                pending.push_back(ts_node_named_child(node, n));
        }

        ts_tree_delete(tree);
        return result;
    }

    // Extract from directory with full callsite extraction
    ExtractionResult extract_directory_full(
        const std::string& path,
        const std::vector<std::string>& exclude = {"node_modules", ".git", "build", "__pycache__", "venv"},
        size_t max_files = 1000
    ) {
        ExtractionResult result;
        // Pre-reserve with reasonable estimate to reduce reallocations
        result.symbols.reserve(max_files * 20);
        result.callsites.reserve(max_files * 10);
        size_t file_count = 0;

        std::function<void(const std::filesystem::path&)> traverse;
        traverse = [&](const std::filesystem::path& dir) {
            if (file_count >= max_files) return;

            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (file_count >= max_files) return;

                std::string name = entry.path().filename().string();

                if (entry.is_directory()) {
                    bool skip = false;
                    for (const auto& ex : exclude) {
                        if (name == ex) { skip = true; break; }
                    }
                    if (!skip) traverse(entry.path());
                } else if (entry.is_regular_file()) {
                    std::string lang = detect_language(entry.path().string());
                    if (!lang.empty()) {
                        auto file_result = extract_file_full(entry.path().string());
                        result.symbols.insert(result.symbols.end(),
                                             std::make_move_iterator(file_result.symbols.begin()),
                                             std::make_move_iterator(file_result.symbols.end()));
                        result.callsites.insert(result.callsites.end(),
                                               std::make_move_iterator(file_result.callsites.begin()),
                                               std::make_move_iterator(file_result.callsites.end()));
                        result.type_relationships.insert(result.type_relationships.end(),
                                                        std::make_move_iterator(file_result.type_relationships.begin()),
                                                        std::make_move_iterator(file_result.type_relationships.end()));
                        result.imports.insert(result.imports.end(),
                                             std::make_move_iterator(file_result.imports.begin()),
                                             std::make_move_iterator(file_result.imports.end()));
                        file_count++;
                    }
                }
            }
        };

        if (std::filesystem::is_directory(path)) {
            traverse(path);
        } else if (std::filesystem::is_regular_file(path)) {
            result = extract_file_full(path);
        }

        return result;
    }

    // Git honors tracked files, untracked edits, .gitignore and .chittaignore.
    // Paths and subprocess arguments never pass through a shell.
    static std::string git_output(const std::vector<std::string>& args);
    static std::string project_name(const std::string& path);
    std::vector<std::string> collect_source_files(
        const std::string& path,
        const std::vector<std::string>& exclude = {"node_modules", ".git", "build", "__pycache__", "venv"},
        size_t max_files = 0);

    // Extract only the specified files (parse with tree-sitter).
    ExtractionResult extract_files(const std::unordered_set<std::string>& file_paths) {
        ExtractionResult result;
        result.symbols.reserve(file_paths.size() * 20);
        result.callsites.reserve(file_paths.size() * 10);
        for (const auto& fp : file_paths) {
            auto file_result = extract_file_full(fp);
            result.symbols.insert(result.symbols.end(),
                                 std::make_move_iterator(file_result.symbols.begin()),
                                 std::make_move_iterator(file_result.symbols.end()));
            result.callsites.insert(result.callsites.end(),
                                   std::make_move_iterator(file_result.callsites.begin()),
                                   std::make_move_iterator(file_result.callsites.end()));
            result.type_relationships.insert(result.type_relationships.end(),
                                            std::make_move_iterator(file_result.type_relationships.begin()),
                                            std::make_move_iterator(file_result.type_relationships.end()));
            result.imports.insert(result.imports.end(),
                                 std::make_move_iterator(file_result.imports.begin()),
                                 std::make_move_iterator(file_result.imports.end()));
            result.references.insert(result.references.end(),
                                     std::make_move_iterator(file_result.references.begin()),
                                     std::make_move_iterator(file_result.references.end()));
        }
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Incremental Extraction (Only process changed files)
    // ═══════════════════════════════════════════════════════════════════════════

    struct IncrementalResult {
        ExtractionResult extracted;     // Newly extracted symbols/callsites
        size_t files_processed = 0;     // Files that were re-indexed
        size_t files_skipped = 0;       // Files that were up-to-date
        size_t symbols_deleted = 0;     // Old symbols removed
        size_t triplets_deleted = 0;    // Old triplets removed
    };


private:
    std::unordered_map<std::string, TSParser*> parsers_;
    void initialize_extended_parsers();
    static std::string detect_extended_language(const std::string& path);
    void extract_file_reference(TSNode node, const std::string& source, const std::string& path, ExtractionResult& result);
    void extract_extended(TSNode root, const std::string& source, const std::string& path,
                          const std::string& language, ExtractionResult& result);

    // Get text for a node
    std::string node_text(TSNode node, const std::string& source) {
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (end > source.length()) end = source.length();
        return source.substr(start, end - start);
    }

    // Get line number (1-based)
    int32_t node_line(TSNode node) {
        return static_cast<int32_t>(ts_node_start_point(node).row + 1);
    }

    int32_t node_end_line(TSNode node) {
        return static_cast<int32_t>(ts_node_end_point(node).row + 1);
    }

    // Find child by type (returns null node if not found)
    TSNode find_child(TSNode node, const char* type) {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_child(node, i);
            if (strcmp(ts_node_type(child), type) == 0) {
                return child;
            }
        }
        TSNode null_node = {};
        null_node.id = nullptr;
        return null_node;
    }

    // Find child by field name
    TSNode find_child_by_field(TSNode node, const char* field_name) {
        return ts_node_child_by_field_name(node, field_name, strlen(field_name));
    }

    // Extract function/method name from declarator tree
    // Handles nested declarators like: function_declarator -> identifier
    // or: function_declarator -> field_identifier
    // or: function_declarator -> qualified_identifier -> identifier
    std::string extract_declarator_name(TSNode node, const std::string& source) {
        const char* type = ts_node_type(node);

        // Direct identifier
        if (strcmp(type, "identifier") == 0 ||
            strcmp(type, "field_identifier") == 0) {
            return node_text(node, source);
        }

        // Qualified identifier (e.g., ClassName::methodName)
        if (strcmp(type, "qualified_identifier") == 0) {
            TSNode name_node = find_child_by_field(node, "name");
            if (!ts_node_is_null(name_node)) {
                return node_text(name_node, source);
            }
        }

        // Destructor (e.g., ~ClassName)
        if (strcmp(type, "destructor_name") == 0) {
            return node_text(node, source);
        }

        // Template function
        if (strcmp(type, "template_function") == 0) {
            TSNode name_node = find_child_by_field(node, "name");
            if (!ts_node_is_null(name_node)) {
                return extract_declarator_name(name_node, source);
            }
        }

        // Try declarator field (nested declarator)
        TSNode declarator = find_child_by_field(node, "declarator");
        if (!ts_node_is_null(declarator)) {
            return extract_declarator_name(declarator, source);
        }

        // Fallback: search children for identifier types
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            TSNode child = ts_node_child(node, i);
            const char* child_type = ts_node_type(child);
            if (strcmp(child_type, "identifier") == 0 ||
                strcmp(child_type, "field_identifier") == 0) {
                return node_text(child, source);
            }
        }

        return "";
    }

    // C/C++ extraction
    void extract_cpp(TSNode node, const std::string& source,
                     const std::string& path, std::vector<ExtractedSymbol>& symbols,
                     const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_definition") == 0) {
            // Get declarator field (contains function_declarator or other declarator types)
            TSNode declarator = find_child_by_field(node, "declarator");
            if (ts_node_is_null(declarator)) declarator = node;

            // Extract function name - recursively find identifier in declarator tree
            std::string name = extract_declarator_name(declarator, source);
            if (!name.empty()) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
            return;  // Don't recurse into function body for basic extraction
        } else if (strcmp(type, "class_specifier") == 0 ||
                   strcmp(type, "struct_specifier") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Extract methods
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; i++) {
                    extract_cpp(ts_node_child(node, i), source, path, symbols, class_name);
                }
                return;
            }
        }

        // Recurse to children
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_cpp(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // C/C++ Full Extraction (Symbols + Callsites)
    // ═══════════════════════════════════════════════════════════════════════

    // Generate symbol ID for a function/method. `path` is accepted but
    // deliberately not part of the id, so the id stays stable when a symbol
    // moves between files.
    // ceiling: two same-named statics (or same-signature overloads) in
    // different files share one id and collide in the symbol table;
    // upgrade: fold the signature into the id, and `path` for internal-linkage
    // symbols only, so cross-file moves still preserve identity. (2026-09-02)
    std::string make_symbol_id(const std::string& kind, const std::string& name,
                               const std::string& parent, const std::string& /*path*/) {
        std::string fqn = parent.empty() ? name : (parent + "::" + name);
        return "cpp:" + kind + ":" + fqn;
    }

    // Count arguments in an argument_list node
    uint32_t count_args(TSNode args_node) {
        if (ts_node_is_null(args_node)) return 0;
        uint32_t count = 0;
        uint32_t child_count = ts_node_child_count(args_node);
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(args_node, i);
            const char* type = ts_node_type(child);
            // Skip punctuation: ( ) ,
            if (strcmp(type, "(") != 0 && strcmp(type, ")") != 0 &&
                strcmp(type, ",") != 0) {
                count++;
            }
        }
        return count;
    }

    // Extract template args from callee text
    std::string extract_template_args(const std::string& text) {
        size_t start = text.find('<');
        if (start == std::string::npos) return "";
        size_t end = text.rfind('>');
        if (end == std::string::npos || end <= start) return "";
        return text.substr(start, end - start + 1);
    }

    // Extract leaf name (after last ::, before <)
    std::string extract_leaf_name(const std::string& text) {
        std::string result = text;
        // Remove template args
        size_t templ = result.find('<');
        if (templ != std::string::npos) {
            result = result.substr(0, templ);
        }
        // Get after last ::
        size_t scope = result.rfind("::");
        if (scope != std::string::npos) {
            result = result.substr(scope + 2);
        }
        return result;
    }

    // Extract scope (everything before last ::)
    std::string extract_scope(const std::string& text) {
        std::string result = text;
        // Remove template args from the end
        size_t templ = result.find('<');
        if (templ != std::string::npos) {
            result = result.substr(0, templ);
        }
        size_t scope = result.rfind("::");
        if (scope != std::string::npos) {
            return result.substr(0, scope);
        }
        return "";
    }

    // Extract callsite from a call_expression node
    Callsite extract_callsite_from_call(TSNode call_node, const std::string& source,
                                         const std::string& path,
                                         const std::string& caller_symbol) {
        Callsite cs;
        cs.file_path = path;
        cs.start_byte = ts_node_start_byte(call_node);
        cs.end_byte = ts_node_end_byte(call_node);
        cs.line = ts_node_start_point(call_node).row + 1;
        cs.column = ts_node_start_point(call_node).column + 1;
        cs.caller_symbol = caller_symbol;

        // Get function node (the thing being called)
        TSNode fn_node = find_child_by_field(call_node, "function");
        if (ts_node_is_null(fn_node)) {
            fn_node = ts_node_child(call_node, 0);  // Fallback
        }

        // Get arguments
        TSNode args_node = find_child_by_field(call_node, "arguments");
        cs.arg_count = count_args(args_node);

        // Classify based on function node type
        const char* fn_type = ts_node_type(fn_node);
        cs.callee_text = node_text(fn_node, source);

        if (strcmp(fn_type, "identifier") == 0) {
            // Simple call: foo()
            cs.kind = CallKind::Call;
            cs.callee_leaf = cs.callee_text;
        } else if (strcmp(fn_type, "field_expression") == 0) {
            // Member call: obj.method() or ptr->method()
            cs.kind = CallKind::MemberCall;

            // Extract receiver and field
            TSNode receiver = find_child_by_field(fn_node, "argument");
            if (ts_node_is_null(receiver)) {
                receiver = ts_node_child(fn_node, 0);
            }
            TSNode field = find_child_by_field(fn_node, "field");
            if (ts_node_is_null(field)) {
                // Try to find field_identifier
                for (uint32_t i = 0; i < ts_node_child_count(fn_node); i++) {
                    TSNode child = ts_node_child(fn_node, i);
                    if (strcmp(ts_node_type(child), "field_identifier") == 0) {
                        field = child;
                        break;
                    }
                }
            }

            if (!ts_node_is_null(receiver)) {
                cs.receiver_text = node_text(receiver, source);
            }
            if (!ts_node_is_null(field)) {
                cs.callee_leaf = node_text(field, source);
            } else {
                cs.callee_leaf = extract_leaf_name(cs.callee_text);
            }

            // Detect member operator
            if (cs.callee_text.find("->") != std::string::npos) {
                cs.member_op = "->";
            } else if (cs.callee_text.find(".") != std::string::npos) {
                cs.member_op = ".";
            }
        } else if (strcmp(fn_type, "qualified_identifier") == 0 ||
                   strcmp(fn_type, "scoped_identifier") == 0 ||
                   strcmp(fn_type, "template_function") == 0) {
            // Qualified call: ns::foo() or Class::method()
            cs.kind = CallKind::Qualified;
            cs.callee_leaf = extract_leaf_name(cs.callee_text);
            cs.scope_text = extract_scope(cs.callee_text);
            cs.template_args = extract_template_args(cs.callee_text);
            cs.member_op = "::";
        } else if (strcmp(fn_type, "lambda_expression") == 0) {
            // Lambda call: [](){}()
            cs.kind = CallKind::LambdaCall;
            cs.callee_leaf = "lambda";
        } else if (strcmp(fn_type, "type_identifier") == 0 ||
                   strcmp(fn_type, "scoped_type_identifier") == 0) {
            // Constructor call as expression: Foo(...)
            cs.kind = CallKind::Ctor;
            cs.callee_leaf = extract_leaf_name(cs.callee_text);
            cs.scope_text = extract_scope(cs.callee_text);
        } else {
            // Indirect/unknown: (*fp)(), fp(), etc.
            cs.kind = CallKind::Indirect;
            cs.callee_leaf = extract_leaf_name(cs.callee_text);
        }

        return cs;
    }

    // Extract callsite from a new_expression node
    Callsite extract_callsite_from_new(TSNode new_node, const std::string& source,
                                        const std::string& path,
                                        const std::string& caller_symbol) {
        Callsite cs;
        cs.file_path = path;
        cs.start_byte = ts_node_start_byte(new_node);
        cs.end_byte = ts_node_end_byte(new_node);
        cs.line = ts_node_start_point(new_node).row + 1;
        cs.column = ts_node_start_point(new_node).column + 1;
        cs.caller_symbol = caller_symbol;
        cs.kind = CallKind::New;

        // Find type being constructed
        TSNode type_node = find_child(new_node, "type_identifier");
        if (ts_node_is_null(type_node)) {
            type_node = find_child(new_node, "scoped_type_identifier");
        }
        if (!ts_node_is_null(type_node)) {
            cs.callee_text = node_text(type_node, source);
            cs.callee_leaf = extract_leaf_name(cs.callee_text);
            cs.scope_text = extract_scope(cs.callee_text);
        } else {
            cs.callee_text = node_text(new_node, source);
            cs.callee_leaf = "unknown";
        }

        // Find arguments if present
        TSNode args_node = find_child(new_node, "argument_list");
        cs.arg_count = count_args(args_node);

        return cs;
    }

    // Extract callsites from a function body
    void extract_callsites_from_body(TSNode body, const std::string& source,
                                      const std::string& path,
                                      const std::string& caller_symbol,
                                      std::vector<Callsite>& callsites) {
        const char* type = ts_node_type(body);

        if (strcmp(type, "call_expression") == 0) {
            callsites.push_back(extract_callsite_from_call(body, source, path, caller_symbol));
        } else if (strcmp(type, "new_expression") == 0) {
            callsites.push_back(extract_callsite_from_new(body, source, path, caller_symbol));
        }

        // Recurse
        uint32_t count = ts_node_child_count(body);
        for (uint32_t i = 0; i < count; i++) {
            extract_callsites_from_body(ts_node_child(body, i), source, path,
                                        caller_symbol, callsites);
        }
    }

    // C/C++ full extraction (symbols + callsites + type relationships + imports)
    void extract_cpp_full(TSNode node, const std::string& source,
                          const std::string& path,
                          std::vector<ExtractedSymbol>& symbols,
                          std::vector<Callsite>& callsites,
                          std::vector<TypeRelationship>& type_rels,
                          std::vector<ImportStatement>& imports,
                          const std::string& parent = "") {
        const char* type = ts_node_type(node);

        // #include statements
        if (strcmp(type, "preproc_include") == 0) {
            TSNode path_node = find_child(node, "string_literal");
            if (ts_node_is_null(path_node)) {
                path_node = find_child(node, "system_lib_string");
            }
            if (!ts_node_is_null(path_node)) {
                ImportStatement imp;
                imp.file_path = path;
                imp.import_path = node_text(path_node, source);
                // Remove quotes/angle brackets
                if (imp.import_path.size() >= 2) {
                    imp.import_path = imp.import_path.substr(1, imp.import_path.size() - 2);
                }
                imp.line = node_line(node);
                imports.push_back(imp);
            }
        }
        // Function definitions
        else if (strcmp(type, "function_definition") == 0) {
            // Get declarator field (contains function_declarator or other declarator types)
            TSNode declarator = find_child_by_field(node, "declarator");
            if (ts_node_is_null(declarator)) declarator = node;

            // Extract function name - recursively find identifier in declarator tree
            std::string name = extract_declarator_name(declarator, source);
            if (!name.empty()) {
                std::string kind = parent.empty() ? "function" : "method";

                // Add symbol
                ExtractedSymbol sym;
                sym.kind = kind;
                sym.name = name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);

                // Generate symbol ID for callsites
                std::string symbol_id = make_symbol_id(kind, name, parent, path);

                // Extract callsites from function body
                TSNode body = find_child_by_field(node, "body");
                if (!ts_node_is_null(body)) {
                    extract_callsites_from_body(body, source, path, symbol_id, callsites);
                }
            }
        }
        // Class/struct definitions with base class extraction
        else if (strcmp(type, "class_specifier") == 0 ||
                   strcmp(type, "struct_specifier") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);

                ExtractedSymbol sym;
                sym.kind = "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Extract base classes from base_class_clause
                TSNode base_clause = find_child(node, "base_class_clause");
                if (!ts_node_is_null(base_clause)) {
                    uint32_t bc_count = ts_node_child_count(base_clause);
                    for (uint32_t i = 0; i < bc_count; i++) {
                        TSNode child = ts_node_child(base_clause, i);
                        const char* child_type = ts_node_type(child);
                        // Look for type_identifier (base class name)
                        if (strcmp(child_type, "type_identifier") == 0 ||
                            strcmp(child_type, "qualified_identifier") == 0 ||
                            strcmp(child_type, "template_type") == 0) {
                            TypeRelationship rel;
                            rel.derived_name = class_name;
                            rel.base_name = node_text(child, source);
                            rel.relationship = "extends";
                            rel.file_path = path;
                            rel.line = node_line(child);
                            type_rels.push_back(rel);
                        }
                    }
                }

                // Extract methods and their callsites
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; i++) {
                    extract_cpp_full(ts_node_child(node, i), source, path,
                                    symbols, callsites, type_rels, imports, class_name);
                }
                return;
            }
        }

        // Recurse to children
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_cpp_full(ts_node_child(node, i), source, path,
                            symbols, callsites, type_rels, imports, parent);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Python Full Extraction (Symbols + Callsites + Type Relationships + Imports)
    // ═══════════════════════════════════════════════════════════════════════

    void extract_python_full(TSNode node, const std::string& source,
                             const std::string& path,
                             std::vector<ExtractedSymbol>& symbols,
                             std::vector<Callsite>& callsites,
                             std::vector<TypeRelationship>& type_rels,
                             std::vector<ImportStatement>& imports,
                             const std::string& parent = "") {
        const char* type = ts_node_type(node);

        // Import statements: import foo, import foo as bar
        if (strcmp(type, "import_statement") == 0) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; i++) {
                TSNode child = ts_node_child(node, i);
                const char* child_type = ts_node_type(child);
                if (strcmp(child_type, "dotted_name") == 0) {
                    ImportStatement imp;
                    imp.file_path = path;
                    imp.import_path = node_text(child, source);
                    imp.line = node_line(node);
                    imports.push_back(imp);
                } else if (strcmp(child_type, "aliased_import") == 0) {
                    TSNode name_node = find_child(child, "dotted_name");
                    TSNode alias_node = find_child(child, "identifier");
                    ImportStatement imp;
                    imp.file_path = path;
                    if (!ts_node_is_null(name_node)) {
                        imp.import_path = node_text(name_node, source);
                    }
                    if (!ts_node_is_null(alias_node)) {
                        imp.alias = node_text(alias_node, source);
                    }
                    imp.line = node_line(node);
                    imports.push_back(imp);
                }
            }
        }
        // From imports: from foo import bar, baz
        else if (strcmp(type, "import_from_statement") == 0) {
            ImportStatement imp;
            imp.file_path = path;
            imp.line = node_line(node);

            TSNode module_node = find_child(node, "dotted_name");
            if (!ts_node_is_null(module_node)) {
                imp.import_path = node_text(module_node, source);
            }

            // Extract imported names
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; i++) {
                TSNode child = ts_node_child(node, i);
                const char* child_type = ts_node_type(child);
                if (strcmp(child_type, "identifier") == 0) {
                    imp.names.push_back(node_text(child, source));
                } else if (strcmp(child_type, "aliased_import") == 0) {
                    TSNode name_node = find_child(child, "identifier");
                    if (!ts_node_is_null(name_node)) {
                        imp.names.push_back(node_text(name_node, source));
                    }
                }
            }
            if (!imp.import_path.empty() || !imp.names.empty()) {
                imports.push_back(imp);
            }
        }
        // Function/method definitions
        else if (strcmp(type, "function_definition") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        }
        // Class definitions with base class extraction
        else if (strcmp(type, "class_definition") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Extract base classes from argument_list (Python class bases)
                TSNode args = find_child(node, "argument_list");
                if (!ts_node_is_null(args)) {
                    uint32_t args_count = ts_node_child_count(args);
                    for (uint32_t i = 0; i < args_count; i++) {
                        TSNode arg = ts_node_child(args, i);
                        const char* arg_type = ts_node_type(arg);
                        if (strcmp(arg_type, "identifier") == 0 ||
                            strcmp(arg_type, "attribute") == 0) {
                            TypeRelationship rel;
                            rel.derived_name = class_name;
                            rel.base_name = node_text(arg, source);
                            rel.relationship = "extends";
                            rel.file_path = path;
                            rel.line = node_line(arg);
                            type_rels.push_back(rel);
                        }
                    }
                }

                TSNode body = find_child(node, "block");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_python_full(ts_node_child(body, i), source, path,
                                           symbols, callsites, type_rels, imports, class_name);
                    }
                }
                return;
            }
        }
        // Call expressions: func(...) or obj.method(...)
        else if (strcmp(type, "call") == 0) {
            TSNode func_node = find_child_by_field(node, "function");
            TSNode args_node = find_child_by_field(node, "arguments");
            if (ts_node_is_null(func_node)) {
                // Try first child as function
                if (ts_node_child_count(node) > 0) {
                    func_node = ts_node_child(node, 0);
                }
            }
            if (!ts_node_is_null(func_node)) {
                Callsite cs;
                cs.file_path = path;
                cs.start_byte = ts_node_start_byte(node);
                cs.end_byte = ts_node_end_byte(node);
                cs.line = node_line(node);
                cs.column = ts_node_start_point(node).column;
                cs.caller_symbol = parent;
                cs.arg_count = count_args(args_node);
                cs.callee_text = node_text(func_node, source);

                const char* func_type = ts_node_type(func_node);
                if (strcmp(func_type, "attribute") == 0) {
                    // obj.method() - attribute call
                    cs.kind = CallKind::MemberCall;
                    TSNode attr_node = find_child_by_field(func_node, "attribute");
                    if (!ts_node_is_null(attr_node)) {
                        cs.callee_leaf = node_text(attr_node, source);
                    }
                    TSNode obj_node = find_child_by_field(func_node, "object");
                    if (!ts_node_is_null(obj_node)) {
                        cs.receiver_text = node_text(obj_node, source);
                    }
                    cs.member_op = ".";
                } else if (strcmp(func_type, "identifier") == 0) {
                    // Simple function call
                    cs.kind = CallKind::Call;
                    cs.callee_leaf = cs.callee_text;
                } else {
                    // Other call types (subscript, etc.)
                    cs.kind = CallKind::Indirect;
                    cs.callee_leaf = cs.callee_text;
                }
                callsites.push_back(cs);
            }
        }

        // Recurse to children
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_python_full(ts_node_child(node, i), source, path,
                               symbols, callsites, type_rels, imports, parent);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // JavaScript/TypeScript Full Extraction (Symbols + Callsites + Types + Imports)
    // ═══════════════════════════════════════════════════════════════════════

    void extract_js_full(TSNode node, const std::string& source,
                         const std::string& path,
                         std::vector<ExtractedSymbol>& symbols,
                         std::vector<Callsite>& callsites,
                         std::vector<TypeRelationship>& type_rels,
                         std::vector<ImportStatement>& imports,
                         const std::string& parent = "") {
        const char* type = ts_node_type(node);

        // Import statements
        if (strcmp(type, "import_statement") == 0) {
            ImportStatement imp;
            imp.file_path = path;
            imp.line = node_line(node);

            // Find source (the module path)
            TSNode source_node = find_child(node, "string");
            if (!ts_node_is_null(source_node)) {
                imp.import_path = node_text(source_node, source);
                // Remove quotes
                if (imp.import_path.size() >= 2) {
                    imp.import_path = imp.import_path.substr(1, imp.import_path.size() - 2);
                }
            }

            // Find imported names
            TSNode clause = find_child(node, "import_clause");
            if (!ts_node_is_null(clause)) {
                uint32_t clause_count = ts_node_child_count(clause);
                for (uint32_t i = 0; i < clause_count; i++) {
                    TSNode child = ts_node_child(clause, i);
                    const char* child_type = ts_node_type(child);
                    if (strcmp(child_type, "identifier") == 0) {
                        imp.names.push_back(node_text(child, source));
                    } else if (strcmp(child_type, "named_imports") == 0) {
                        uint32_t spec_count = ts_node_child_count(child);
                        for (uint32_t j = 0; j < spec_count; j++) {
                            TSNode spec = ts_node_child(child, j);
                            if (strcmp(ts_node_type(spec), "import_specifier") == 0) {
                                TSNode name_node = find_child(spec, "identifier");
                                if (!ts_node_is_null(name_node)) {
                                    imp.names.push_back(node_text(name_node, source));
                                }
                            }
                        }
                    }
                }
            }
            imports.push_back(imp);
        }
        // Function declarations
        else if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "method_definition") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "property_identifier");
            }
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        }
        // Arrow functions with variable declaration
        else if (strcmp(type, "lexical_declaration") == 0 ||
                 strcmp(type, "variable_declaration") == 0) {
            TSNode decl = find_child(node, "variable_declarator");
            if (!ts_node_is_null(decl)) {
                TSNode name_node = find_child(decl, "identifier");
                TSNode value_node = find_child(decl, "value");
                if (!ts_node_is_null(name_node) && !ts_node_is_null(value_node)) {
                    const char* val_type = ts_node_type(value_node);
                    if (strcmp(val_type, "arrow_function") == 0 ||
                        strcmp(val_type, "function_expression") == 0) {
                        ExtractedSymbol sym;
                        sym.kind = "function";
                        sym.name = node_text(name_node, source);
                        sym.file_path = path;
                        sym.line_start = node_line(node);
                        sym.line_end = node_end_line(node);
                        symbols.push_back(sym);
                    }
                }
            }
        }
        // Class declarations with extends/implements
        else if (strcmp(type, "class_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Extract extends clause
                TSNode heritage = find_child(node, "class_heritage");
                if (!ts_node_is_null(heritage)) {
                    TSNode extends_clause = find_child(heritage, "extends_clause");
                    if (!ts_node_is_null(extends_clause)) {
                        TSNode base_node = find_child(extends_clause, "identifier");
                        if (!ts_node_is_null(base_node)) {
                            TypeRelationship rel;
                            rel.derived_name = class_name;
                            rel.base_name = node_text(base_node, source);
                            rel.relationship = "extends";
                            rel.file_path = path;
                            rel.line = node_line(base_node);
                            type_rels.push_back(rel);
                        }
                    }

                    // TypeScript implements clause
                    TSNode implements_clause = find_child(heritage, "implements_clause");
                    if (!ts_node_is_null(implements_clause)) {
                        uint32_t impl_count = ts_node_child_count(implements_clause);
                        for (uint32_t i = 0; i < impl_count; i++) {
                            TSNode impl = ts_node_child(implements_clause, i);
                            if (strcmp(ts_node_type(impl), "type_identifier") == 0) {
                                TypeRelationship rel;
                                rel.derived_name = class_name;
                                rel.base_name = node_text(impl, source);
                                rel.relationship = "implements";
                                rel.file_path = path;
                                rel.line = node_line(impl);
                                type_rels.push_back(rel);
                            }
                        }
                    }
                }

                TSNode body = find_child(node, "class_body");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_js_full(ts_node_child(body, i), source, path,
                                       symbols, callsites, type_rels, imports, class_name);
                    }
                }
                return;
            }
        }
        // Call expressions
        else if (strcmp(type, "call_expression") == 0) {
            TSNode func_node = find_child_by_field(node, "function");
            TSNode args_node = find_child_by_field(node, "arguments");
            if (ts_node_is_null(func_node)) {
                if (ts_node_child_count(node) > 0) {
                    func_node = ts_node_child(node, 0);
                }
            }
            if (!ts_node_is_null(func_node)) {
                Callsite cs;
                cs.file_path = path;
                cs.start_byte = ts_node_start_byte(node);
                cs.end_byte = ts_node_end_byte(node);
                cs.line = node_line(node);
                cs.column = ts_node_start_point(node).column;
                cs.caller_symbol = parent;
                cs.arg_count = count_args(args_node);
                cs.callee_text = node_text(func_node, source);

                const char* func_type = ts_node_type(func_node);
                if (strcmp(func_type, "member_expression") == 0) {
                    cs.kind = CallKind::MemberCall;
                    TSNode prop = find_child_by_field(func_node, "property");
                    if (!ts_node_is_null(prop)) {
                        cs.callee_leaf = node_text(prop, source);
                    }
                    TSNode obj = find_child_by_field(func_node, "object");
                    if (!ts_node_is_null(obj)) {
                        cs.receiver_text = node_text(obj, source);
                    }
                    cs.member_op = ".";
                } else if (strcmp(func_type, "identifier") == 0) {
                    cs.kind = CallKind::Call;
                    cs.callee_leaf = cs.callee_text;
                } else {
                    cs.kind = CallKind::Indirect;
                    cs.callee_leaf = cs.callee_text;
                }
                callsites.push_back(cs);
            }
        }
        // new expression
        else if (strcmp(type, "new_expression") == 0) {
            TSNode constructor = find_child_by_field(node, "constructor");
            TSNode args_node = find_child_by_field(node, "arguments");
            if (ts_node_is_null(constructor) && ts_node_child_count(node) > 0) {
                constructor = ts_node_child(node, 1);  // Skip 'new' keyword
            }
            if (!ts_node_is_null(constructor)) {
                Callsite cs;
                cs.file_path = path;
                cs.start_byte = ts_node_start_byte(node);
                cs.end_byte = ts_node_end_byte(node);
                cs.line = node_line(node);
                cs.column = ts_node_start_point(node).column;
                cs.caller_symbol = parent;
                cs.kind = CallKind::New;
                cs.callee_text = node_text(constructor, source);
                cs.callee_leaf = cs.callee_text;
                cs.arg_count = count_args(args_node);
                callsites.push_back(cs);
            }
        }

        // Recurse
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_js_full(ts_node_child(node, i), source, path,
                           symbols, callsites, type_rels, imports, parent);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Go Full Extraction (Symbols + Callsites + Types + Imports)
    // ═══════════════════════════════════════════════════════════════════════

    void extract_go_full(TSNode node, const std::string& source,
                         const std::string& path,
                         std::vector<ExtractedSymbol>& symbols,
                         std::vector<Callsite>& callsites,
                         std::vector<TypeRelationship>& type_rels,
                         std::vector<ImportStatement>& imports,
                         const std::string& parent = "") {
        const char* type = ts_node_type(node);

        // Import declarations
        if (strcmp(type, "import_declaration") == 0) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; i++) {
                TSNode child = ts_node_child(node, i);
                const char* child_type = ts_node_type(child);

                if (strcmp(child_type, "import_spec") == 0) {
                    ImportStatement imp;
                    imp.file_path = path;
                    imp.line = node_line(child);

                    TSNode path_node = find_child(child, "interpreted_string_literal");
                    if (!ts_node_is_null(path_node)) {
                        imp.import_path = node_text(path_node, source);
                        // Remove quotes
                        if (imp.import_path.size() >= 2) {
                            imp.import_path = imp.import_path.substr(1, imp.import_path.size() - 2);
                        }
                    }

                    // Check for alias
                    TSNode name_node = find_child(child, "package_identifier");
                    if (!ts_node_is_null(name_node)) {
                        imp.alias = node_text(name_node, source);
                    }

                    imports.push_back(imp);
                } else if (strcmp(child_type, "import_spec_list") == 0) {
                    // Multiple imports: import ( "foo" "bar" )
                    uint32_t spec_count = ts_node_child_count(child);
                    for (uint32_t j = 0; j < spec_count; j++) {
                        TSNode spec = ts_node_child(child, j);
                        if (strcmp(ts_node_type(spec), "import_spec") == 0) {
                            ImportStatement imp;
                            imp.file_path = path;
                            imp.line = node_line(spec);

                            TSNode path_node = find_child(spec, "interpreted_string_literal");
                            if (!ts_node_is_null(path_node)) {
                                imp.import_path = node_text(path_node, source);
                                if (imp.import_path.size() >= 2) {
                                    imp.import_path = imp.import_path.substr(1, imp.import_path.size() - 2);
                                }
                            }

                            TSNode name_node = find_child(spec, "package_identifier");
                            if (!ts_node_is_null(name_node)) {
                                imp.alias = node_text(name_node, source);
                            }

                            imports.push_back(imp);
                        }
                    }
                }
            }
        }
        // Function declarations
        else if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "method_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "field_identifier");
            }
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        }
        // Type declarations (structs, interfaces) with embedded types
        else if (strcmp(type, "type_declaration") == 0) {
            TSNode spec = find_child(node, "type_spec");
            if (!ts_node_is_null(spec)) {
                TSNode name_node = find_child(spec, "type_identifier");
                if (!ts_node_is_null(name_node)) {
                    std::string type_name = node_text(name_node, source);
                    ExtractedSymbol sym;
                    sym.kind = "struct";
                    sym.name = type_name;
                    sym.file_path = path;
                    sym.line_start = node_line(node);
                    sym.line_end = node_end_line(node);
                    symbols.push_back(sym);

                    // Extract embedded types from struct fields
                    TSNode struct_type = find_child(spec, "struct_type");
                    if (!ts_node_is_null(struct_type)) {
                        TSNode field_list = find_child(struct_type, "field_declaration_list");
                        if (!ts_node_is_null(field_list)) {
                            uint32_t field_count = ts_node_child_count(field_list);
                            for (uint32_t i = 0; i < field_count; i++) {
                                TSNode field = ts_node_child(field_list, i);
                                if (strcmp(ts_node_type(field), "field_declaration") == 0) {
                                    // Check if this is an embedded type (no name, just type)
                                    TSNode field_type = find_child(field, "type_identifier");
                                    TSNode field_name = find_child(field, "field_identifier");
                                    if (!ts_node_is_null(field_type) && ts_node_is_null(field_name)) {
                                        TypeRelationship rel;
                                        rel.derived_name = type_name;
                                        rel.base_name = node_text(field_type, source);
                                        rel.relationship = "embeds";
                                        rel.file_path = path;
                                        rel.line = node_line(field_type);
                                        type_rels.push_back(rel);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Call expressions
        else if (strcmp(type, "call_expression") == 0) {
            TSNode func_node = find_child_by_field(node, "function");
            TSNode args_node = find_child_by_field(node, "arguments");
            if (ts_node_is_null(func_node) && ts_node_child_count(node) > 0) {
                func_node = ts_node_child(node, 0);
            }
            if (!ts_node_is_null(func_node)) {
                Callsite cs;
                cs.file_path = path;
                cs.start_byte = ts_node_start_byte(node);
                cs.end_byte = ts_node_end_byte(node);
                cs.line = node_line(node);
                cs.column = ts_node_start_point(node).column;
                cs.caller_symbol = parent;
                cs.arg_count = count_args(args_node);
                cs.callee_text = node_text(func_node, source);

                const char* func_type = ts_node_type(func_node);
                if (strcmp(func_type, "selector_expression") == 0) {
                    cs.kind = CallKind::MemberCall;
                    TSNode field = find_child_by_field(func_node, "field");
                    if (!ts_node_is_null(field)) {
                        cs.callee_leaf = node_text(field, source);
                    }
                    TSNode operand = find_child_by_field(func_node, "operand");
                    if (!ts_node_is_null(operand)) {
                        cs.receiver_text = node_text(operand, source);
                    }
                    cs.member_op = ".";
                } else if (strcmp(func_type, "identifier") == 0) {
                    cs.kind = CallKind::Call;
                    cs.callee_leaf = cs.callee_text;
                } else if (strcmp(func_type, "qualified_identifier") == 0) {
                    cs.kind = CallKind::Qualified;
                    // package.Function
                    uint32_t cc = ts_node_child_count(func_node);
                    if (cc > 0) {
                        cs.scope_text = node_text(ts_node_child(func_node, 0), source);
                    }
                    if (cc > 2) {
                        cs.callee_leaf = node_text(ts_node_child(func_node, 2), source);
                    }
                } else {
                    cs.kind = CallKind::Indirect;
                    cs.callee_leaf = cs.callee_text;
                }
                callsites.push_back(cs);
            }
        }

        // Recurse
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_go_full(ts_node_child(node, i), source, path,
                           symbols, callsites, type_rels, imports, parent);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Rust Full Extraction (Symbols + Callsites + Types + Imports)
    // ═══════════════════════════════════════════════════════════════════════

    void extract_rust_full(TSNode node, const std::string& source,
                           const std::string& path,
                           std::vector<ExtractedSymbol>& symbols,
                           std::vector<Callsite>& callsites,
                           std::vector<TypeRelationship>& type_rels,
                           std::vector<ImportStatement>& imports,
                           const std::string& parent = "") {
        const char* type = ts_node_type(node);

        // Use declarations (imports)
        if (strcmp(type, "use_declaration") == 0) {
            // Recursively extract use paths
            std::function<void(TSNode, std::string)> extract_use_paths;
            extract_use_paths = [&](TSNode n, std::string prefix) {
                const char* t = ts_node_type(n);

                if (strcmp(t, "identifier") == 0 || strcmp(t, "scoped_identifier") == 0) {
                    ImportStatement imp;
                    imp.file_path = path;
                    imp.import_path = prefix.empty() ? node_text(n, source)
                                                    : prefix + "::" + node_text(n, source);
                    imp.line = node_line(n);
                    imports.push_back(imp);
                } else if (strcmp(t, "use_wildcard") == 0) {
                    ImportStatement imp;
                    imp.file_path = path;
                    imp.import_path = prefix + "::*";
                    imp.line = node_line(n);
                    imports.push_back(imp);
                } else if (strcmp(t, "use_list") == 0) {
                    uint32_t count = ts_node_child_count(n);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_use_paths(ts_node_child(n, i), prefix);
                    }
                } else if (strcmp(t, "scoped_use_list") == 0) {
                    TSNode scope = find_child(n, "path");
                    TSNode list = find_child(n, "use_list");
                    std::string new_prefix = prefix;
                    if (!ts_node_is_null(scope)) {
                        new_prefix = prefix.empty() ? node_text(scope, source)
                                                   : prefix + "::" + node_text(scope, source);
                    }
                    if (!ts_node_is_null(list)) {
                        extract_use_paths(list, new_prefix);
                    }
                } else if (strcmp(t, "use_as_clause") == 0) {
                    TSNode path_node = find_child(n, "path");
                    TSNode alias_node = find_child(n, "identifier");
                    ImportStatement imp;
                    imp.file_path = path;
                    if (!ts_node_is_null(path_node)) {
                        imp.import_path = prefix.empty() ? node_text(path_node, source)
                                                        : prefix + "::" + node_text(path_node, source);
                    }
                    if (!ts_node_is_null(alias_node)) {
                        imp.alias = node_text(alias_node, source);
                    }
                    imp.line = node_line(n);
                    imports.push_back(imp);
                } else {
                    uint32_t count = ts_node_child_count(n);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_use_paths(ts_node_child(n, i), prefix);
                    }
                }
            };

            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; i++) {
                extract_use_paths(ts_node_child(node, i), "");
            }
        }
        // Function definitions
        else if (strcmp(type, "function_item") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        }
        // Impl blocks with trait detection
        else if (strcmp(type, "impl_item") == 0) {
            TSNode type_node = find_child(node, "type_identifier");
            std::string impl_name;
            if (!ts_node_is_null(type_node)) {
                impl_name = node_text(type_node, source);
                ExtractedSymbol sym;
                sym.kind = "impl";
                sym.name = impl_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Check for trait implementation: impl Trait for Type
                // Look for "for" keyword which indicates trait impl
                uint32_t child_count = ts_node_child_count(node);
                TSNode trait_node = {};
                bool found_for = false;
                for (uint32_t i = 0; i < child_count; i++) {
                    TSNode child = ts_node_child(node, i);
                    const char* child_type = ts_node_type(child);
                    if (strcmp(child_type, "type_identifier") == 0 && !found_for) {
                        trait_node = child;
                    }
                    if (strcmp(node_text(child, source).c_str(), "for") == 0) {
                        found_for = true;
                    }
                }
                if (found_for && !ts_node_is_null(trait_node)) {
                    TypeRelationship rel;
                    rel.derived_name = impl_name;
                    rel.base_name = node_text(trait_node, source);
                    rel.relationship = "implements";
                    rel.file_path = path;
                    rel.line = node_line(trait_node);
                    type_rels.push_back(rel);
                }
            }
            // Extract methods inside impl block
            TSNode body = find_child(node, "declaration_list");
            if (!ts_node_is_null(body)) {
                uint32_t count = ts_node_child_count(body);
                for (uint32_t i = 0; i < count; i++) {
                    extract_rust_full(ts_node_child(body, i), source, path,
                                     symbols, callsites, type_rels, imports, impl_name);
                }
                return;
            }
        }
        // Struct definitions
        else if (strcmp(type, "struct_item") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = "struct";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);
            }
        }
        // Trait definitions
        else if (strcmp(type, "trait_item") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = "trait";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);
            }
        }
        // Call expressions
        else if (strcmp(type, "call_expression") == 0) {
            TSNode func_node = find_child_by_field(node, "function");
            TSNode args_node = find_child_by_field(node, "arguments");
            if (ts_node_is_null(func_node) && ts_node_child_count(node) > 0) {
                func_node = ts_node_child(node, 0);
            }
            if (!ts_node_is_null(func_node)) {
                Callsite cs;
                cs.file_path = path;
                cs.start_byte = ts_node_start_byte(node);
                cs.end_byte = ts_node_end_byte(node);
                cs.line = node_line(node);
                cs.column = ts_node_start_point(node).column;
                cs.caller_symbol = parent;
                cs.arg_count = count_args(args_node);
                cs.callee_text = node_text(func_node, source);

                const char* func_type = ts_node_type(func_node);
                if (strcmp(func_type, "field_expression") == 0) {
                    cs.kind = CallKind::MemberCall;
                    TSNode field = find_child_by_field(func_node, "field");
                    if (!ts_node_is_null(field)) {
                        cs.callee_leaf = node_text(field, source);
                    }
                    TSNode value = find_child_by_field(func_node, "value");
                    if (!ts_node_is_null(value)) {
                        cs.receiver_text = node_text(value, source);
                    }
                    cs.member_op = ".";
                } else if (strcmp(func_type, "scoped_identifier") == 0) {
                    cs.kind = CallKind::Qualified;
                    TSNode path_node = find_child_by_field(func_node, "path");
                    if (!ts_node_is_null(path_node)) {
                        cs.scope_text = node_text(path_node, source);
                    }
                    TSNode name = find_child_by_field(func_node, "name");
                    if (!ts_node_is_null(name)) {
                        cs.callee_leaf = node_text(name, source);
                    }
                } else if (strcmp(func_type, "identifier") == 0) {
                    cs.kind = CallKind::Call;
                    cs.callee_leaf = cs.callee_text;
                } else {
                    cs.kind = CallKind::Indirect;
                    cs.callee_leaf = cs.callee_text;
                }
                callsites.push_back(cs);
            }
        }

        // Recurse
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_rust_full(ts_node_child(node, i), source, path,
                             symbols, callsites, type_rels, imports, parent);
        }
    }

    // Python extraction
    void extract_python(TSNode node, const std::string& source,
                        const std::string& path, std::vector<ExtractedSymbol>& symbols,
                        const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_definition") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "class_definition") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Extract methods from class body
                TSNode body = find_child(node, "block");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_python(ts_node_child(body, i), source, path, symbols, class_name);
                    }
                }
                return;
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_python(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // JavaScript/TypeScript extraction
    void extract_js(TSNode node, const std::string& source,
                    const std::string& path, std::vector<ExtractedSymbol>& symbols,
                    const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "method_definition") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "property_identifier");
            }
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "class_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                TSNode body = find_child(node, "class_body");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_js(ts_node_child(body, i), source, path, symbols, class_name);
                    }
                }
                return;
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_js(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // Go extraction
    void extract_go(TSNode node, const std::string& source,
                    const std::string& path, std::vector<ExtractedSymbol>& symbols,
                    const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "method_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "field_identifier");
            }
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = strcmp(type, "method_declaration") == 0 ? "method" : "function";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "type_declaration") == 0) {
            TSNode spec = find_child(node, "type_spec");
            if (!ts_node_is_null(spec)) {
                TSNode name_node = find_child(spec, "type_identifier");
                if (!ts_node_is_null(name_node)) {
                    ExtractedSymbol sym;
                    sym.kind = "type";
                    sym.name = node_text(name_node, source);
                    sym.file_path = path;
                    sym.line_start = node_line(node);
                    sym.line_end = node_end_line(node);
                    symbols.push_back(sym);
                }
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_go(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // Rust extraction
    void extract_rust(TSNode node, const std::string& source,
                      const std::string& path, std::vector<ExtractedSymbol>& symbols,
                      const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_item") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "struct_item") == 0 || strcmp(type, "impl_item") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string struct_name = node_text(name_node, source);
                if (strcmp(type, "struct_item") == 0) {
                    ExtractedSymbol sym;
                    sym.kind = "struct";
                    sym.name = struct_name;
                    sym.file_path = path;
                    sym.line_start = node_line(node);
                    sym.line_end = node_end_line(node);
                    symbols.push_back(sym);
                }

                // Extract impl methods
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; i++) {
                    extract_rust(ts_node_child(node, i), source, path, symbols, struct_name);
                }
                return;
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_rust(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // Java extraction
    void extract_java(TSNode node, const std::string& source,
                      const std::string& path, std::vector<ExtractedSymbol>& symbols,
                      const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "method_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "class_declaration") == 0 ||
                   strcmp(type, "interface_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = strcmp(type, "interface_declaration") == 0 ? "interface" : "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                TSNode body = find_child(node, "class_body");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_java(ts_node_child(body, i), source, path, symbols, class_name);
                    }
                }
                return;
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_java(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // Ruby extraction
    void extract_ruby(TSNode node, const std::string& source,
                      const std::string& path, std::vector<ExtractedSymbol>& symbols,
                      const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "method") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "class") == 0 || strcmp(type, "module") == 0) {
            TSNode name_node = find_child(node, "constant");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = strcmp(type, "module") == 0 ? "module" : "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; i++) {
                    extract_ruby(ts_node_child(node, i), source, path, symbols, class_name);
                }
                return;
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_ruby(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // C# extraction
    void extract_csharp(TSNode node, const std::string& source,
                        const std::string& path, std::vector<ExtractedSymbol>& symbols,
                        const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "method_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "class_declaration") == 0 ||
                   strcmp(type, "interface_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                std::string class_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = strcmp(type, "interface_declaration") == 0 ? "interface" : "class";
                sym.name = class_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                TSNode body = find_child(node, "declaration_list");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_csharp(ts_node_child(body, i), source, path, symbols, class_name);
                    }
                }
                return;
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_csharp(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // Swift extraction
    // Node types from alex-pinkus/tree-sitter-swift:
    //   class_declaration (covers class, struct, enum, actor via keyword child)
    //   protocol_declaration, function_declaration, init_declaration, property_declaration
    void extract_swift(TSNode node, const std::string& source,
                       const std::string& path, std::vector<ExtractedSymbol>& symbols,
                       const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "protocol_function_declaration") == 0) {
            TSNode name_node = find_child(node, "simple_identifier");
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
            return;  // Don't recurse into function bodies (local let/var are property_declaration)
        } else if (strcmp(type, "init_declaration") == 0) {
            ExtractedSymbol sym;
            sym.kind = "method";
            sym.name = "init";
            sym.file_path = path;
            sym.line_start = node_line(node);
            sym.line_end = node_end_line(node);
            sym.parent = parent;
            symbols.push_back(sym);
            return;  // Don't recurse into init bodies
        } else if (strcmp(type, "class_declaration") == 0) {
            // class_declaration covers class, struct, enum, actor
            // The first keyword child determines which kind
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string decl_name = node_text(name_node, source);

                // Determine kind from the keyword child
                std::string kind = "class";
                uint32_t child_count = ts_node_child_count(node);
                for (uint32_t i = 0; i < child_count; i++) {
                    TSNode child = ts_node_child(node, i);
                    std::string child_text = node_text(child, source);
                    if (child_text == "struct") { kind = "struct"; break; }
                    if (child_text == "enum") { kind = "enum"; break; }
                    if (child_text == "actor") { kind = "class"; break; }
                    if (child_text == "class") { kind = "class"; break; }
                    // Stop at body
                    if (strcmp(ts_node_type(child), "class_body") == 0) break;
                }

                ExtractedSymbol sym;
                sym.kind = kind;
                sym.name = decl_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // class/struct/actor use class_body, enum uses enum_class_body
                TSNode body = find_child(node, "class_body");
                if (ts_node_is_null(body)) {
                    body = find_child(node, "enum_class_body");
                }
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_swift(ts_node_child(body, i), source, path, symbols, decl_name);
                    }
                }
                return;
            }
        } else if (strcmp(type, "protocol_declaration") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string proto_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "interface";
                sym.name = proto_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                TSNode body = find_child(node, "protocol_body");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_swift(ts_node_child(body, i), source, path, symbols, proto_name);
                    }
                }
                return;
            }
        } else if (strcmp(type, "property_declaration") == 0 ||
                   strcmp(type, "protocol_property_declaration") == 0) {
            // Properties: look for pattern -> simple_identifier
            TSNode pattern = find_child(node, "pattern");
            if (!ts_node_is_null(pattern)) {
                TSNode name_node = find_child(pattern, "simple_identifier");
                if (ts_node_is_null(name_node)) {
                    // Pattern might be the identifier itself
                    if (strcmp(ts_node_type(pattern), "simple_identifier") == 0) {
                        name_node = pattern;
                    }
                }
                if (!ts_node_is_null(name_node)) {
                    ExtractedSymbol sym;
                    sym.kind = "variable";
                    sym.name = node_text(name_node, source);
                    sym.file_path = path;
                    sym.line_start = node_line(node);
                    sym.line_end = node_end_line(node);
                    sym.parent = parent;
                    symbols.push_back(sym);
                }
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_swift(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Swift Full Extraction (Symbols + Callsites + Types + Imports)
    // ═══════════════════════════════════════════════════════════════════════

    void extract_swift_full(TSNode node, const std::string& source,
                            const std::string& path,
                            std::vector<ExtractedSymbol>& symbols,
                            std::vector<Callsite>& callsites,
                            std::vector<TypeRelationship>& type_rels,
                            std::vector<ImportStatement>& imports,
                            const std::string& parent = "") {
        const char* type = ts_node_type(node);

        // Import declarations: import Foundation, import UIKit
        if (strcmp(type, "import_declaration") == 0) {
            TSNode id_node = find_child(node, "identifier");
            if (!ts_node_is_null(id_node)) {
                ImportStatement imp;
                imp.file_path = path;
                imp.import_path = node_text(id_node, source);
                imp.line = node_line(node);
                imports.push_back(imp);
            }
        }
        // Function declarations
        else if (strcmp(type, "function_declaration") == 0 ||
                 strcmp(type, "protocol_function_declaration") == 0) {
            TSNode name_node = find_child(node, "simple_identifier");
            std::string func_name;
            if (!ts_node_is_null(name_node)) {
                func_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = func_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
            // Build caller symbol ID for callsites
            std::string caller = parent.empty() ? func_name
                                                : parent + "." + func_name;
            // Recurse into function body for callsites
            TSNode body = find_child(node, "function_body");
            if (!ts_node_is_null(body)) {
                uint32_t count = ts_node_child_count(body);
                for (uint32_t i = 0; i < count; i++) {
                    extract_swift_full(ts_node_child(body, i), source, path,
                                      symbols, callsites, type_rels, imports, caller);
                }
            }
            return;
        }
        // Init declarations
        else if (strcmp(type, "init_declaration") == 0) {
            ExtractedSymbol sym;
            sym.kind = "method";
            sym.name = "init";
            sym.file_path = path;
            sym.line_start = node_line(node);
            sym.line_end = node_end_line(node);
            sym.parent = parent;
            symbols.push_back(sym);

            std::string caller = parent.empty() ? "init" : parent + ".init";
            TSNode body = find_child(node, "function_body");
            if (!ts_node_is_null(body)) {
                uint32_t count = ts_node_child_count(body);
                for (uint32_t i = 0; i < count; i++) {
                    extract_swift_full(ts_node_child(body, i), source, path,
                                      symbols, callsites, type_rels, imports, caller);
                }
            }
            return;
        }
        // Class/struct/enum/actor declarations
        else if (strcmp(type, "class_declaration") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string decl_name = node_text(name_node, source);

                // Determine kind from keyword child
                std::string kind = "class";
                uint32_t child_count = ts_node_child_count(node);
                for (uint32_t i = 0; i < child_count; i++) {
                    TSNode child = ts_node_child(node, i);
                    std::string child_text = node_text(child, source);
                    if (child_text == "struct") { kind = "struct"; break; }
                    if (child_text == "enum") { kind = "enum"; break; }
                    if (child_text == "actor") { kind = "class"; break; }
                    if (child_text == "class") { kind = "class"; break; }
                    if (strcmp(ts_node_type(child), "class_body") == 0 ||
                        strcmp(ts_node_type(child), "enum_class_body") == 0) break;
                }

                ExtractedSymbol sym;
                sym.kind = kind;
                sym.name = decl_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Extract inheritance: inheritance_specifier → user_type → type_identifier
                for (uint32_t i = 0; i < child_count; i++) {
                    TSNode child = ts_node_child(node, i);
                    if (strcmp(ts_node_type(child), "inheritance_specifier") == 0) {
                        TSNode user_type = find_child(child, "user_type");
                        if (!ts_node_is_null(user_type)) {
                            TSNode base_name = find_child(user_type, "type_identifier");
                            if (!ts_node_is_null(base_name)) {
                                TypeRelationship rel;
                                rel.derived_name = decl_name;
                                rel.base_name = node_text(base_name, source);
                                rel.relationship = "extends";
                                rel.file_path = path;
                                rel.line = node_line(base_name);
                                type_rels.push_back(rel);
                            }
                        }
                    }
                }

                // Recurse into body
                TSNode body = find_child(node, "class_body");
                if (ts_node_is_null(body)) {
                    body = find_child(node, "enum_class_body");
                }
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_swift_full(ts_node_child(body, i), source, path,
                                          symbols, callsites, type_rels, imports, decl_name);
                    }
                }
                return;
            }
        }
        // Protocol declarations
        else if (strcmp(type, "protocol_declaration") == 0) {
            TSNode name_node = find_child(node, "type_identifier");
            if (!ts_node_is_null(name_node)) {
                std::string proto_name = node_text(name_node, source);
                ExtractedSymbol sym;
                sym.kind = "interface";
                sym.name = proto_name;
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                symbols.push_back(sym);

                // Protocol inheritance
                uint32_t child_count = ts_node_child_count(node);
                for (uint32_t i = 0; i < child_count; i++) {
                    TSNode child = ts_node_child(node, i);
                    if (strcmp(ts_node_type(child), "inheritance_specifier") == 0) {
                        TSNode user_type = find_child(child, "user_type");
                        if (!ts_node_is_null(user_type)) {
                            TSNode base_name = find_child(user_type, "type_identifier");
                            if (!ts_node_is_null(base_name)) {
                                TypeRelationship rel;
                                rel.derived_name = proto_name;
                                rel.base_name = node_text(base_name, source);
                                rel.relationship = "extends";
                                rel.file_path = path;
                                rel.line = node_line(base_name);
                                type_rels.push_back(rel);
                            }
                        }
                    }
                }

                TSNode body = find_child(node, "protocol_body");
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_child_count(body);
                    for (uint32_t i = 0; i < count; i++) {
                        extract_swift_full(ts_node_child(body, i), source, path,
                                          symbols, callsites, type_rels, imports, proto_name);
                    }
                }
                return;
            }
        }
        // Property declarations (class/struct members only — parent is set)
        else if ((strcmp(type, "property_declaration") == 0 ||
                  strcmp(type, "protocol_property_declaration") == 0) &&
                 !parent.empty()) {
            TSNode pattern = find_child(node, "pattern");
            if (!ts_node_is_null(pattern)) {
                TSNode name_node = find_child(pattern, "simple_identifier");
                if (ts_node_is_null(name_node) &&
                    strcmp(ts_node_type(pattern), "simple_identifier") == 0) {
                    name_node = pattern;
                }
                if (!ts_node_is_null(name_node)) {
                    ExtractedSymbol sym;
                    sym.kind = "variable";
                    sym.name = node_text(name_node, source);
                    sym.file_path = path;
                    sym.line_start = node_line(node);
                    sym.line_end = node_end_line(node);
                    sym.parent = parent;
                    symbols.push_back(sym);
                }
            }
        }
        // Call expressions
        // Structure: call_expression → (simple_identifier | navigation_expression) + call_suffix
        else if (strcmp(type, "call_expression") == 0) {
            // First named child is the callee, call_suffix contains args
            TSNode callee_node = {};
            TSNode suffix_node = {};
            uint32_t child_count = ts_node_child_count(node);
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                if (ts_node_is_named(child)) {
                    const char* ct = ts_node_type(child);
                    if (strcmp(ct, "call_suffix") == 0) {
                        suffix_node = child;
                    } else if (ts_node_is_null(callee_node)) {
                        callee_node = child;
                    }
                }
            }

            if (!ts_node_is_null(callee_node)) {
                Callsite cs;
                cs.file_path = path;
                cs.start_byte = ts_node_start_byte(node);
                cs.end_byte = ts_node_end_byte(node);
                cs.line = node_line(node);
                cs.column = ts_node_start_point(node).column;
                cs.caller_symbol = parent;
                cs.callee_text = node_text(callee_node, source);

                // Count args from call_suffix → value_arguments → value_argument
                if (!ts_node_is_null(suffix_node)) {
                    TSNode args = find_child(suffix_node, "value_arguments");
                    if (!ts_node_is_null(args)) {
                        uint32_t arg_count = 0;
                        uint32_t ac = ts_node_child_count(args);
                        for (uint32_t i = 0; i < ac; i++) {
                            if (strcmp(ts_node_type(ts_node_child(args, i)),
                                      "value_argument") == 0) {
                                arg_count++;
                            }
                        }
                        cs.arg_count = arg_count;
                    }
                }

                const char* callee_type = ts_node_type(callee_node);

                if (strcmp(callee_type, "navigation_expression") == 0) {
                    // Member call: receiver.method(...)
                    cs.kind = CallKind::MemberCall;
                    cs.member_op = ".";

                    // Last navigation_suffix contains the method name
                    TSNode nav_suffix = {};
                    TSNode receiver = {};
                    uint32_t nc = ts_node_child_count(callee_node);
                    for (uint32_t i = 0; i < nc; i++) {
                        TSNode child = ts_node_child(callee_node, i);
                        if (strcmp(ts_node_type(child), "navigation_suffix") == 0) {
                            nav_suffix = child;
                        } else if (ts_node_is_named(child) && ts_node_is_null(receiver)) {
                            receiver = child;
                        }
                    }

                    if (!ts_node_is_null(nav_suffix)) {
                        TSNode leaf = find_child(nav_suffix, "simple_identifier");
                        if (!ts_node_is_null(leaf)) {
                            cs.callee_leaf = node_text(leaf, source);
                        }
                    }
                    if (!ts_node_is_null(receiver)) {
                        cs.receiver_text = node_text(receiver, source);
                    }
                } else if (strcmp(callee_type, "simple_identifier") == 0) {
                    // Direct call or constructor: foo(...) or Dog(...)
                    std::string name = node_text(callee_node, source);
                    cs.callee_leaf = name;
                    // Heuristic: uppercase first letter = constructor
                    if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z') {
                        cs.kind = CallKind::Ctor;
                    } else {
                        cs.kind = CallKind::Call;
                    }
                } else {
                    cs.kind = CallKind::Indirect;
                    cs.callee_leaf = cs.callee_text;
                }

                callsites.push_back(cs);
            }

            // Recurse into call_suffix for nested calls in arguments
            if (!ts_node_is_null(suffix_node)) {
                uint32_t sc = ts_node_child_count(suffix_node);
                for (uint32_t i = 0; i < sc; i++) {
                    extract_swift_full(ts_node_child(suffix_node, i), source, path,
                                      symbols, callsites, type_rels, imports, parent);
                }
            }
            // Recurse into callee for nested calls (e.g., foo().bar())
            if (!ts_node_is_null(callee_node)) {
                uint32_t cc = ts_node_child_count(callee_node);
                for (uint32_t i = 0; i < cc; i++) {
                    extract_swift_full(ts_node_child(callee_node, i), source, path,
                                      symbols, callsites, type_rels, imports, parent);
                }
            }
            return;
        }

        // Default: recurse into children
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_swift_full(ts_node_child(node, i), source, path,
                              symbols, callsites, type_rels, imports, parent);
        }
    }

    // Lua extraction
    // Node types from tree-sitter-lua:
    //   function_declaration - global function
    //   local_function_declaration - local function
    //   function_definition - anonymous function
    //   variable_declaration - can contain table definitions
    void extract_lua(TSNode node, const std::string& source,
                     const std::string& path, std::vector<ExtractedSymbol>& symbols,
                     const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "local_function_declaration") == 0) {
            // Find the function name (identifier or dot_index_expression or method_index_expression)
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "dot_index_expression");
            }
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "method_index_expression");
            }
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
            return;
        }

        // Recurse to children
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_lua(ts_node_child(node, i), source, path, symbols, parent);
        }
    }

    // Lua full extraction (with callsites)
    void extract_lua_full(TSNode node, const std::string& source,
                          const std::string& path,
                          std::vector<ExtractedSymbol>& symbols,
                          std::vector<Callsite>& callsites,
                          std::vector<TypeRelationship>& type_rels,
                          std::vector<ImportStatement>& imports,
                          const std::string& parent = "") {
        const char* type = ts_node_type(node);

        if (strcmp(type, "function_declaration") == 0 ||
            strcmp(type, "local_function_declaration") == 0) {
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "dot_index_expression");
            }
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "method_index_expression");
            }
            if (!ts_node_is_null(name_node)) {
                ExtractedSymbol sym;
                sym.kind = parent.empty() ? "function" : "method";
                sym.name = node_text(name_node, source);
                sym.file_path = path;
                sym.line_start = node_line(node);
                sym.line_end = node_end_line(node);
                sym.parent = parent;
                symbols.push_back(sym);
            }
        } else if (strcmp(type, "function_call") == 0) {
            // Extract callsite
            TSNode name_node = find_child(node, "identifier");
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "dot_index_expression");
            }
            if (ts_node_is_null(name_node)) {
                name_node = find_child(node, "method_index_expression");
            }
            if (!ts_node_is_null(name_node)) {
                Callsite cs;
                cs.caller_symbol = parent;
                std::string callee_name = node_text(name_node, source);
                cs.callee_text = callee_name;
                cs.callee_leaf = callee_name;
                cs.file_path = path;
                cs.line = node_line(node);
                callsites.push_back(cs);
            }
        } else if (strcmp(type, "call") == 0) {
            // require("module") pattern
            TSNode prefix = find_child(node, "identifier");
            if (!ts_node_is_null(prefix) && node_text(prefix, source) == "require") {
                TSNode args = find_child(node, "arguments");
                if (!ts_node_is_null(args)) {
                    TSNode str_node = find_child(args, "string");
                    if (!ts_node_is_null(str_node)) {
                        ImportStatement imp;
                        imp.file_path = path;
                        std::string mod = node_text(str_node, source);
                        // Strip quotes
                        if (mod.size() >= 2) {
                            mod = mod.substr(1, mod.size() - 2);
                        }
                        imp.import_path = mod;
                        imp.line = node_line(node);
                        imports.push_back(imp);
                    }
                }
            }
        }

        // Recurse
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            extract_lua_full(ts_node_child(node, i), source, path,
                            symbols, callsites, type_rels, imports, parent);
        }
    }
};

}  // namespace chitta
