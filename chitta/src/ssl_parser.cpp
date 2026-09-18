#include "../include/chitta/ssl_parser.hpp"
#include "../include/chitta/temporal.hpp"
#include <sstream>
#include <algorithm>

namespace chitta {

// Extract @date annotation from triplet object
// Format: "object @YYYY-MM-DD" or "object @yesterday" etc.
std::pair<std::string, std::string> SSLParser::extract_date_from_object(const std::string& object) {
    // Match @YYYY-MM-DD or @relative_expression at end of object
    static const std::regex date_suffix_pattern(
        R"(^(.*?)\s*@(\d{4}-\d{2}-\d{2}|yesterday|today|last\s+\d*\s*(?:day|week|month|year)s?)$)",
        std::regex::icase
    );

    std::smatch match;
    if (std::regex_match(object, match, date_suffix_pattern)) {
        std::string clean_object = match[1].str();
        std::string date_expr = match[2].str();

        // Trim trailing whitespace from clean_object
        while (!clean_object.empty() && std::isspace(clean_object.back())) {
            clean_object.pop_back();
        }

        return {clean_object, date_expr};
    }

    return {object, ""};
}

std::string SSLParser::type_to_category(const std::string& type) {
    if (type == "SOLUTION") return "solution";
    if (type == "GOTCHA") return "gotcha";
    if (type == "DECISION") return "decision";
    if (type == "PATTERN") return "pattern";
    if (type == "PREFERENCE") return "preference";
    if (type == "BELIEF") return "belief";
    if (type == "FAILURE") return "failure";
    if (type == "AFFECT") return "affect";
    if (type == "CORRECTION") return "correction";
    if (type == "EVENT") return "event";
    if (type == "OPERATIONAL") return "operational";
    return "wisdom";
}

// Parse affect valence/arousal from SSL v0.3 A:v,a format or legacy (valence:v, arousal:a)
static void parse_affect_values(const std::string& content, float& valence, float& arousal) {
    // v0.3 format: A:+0.5,0.4 or A:-0.3,0.6
    static const std::regex v03_pattern(R"(A:([+\-]?\d+\.?\d*),(\d+\.?\d*))");
    std::smatch match;
    if (std::regex_search(content, match, v03_pattern)) {
        try {
            valence = std::stof(match[1].str());
            arousal = std::stof(match[2].str());
            valence = std::max(-1.0f, std::min(1.0f, valence));
            arousal = std::max(0.0f, std::min(1.0f, arousal));
            return;
        } catch (...) {}
    }
    // Legacy format: (valence:-0.7, arousal:0.8)
    static const std::regex legacy_pattern(
        R"(valence:\s*([+\-]?\d+\.?\d*)\s*,\s*arousal:\s*(\d+\.?\d*))");
    if (std::regex_search(content, match, legacy_pattern)) {
        try {
            valence = std::stof(match[1].str());
            arousal = std::stof(match[2].str());
            valence = std::max(-1.0f, std::min(1.0f, valence));
            arousal = std::max(0.0f, std::min(1.0f, arousal));
        } catch (...) {}
    }
}

// Parse F:FLAG annotations — returns vector of flag names
static std::vector<std::string> parse_flags(const std::string& content) {
    std::vector<std::string> flags;
    static const std::regex flag_pattern(R"(F:([A-Z_,]+))");
    std::smatch match;
    if (std::regex_search(content, match, flag_pattern)) {
        std::istringstream iss(match[1].str());
        std::string flag;
        while (std::getline(iss, flag, ',')) {
            if (!flag.empty()) flags.push_back(flag);
        }
    }
    return flags;
}

// Parse →@ref cross-references — returns vector of ref names
static std::vector<std::string> parse_refs(const std::string& content) {
    std::vector<std::string> refs;
    static const std::regex ref_pattern(R"(→@([a-zA-Z0-9_-]+))");
    auto begin = std::sregex_iterator(content.begin(), content.end(), ref_pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        refs.push_back((*it)[1].str());
    }
    return refs;
}

// Strip v0.3 annotations (A:v,a F:FLAG →@ref) from content for clean storage
static std::string strip_annotations(const std::string& content) {
    std::string result = content;
    // Strip A:v,a
    result = std::regex_replace(result, std::regex(R"(\s*A:[+\-]?\d+\.?\d*,\d+\.?\d*)"), "");
    // Strip F:FLAG
    result = std::regex_replace(result, std::regex(R"(\s*F:[A-Z_,]+)"), "");
    // Strip →@ref
    result = std::regex_replace(result, std::regex(R"(\s*→@[a-zA-Z0-9_-]+)"), "");
    // Trim trailing whitespace
    while (!result.empty() && std::isspace(result.back())) result.pop_back();
    return result;
}

// Extract @file:line citations from text
std::vector<SSLCitation> SSLParser::extract_inline_citations(const std::string& text) {
    std::vector<SSLCitation> citations;

    // Match @path/file.ext:line or @path/file.ext patterns
    static const std::regex cite_pattern(R"(@([\w./\-]+(?:\.\w+)?):?(\d*))");

    auto begin = std::sregex_iterator(text.begin(), text.end(), cite_pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        SSLCitation cite;
        cite.file = (*it)[1].str();
        std::string line_str = (*it)[2].str();
        try { cite.line = line_str.empty() ? 0 : std::stoi(line_str); }
        catch (const std::out_of_range&) { cite.line = 0; }
        citations.push_back(std::move(cite));
    }

    return citations;
}

// Parse explicit [CITE] line: file:line optional context
SSLCitation SSLParser::parse_cite_line(const std::string& line) {
    SSLCitation cite;

    // Format: [CITE] file:line context  OR  [CITE] file context
    static const std::regex cite_line_pattern(R"(^\[CITE\]\s+([\w./\-]+(?:\.\w+)?):?(\d*)\s*(.*)?$)");

    std::smatch match;
    if (std::regex_match(line, match, cite_line_pattern)) {
        cite.file = match[1].str();
        std::string line_str = match[2].str();
        try { cite.line = line_str.empty() ? 0 : std::stoi(line_str); }
        catch (const std::out_of_range&) { cite.line = 0; }
        cite.context = match[3].str();

        // Trim context whitespace
        while (!cite.context.empty() && std::isspace(cite.context.back())) {
            cite.context.pop_back();
        }
    }

    return cite;
}

// Normalize the spellings emitted by distillation models before splitting chains.
static std::string normalize_arrows(std::string text) {
    static const std::regex latex(R"(\$?\s*\\rightarrow\s*\$?)");
    text = std::regex_replace(text, latex, "→");
    for (const std::string arrow : {"->", "=>"}) {
        size_t at = 0;
        while ((at = text.find(arrow, at)) != std::string::npos) {
            text.replace(at, arrow.size(), "→");
            at += std::string("→").size();
        }
    }
    return text;
}

static std::string trim_entity(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

static std::vector<std::string> chain_entities(const std::string& part) {
    std::string value = trim_entity(part);
    if (value.empty()) return {};
    if (value.front() != '{') return {value};
    if (value.back() != '}') return {};
    std::vector<std::string> entities;
    std::istringstream members(value.substr(1, value.size() - 2));
    std::string member;
    while (std::getline(members, member, ',')) {
        member = trim_entity(member);
        if (member.empty()) return {};
        entities.push_back(member);
    }
    return entities;
}

SSLParser::Result SSLParser::parse(const std::string& output) {
    Result result;

    // State machine for parsing
    std::string current_type;
    std::string current_content;
    std::vector<SSLCitation> current_citations;

    // Regex for typed markers
    static const std::regex type_pattern(R"(^\[(SOLUTION|GOTCHA|DECISION|PATTERN|PREFERENCE|BELIEF|INSIGHT|FAILURE|AFFECT|CORRECTION|EVENT|OPERATIONAL)\]\s+(.*)$)");
    static const std::regex triplet_pattern(R"(^\[TRIPLET\]\s+(\S+)\s+(\S+)\s+(.+)$)");
    static const std::regex cite_line_pattern(R"(^\[CITE\]\s+)");

    auto store_current = [&]() {
        if (current_type.empty() || current_content.empty()) return;

        SSLLearning learning;
        learning.type = current_type;
        learning.content = current_content;
        learning.category = type_to_category(current_type);

        // Extract title (first line, truncated)
        size_t newline_pos = current_content.find('\n');
        if (newline_pos != std::string::npos) {
            learning.title = current_content.substr(0, std::min(newline_pos, size_t(100)));
        } else {
            learning.title = current_content.substr(0, std::min(current_content.size(), size_t(100)));
        }

        // Add explicit [CITE] citations collected during parsing
        learning.citations = std::move(current_citations);
        current_citations.clear();

        // Parse v0.3 annotations from all types (affect, flags, refs)
        parse_affect_values(current_content, learning.affect_valence, learning.affect_arousal);
        learning.flags = parse_flags(current_content);
        learning.refs = parse_refs(current_content);

        // Strip annotations from stored content (keep it clean)
        learning.content = strip_annotations(learning.content);
        // Recompute title from cleaned content
        size_t np = learning.content.find('\n');
        learning.title = learning.content.substr(0, std::min(np == std::string::npos ? learning.content.size() : np, size_t(100)));

        // Also extract inline @file:line citations from content
        auto inline_cites = extract_inline_citations(current_content);
        for (auto& cite : inline_cites) {
            // Avoid duplicates
            bool exists = false;
            for (const auto& existing : learning.citations) {
                if (existing.file == cite.file && existing.line == cite.line) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                learning.citations.push_back(std::move(cite));
            }
        }

        // Each physical line is a separate chain; verbatim epsilon lines are not facts.
        std::istringstream chains(current_content);
        std::string chain;
        while (std::getline(chains, chain)) {
            chain = trim_entity(chain);
            if (chain.rfind("[ε]", 0) == 0) continue;
            chain = trim_entity(strip_annotations(normalize_arrows(chain)));
            // A leading [domain] scopes the learning, not the subject entity.
            if (!chain.empty() && chain.front() == '[') {
                const auto domain_end = chain.find(']');
                if (domain_end != std::string::npos)
                    chain = trim_entity(chain.substr(domain_end + 1));
            }
            const bool choice = current_type == "DECISION" && chain.find('|') != std::string::npos;
            const std::string predicate = choice ? "chosen_over" :
                current_type == "GOTCHA" ? "causes" : "leads_to";
            if (choice) {
                chain = chain.substr(0, chain.find('|'));
                // The daemon's prompt also uses choice>alternative|reason.
                if (chain.find("→") == std::string::npos) {
                    const auto greater = chain.find('>');
                    if (greater != std::string::npos) chain.replace(greater, 1, "→");
                }
            }
            if (chain.find("→") == std::string::npos) continue;
            std::vector<std::vector<std::string>> nodes;
            size_t begin = 0;
            while (true) {
                const auto arrow = chain.find("→", begin);
                nodes.push_back(chain_entities(chain.substr(begin, arrow == std::string::npos ? arrow : arrow - begin)));
                if (arrow == std::string::npos) break;
                begin = arrow + std::string("→").size();
            }
            if (std::any_of(nodes.begin(), nodes.end(), [](const auto& n) { return n.empty(); })) continue;
            for (size_t i = 1; i < nodes.size(); ++i) {
                for (const auto& subject : nodes[i - 1]) {
                    for (const auto& object : nodes[i]) {
                        auto [clean_object, date_expr] = extract_date_from_object(object);
                        if (clean_object.empty()) continue;
                        SSLTriplet triplet;
                        triplet.subject = subject;
                        triplet.predicate = predicate;
                        triplet.object = clean_object;
                        triplet.date_annotation = date_expr;
                        result.triplets.push_back(std::move(triplet));
                    }
                }
            }
        }
        result.learnings.push_back(std::move(learning));
        current_type.clear();
        current_content.clear();
    };

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip empty lines and markdown artifacts
        if (line.empty()) continue;
        if (line.substr(0, 3) == "```") continue;
        if (line == "---") continue;

        std::smatch match;

        // Check for typed marker line
        if (std::regex_match(line, match, type_pattern)) {
            store_current();
            current_type = match[1].str();
            current_content = match[2].str();
        }
        // Check for epsilon (verbatim) line
        else if (line.rfind("[ε]", 0) == 0 && !current_type.empty()) {
            current_content += "\n" + line;
        }
        // Check for citation line
        else if (std::regex_search(line, cite_line_pattern) && !current_type.empty()) {
            auto cite = parse_cite_line(line);
            if (!cite.file.empty()) {
                current_citations.push_back(std::move(cite));
            }
        }
        // Check for triplet relationship
        else if (std::regex_match(line, match, triplet_pattern)) {
            store_current();

            SSLTriplet triplet;
            triplet.subject = match[1].str();
            triplet.predicate = match[2].str();
            std::string raw_object = strip_annotations(match[3].str());

            // Trim whitespace from object
            while (!raw_object.empty() && std::isspace(raw_object.back())) {
                raw_object.pop_back();
            }

            // Extract @date annotation from object if present
            auto [clean_object, date_expr] = extract_date_from_object(raw_object);
            triplet.object = clean_object;
            triplet.date_annotation = date_expr;

            if (!triplet.subject.empty() && !triplet.predicate.empty() && !triplet.object.empty()) {
                result.triplets.push_back(std::move(triplet));
            }
        }
        // Continuation line for current marker (no prefix)
        else if (!current_type.empty() && line[0] != '[') {
            current_content += "\n" + line;
        }
    }

    // Store final marker if pending
    store_current();

    return result;
}

SSLParser::Result SSLParser::parse_with_context(const std::string& output, int64_t context_date_ms) {
    // First parse normally
    Result result = parse(output);

    // If context_date_ms is provided, resolve @date annotations in triplets
    if (context_date_ms > 0) {
        for (auto& triplet : result.triplets) {
            if (!triplet.date_annotation.empty()) {
                auto resolved = TemporalResolver::resolve(triplet.date_annotation, context_date_ms);
                if (resolved) {
                    triplet.valid_from_ms = resolved->timestamp_ms;
                }
            }
        }
    }

    return result;
}

} // namespace chitta
