#include "../include/chitta/ingester.hpp"
#include "../include/chitta/ssl_gloss.hpp"
#include <array>
#include <cstdio>
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace chitta {

Ingester::Ingester(FieldStore& field, EmbedFn embedder, const IngestConfig& config)
    : field_store_(&field), embedder_(std::move(embedder)), config_(config) {}

void Ingester::log(const std::string& msg) {
    if (log_callback_) log_callback_(msg);
    else if (config_.verbose) std::cerr << msg << "\n";
}

SourceType Ingester::detect_type(const std::string& source) {
    if (source.substr(0, 7) == "http://" || source.substr(0, 8) == "https://")
        return SourceType::Url;
    if (std::filesystem::is_directory(source))
        return SourceType::Directory;
    return SourceType::File;
}

std::string Ingester::fetch_url(const std::string& url) {
    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0) return "";

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return "";
    }

    if (pid == 0) {
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(stdout_pipe[1]);
        execlp("curl", "curl", "-sL", "--max-time", "30", url.c_str(), nullptr);
        _exit(1);
    }

    close(stdout_pipe[1]);
    std::string output;
    std::array<char, 8192> buffer;
    ssize_t n;
    while ((n = read(stdout_pipe[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), n);
    }
    close(stdout_pipe[0]);
    waitpid(pid, nullptr, 0);

    // Strip HTML tags if present (crude but functional)
    if (output.find("<html") != std::string::npos || output.find("<HTML") != std::string::npos) {
        // Try to extract text from HTML using a simple approach
        std::string text;
        bool in_tag = false;
        bool in_script = false;
        bool in_style = false;
        for (size_t i = 0; i < output.size(); ++i) {
            if (output[i] == '<') {
                // Check for script/style opening
                std::string lower;
                for (size_t j = i; j < std::min(i + 20, output.size()); ++j)
                    lower += std::tolower(output[j]);
                if (lower.find("<script") == 0) in_script = true;
                if (lower.find("<style") == 0) in_style = true;
                if (lower.find("</script") == 0) in_script = false;
                if (lower.find("</style") == 0) in_style = false;
                in_tag = true;
            } else if (output[i] == '>') {
                in_tag = false;
            } else if (!in_tag && !in_script && !in_style) {
                text += output[i];
            }
        }
        // Collapse whitespace
        std::string collapsed;
        bool was_space = false;
        for (char c : text) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (c == ' ' && was_space) continue;
            was_space = (c == ' ');
            collapsed += c;
        }
        return collapsed;
    }

    return output;
}

std::string Ingester::read_file(const std::string& path) {
    // PDF: use pdftotext
    if (path.size() > 4 && path.substr(path.size() - 4) == ".pdf") {
        int stdout_pipe[2];
        if (pipe(stdout_pipe) < 0) return "";

        pid_t pid = fork();
        if (pid < 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); return ""; }

        if (pid == 0) {
            close(stdout_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
            close(stdout_pipe[1]);
            execlp("pdftotext", "pdftotext", "-layout", path.c_str(), "-", nullptr);
            _exit(1);
        }
        close(stdout_pipe[1]);
        std::string output;
        std::array<char, 8192> buffer;
        ssize_t n;
        while ((n = read(stdout_pipe[0], buffer.data(), buffer.size())) > 0)
            output.append(buffer.data(), n);
        close(stdout_pipe[0]);
        waitpid(pid, nullptr, 0);
        return output;
    }

    // Regular file
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::vector<std::string> Ingester::read_directory(const std::string& path) {
    std::vector<std::string> contents;
    static const std::vector<std::string> extensions = {
        ".md", ".txt", ".py", ".rs", ".cpp", ".hpp", ".c", ".h",
        ".yaml", ".yml", ".json", ".toml", ".sh", ".r", ".R", ".pdf"
    };

    for (auto& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        bool match = false;
        for (const auto& e : extensions) {
            if (ext == e) { match = true; break; }
        }
        if (!match) continue;

        std::string content = read_file(entry.path().string());
        if (!content.empty()) {
            contents.push_back("# Source: " + entry.path().string() + "\n\n" + content);
        }
    }
    return contents;
}

std::vector<std::string> Ingester::chunk_text(const std::string& text) {
    std::vector<std::string> chunks;
    if (text.size() <= config_.chunk_size_chars) {
        chunks.push_back(text);
        return chunks;
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = std::min(pos + config_.chunk_size_chars, text.size());

        // Try to break at paragraph boundary
        if (end < text.size()) {
            size_t para = text.rfind("\n\n", end);
            if (para != std::string::npos && para > pos + config_.chunk_size_chars / 2)
                end = para + 2;
        }

        chunks.push_back(text.substr(pos, end - pos));
        pos = (end > config_.chunk_overlap) ? end - config_.chunk_overlap : end;

        if (static_cast<int>(chunks.size()) >= config_.max_chunks) {
            log("[ingest] Hit max_chunks limit (" + std::to_string(config_.max_chunks) + ")");
            break;
        }
    }
    return chunks;
}

std::string Ingester::build_ingest_prompt(const std::string& chunk, const std::string& source) {
    return std::string(R"(Extract learnings from this document in SSL v0.3 format.

Source: )") + source + R"(

## SSL v0.3 format

Two tiers: Tier 1 (code-bearing: SOLUTION, GOTCHA, PATTERN) with [ε], Tier 2 (narrative: DECISION, PREFERENCE, FAILURE) dense.

Types:
| Type | Tier | Use for |
|------|------|---------|
| [SOLUTION] | 1 | What worked: commands, fixes, approaches |
| [GOTCHA] | 1 | Traps: counterintuitive behavior, edge cases |
| [PATTERN] | 1 | Reusable techniques that generalize |
| [DECISION] | 2 | Design choices: why X over Y |
| [CORRECTION] | 1 | Updates to prior beliefs |

SSL symbols: → (produces) > (chose over) | (or/reason) + (with) @ (location) ! (negation) ? (uncertainty)

Annotations: A:v,a (affect: valence,arousal) F:FLAG (ORIGIN,CORE,PIVOT,GENESIS,TURNING) →@ref (cross-reference)

Relationships:
```
[TRIPLET] subject predicate object
```

## Rules

1. **Verbatim**: Commands, code, exact values go in [ε] lines (Tier 1 only)
2. **Compress**: Tier 2 uses dense symbol chains — no [ε]
3. **Specific**: Include exact values, names, versions
4. **No fluff**: Only non-obvious findings worth remembering
5. **Domain tags**: Use [domain] after the type marker
6. **Affect required**: Every learning must have A:v,a

---

DOCUMENT:
)" + chunk + R"(

---

Output ONLY SSL-formatted learnings with A:v,a affect annotations (no explanations, no markdown headers):)";
}

std::string Ingester::call_llm(const std::string& prompt) {
    return call_llm_http(
        config_.endpoint, config_.model, prompt,
        "You are a research knowledge extractor. Extract learnings in SSL v0.3 format. "
        "Output ONLY SSL-formatted learnings with A:v,a affect annotations.",
        config_.timeout_secs, 0.3f, 4096,
        [this](const std::string& msg) { log(msg); }, std::nullopt, "teacher", true);
}

void Ingester::store_learnings(const SSLParser::Result& ssl, const std::string& realm,
                                const std::string& source, IngestResult& result) {
    // Store source episode first
    std::string ep_text = "[ingest] source=" + source;
    std::vector<float> ep_emb;
    if (embedder_) ep_emb = embedder_(ep_text);
    uint64_t episode_id = 0;
    try {
        episode_id = field_store_->remember("episode", realm, ep_text, ep_emb, 1.0f, 0.0f);
    } catch (...) {}

    for (const auto& learning : ssl.learnings) {
        std::string full_text = learning.title + "\n" + learning.content;

        std::vector<float> embedding;
        if (embedder_) {
            embedding = embedder_(chitta::ssl::retrieval_text(full_text));
        }

        // Dedup check
        bool deduped = false;
        if (!embedding.empty() && config_.dedup_threshold > 0.0f) {
            auto hits = field_store_->recall(embedding, 5, realm);
            for (const auto& hit : hits) {
                if (hit.semantic_score >= config_.dedup_threshold) {
                    field_store_->strengthen(hit.memory_id, 0.05f);
                    result.learnings_deduped++;
                    deduped = true;
                    break;
                }
            }
        }
        if (deduped) continue;

        float confidence = 0.85f;
        float decay = 0.001f;
        if (learning.category == "correction") { confidence = 0.95f; decay = 0.0001f; }
        else if (learning.category == "preference") { confidence = 0.90f; decay = 0.0001f; }

        uint64_t mem_id = 0;
        try {
            mem_id = field_store_->remember("wisdom", realm, full_text,
                                            embedding, confidence, decay);
        } catch (...) { continue; }

        if (mem_id == 0) continue;
        result.learnings_stored++;

        // Apply affect dimensions (v0.3: from A:v,a on any type)
        if (learning.affect_valence != 0.0f || learning.affect_arousal != 0.0f) {
            field_store_->set_affect(mem_id, learning.affect_valence, learning.affect_arousal);
            log("[ingest]     affect: v=" + std::to_string(learning.affect_valence) +
                " a=" + std::to_string(learning.affect_arousal));
        }

        // Apply structural flags (v0.3: F:FLAG)
        for (const auto& flag : learning.flags) {
            field_store_->add_triplet(std::to_string(mem_id), "has_flag", flag, 1.0f, mem_id);
        }

        // Apply cross-references (v0.3: →@ref)
        for (const auto& ref : learning.refs) {
            field_store_->add_triplet(std::to_string(mem_id), "references", ref, 1.0f, mem_id);
        }

        // Link to source episode
        if (episode_id > 0) {
            field_store_->add_triplet(std::to_string(mem_id), "derived_from",
                                      std::to_string(episode_id), 1.0f, mem_id);
        }

        // Source triplet
        field_store_->add_triplet(std::to_string(mem_id), "ingested_from", source, 1.0f, mem_id);

        log("[ingest]   +" + learning.category + ": " +
            learning.title.substr(0, 60));
    }

    // Store triplets
    for (const auto& triplet : ssl.triplets) {
        field_store_->add_triplet(triplet.subject, triplet.predicate, triplet.object);
        result.triplets_created++;
    }
}

IngestResult Ingester::ingest(const std::string& source, const std::string& realm,
                               SourceType type) {
    IngestResult result;

    if (type == SourceType::Auto)
        type = detect_type(source);

    log("[ingest] Source: " + source + " (type=" +
        (type == SourceType::Url ? "url" : type == SourceType::File ? "file" : "directory") + ")");

    std::vector<std::string> all_text;

    if (type == SourceType::Url) {
        std::string content = fetch_url(source);
        if (content.empty()) {
            result.error = "Failed to fetch URL: " + source;
            return result;
        }
        all_text.push_back(content);
    } else if (type == SourceType::Directory) {
        all_text = read_directory(source);
        if (all_text.empty()) {
            result.error = "No supported files in directory: " + source;
            return result;
        }
    } else {
        std::string content = read_file(source);
        if (content.empty()) {
            result.error = "Failed to read file: " + source;
            return result;
        }
        all_text.push_back(content);
    }

    log("[ingest] Read " + std::to_string(all_text.size()) + " source(s)");

    for (const auto& text : all_text) {
        auto chunks = chunk_text(text);
        log("[ingest] " + std::to_string(chunks.size()) + " chunk(s)");

        for (size_t i = 0; i < chunks.size(); ++i) {
            log("[ingest] Processing chunk " + std::to_string(i + 1) + "/" +
                std::to_string(chunks.size()));

            std::string prompt = build_ingest_prompt(chunks[i], source);
            std::string llm_output = call_llm(prompt);

            if (llm_output.empty()) {
                log("[ingest] Chunk " + std::to_string(i + 1) + " returned empty — skipping");
                continue;
            }

            auto ssl_result = ssl_parser_.parse(llm_output);
            store_learnings(ssl_result, realm, source, result);
            result.chunks_processed++;
        }
    }

    result.success = result.chunks_processed > 0;
    log("[ingest] Done: " + std::to_string(result.learnings_stored) + " stored, " +
        std::to_string(result.learnings_deduped) + " deduped, " +
        std::to_string(result.triplets_created) + " triplets");

    return result;
}

} // namespace chitta
