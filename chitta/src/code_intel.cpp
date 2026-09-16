#include "chitta/code_intel.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <regex>
#include <openssl/sha.h>

namespace chitta {
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
    // No trusted cached chunks: restart validates every discovered source.
    for (const auto& [realm, root] : roots_) refresh(realm);
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
