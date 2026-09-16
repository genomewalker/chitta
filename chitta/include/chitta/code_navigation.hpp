#pragma once
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace chitta {
struct ExtractionResult;
// Optional derived sidecar; never participates in recall or snapshot codecs.
class CodeNavigation {
public:
    CodeNavigation();
    ~CodeNavigation();
    void open(const std::string& sidecar);
    void update(const std::string& root, const std::string& project,
                const std::vector<std::string>& files,
                const std::unordered_set<std::string>& changed,
                const ExtractionResult& extraction, bool full);
    void remove(const std::string& path);
    void clear_project(const std::string& project);
    bool has_file(const std::string& path);
    nlohmann::json query(const nlohmann::json& params);
    nlohmann::json overview(const nlohmann::json& params);
    nlohmann::json path(const nlohmann::json& params);
    nlohmann::json read(const nlohmann::json& params);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
