// Included into FieldRpcHandler class body — declarations only.
// Bodies live in chitta/src/handlers/field_code_intel.cpp.

// Included into FieldRpcHandler class body — not a standalone header.
// Code intelligence tools: find_symbol, search_symbols, extract_symbols,
// learn_codebase, symbol_callers/callees, read_symbol/function, code_context,
// codebase_overview, smart_context, embed_symbols, dedupe_symbols, describe_symbol,
// enrichment_status, clear_codebase, clear_triplets, resolve_callsites,
// type_hierarchy, file_imports, file_dependents, restore_code_intel_confidence

    // ── Symbol helper: convert CfSymbolHit to JSON ─────────────────────────

    struct ResolvedSymbol {
        uint64_t id;
        std::string kind;
        std::string name;
        std::string signature;
        std::string file_path;
        uint32_t line_start;
        uint32_t line_end;
    };

    static ResolvedSymbol from_cf_hit(const CfSymbolHit& h) {
        ResolvedSymbol s;
        s.id = h.symbol_id;
        s.kind = reinterpret_cast<const char*>(h.kind);
        s.name = reinterpret_cast<const char*>(h.name);
        s.signature = reinterpret_cast<const char*>(h.signature);
        s.file_path = reinterpret_cast<const char*>(h.file_path);
        s.line_start = h.line_start;
        s.line_end = h.line_end;
        return s;
    }

    static json sym_to_json(const ResolvedSymbol& s) {
        return {
            {"id", s.id},
            {"kind", s.kind},
            {"name", s.name},
            {"file", s.file_path},
            {"line_start", s.line_start},
            {"line_end", s.line_end},
            {"signature", s.signature}
        };
    }

    static json cf_hit_to_json(const CfSymbolHit& h) {
        return sym_to_json(from_cf_hit(h));
    }

    // ── resolve_symbol: find symbol by id or name ──────────────────────────

    std::optional<ResolvedSymbol> resolve_symbol_field(const json& params) {
        if (params.contains("id") && params["id"].is_number_integer()) {
            uint64_t id = static_cast<uint64_t>(params["id"].get<int64_t>());
            // Search by name is the only way to find by id in FieldStore —
            // scan a generous set and match
            // This is a limitation; for now, do a broad search
            auto hits = field_store_->search_symbols_by_name("", 1000);
            for (const auto& h : hits) {
                if (h.symbol_id == id) return from_cf_hit(h);
            }
            return std::nullopt;
        }

        std::string name = params.value("name", "");
        if (name.empty()) return std::nullopt;

        auto hits = field_store_->search_symbols_by_name(name, 50);
        if (hits.empty()) return std::nullopt;

        // Find exact name matches
        std::vector<ResolvedSymbol> exact;
        for (const auto& h : hits) {
            auto s = from_cf_hit(h);
            if (s.name == name) exact.push_back(s);
        }
        if (exact.empty()) return from_cf_hit(hits[0]);
        if (exact.size() == 1) return exact[0];

        // Disambiguate by path substring (duplicate names across checkouts)
        std::string path_filter = params.value("path", "");
        if (!path_filter.empty()) {
            for (const auto& s : exact) {
                if (s.file_path.find(path_filter) != std::string::npos) return s;
            }
        }

        // Disambiguate by kind
        std::string kind = params.value("kind", "");
        if (!kind.empty()) {
            for (const auto& s : exact) {
                if (s.kind == kind) return s;
            }
        }

        // Prefer symbols with call edges
        for (const auto& s : exact) {
            auto callers = field_store_->get_callers(s.id);
            auto callees = field_store_->get_callees(s.id);
            if (!callers.empty() || !callees.empty()) return s;
        }

        return exact[0];
    }

    // ── read source lines helper ───────────────────────────────────────────

    static std::string read_source_lines(const std::string& file_path,
                                          uint32_t line_start, uint32_t line_end) {
        std::ifstream file(file_path);
        if (!file) return "";
        std::ostringstream source;
        std::string line;
        uint32_t line_num = 1;
        while (std::getline(file, line)) {
            if (line_num >= line_start && line_num <= line_end) {
                source << line << "\n";
            }
            if (line_num > line_end) break;
            line_num++;
        }
        return source.str();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Tool implementations
    // ═══════════════════════════════════════════════════════════════════════






    // Returns true if the string looks like a remote URL (https://, git://, git@, ssh://).
    static bool is_remote_url(const std::string& s) {
        return s.find("://") != std::string::npos ||
               s.rfind("git@", 0) == 0 ||
               s.rfind("ssh://", 0) == 0;
    }

    // Derive a short project name from a remote URL.
    // "https://github.com/owner/repo.git" → "repo"
    static std::string project_from_url(const std::string& url) {
        std::string u = url;
        if (!u.empty() && u.back() == '/') u.pop_back();
        if (u.size() >= 4 && u.substr(u.size() - 4) == ".git") u = u.substr(0, u.size() - 4);
        auto pos = u.rfind('/');
        if (pos != std::string::npos) return u.substr(pos + 1);
        pos = u.rfind(':');
        if (pos != std::string::npos) return u.substr(pos + 1);
        return u;
    }

    // Shallow-clone a remote URL into a temporary directory.
    // Returns the tmpdir path on success, empty string on failure.
    static std::string clone_remote(const std::string& url,
                                    const std::string& branch,
                                    std::string& error_out) {
        // mkdtemp requires a writable template
        char tmpl[] = "/tmp/chitta-clone-XXXXXX";
        char* tmpdir = mkdtemp(tmpl);
        if (!tmpdir) { error_out = "mkdtemp failed"; return ""; }

        std::string cmd = "git clone --depth 1 --quiet";
        if (!branch.empty()) cmd += " --branch " + branch;
        cmd += " -- " + url + " " + std::string(tmpdir) + " 2>&1";

        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) { error_out = "popen failed"; return ""; }
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) error_out += buf;
        int rc = pclose(fp);
        if (rc != 0) return "";  // error_out has the git output
        error_out.clear();
        return std::string(tmpdir);
    }

    // Git provenance for a single file: {commit, author, timestamp_ms}
    struct GitProvenance {
        std::string commit;
        std::string author;
        int64_t timestamp_ms{-1};
    };

    static std::string git_repo_root(const std::string& path) {
        return RepositoryIndex::repository_root(path);
    }

    static std::unordered_map<std::string, GitProvenance>
    git_batch_provenance(const std::string& root,
                         const std::unordered_set<std::string>& paths) {
        std::unordered_map<std::string, GitProvenance> result;
        if (root.empty() || paths.empty()) return result;
        // One history walk, rather than one process and history walk per file.
        auto output = CodeIntel::git_output({"-C", root, "log", "--format=%x1e%H %ae %at", "--name-only"});
        std::istringstream input(output);
        GitProvenance current;
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line[0] == '\x1e') {
                std::istringstream fields(line.substr(1));
                int64_t seconds = 0;
                fields >> current.commit >> current.author >> seconds;
                current.timestamp_ms = seconds * 1000;
            } else if (!line.empty()) {
                auto absolute = (std::filesystem::path(root) / line).string();
                if (paths.count(absolute)) result.emplace(absolute, current);
            }
            if (result.size() == paths.size()) break;
        }
        return result;
    }

    // Compute FNV-1a 64-bit content hash (deterministic, good distribution).
    static std::string compute_content_hash(const std::string& file_path) {
        std::ifstream f(file_path, std::ios::binary);
        if (!f) return "";
        uint64_t hash = 14695981039346656037ULL;
        char buf[8192];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
            for (std::streamsize i = 0; i < f.gcount(); ++i) {
                hash ^= static_cast<uint8_t>(buf[i]);
                hash *= 1099511628211ULL;
            }
        }
        std::ostringstream hex;
        hex << std::hex << std::setfill('0') << std::setw(16) << hash;
        return hex.str();
    }

    ToolResult tool_enrichment_status(const json&);
    ToolResult tool_describe_symbol(const json& params);
    ToolResult tool_embed_symbols(const json& params);
    ToolResult tool_dedupe_symbols(const json&);
    ToolResult tool_extract_symbols(const json& params);
    ToolResult tool_learn_codebase(const json& params);
    ToolResult tool_find_symbol(const json& params);
    ToolResult tool_symbol_callers(const json& params);
    ToolResult tool_symbol_callees(const json& params);
    ToolResult tool_read_symbol(const json& params);
    ToolResult tool_read_function(const json& params);
    ToolResult tool_search_symbols(const json& params);
    ToolResult tool_code_context(const json& params);
    ToolResult tool_smart_context(const json& params);
    ToolResult tool_codebase_overview(const json& params);
    ToolResult tool_clear_codebase(const json& params);
    ToolResult tool_clear_triplets(const json& params);
    ToolResult tool_resolve_callsites(const json& params);
    ToolResult tool_type_hierarchy(const json& params);
    ToolResult tool_file_imports(const json& params);
    ToolResult tool_file_dependents(const json& params);
    ToolResult tool_restore_code_intel_confidence(const json& params);
