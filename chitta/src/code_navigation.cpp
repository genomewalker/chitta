#include "chitta/code_navigation.hpp"
#include "chitta/code_intel.hpp"
#include <algorithm>
#include <cmath>
#include <climits>
#include <deque>
#include <iostream>
#include <map>
#include <set>

namespace chitta {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
std::string canonical(std::string p) {
    return p.rfind("project:", 0) == 0 ? p.substr(8) : p;
}
std::string lower(std::string s) {
    for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c));
    return s;
}
std::vector<std::string> words(const std::string& text) {
    static const std::set<std::string> stop = {"the", "and", "for", "with", "what", "which", "where", "how", "does", "this", "that", "from", "are", "when", "into", "can", "code", "file", "function", "implemented", "implements"};
    std::vector<std::string> out;
    std::string token;
    auto emit = [&] {
        if (token.size() > 1 && !stop.count(token)) {
            // Light morphology shares identifiers with ordinary navigation prose.
            if (token.size() > 5 && token.compare(token.size() - 3, 3, "ing") == 0) token.resize(token.size() - 3);
            else if (token.size() > 4 && token.compare(token.size() - 2, 2, "ed") == 0) token.resize(token.size() - 2);
            else if (token.size() > 4 && token.back() == 's') token.pop_back();
            if (token.size() > 4 && token.back() == 'e') token.pop_back();
            out.push_back(token);
        }
        token.clear();
    };
    unsigned char previous = 0;
    for (unsigned char c : text) {
        if ((std::isupper(c) && std::islower(previous)) ||
            (std::isalnum(c) && std::isalnum(previous) && bool(std::isdigit(c)) != bool(std::isdigit(previous)))) emit();
        if (std::isalnum(c)) token += std::tolower(c); else emit();
        previous = c;
    }
    emit();
    return out;
}
std::string language(const std::string& path) {
    auto lang = CodeIntel::detect_language(path);
    return lang == "typescript" ? "javascript" : lang;
}
std::string contents(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
std::string one_line(std::string s, size_t cap = 160) {
    for (auto& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (s.size() > cap) {
        while (cap > 0 && (static_cast<unsigned char>(s[cap]) & 0xc0) == 0x80) --cap;
        s = s.substr(0, cap) + "…";
    }
    return s;
}
bool under(const std::string& path, const std::string& scope) {
    if (scope.empty()) return true;
    return path == scope || (path.size() > scope.size() && path.compare(0, scope.size(), scope) == 0 && path[scope.size()] == '/');
}
int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}

struct CodeNavigation::Impl {
    struct Node {
        json data;
        std::string file, project, root, body, lang;
        std::map<std::string, double> terms;
        size_t length = 0;
        int first = 1, last = 1;
        std::vector<size_t> edges;
    };
    struct Edge {
        size_t from;
        int to;
        std::string kind, surface, confidence, resolution, evidence_file;
        int line;
    };
    std::mutex mutex;
    std::string sidecar, warning;
    json files = json::object();
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::map<std::string, std::vector<size_t>> names, by_file;
    std::map<std::string, size_t> frequency;
    std::vector<size_t> labels;
    double average_length = 1;

    void save() {
        if (sidecar.empty()) return;
        auto tmp = sidecar + ".tmp";
        std::ofstream out(tmp);
        out << json{{"version", 1}, {"files", files}}.dump();
        out.close();
        if (!out) throw std::runtime_error("code navigation sidecar write failed");
        fs::rename(tmp, sidecar);
    }
    int enclosing(const std::string& file, int line) const {
        auto found = by_file.find(file);
        if (found == by_file.end()) return -1;
        int best = -1, span = INT_MAX;
        for (auto n : found->second) {
            int first = nodes[n].first, last = nodes[n].last;
            if (first <= line && line <= last && last - first < span) {
                best = static_cast<int>(n); span = last - first;
            }
        }
        return best;
    }
    void rebuild() {
        nodes.clear(); edges.clear(); names.clear(); by_file.clear(); frequency.clear();
        size_t lengths = 0;
        for (const auto& item : files.items()) {
            const auto& f = item.value();
            for (const auto& symbol : f["symbols"]) {
                Node n;
                n.data = symbol;
                n.first = symbol["line_start"]; n.last = symbol["line_end"];
                n.file = item.key(); n.root = f["root"]; n.project = f["project"]; n.lang = language(n.file);
                n.body = n.data.value("body", ""); n.data.erase("body");
                auto tokens = words(n.body);
                n.length = tokens.size(); lengths += n.length;
                for (const auto& w : tokens) n.terms[w] += 1;
                for (const auto& w : words(n.data.value("name", "") + " " + n.data.value("signature", ""))) n.terms[w] += 8;
                for (const auto& w : words(fs::path(n.file).lexically_relative(n.root).string())) n.terms[w] += 2;
                for (const auto& t : n.terms) ++frequency[t.first];
                auto name = n.data.value("name", "");
                names[name].push_back(nodes.size());
                auto pos = name.rfind("::");
                if (pos != std::string::npos) names[name.substr(pos + 2)].push_back(nodes.size());
                by_file[n.file].push_back(nodes.size());
                nodes.push_back(std::move(n));
            }
        }
        average_length = nodes.empty() ? 1 : std::max(1.0, double(lengths) / nodes.size());
        std::set<std::tuple<size_t, int, std::string, std::string>> seen;
        auto add = [&](size_t from, int to, const std::string& kind, const std::string& surface, int line, const std::string& resolution, const std::string& evidence_file) {
            if (to == static_cast<int>(from)) return;
            if (!seen.emplace(from, to, kind, surface).second) return;
            auto id = edges.size();
            edges.push_back({from, to, kind, surface, to < 0 ? "EXTRACTED" : "INFERRED", resolution, evidence_file, line});
            nodes[from].edges.push_back(id);
            if (to >= 0) nodes[to].edges.push_back(id);
        };
        for (const auto& item : files.items()) {
            std::map<int, int> enclosing_cache;
            for (const auto& raw : item.value()["edges"]) {
                int line = raw.value("line", 1);
                if (!enclosing_cache.count(line)) enclosing_cache[line] = enclosing(item.key(), line);
                int from = enclosing_cache[line];
                if (from < 0) continue;
                std::string target = raw["target"], kind = raw["kind"];
                const auto derived = raw.value("derived", "");
                if (kind == "inherits" && !derived.empty() && nodes[from].data.value("name", "") != derived) {
                    std::vector<size_t> local;
                    for (auto id : names[derived]) if (nodes[id].file == item.key()) local.push_back(id);
                    if (local.size() == 1) from = static_cast<int>(local.front());
                }
                const auto source_name = raw.value("source_name", "");
                if (!source_name.empty()) {
                    std::vector<size_t> sources;
                    for (auto id : names[source_name]) {
                        const auto& source = nodes[id];
                        const auto source_kind = source.data.value("kind", "");
                        if (source.root == nodes[from].root && source.lang == nodes[from].lang &&
                            (source_kind == "process" || source_kind == "workflow" || source_kind == "rule" || source_kind == "checkpoint" || source_kind == "target")) sources.push_back(id);
                    }
                    if (sources.size() != 1) continue; // A channel producer must resolve uniquely.
                    from = static_cast<int>(sources.front());
                }
                std::vector<size_t> candidates;
                if (kind == "imports") {
                    from = static_cast<int>(by_file[item.key()].back());
                    auto relative = (fs::path(item.key()).parent_path() / target).lexically_normal().string();
                    if (by_file.count(relative)) candidates.push_back(by_file[relative].back());
                    else {
                        std::string module = target;
                        std::replace(module.begin(), module.end(), '.', '/');
                        for (const auto& entry : by_file) {
                            if (nodes[entry.second.front()].root != nodes[from].root) continue;
                            auto rel = fs::path(entry.first).lexically_relative(nodes[from].root).string();
                            if (rel == target || (nodes[from].lang == "python" &&
                                (rel == module + ".py" || rel == module + "/__init__.py")))
                                candidates.push_back(entry.second.back());
                        }
                    }
                    if (candidates.empty() && names.count(target)) {
                        for (auto id : names[target]) {
                            const auto& candidate = nodes[id];
                            if (candidate.root == nodes[from].root && candidate.lang == nodes[from].lang &&
                                candidate.data.value("kind", "") == "module") candidates.push_back(id);
                        }
                    }
                } else if (names.count(target)) {
                    const auto caller_parent = nodes[from].data.value("parent", "");
                    const auto receiver = raw.value("receiver", "");
                    const auto qualified = raw.value("scope", "");
                    for (auto id : names[target]) {
                        const auto& candidate = nodes[id];
                        if (candidate.root != nodes[from].root || candidate.lang != nodes[from].lang) continue;
                        auto parent = candidate.data.value("parent", "");
                        if (kind == "calls" && !receiver.empty()) {
                            // A leaf-name match cannot establish an object's type.
                            if ((receiver != "self" && receiver != "this") || caller_parent.empty() || parent != caller_parent) continue;
                        }
                        if (kind == "calls" && receiver.empty() && qualified.empty() &&
                            !parent.empty() && parent != caller_parent &&
                            candidate.data.value("kind", "") != "class" && candidate.data.value("kind", "") != "struct") continue;
                        if (kind == "calls" && !qualified.empty() && parent != qualified &&
                            candidate.data.value("name", "").rfind(qualified + "::", 0) != 0) continue;
                        if (kind == "references") {
                            if (target.size() <= 3) continue;
                            auto candidate_kind = candidate.data.value("kind", "");
                            if (candidate_kind == "variable" || candidate_kind == "file") continue;
                            if (!parent.empty() && parent != caller_parent && candidate_kind != "class" && candidate_kind != "struct") continue;
                        }
                        candidates.push_back(id);
                    }
                    std::vector<size_t> local;
                    for (auto id : candidates) if (nodes[id].file == item.key()) local.push_back(id);
                    if (!local.empty()) candidates = std::move(local);
                }
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
                // Multiple definitions remain ambiguous; never choose hash-map order.
                int to = candidates.size() == 1 ? static_cast<int>(candidates[0]) : -1;
                if (kind == "references" && to < 0) continue;
                add(from, to, kind, target, raw.value("line", 1),
                    to >= 0 ? "unique_name" : candidates.empty() ? "unresolved" : "AMBIGUOUS", item.key());
            }
        }
        labels.resize(nodes.size());
        for (size_t i = 0; i < labels.size(); ++i) labels[i] = i;
        // Synchronous label propagation with fixed iteration count and stable ties.
        for (int iteration = 0; iteration < 6; ++iteration) {
            auto next = labels;
            for (size_t i = 0; i < nodes.size(); ++i) {
                std::map<size_t, size_t> votes;
                for (auto ei : nodes[i].edges) {
                    const auto& e = edges[ei];
                    if (e.to >= 0 && e.kind == "calls") ++votes[labels[e.from == i ? e.to : e.from]];
                }
                size_t best = 0;
                for (auto [label, count] : votes) if (count > best) { best = count; next[i] = label; }
            }
            labels.swap(next);
        }
    }
    std::vector<size_t> scope(const json& p) const {
        std::string project = canonical(p.value("realm", p.value("project", "")));
        std::string path = p.value("path", "");
        if (!path.empty()) path = fs::weakly_canonical(path).string();
        std::vector<size_t> selected;
        for (size_t i = 0; i < nodes.size(); ++i)
            if ((project.empty() || nodes[i].project == project) && under(nodes[i].file, path)) selected.push_back(i);
        return selected;
    }
    json edge_json(const Edge& e) const {
        return {{"source", nodes[e.from].data["id"]},
            {"target", e.to < 0 ? json("external:" + e.surface) : nodes[e.to].data["id"]},
            {"kind", e.kind}, {"surface", e.surface}, {"confidence", e.confidence},
            {"resolution", e.resolution}, {"file", e.evidence_file}, {"line", e.line},
            {"evidence", "AST"}};
    }
    json render(const std::vector<size_t>& selected, const std::map<size_t, double>& scores = {}, bool commands = true) const {
        json out = {{"symbols", json::array()}, {"files", json::array()}, {"edges", json::array()}};
        std::set<size_t> keep(selected.begin(), selected.end());
        std::set<std::string> paths;
        std::ostringstream text;
        if (!selected.empty()) text << "[code-nav] root " << nodes[selected.front()].root << "\n";
        for (auto id : selected) {
            auto s = nodes[id].data;
            s["file"] = nodes[id].file;
            if (scores.count(id)) s["score"] = scores.at(id);
            s["read_symbol"] = {{"name", s["name"]}, {"path", nodes[id].file}, {"line", s["line_start"]}};
            s["in"] = json::array(); s["out"] = json::array();
            for (auto ei : nodes[id].edges) {
                const auto& e = edges[ei];
                auto& list = s[e.from == id ? "out" : "in"];
                if (list.size() < 2) list.push_back(e.kind + ":" +
                    (e.from == id ? e.surface : nodes[e.from].data.value("name", "")) +
                    "[" + e.confidence + (e.resolution == "AMBIGUOUS" ? ":AMBIGUOUS" : "") + "]");
            }
            out["symbols"].push_back(s);
            auto signature = s.value("signature", "");
            if (signature.empty() || s.value("kind", "") == "file") signature = s.value("name", "");
            text << fs::path(nodes[id].file).lexically_relative(nodes[id].root).string() << ":" << s["line_start"] << " "
                 << one_line(signature, 100) << "\n  ";
            if (!s.value("doc", "").empty()) text << one_line(s.value("doc", ""), 80) << " | ";
            text << "in " << s["in"].dump() << " out " << s["out"].dump() << "\n";
            if (commands) text << "  read_symbol " << s["read_symbol"].dump() << "\n";
            paths.insert(nodes[id].file);
        }
        for (const auto& p : paths) out["files"].push_back(p);
        for (const auto& e : edges) if (keep.count(e.from) && (e.to < 0 || keep.count(e.to))) {
            if (out["edges"].size() >= 100) { out["edges_truncated"] = true; break; }
            out["edges"].push_back(edge_json(e));
        }
        out["text"] = text.str();
        return out;
    }
};

CodeNavigation::CodeNavigation() : impl_(std::make_unique<Impl>()) {}
CodeNavigation::~CodeNavigation() = default;
void CodeNavigation::open(const std::string& sidecar) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->sidecar = sidecar;
    if (!fs::exists(sidecar)) return;
    try {
        auto data = json::parse(contents(sidecar));
        if (data.at("version") != 1) throw std::runtime_error("unsupported sidecar version");
        impl_->files = data.at("files");
        impl_->rebuild();
    } catch (const std::exception& e) {
        impl_->files = json::object(); impl_->rebuild();
        impl_->warning = std::string("navigation index unavailable: ") + e.what();
        std::cerr << impl_->warning << "\n";
    }
}
void CodeNavigation::update(const std::string& root, const std::string& project,
    const std::vector<std::string>& paths, const std::unordered_set<std::string>& changed,
    const ExtractionResult& extraction, bool full) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto stamp = now();
    if (full) {
        std::set<std::string> present(paths.begin(), paths.end());
        for (auto it = impl_->files.begin(); it != impl_->files.end();) {
            if (under(it.key(), root) && !present.count(it.key())) it = impl_->files.erase(it); else ++it;
        }
    }
    std::unordered_map<std::string, std::vector<const ExtractedSymbol*>> symbols;
    std::unordered_map<std::string, std::vector<const Callsite*>> calls;
    std::unordered_map<std::string, std::vector<const ImportStatement*>> imports;
    std::unordered_map<std::string, std::vector<const TypeRelationship*>> inherits;
    std::unordered_map<std::string, std::vector<const CodeReference*>> references;
    for (const auto& s : extraction.symbols) symbols[s.file_path].push_back(&s);
    for (const auto& s : extraction.callsites) calls[s.file_path].push_back(&s);
    for (const auto& s : extraction.imports) imports[s.file_path].push_back(&s);
    for (const auto& s : extraction.type_relationships) inherits[s.file_path].push_back(&s);
    for (const auto& s : extraction.references) references[s.file_path].push_back(&s);
    for (const auto& path : paths) {
        if (!changed.count(path) && impl_->files.contains(path)) continue;
        auto body = contents(path);
        std::vector<std::string> lines;
        std::istringstream input(body);
        for (std::string line; std::getline(input, line);) lines.push_back(line);
        json file = {{"root", root}, {"project", canonical(project)}, {"hash", RepositoryIndex::content_hash(body)},
                     {"indexed_at", stamp}, {"symbols", json::array()}, {"edges", json::array()}};
        std::set<std::string> seen;
        for (const auto* extracted : symbols[path]) {
            const auto& s = *extracted;
            std::string id = path + ":" + std::to_string(s.line_start) + ":" + s.kind + ":" + s.name;
            if (!seen.insert(id).second) continue;
            std::string source, doc;
            for (int n = std::max(1, s.line_start) - 1; n < s.line_end && n < static_cast<int>(lines.size()); ++n) source += lines[n] + "\n";
            if (s.line_start > 1 && s.line_start <= static_cast<int>(lines.size())) {
                auto previous = lines[s.line_start - 2];
                auto first = previous.find_first_not_of(" \t");
                if (first != std::string::npos && (previous.compare(first, 2, "//") == 0 ||
                    previous[first] == '#' || previous[first] == '*')) doc = previous.substr(first);
            }
            if (doc.empty() && language(path) == "python") {
                bool header_ended = false;
                for (int n = std::max(1, s.line_start) - 1;
                     n < std::min(s.line_end, s.line_start + 16) && n < static_cast<int>(lines.size()); ++n) {
                    auto line = lines[n];
                    auto first = line.find_first_not_of(" \t");
                    if (first == std::string::npos) continue;
                    line.erase(0, first);
                    if (!header_ended) { header_ended = !line.empty() && line.back() == ':'; continue; }
                    if (line.rfind(std::string(3, char(34)), 0) == 0 || line.rfind(std::string(3, char(39)), 0) == 0) {
                        doc = line.substr(3);
                        auto end = doc.find(line.substr(0, 3));
                        if (end != std::string::npos) doc.resize(end);
                    }
                    break;
                }
            }
            file["symbols"].push_back({{"id", id}, {"name", s.name}, {"kind", s.kind}, {"signature", s.signature},
                {"line_start", s.line_start}, {"line_end", s.line_end}, {"parent", s.parent}, {"doc", one_line(doc)}, {"body", source}});
        }
        // File scope connects imports and globals outside a named definition.
        file["symbols"].push_back({{"id", path + ":0:<file>"}, {"name", "<file>"}, {"kind", "file"}, {"signature", path},
            {"line_start", 1}, {"line_end", lines.size()}, {"doc", "File scope"}, {"body", body}});
        for (const auto* c : calls[path])
            file["edges"].push_back({{"kind", "calls"}, {"target", c->callee_leaf}, {"line", c->line},
                {"receiver", c->receiver_text}, {"scope", c->scope_text},
                {"source_name", c->kind == CallKind::Channel ? c->caller_symbol : ""}});
        for (const auto* extracted : imports[path]) {
            const auto& c = *extracted;
            auto target = c.import_path;
            if (target.size() > 1 && (target.front() == '"' || target.front() == '<' || target.front() == '\'')) target = target.substr(1, target.size() - 2);
            file["edges"].push_back({{"kind", "imports"}, {"target", target}, {"line", c.line}});
        }
        for (const auto* c : inherits[path])
            file["edges"].push_back({{"kind", "inherits"}, {"target", c->base_name}, {"derived", c->derived_name}, {"line", c->line}});
        for (const auto* c : references[path])
            file["edges"].push_back({{"kind", "references"}, {"target", c->name}, {"line", c->line}});
        impl_->files[path] = std::move(file);
    }
    impl_->warning.clear(); impl_->rebuild(); impl_->save();
}
bool CodeNavigation::has_file(const std::string& path) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->files.contains(path);
}
void CodeNavigation::remove(const std::string& path) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->files.erase(fs::weakly_canonical(path).string());
    impl_->rebuild(); impl_->save();
}
void CodeNavigation::clear_project(const std::string& project) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (auto it = impl_->files.begin(); it != impl_->files.end();) {
        if (it.value().value("project", "") == canonical(project)) it = impl_->files.erase(it);
        else ++it;
    }
    impl_->rebuild(); impl_->save();
}
json CodeNavigation::query(const json& p) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto eligible = impl_->scope(p);
    auto query = p.value("question", "");
    bool file_mode = query.empty() && !p.value("path", "").empty();
    if (query.empty() && !file_mode) return {{"error", "question is required"}};
    size_t limit = std::clamp(p.value("limit", file_mode ? 20 : 8), 1, 40);
    auto terms = words(query);
    std::sort(terms.begin(), terms.end()); terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    std::vector<std::pair<double, size_t>> ranked;
    for (auto id : eligible) {
        const auto& n = impl_->nodes[id];
        double score = 0;
        for (const auto& word : terms) {
            auto term = n.terms.find(word);
            if (term == n.terms.end()) continue;
            double idf = std::log(1 + (impl_->nodes.size() - impl_->frequency.at(word) + .5) / (impl_->frequency.at(word) + .5));
            double tf = term->second;
            score += idf * tf * 2.2 / (tf + 1.2 * (.25 + .75 * n.length / impl_->average_length));
        }
        auto name = n.data.value("name", "");
        if ((name.find('_') != std::string::npos || name.size() > 10) &&
            lower(query).find(lower(name)) != std::string::npos) score += 20;
        // Navigation defaults to implementations. Explicit documentation path
        // scopes still rank headings against one another with the same weights.
        if (n.data.value("kind", "") == "file") score *= .25;
        if (fs::path(n.file).extension() == ".md" || fs::path(n.file).extension() == ".markdown") score *= .2;
        if (file_mode || score > 0) ranked.emplace_back(file_mode ? double(n.data.value("line_start", 0)) : -score, id);
    }
    std::sort(ranked.begin(), ranked.end());
    std::vector<size_t> selected;
    std::map<size_t, double> scores;
    for (const auto& [negative, id] : ranked) {
        if (selected.size() >= limit) break;
        selected.push_back(id); scores[id] = -negative;
    }
    if (!file_mode && p.value("neighbors", true)) {
        std::set<size_t> allowed(eligible.begin(), eligible.end()), present(selected.begin(), selected.end());
        auto seeds = selected;
        for (auto seed : seeds) for (auto ei : impl_->nodes[seed].edges) {
            const auto& e = impl_->edges[ei];
            if (e.to < 0 || e.kind == "references") continue;
            size_t neighbor = e.from == seed ? e.to : e.from;
            if (selected.size() < limit + 8 && allowed.count(neighbor) && present.insert(neighbor).second) selected.push_back(neighbor);
        }
    }
    auto out = impl_->render(selected, scores, file_mode);
    out["question"] = query; out["matched"] = ranked.size(); out["indexed"] = !eligible.empty();
    out["truncated"] = ranked.size() > limit;
    if (file_mode) {
        auto path = fs::weakly_canonical(p.value("path", "")).string();
        if (impl_->files.contains(path)) {
            const auto& f = impl_->files[path];
            out["index_age_minutes"] = std::max<int64_t>(0, (now() - f.value("indexed_at", int64_t(0))) / 60);
            out["stale"] = RepositoryIndex::file_hash(path) != f.value("hash", "");
            out["text"] = "[code-nav] index " + std::to_string(out["index_age_minutes"].get<int64_t>()) + " minutes old" +
                (out["stale"].get<bool>() ? "; STALE — refresh with learn_codebase\n" : "\n") + out["text"].get<std::string>();
        }
    }
    if (!impl_->warning.empty()) out["warning"] = impl_->warning;
    return out;
}
json CodeNavigation::overview(const json& p) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto selected = impl_->scope(p);
    std::set<std::string> paths;
    std::map<size_t, std::vector<size_t>> communities;
    int64_t oldest = now();
    for (auto id : selected) {
        const auto& n = impl_->nodes[id];
        paths.insert(n.file);
        if (n.data.value("kind", "") != "file") communities[impl_->labels[id]].push_back(id);
        oldest = std::min(oldest, impl_->files[n.file].value("indexed_at", oldest));
    }
    std::vector<std::pair<size_t, size_t>> sizes, degrees;
    for (const auto& c : communities) sizes.emplace_back(c.second.size(), c.first);
    for (auto id : selected) if (impl_->nodes[id].data.value("kind", "") != "file") {
        size_t degree = 0;
        for (auto edge : impl_->nodes[id].edges) if (impl_->edges[edge].kind == "calls") ++degree;
        degrees.emplace_back(degree, id);
    }
    auto descending = [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; };
    std::sort(sizes.begin(), sizes.end(), descending); std::sort(degrees.begin(), degrees.end(), descending);
    json out = {{"indexed", !selected.empty()}, {"files", paths.size()}, {"symbols", selected.size() - paths.size()},
                {"communities", json::array()}, {"god_nodes", json::array()}, {"index_age_minutes", (now() - oldest) / 60}};
    std::ostringstream report;
    report << "[code-nav] " << paths.size() << " files, " << out["symbols"] << " symbols; index " << out["index_age_minutes"] << " minutes old\n";
    report << "Communities (deterministic call-graph label propagation):\n";
    for (size_t i = 0; i < std::min<size_t>(8, sizes.size()); ++i) {
        std::map<std::string, size_t> dirs;
        for (auto id : communities[sizes[i].second]) {
            const auto& n = impl_->nodes[id];
            ++dirs[fs::path(n.file).parent_path().lexically_relative(n.root).string()];
        }
        std::string label; size_t best = 0;
        for (const auto& [dir, count] : dirs) if (count > best) { label = dir; best = count; }
        auto representative = communities[sizes[i].second].front();
        for (auto id : communities[sizes[i].second])
            if (impl_->nodes[id].edges.size() > impl_->nodes[representative].edges.size()) representative = id;
        label += " / " + impl_->nodes[representative].data.value("name", "");
        out["communities"].push_back({{"id", sizes[i].second}, {"label", label}, {"symbols", sizes[i].first}});
        report << "  " << one_line(label, 100) << ": " << sizes[i].first << " symbols\n";
    }
    report << "God nodes (highest call-graph degree):\n";
    for (size_t i = 0; i < std::min<size_t>(5, degrees.size()); ++i) {
        const auto& n = impl_->nodes[degrees[i].second];
        auto item = n.data; item["file"] = n.file; item["degree"] = degrees[i].first;
        out["god_nodes"].push_back(item);
        report << "  " << n.data.value("name", "") << " (" << degrees[i].first << ") " << n.file << ":" << n.data["line_start"] << "\n";
    }
    report << "Navigate: code_query(question=..., path=...) then read_symbol(name=..., path=..., line=...)\n";
    out["project"] = selected.empty() ? p.value("project", "") : impl_->nodes[selected.front()].project;
    out["indexed_files"] = paths;
    out["text"] = report.str();
    return out;
}
json CodeNavigation::path(const json& p) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto allowed = impl_->scope(p);
    auto resolve = [&](const std::string& name) {
        std::vector<size_t> result;
        for (auto id : allowed) if (impl_->nodes[id].data["id"] == name || impl_->nodes[id].data["name"] == name) result.push_back(id);
        return result;
    };
    auto from = resolve(p.value("from", "")), to = resolve(p.value("to", ""));
    if (from.size() != 1 || to.size() != 1) return {{"error", "endpoints must resolve uniquely; use code_query IDs or path scope"}, {"from_matches", from.size()}, {"to_matches", to.size()}};
    std::set<size_t> scope(allowed.begin(), allowed.end());
    std::map<size_t, size_t> previous{{from[0], from[0]}};
    std::deque<size_t> queue{from[0]};
    while (!queue.empty() && !previous.count(to[0])) {
        auto id = queue.front(); queue.pop_front();
        for (auto ei : impl_->nodes[id].edges) {
            const auto& edge = impl_->edges[ei];
            if (edge.to < 0) continue;
            size_t next = edge.from == id ? edge.to : edge.from;
            if (scope.count(next) && !previous.count(next)) { previous[next] = id; queue.push_back(next); }
        }
    }
    if (!previous.count(to[0])) return {{"found", false}, {"text", "No connection in the indexed graph"}};
    std::vector<size_t> route;
    for (auto id = to[0];; id = previous[id]) { route.push_back(id); if (id == from[0]) break; }
    std::reverse(route.begin(), route.end());
    auto out = impl_->render(route); out["found"] = true; out["hops"] = route.size() - 1; out["direction"] = "undirected";
    return out;
}
json CodeNavigation::read(const json& p) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto eligible = impl_->scope(p);
    std::vector<size_t> found;
    for (auto id : eligible) {
        const auto& s = impl_->nodes[id].data;
        if (s["name"] == p.value("name", "") && (!p.contains("line") || s["line_start"] == p["line"])) found.push_back(id);
    }
    if (found.empty()) return nullptr;
    if (found.size() != 1) return {{"error", "ambiguous symbol; provide path and line from code_query"}};
    const auto& n = impl_->nodes[found[0]];
    if (RepositoryIndex::file_hash(n.file) != impl_->files[n.file].value("hash", ""))
        return {{"error", "source changed since indexing; run learn_codebase before read_symbol"}};
    auto out = n.data; out["file"] = n.file; out["code"] = n.body;
    out["text"] = n.file + ":" + std::to_string(n.data.value("line_start", 1)) + "\n" + n.body;
    return out;
}
}
