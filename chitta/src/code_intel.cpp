#include "chitta/code_intel.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <regex>
#include <openssl/sha.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

extern char** environ;

extern "C" const TSLanguage* tree_sitter_bash();
extern "C" const TSLanguage* tree_sitter_r();
extern "C" const TSLanguage* tree_sitter_julia();
extern "C" const TSLanguage* tree_sitter_fortran();
extern "C" const TSLanguage* tree_sitter_nextflow();
extern "C" const TSLanguage* tree_sitter_snakemake();
extern "C" const TSLanguage* tree_sitter_perl();
extern "C" const TSLanguage* tree_sitter_make();
extern "C" const TSLanguage* tree_sitter_cmake();
extern "C" const TSLanguage* tree_sitter_sql();
extern "C" const TSLanguage* tree_sitter_php();

extern "C" const TSLanguage* tree_sitter_kotlin();

namespace chitta {
const TSLanguage* CodeIntel::extended_grammar(const std::string& language) {
    if (language == "bash") return tree_sitter_bash();
    if (language == "r") return tree_sitter_r();
    if (language == "julia") return tree_sitter_julia();
    if (language == "fortran") return tree_sitter_fortran();
    if (language == "nextflow") return tree_sitter_nextflow();
    if (language == "snakemake") return tree_sitter_snakemake();
    if (language == "perl") return tree_sitter_perl();
    if (language == "make") return tree_sitter_make();
    if (language == "cmake") return tree_sitter_cmake();
    if (language == "sql") return tree_sitter_sql();
#ifdef CHITTA_EXTRA_GRAMMARS
    if (language == "php") return tree_sitter_php();
    if (language == "kotlin") return tree_sitter_kotlin();
#endif
    return nullptr;
}
void CodeIntel::initialize_extended_parsers() {
    for (const auto* language : {"bash", "r", "julia", "fortran", "nextflow", "snakemake", "perl", "make", "cmake", "sql", "kotlin", "php"}) {
        if (!extended_grammar(language)) continue; // Optional grammar group disabled.
        auto* parser = ts_parser_new();
        if (!ts_parser_set_language(parser, extended_grammar(language))) {
            ts_parser_delete(parser);
            throw std::runtime_error(std::string("unsupported grammar ABI: ") + language);
        }
        parsers_[language] = parser;
    }
}
std::string CodeIntel::detect_extended_language(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    if (ext == ".sh" || ext == ".bash") return "bash";
    auto filename = std::filesystem::path(path).filename().string();
    if ((ext == ".kt" || ext == ".kts") && extended_grammar("kotlin")) return "kotlin";
    if ((ext == ".php" || ext == ".phtml") && extended_grammar("php")) return "php";
    if (ext == ".sql" || ext == ".ddl") return "sql";
    if (ext == ".cmake" || filename == "CMakeLists.txt") return "cmake";
    if (ext == ".mk" || ext == ".mak" || filename == "Makefile" || filename == "makefile" || filename == "GNUmakefile" || filename.rfind("Makefile.", 0) == 0) return "make";
    if (ext == ".smk" || filename == "Snakefile" || filename == "snakefile") return "snakemake";
    if (ext == ".pl" || ext == ".pm" || ext == ".t" || ext == ".perl") return "perl";
    if (ext == ".nf") return "nextflow";
    if (ext == ".jl") return "julia";
    if (ext == ".R" || ext == ".r" || std::filesystem::path(path).filename() == ".Rprofile") return "r";
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".f" || ext == ".for" || ext == ".f77" || ext == ".f90" || ext == ".f95" || ext == ".f03" || ext == ".f08") return "fortran";
    return {};
}

void CodeIntel::extract_extended(TSNode root, const std::string& source,
                                const std::string& path, const std::string& language,
                                ExtractionResult& result) {
    // Optional fields and error-recovery nodes are null during partial edits.
    // Tree-sitter's C child/count APIs require a non-null node.
    auto ast_type = [](TSNode node) { return ts_node_is_null(node) ? "" : ts_node_type(node); };
    auto ast_child_count = [](TSNode node) { return ts_node_is_null(node) ? 0u : ts_node_child_count(node); };
    auto ast_named_count = [](TSNode node) { return ts_node_is_null(node) ? 0u : ts_node_named_child_count(node); };
    auto ast_child = [](TSNode node, uint32_t i) { return ts_node_is_null(node) ? TSNode{} : ts_node_child(node, i); };
    auto ast_named_child = [](TSNode node, uint32_t i) { return ts_node_is_null(node) ? TSNode{} : ts_node_named_child(node, i); };
    auto ast_find = [&](TSNode node, const char* type) { return ts_node_is_null(node) ? TSNode{} : find_child(node, type); };
    if (language == "snakemake")
        extract_python_full(root, source, path, result.symbols, result.callsites, result.type_relationships, result.imports);
    auto field = [](TSNode node, const char* name) {
        return ts_node_is_null(node) ? TSNode{} : ts_node_child_by_field_name(node, name, std::strlen(name));
    };
    auto text = [&](TSNode node) { return ts_node_is_null(node) ? std::string{} : node_text(node, source); };
    auto literal = [&](TSNode node) {
        auto value = text(node);
        if (value.size() > 1 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        return value;
    };
    auto define = [&](TSNode node, const std::string& name, const std::string& kind,
                      const std::string& parent, TSNode body = TSNode{}) {
        if (name.empty()) return;
        auto signature = text(node);
        if (!ts_node_is_null(body)) signature.resize(ts_node_start_byte(body) - ts_node_start_byte(node));
        if (ts_node_is_null(body)) {
            auto newline = signature.find('\n');
            if (newline != std::string::npos) signature.resize(newline);
        }
        while (!signature.empty() && std::isspace(static_cast<unsigned char>(signature.back()))) signature.pop_back();
        auto end = ts_node_end_point(node);
        auto last = std::max(node_line(node), int(end.row) + (end.column ? 1 : 0));
        result.symbols.push_back({kind, name, signature, path, node_line(node), last, parent});
    };
    auto call = [&](TSNode node, const std::string& name, const std::string& parent,
                    const std::string& scope = "", const std::string& receiver = "") {
        if (name.empty()) return;
        Callsite site;
        site.file_path = path; site.start_byte = ts_node_start_byte(node); site.end_byte = ts_node_end_byte(node);
        site.line = node_line(node); site.column = ts_node_start_point(node).column + 1;
        site.caller_symbol = parent; site.callee_text = name; site.callee_leaf = name;
        site.scope_text = scope; site.receiver_text = receiver;
        if (!scope.empty()) site.kind = CallKind::Qualified;
        if (!receiver.empty()) site.kind = CallKind::MemberCall;
        result.callsites.push_back(std::move(site));
    };
    std::function<std::string(TSNode)> leaf_name = [&](TSNode node) -> std::string {
        if (ts_node_is_null(node)) return {};
        std::string type = ast_type(node);
        if (type == "identifier" || type == "type_identifier" || type == "name") return text(node);
        auto count = ast_named_count(node);
        if (!count) return {};
        if (type == "field_expression" || type == "dot_expression") return leaf_name(ast_named_child(node, count - 1));
        return leaf_name(ast_named_child(node, 0));
    };
    std::string perl_package;
    std::function<void(TSNode, std::string)> visit = [&](TSNode node, std::string parent) {
        std::string type = ast_type(node);
        if (language == "bash" && type == "function_definition") {
            auto name = text(field(node, "name"));
            auto body = field(node, "body");
            auto signature = source.substr(ts_node_start_byte(node), ts_node_start_byte(body) - ts_node_start_byte(node));
            while (!signature.empty() && std::isspace(static_cast<unsigned char>(signature.back()))) signature.pop_back();
            result.symbols.push_back({"function", name, signature, path, node_line(node), node_end_line(node), parent});
            parent = name;
        }
        if (language == "bash" && type == "command") {
            auto name = text(field(node, "name"));
            if (name == "source" || name == ".") {
                auto target = literal(field(node, "argument"));
                if (!target.empty()) result.imports.push_back({path, target, "", {}, uint32_t(node_line(node))});
            } else if (!name.empty() && name.find_first_of("$`\"'") == std::string::npos) {
                Callsite call;
                call.file_path = path; call.start_byte = ts_node_start_byte(node); call.end_byte = ts_node_end_byte(node);
                call.line = node_line(node); call.column = ts_node_start_point(node).column + 1;
                call.caller_symbol = parent; call.callee_text = name; call.callee_leaf = name;
                result.callsites.push_back(std::move(call));
            }
        }
        if (language == "r") {
            if (type == "binary_operator" || type == "argument") {
                auto value = field(node, type == "argument" ? "value" : "rhs");
                auto name = field(node, type == "argument" ? "name" : "lhs");
                auto op = text(field(node, "operator"));
                if (op == "->" || op == "->>") std::swap(value, name);
                while (!ts_node_is_null(value) && std::strcmp(ast_type(value), "parenthesized_expression") == 0)
                    value = ast_named_child(value, 0);
                const bool assignment = type == "argument" || op == "<-" || op == "<<-" || op == "=" || op == "->" || op == "->>";
                if (assignment && !ts_node_is_null(value) && std::strcmp(ast_type(value), "function_definition") == 0 && !ts_node_is_null(name)) {
                    const bool class_scope = std::any_of(result.symbols.begin(), result.symbols.end(), [&](const auto& s) {
                        return s.kind == "class" && s.name == parent;
                    });
                    // A named callback argument is not a global definition.
                    // R6's public/private method lists do name class methods.
                    if (type != "argument" || class_scope) {
                        auto function = literal(name);
                        define(node, function, class_scope ? "method" : "function", parent, field(value, "body"));
                        parent = function;
                    }
                }
            }
            if (type == "call") {
                auto function = field(node, "function");
                auto args = field(node, "arguments");
                auto function_type = ts_node_is_null(function) ? std::string{} : std::string(ast_type(function));
                auto name = text(function), scope = std::string{}, receiver = std::string{};
                if (function_type == "namespace_operator" || function_type == "extract_operator") {
                    name = text(field(function, "rhs"));
                    (function_type == "namespace_operator" ? scope : receiver) = text(field(function, "lhs"));
                }
                if (function_type == "identifier" || function_type == "namespace_operator" || function_type == "extract_operator")
                    call(node, name, parent, scope, receiver);
                auto first = field(ast_named_child(args, 0), "value");
                if ((name == "source" || name == "library" || name == "require") && !ts_node_is_null(first))
                    result.imports.push_back({path, literal(first), "", {}, uint32_t(node_line(node))});
                if ((name == "setClass" || name == "R6Class") && !ts_node_is_null(first) && std::strcmp(ast_type(first), "string") == 0) {
                    auto class_name = literal(first);
                    define(node, class_name, "class", parent);
                    parent = class_name;
                    for (uint32_t i = 0; i < ast_named_count(args); ++i) {
                        auto arg = ast_named_child(args, i);
                        auto key = text(field(arg, "name"));
                        if (key != "contains" && key != "inherit") continue;
                        auto value = field(arg, "value");
                        if (ts_node_is_null(value)) continue;
                        auto base = literal(value);
                        if (std::strcmp(ast_type(value), "identifier") == 0 || std::strcmp(ast_type(value), "string") == 0)
                            result.type_relationships.push_back({class_name, base, "extends", path, uint32_t(node_line(arg))});
                    }
                }
            }
            if (type == "namespace_operator")
                result.imports.push_back({path, text(field(node, "lhs")), "", {}, uint32_t(node_line(node))});
        }
        if (language == "julia") {
            auto first = ast_named_child(node, 0);
            if (type == "module_definition" || type == "struct_definition" || type == "abstract_definition") {
                auto name = type == "module_definition" ? text(field(node, "name")) : leaf_name(first);
                define(node, name, type == "module_definition" ? "module" : type == "struct_definition" ? "struct" : "class", parent);
                if (type != "module_definition") {
                    auto head = ast_named_child(first, 0);
                    if (!ts_node_is_null(head) && std::strcmp(ast_type(head), "binary_expression") == 0 &&
                        text(ast_named_child(head, 1)) == "<:")
                        result.type_relationships.push_back({name, leaf_name(ast_named_child(head, 2)), "extends", path, uint32_t(node_line(node))});
                }
                parent = name;
            }
            if (type == "function_definition" || type == "macro_definition" ||
                (type == "assignment" && !ts_node_is_null(first) && std::strcmp(ast_type(first), "call_expression") == 0)) {
                auto name = leaf_name(first);
                define(node, name, type == "macro_definition" ? "macro" : "function", parent);
                if (!name.empty()) result.symbols.back().signature = text(first);
                for (uint32_t i = 1; i < ast_named_count(node); ++i) visit(ast_named_child(node, i), name);
                return; // The declaration's call-shaped signature is not a call.
            }
            if (type == "using_statement" || type == "import_statement") {
                for (uint32_t i = 0; i < ast_named_count(node); ++i) {
                    auto module = ast_named_child(node, i);
                    std::vector<std::string> imported;
                    if (std::strcmp(ast_type(module), "selected_import") == 0) {
                        for (uint32_t j = 1; j < ast_named_count(module); ++j) imported.push_back(text(ast_named_child(module, j)));
                        module = ast_named_child(module, 0);
                    }
                    result.imports.push_back({path, text(module), "", imported, uint32_t(node_line(node))});
                }
            }
            if (type == "call_expression") {
                auto name = leaf_name(first);
                auto surface = text(first);
                std::string scope, receiver;
                auto dot = surface.rfind('.');
                if (dot != std::string::npos) {
                    receiver = surface.substr(0, dot);
                    const bool module = std::any_of(result.imports.begin(), result.imports.end(), [&](const auto& i) { return i.import_path == receiver; }) ||
                        std::any_of(result.symbols.begin(), result.symbols.end(), [&](const auto& s) { return s.kind == "module" && s.name == receiver; });
                    if (module) { scope = receiver; receiver.clear(); }
                }
                call(node, name, parent, scope, receiver);
                if (name == "include") {
                    auto arg = ast_named_child(ast_named_child(node, 1), 0);
                    if (!ts_node_is_null(arg) && std::strcmp(ast_type(arg), "string_literal") == 0)
                        result.imports.push_back({path, literal(arg), "", {}, uint32_t(node_line(node))});
                }
            }
        }
        if (language == "fortran") {
            auto folded = [](std::string value) {
                for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return value;
            };
            if (type == "module" || type == "subroutine" || type == "function" || type == "program" || type == "derived_type_definition") {
                auto statement = ast_named_child(node, 0);
                auto name_node = field(statement, "name");
                if (ts_node_is_null(name_node)) name_node = ast_find(statement, type == "derived_type_definition" ? "type_name" : "name");
                auto name = folded(text(name_node));
                define(node, name, type == "derived_type_definition" ? "struct" : type == "program" ? "module" : type, parent);
                if (!name.empty()) result.symbols.back().signature = text(statement);
                auto base = field(statement, "base");
                if (!ts_node_is_null(base))
                    result.type_relationships.push_back({name, folded(leaf_name(base)), "extends", path, uint32_t(node_line(statement))});
                parent = name;
            }
            if (type == "use_statement") {
                auto module = ast_find(node, "module_name");
                if (!ts_node_is_null(module)) result.imports.push_back({path, folded(text(module)), "", {}, uint32_t(node_line(node))});
            }
            if (type == "include_statement")
                result.imports.push_back({path, literal(field(node, "path")), "", {}, uint32_t(node_line(node))});
            if (type == "subroutine_call" || type == "call_expression") {
                auto callee = type == "subroutine_call" ? field(node, "subroutine") : ast_named_child(node, 0);
                auto name = folded(text(callee));
                std::string receiver;
                auto member = name.rfind('%');
                if (member != std::string::npos) { receiver = name.substr(0, member); name = name.substr(member + 1); }
                call(node, name, parent, "", receiver);
            }
        }
        if (language == "nextflow") {
            if (type == "process_definition" || type == "workflow_definition" || type == "function_definition") {
                auto name_node = ast_find(node, "identifier");
                if (ts_node_eq(name_node, field(node, "return_type"))) {
                    for (uint32_t i = 0; i < ast_named_count(node); ++i) {
                        auto child = ast_named_child(node, i);
                        if (std::strcmp(ast_type(child), "identifier") == 0 && !ts_node_eq(child, name_node)) { name_node = child; break; }
                    }
                }
                auto name = text(name_node);
                if (name.empty() && type == "workflow_definition") name = "<workflow>";
                define(node, name, type == "process_definition" ? "process" : type == "workflow_definition" ? "workflow" : "function", parent);
                parent = name;
            }
            if (type == "include") {
                auto target = ast_find(node, "string");
                result.imports.push_back({path, literal(target), "", {}, uint32_t(node_line(node))});
            }
            auto channel = [&](TSNode evidence, const std::string& producer, const std::string& consumer) {
                if (producer.empty() || consumer.empty()) return;
                call(evidence, consumer, producer);
                result.callsites.back().kind = CallKind::Channel;
            };
            if (type == "function_call") {
                auto name = text(ast_named_child(node, 0));
                call(node, name, parent);
                for (uint32_t i = 1; i < ast_named_count(node); ++i) {
                    auto argument = ast_named_child(node, i);
                    while (!ts_node_is_null(argument) &&
                        (std::strcmp(ast_type(argument), "simple_expression") == 0 || std::strcmp(ast_type(argument), "parenthesized_expression") == 0))
                        argument = ast_named_child(argument, 0);
                    if (!ts_node_is_null(argument) && std::strcmp(ast_type(argument), "process_output") == 0)
                        channel(argument, text(ast_named_child(argument, 0)), name);
                }
            }
            if (type == "pipe_expression") {
                auto lhs = ast_named_child(node, 0);
                auto rhs = ast_named_child(node, 1);
                auto operation = ast_named_child(rhs, 0);
                auto consumer = leaf_name(operation);
                if (!ts_node_is_null(operation) && std::strcmp(ast_type(operation), "identifier") == 0)
                    call(operation, consumer, parent);
                std::string producer;
                if (!ts_node_is_null(lhs) && std::strcmp(ast_type(lhs), "pipe_expression") == 0)
                    producer = leaf_name(ast_named_child(lhs, 1));
                else if (!ts_node_is_null(lhs) && std::strcmp(ast_type(lhs), "process_output") == 0)
                    producer = text(ast_named_child(lhs, 0));
                channel(node, producer, consumer);
            }
            if (type == "method_call") {
                auto receiver = text(ast_named_child(node, 0));
                std::string name;
                for (uint32_t i = 1; i < ast_child_count(node); ++i) {
                    auto child = ast_child(node, i);
                    if (std::strcmp(ast_type(child), "(") == 0 || std::strcmp(ast_type(child), "closure") == 0) break;
                    if (std::strcmp(ast_type(child), "identifier") == 0) name = text(child);
                }
                call(node, name, parent, "", receiver);
            }
            if (type == "channel_of" || type == "channel_from" || type == "channel_from_list" ||
                type == "channel_value" || type == "channel_factory") {
                auto name = type == "channel_factory" ? text(ast_find(node, "identifier")) :
                    type == "channel_from_list" ? std::string("fromList") : type.substr(8);
                call(node, name, parent, "Channel");
            }
        }
        if (language == "snakemake") {
            if (type == "rule_definition" || type == "checkpoint_definition" || type == "module_definition") {
                auto name = text(field(node, "name"));
                if (name.empty()) name = "<rule>";
                define(node, name, type == "module_definition" ? "module" : type == "checkpoint_definition" ? "checkpoint" : "rule", parent, field(node, "body"));
                parent = name;
            }
            if (type == "directive") {
                auto directive = text(ast_child(node, 0));
                if (!parent.empty() && (directive == "input" || directive == "output" || directive == "params")) {
                    define(node, parent + "." + directive, directive, parent);
                    result.symbols.back().signature = text(node);
                }
                if (directive == "include" || directive == "snakefile" || directive == "configfile") {
                    auto value = ast_named_child(field(node, "arguments"), 0);
                    if (!ts_node_is_null(value) && std::strcmp(ast_type(value), "string") == 0)
                        result.imports.push_back({path, literal(value), "", {}, uint32_t(node_line(node))});
                }
            }
            if (type == "attribute" && text(field(node, "attribute")) == "output" && !parent.empty()) {
                auto producer = field(node, "object");
                if (!ts_node_is_null(producer) && std::strcmp(ast_type(producer), "attribute") == 0 && text(field(producer, "object")) == "rules") {
                    call(node, parent, text(field(producer, "attribute")));
                    result.callsites.back().kind = CallKind::Channel;
                }
            }
        }
        if (language == "perl") {
            if (type == "package_statement") {
                auto previous = perl_package;
                perl_package = text(field(node, "name"));
                define(node, perl_package, "module", "");
                auto block = ast_find(node, "block");
                if (!ts_node_is_null(block)) {
                    visit(block, perl_package);
                    perl_package = previous;
                    return;
                }
            }
            if (type == "subroutine_declaration_statement") {
                auto name = text(field(node, "name")), scope = perl_package;
                auto split = name.rfind("::");
                if (split != std::string::npos) { scope = name.substr(0, split); name = name.substr(split + 2); }
                define(node, name, "function", scope, field(node, "body"));
                parent = name;
            }
            if (type == "use_statement") {
                auto module = text(field(node, "module"));
                result.imports.push_back({path, module, "", {}, uint32_t(node_line(node))});
                if ((module == "parent" || module == "base") && !perl_package.empty()) {
                    for (uint32_t i = 0; i < ast_named_count(node); ++i) {
                        auto value = ast_named_child(node, i);
                        if (std::strcmp(ast_type(value), "string_literal") == 0)
                            result.type_relationships.push_back({perl_package, literal(value), "extends", path, uint32_t(node_line(node))});
                    }
                }
            }
            if (type == "require_expression") {
                auto value = ast_named_child(node, 0);
                if (!ts_node_is_null(value)) result.imports.push_back({path, literal(value), "", {}, uint32_t(node_line(node))});
            }
            if (type == "function_call_expression" || type == "ambiguous_function_call_expression") {
                auto name = text(field(node, "function")), scope = std::string{};
                auto split = name.rfind("::");
                if (split != std::string::npos) { scope = name.substr(0, split); name = name.substr(split + 2); }
                call(node, name, parent, scope);
            }
            if (type == "method_call_expression")
                call(node, text(field(node, "method")), parent, "", text(field(node, "invocant")));
        }
        if (language == "make") {
            if (type == "rule") {
                static const std::unordered_set<std::string> special = {".PHONY", ".SUFFIXES", ".DEFAULT", ".PRECIOUS", ".INTERMEDIATE", ".SECONDARY", ".SECONDEXPANSION", ".DELETE_ON_ERROR", ".IGNORE", ".LOW_RESOLUTION_TIME", ".SILENT", ".EXPORT_ALL_VARIABLES", ".NOTPARALLEL", ".ONESHELL", ".POSIX"};
                auto targets = ast_find(node, "targets");
                for (uint32_t i = 0; i < ast_named_count(targets); ++i) {
                    auto target = ast_named_child(targets, i);
                    auto name = text(target);
                    if (std::strcmp(ast_type(target), "word") != 0 || special.count(name)) continue;
                    define(node, name, "target", parent);
                    for (const auto* edge : {"normal", "order_only"}) {
                        auto dependencies = field(node, edge);
                        for (uint32_t j = 0; j < ast_named_count(dependencies); ++j) {
                            auto dependency = ast_named_child(dependencies, j);
                            if (std::strcmp(ast_type(dependency), "word") != 0) continue;
                            call(dependency, text(dependency), name);
                            result.callsites.back().kind = CallKind::Channel;
                        }
                    }
                }
            }
            if (type == "define_directive") {
                auto name = text(field(node, "name"));
                define(node, name, "function", parent, field(node, "value"));
                parent = name;
            }
            if (type == "include_directive") {
                auto files = field(node, "filenames");
                for (uint32_t i = 0; i < ast_named_count(files); ++i) {
                    auto file = ast_named_child(files, i);
                    if (std::strcmp(ast_type(file), "word") == 0)
                        result.imports.push_back({path, text(file), "", {}, uint32_t(node_line(node))});
                }
            }
            if (type == "function_call" || type == "shell_function") {
                auto name = text(field(node, "function"));
                if (name == "call") {
                    auto args = ast_find(node, "arguments");
                    name = text(field(args, "argument"));
                    name = name.substr(0, name.find(','));
                    auto begin = name.find_first_not_of(" \t");
                    name = begin == std::string::npos ? "" : name.substr(begin, name.find_last_not_of(" \t") - begin + 1);
                }
                if (name.find('$') == std::string::npos) call(node, name, parent);
            }
        }
        if (language == "cmake") {
            auto args_of = [&](TSNode command) {
                std::vector<std::string> args;
                auto list = ast_find(command, "argument_list");
                for (uint32_t i = 0; i < ast_named_count(list); ++i) args.push_back(literal(ast_named_child(list, i)));
                return args;
            };
            auto lowercase = [](std::string value) {
                for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return value;
            };
            if (type == "function_def" || type == "macro_def") {
                auto args = args_of(ast_named_child(node, 0));
                if (!args.empty()) {
                    auto name = lowercase(args[0]);
                    define(node, name, type == "macro_def" ? "macro" : "function", parent, ast_find(node, "body"));
                    parent = name;
                }
            }
            if (type == "normal_command") {
                auto name = lowercase(text(ast_find(node, "identifier")));
                auto args = args_of(node);
                call(node, name, parent);
                if (!args.empty() && args[0].find('$') == std::string::npos) {
                    if (name == "include" || name == "add_subdirectory") {
                        auto target = args[0];
                        if (name == "add_subdirectory") target += "/CMakeLists.txt";
                        else if (std::filesystem::path(target).extension().empty()) target += ".cmake";
                        result.imports.push_back({path, target, "", {}, uint32_t(node_line(node))});
                    }
                    if (name == "add_library" || name == "add_executable" || name == "add_custom_target")
                        define(node, args[0], "target", parent);
                    if (name == "target_link_libraries" || name == "add_dependencies")
                        for (size_t i = 1; i < args.size(); ++i)
                            if (args[i] != "PRIVATE" && args[i] != "PUBLIC" && args[i] != "INTERFACE" && args[i].find('$') == std::string::npos) {
                                call(node, args[i], args[0]);
                                result.callsites.back().kind = CallKind::Channel;
                            }
                }
            }
        }
        if (language == "sql") {
            auto object_name = [&](TSNode object) {
                auto name = text(field(object, "name"));
                if (name.size() > 1 && name.front() == '\"' && name.back() == '\"') return name.substr(1, name.size() - 2);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return name;
            };
            if (type == "create_table" || type == "create_view" || type == "create_function" || type == "create_procedure") {
                auto name = object_name(ast_find(node, "object_reference"));
                auto kind = type.substr(7);
                define(node, name, kind, parent);
                parent = name;
            }
            if (type == "invocation") call(node, object_name(ast_find(node, "object_reference")), parent);
            if (type == "object_reference") {
                auto name = object_name(node);
                auto owner = ts_node_parent(node);
                auto owner_type = std::string(ast_type(owner));
                bool declaration = owner_type.starts_with("create_") && ts_node_eq(ast_find(owner, "object_reference"), node);
                if (!declaration && !name.empty()) result.references.push_back({path, name, uint32_t(node_line(node))});
            }
        }
        if (language == "php") {
            if (type == "function_definition" || type == "method_declaration" || type == "class_declaration" || type == "interface_declaration" || type == "trait_declaration" || type == "enum_declaration") {
                auto name = text(field(node, "name"));
                auto kind = type == "function_definition" ? "function" : type == "method_declaration" ? "method" : type.substr(0, type.find('_'));
                define(node, name, kind, parent, field(node, "body"));
                for (const auto* clause : {"base_clause", "class_interface_clause"}) {
                    auto bases = ast_find(node, clause);
                    for (uint32_t i = 0; i < ast_named_count(bases); ++i)
                        result.type_relationships.push_back({name, text(ast_named_child(bases, i)), "inherits", path, uint32_t(node_line(bases))});
                }
                parent = name;
            }
            if (type == "function_call_expression") call(node, text(field(node, "function")), parent);
            if (type == "member_call_expression" || type == "nullsafe_member_call_expression") {
                auto receiver = text(field(node, "object"));
                if (receiver == "$this") receiver = "this";
                call(node, text(field(node, "name")), parent, "", receiver);
            }
            if (type == "scoped_call_expression") call(node, text(field(node, "name")), parent, text(field(node, "scope")));
            if (type == "include_expression" || type == "include_once_expression" || type == "require_expression" || type == "require_once_expression") {
                auto argument = ast_named_child(node, 0);
                if (std::string(ast_type(argument)) == "string")
                    result.imports.push_back({path, literal(argument), "", {}, uint32_t(node_line(node))});
            }
            if (type == "namespace_use_declaration") {
                auto declaration = text(node);
                result.imports.push_back({path, declaration.substr(4, declaration.size() - 5), "", {}, uint32_t(node_line(node))});
            }
        }
        if (language == "kotlin") {
            if (type == "function_declaration" || type == "class_declaration" || type == "object_declaration") {
                auto name = text(ast_find(node, type == "function_declaration" ? "simple_identifier" : "type_identifier"));
                std::string kind = type == "function_declaration" ? "function" : type == "object_declaration" ? "object" : "class";
                for (uint32_t i = 0; i < ast_child_count(node); ++i)
                    if (std::string(ast_type(ast_child(node, i))) == "interface") kind = "interface";
                define(node, name, kind, parent, ast_find(node, type == "function_declaration" ? "function_body" : "class_body"));
                for (uint32_t i = 0; i < ast_named_count(node); ++i) {
                    auto child = ast_named_child(node, i);
                    if (std::string(ast_type(child)) == "delegation_specifier") {
                        auto base = leaf_name(child);
                        if (!base.empty()) result.type_relationships.push_back({name, base, "inherits", path, uint32_t(node_line(child))});
                    }
                }
                parent = name;
            }
            if (type == "call_expression") {
                auto fn = ast_named_child(node, 0);
                if (std::string(ast_type(fn)) == "simple_identifier") call(node, text(fn), parent);
                else if (std::string(ast_type(fn)) == "navigation_expression") {
                    auto suffix = ast_named_child(fn, ast_named_count(fn) - 1);
                    call(node, text(ast_named_child(suffix, ast_named_count(suffix) - 1)), parent, "", text(ast_named_child(fn, 0)));
                }
            }
            if (type == "import_header") result.imports.push_back({path, text(ast_find(node, "identifier")), "", {}, uint32_t(node_line(node))});
        }
        for (uint32_t i = 0; i < ast_named_count(node); ++i) visit(ast_named_child(node, i), parent);
    };
    visit(root, "");
    // Shell command syntax alone cannot distinguish an executable from a
    // function. Keep literal calls as extracted evidence; graph resolution
    // only binds them when a matching function exists in the indexed scope.
}

namespace {
using json = nlohmann::json;
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string scoped(std::string realm) {
    if (!realm.empty() && realm.find(':') == std::string::npos) realm = "project:" + realm;
    return realm;
}
std::vector<std::string> terms(const std::string& text) {
    static const std::unordered_set<std::string> stop = {
        "the", "and", "for", "with", "what", "which", "where", "how", "does", "this", "that",
        "was", "were", "are", "from", "about", "into", "when", "why", "its", "can", "have", "has"};
    std::vector<std::string> out;
    std::string token;
    auto emit = [&] {
        if (token.size() > 2 && !stop.count(token)) out.push_back(token);
        token.clear();
    };
    for (unsigned char c : text) {
        if (std::isalnum(c)) token += static_cast<char>(std::tolower(c));
        else emit();
    }
    emit();
    return out;
}
std::string read_source(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 1024 * 1024) return {};
    std::ifstream in(path, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string{};
}
bool supported(const std::filesystem::path& p) {
    static const std::unordered_set<std::string> extensions = {
        ".md", ".markdown", ".mdown", ".cpp", ".hpp", ".h", ".c", ".cc", ".rs",
        ".py", ".sh", ".js", ".ts", ".go", ".toml", ".yaml", ".yml", ".cmake"};
    const auto name = p.filename().string();
    return (extensions.count(lower(p.extension().string())) || name == "CMakeLists.txt") &&
        name != "Plan.md" && name != "tests.rs" && name.rfind("test_", 0) != 0 && name.find("_test.") == std::string::npos;
}
bool inside(const std::filesystem::path& p, const std::filesystem::path& root) {
    auto rel = p.lexically_relative(root);
    return !rel.empty() && *rel.begin() != "..";
}
std::string numeric_id(const std::string& identity) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : identity) { hash ^= c; hash *= 1099511628211ULL; }
    return std::to_string(hash);
}
}

std::string CodeIntel::git_output(const std::vector<std::string>& args) {
    int fds[2];
    if (pipe(fds) != 0) return {};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    std::vector<char*> argv{const_cast<char*>("git")};
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    int rc = posix_spawnp(&pid, "git", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    std::string output;
    if (rc == 0) {
        char buffer[8192];
        ssize_t n;
        while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) output.append(buffer, n);
    }
    close(fds[0]);
    if (rc != 0) return {};
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? output : std::string{};
}

std::string CodeIntel::project_name(const std::string& path) {
    auto root = RepositoryIndex::repository_root(path);
    if (root.empty()) root = std::filesystem::absolute(path).string();
    auto common = git_output({"-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"});
    while (!common.empty() && (common.back() == '\n' || common.back() == '\r')) common.pop_back();
    // All worktrees share the main checkout's project identity.
    if (!common.empty() && std::filesystem::path(common).filename() == ".git")
        return std::filesystem::path(common).parent_path().filename().string();
    return std::filesystem::path(root).filename().string();
}

std::vector<std::string> CodeIntel::collect_source_files(
    const std::string& path, const std::vector<std::string>& exclude, size_t max_files) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    std::error_code ec;
    auto scope = fs::weakly_canonical(path, ec);
    if (ec) return files;
    auto root = RepositoryIndex::repository_root(scope.string());
    auto add = [&](const fs::path& candidate) {
        if (!fs::is_regular_file(candidate, ec) || fs::is_symlink(candidate, ec) ||
            detect_language(candidate.string()).empty()) return;
        if (candidate != scope && !inside(candidate, scope)) return;
        const auto relative = candidate.lexically_relative(root.empty() ? scope : fs::path(root));
        for (const auto& part : relative)
            if (std::find(exclude.begin(), exclude.end(), part.string()) != exclude.end()) return;
        files.push_back(candidate.string());
    };
    if (!root.empty()) {
        std::vector<std::string> listed_args = {"-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"};
        std::vector<std::string> ignored_args = {"-C", root, "ls-files", "-z", "--cached", "--ignored", "--exclude-standard"};
        if (fs::is_regular_file(fs::path(root) / ".chittaignore")) {
            auto ignore = "--exclude-from=" + (fs::path(root) / ".chittaignore").string();
            listed_args.push_back(ignore); ignored_args.push_back(ignore);
        }
        // Untracked ignores are already excluded by the first listing. Asking
        // Git to enumerate them again walks potentially huge build caches.
        auto listed = git_output(listed_args);
        std::unordered_set<std::string> ignored;
        std::istringstream excluded(git_output(ignored_args));
        for (std::string rel; std::getline(excluded, rel, '\0');) ignored.insert(rel);
        std::istringstream input(listed);
        for (std::string rel; std::getline(input, rel, '\0');) {
            if (ignored.count(rel)) continue;
            auto candidate = fs::path(root) / rel;
            if (fs::is_directory(candidate, ec) && fs::exists(candidate / ".git") &&
                (candidate == scope || inside(candidate, scope))) {
                auto nested = collect_source_files(candidate.string(), exclude, 0);
                files.insert(files.end(), nested.begin(), nested.end());
            } else add(candidate);
        }
    } else if (fs::is_regular_file(scope, ec)) add(scope);
    else {
        fs::recursive_directory_iterator it(scope, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec) && std::find(exclude.begin(), exclude.end(), it->path().filename()) != exclude.end())
                it.disable_recursion_pending();
            else if (it->is_regular_file(ec)) add(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (max_files && files.size() > max_files) files.resize(max_files);
    return files;
}

std::string RepositoryIndex::content_hash(const std::string& bytes) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    std::ostringstream out;
    for (auto c : digest) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}

std::string RepositoryIndex::file_hash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    // Anchor hashing is not subject to the index's 1 MiB chunk budget.
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    char bytes[16384];
    while (file.read(bytes, sizeof(bytes)) || file.gcount())
        SHA256_Update(&ctx, bytes, static_cast<size_t>(file.gcount()));
    if (!file.eof()) return {};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
    std::ostringstream out;
    for (auto c : digest) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}

std::string RepositoryIndex::repository_root(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(path, ec);
    if (ec) return {};
    if (!std::filesystem::is_directory(p, ec)) p = p.parent_path();
    for (; !p.empty(); p = p.parent_path()) {
        if (std::filesystem::exists(p / ".git", ec)) return p.string();
        if (p == p.parent_path()) break;
    }
    return {};
}

bool RepositoryIndex::repository_question(const std::string& query) {
    static const std::regex cue(
        R"((\b[A-Z][A-Z0-9]*_[A-Z0-9_]+\b)|([[:alnum:]_.-]+/[[:alnum:]_./-]+)|(\b[[:alnum:]_.-]+\.(md|sh|py|rs|cpp|hpp|toml)\b))");
    if (std::regex_search(query, cue)) return true;
    const auto q = lower(query);
    for (const auto* word : {"where", "which file", "hook", "repository", "codebase", "env var",
                            "chitta", "daemon", "mcp", "codex", "snapshot", "benchmark", "nfs",
                            "posttooluse", "decision", "startup"})
        if (q.find(word) != std::string::npos) return true;
    return false;
}

nlohmann::json RepositoryIndex::source_anchor(const std::string& content, const json& supplied) {
    if (content.rfind("[artifact]", 0) != 0 && content.rfind("[done]", 0) != 0) return nullptr;
    json anchor = supplied;
    if (!anchor.is_object()) {
        std::smatch match;
        static const std::regex input(R"re(input:(?:"([^"]+)"|([^\s|,;]+)))re");
        if (!std::regex_search(content, match, input)) return nullptr;
        const std::string path = match[1].matched ? match[1].str() : match[2].str();
        if (!std::filesystem::path(path).is_absolute()) return nullptr;
        const auto root = repository_root(path);
        if (root.empty()) return nullptr;
        auto hash = file_hash(path);
        // Preserve a capture-time hash even if observation was delayed. A short
        // legacy hash is insufficient to invent a full anchor for an old fact.
        static const std::regex sha(R"(\bsha:([a-f0-9]+))");
        std::smatch recorded;
        if (std::regex_search(content, recorded, sha)) {
            if (recorded[1].length() != 64) return nullptr;
            hash = recorded[1].str();
        }
        const auto scope = content.rfind("[artifact]", 0) == 0 ? "<file>" :
            "done:" + content_hash(std::regex_replace(content, sha, "sha:"));
        anchor = {{"repo", root}, {"path", std::filesystem::path(path).lexically_relative(root).string()},
                  {"scope", scope}, {"content_hash", hash}};
    }
    for (const auto* key : {"repo", "path", "scope", "content_hash"})
        if (!anchor.contains(key) || !anchor[key].is_string() || anchor[key].get<std::string>().empty()) return nullptr;
    const std::filesystem::path relative(anchor["path"].get<std::string>());
    if (!std::filesystem::path(anchor["repo"].get<std::string>()).is_absolute() || relative.is_absolute()) return nullptr;
    for (const auto& part : relative) if (part == ".." || part == "." || part.empty()) return nullptr;
    if (!std::regex_match(anchor["content_hash"].get<std::string>(), std::regex("[a-f0-9]{64}"))) return nullptr;
    return anchor;
}

void RepositoryIndex::open(const std::string& sidecar, const std::string& known_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    sidecar_ = sidecar;
    roots_.clear(); files_.clear();
    std::ifstream in(sidecar_);
    json registry = in ? json::parse(in, nullptr, false) : json();
    if (registry.is_object() && registry.value("version", 0) == 1 && registry["roots"].is_object()) {
        for (auto it = registry["roots"].begin(); it != registry["roots"].end(); ++it)
            if (it.value().is_string()) roots_[it.key()] = it.value().get<std::string>();
    } else {
        auto known = json::parse(known_files, nullptr, false);
        if (known.is_array()) for (const auto& f : known) {
            const auto realm = scoped(f.value("project", ""));
            if (realm.empty() || roots_.count(realm)) continue;
            const auto root = repository_root(f.value("path", ""));
            if (!root.empty()) roots_[realm] = root;
        }
    }
    // search() refreshes the requested realm before returning any source.
    // Do not eagerly walk every historical root at daemon startup: restored
    // stores can name abandoned checkouts or entire shared data directories.
}

void RepositoryIndex::index(const std::string& path, const std::string& realm) {
    const auto root = repository_root(path);
    if (root.empty() || realm.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    roots_[scoped(realm)] = root;
    refresh(scoped(realm));
    if (!sidecar_.empty()) {
        auto tmp = sidecar_ + ".tmp";
        std::ofstream out(tmp);
        out << json{{"version", 1}, {"roots", roots_}}.dump();
        out.close();
        if (out) { std::error_code ec; std::filesystem::rename(tmp, sidecar_, ec); }
    }
}

void RepositoryIndex::refresh(const std::string& realm) {
    const auto registered = roots_.find(realm);
    if (registered == roots_.end()) return;
    const std::filesystem::path root(registered->second);
    static const std::unordered_set<std::string> exclude = {
        ".git", "build", "target", "node_modules", "_deps", "__pycache__", ".venv", "venv",
        "results", "fixtures", "tests", ".codex", ".claude", ".cache"};
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root,
        std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && exclude.count(it->path().filename().string())) {
            it.disable_recursion_pending(); continue;
        }
        if (!it->is_regular_file(ec) || !supported(it->path())) continue;
        auto canonical = std::filesystem::weakly_canonical(it->path(), ec);
        if (!ec && inside(canonical, root)) paths.push_back(canonical);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.size() > 5000) paths.resize(5000);
    std::unordered_set<std::string> present;
    CodeIntel intel;
    for (const auto& path : paths) {
        const auto key = realm + "\n" + path.string();
        present.insert(key);
        auto body = read_source(path);
        if (body.empty()) { files_.erase(key); continue; }
        const auto hash = content_hash(body);
        auto old = files_.find(key);
        if (old != files_.end() && old->second.hash == hash) continue;
        File f{root.string(), realm, path.string(), hash, {}};
        auto symbols = intel.extract_file(path.string());
        const bool markdown = intel.detect_language(path.string()) == "markdown";
        std::vector<std::string> lines;
        std::istringstream input(body);
        for (std::string line; std::getline(input, line);) lines.push_back(line);
        std::unordered_map<std::string, size_t> occurrences;
        for (const auto& symbol : symbols) {
            auto name = symbol.parent.empty() ? symbol.name : symbol.parent + "::" + symbol.name;
            // Overloads get signature identity rather than line-position identity.
            if (!markdown) name += " " + symbol.signature;
            auto count = ++occurrences[name];
            if (count > 1) name += " [" + std::to_string(count) + "]";
            std::string text;
            for (size_t n = std::max(1, symbol.line_start) - 1;
                 n < lines.size() && n < static_cast<size_t>(std::max(symbol.line_start, symbol.line_end)); ++n)
                text += lines[n] + "\n";
            const auto identity = root.string() + "\n" + path.lexically_relative(root).string() + "\n" + name;
            f.chunks.push_back({identity, name, text, markdown ? "doc" : "code",
                                static_cast<size_t>(std::max(1, symbol.line_start))});
        }
        // File scope captures imports, env declarations and shell/config text
        // outside named symbols. The identity remains stable as lines move.
        if (!markdown && !body.empty())
            f.chunks.push_back({root.string() + "\n" + path.lexically_relative(root).string() + "\n<file>",
                                "<file>", body, "code", 1});
        for (auto& chunk : f.chunks) {
            auto tokens = terms(chunk.name + " " + chunk.text + " " + path.lexically_relative(root).string());
            chunk.token_count = tokens.size();
            for (const auto& token : tokens) ++chunk.terms[token];
        }
        // A concurrent edit must never pair one version's hash with another's chunks.
        if (file_hash(path.string()) == hash) files_[key] = std::move(f);
        else files_.erase(key);
    }
    for (auto i = files_.begin(); i != files_.end();) {
        if (i->second.realm == realm && !present.count(i->first)) i = files_.erase(i);
        else ++i;
    }
}

nlohmann::json RepositoryIndex::search(const std::string& query, const std::string& realm, size_t limit) {
    json result = json::array();
    if (!repository_question(query) || realm.empty() || !limit) return result;
    auto query_terms = terms(query);
    std::sort(query_terms.begin(), query_terms.end());
    query_terms.erase(std::unique(query_terms.begin(), query_terms.end()), query_terms.end());
    if (query_terms.empty()) return result;
    std::lock_guard<std::mutex> lock(mutex_);
    refresh(scoped(realm));
    struct Candidate { const File* file; const Chunk* chunk; double score; size_t matched; };
    std::vector<Candidate> candidates;
    std::map<std::string, size_t> df;
    size_t documents = 0;
    for (const auto& [key, file] : files_) if (file.realm == scoped(realm))
        for (const auto& c : file.chunks) {
            ++documents;
            for (const auto& q : query_terms) df[q] += c.terms.count(q);
        }
    for (const auto& [key, file] : files_) if (file.realm == scoped(realm))
        for (const auto& c : file.chunks) {
            const auto& counts = c.terms;
            double score = 0;
            size_t matched = 0;
            for (const auto& q : query_terms) if (counts.count(q)) {
                ++matched;
                double tf = counts.at(q);
                const auto label = lower(c.name + " " + std::filesystem::path(file.path).lexically_relative(file.root).string());
                const double identity_weight = label.find(q) != std::string::npos ? 1.75 : 1.0;
                score += identity_weight * std::log(1.0 + (documents - df[q] + 0.5) / (df[q] + 0.5)) *
                    tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * c.token_count / 180.0));
            }
            // Reject incidental one-word matches except precise identifiers.
            bool precise = false;
            for (const auto& q : query_terms) if (q.size() >= 8 && counts.count(q)) precise = true;
            if (matched < std::min<size_t>(2, query_terms.size()) && !precise) continue;
            if (score > 0) candidates.push_back({&file, &c, score, matched});
        }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.chunk->identity < b.chunk->identity;
    });
    std::unordered_set<std::string> excerpts;
    for (const auto& c : candidates) {
        if (result.size() >= limit) break;
        // Revalidate the selected file immediately before presenting its text.
        if (file_hash(c.file->path) != c.file->hash) continue;
        const auto& chunk = *c.chunk;
        if (!excerpts.insert(content_hash(chunk.text)).second) continue;
        auto body = chunk.text;
        auto line = chunk.line;
        // Bounded excerpt around the densest matching line, not a generated answer.
        if (body.size() > 2400) {
            size_t best = 0, best_count = 0, pos = 0;
            while (pos < body.size()) {
                size_t end = body.find('\n', pos);
                if (end == std::string::npos) end = body.size();
                auto window = lower(body.substr(pos, std::min<size_t>(1800, body.size() - pos)));
                size_t count = 0;
                for (const auto& q : query_terms) count += window.find(q) != std::string::npos;
                if (count > best_count) { best = pos; best_count = count; }
                pos = end + 1;
            }
            line += std::count(body.begin(), body.begin() + best, '\n');
            body = body.substr(best, 2400);
        }
        std::string citation = c.file->path + ":" + std::to_string(line) + " # " + chunk.name;
        std::string text = citation + " [current sha256:" + c.file->hash + "]\n" + body;
        result.push_back({{"id", numeric_id(chunk.identity)}, {"type", chunk.kind}, {"source", chunk.kind},
            {"text", text}, {"realm", realm}, {"relevance", 1.0}, {"lexical", 1.0}, {"confidence", 1.0},
            {"ts_ms", 0}, {"source_identity", chunk.identity}, {"content_hash", c.file->hash}});
    }
    return result;
}
} // namespace chitta
