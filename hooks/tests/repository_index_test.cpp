#include "chitta/code_intel.hpp"
#include <cassert>
#include <cstdlib>

int main() {
    char scratch[] = "/tmp/chitta-source-index-XXXXXX";
    auto dir = mkdtemp(scratch);
    assert(dir);
    const std::filesystem::path base(dir), repo = base / "repo", other = base / "other";
    std::filesystem::create_directories(repo / ".git");
    std::filesystem::create_directories(other / ".git");
    auto write = [](const auto& p, const auto& text) { std::ofstream out(p); out << text; };
    write(repo / "guide.md", "# Runtime\nUse CHITTA_EXAMPLE_CACHE for the cache timeout.\n");
    write(other / "guide.md", "# Runtime\nUse CHITTA_OTHER_CACHE for the unrelated cache.\n");
    chitta::RepositoryIndex index;
    index.open((base / "roots.json").string(), "[]");
    index.index(repo.string(), "project:test");
    index.index(other.string(), "project:other");
    auto hits = index.search("which env var controls CHITTA_EXAMPLE_CACHE?", "project:test", 3);
    assert(!hits.empty() && hits[0]["type"] == "doc");
    auto identity = hits[0]["id"];
    auto hash = hits[0]["content_hash"];
    auto stamp = std::filesystem::last_write_time(repo / "guide.md");
    // Missed watcher, same mtime: content hash still forces new chunks.
    write(repo / "guide.md", "# Runtime\nUse CHITTA_REPLACEMENT_CACHE for the cache timeout.\n");
    std::filesystem::last_write_time(repo / "guide.md", stamp);
    hits = index.search("which env var controls CHITTA_REPLACEMENT_CACHE?", "project:test", 3);
    assert(!hits.empty() && hits[0]["id"] == identity && hits[0]["content_hash"] != hash);
    assert(hits[0]["text"].get<std::string>().find("CHITTA_EXAMPLE_CACHE") == std::string::npos);
    assert(index.search("CHITTA_REPLACEMENT_CACHE", "project:absent", 3).empty());
    chitta::RepositoryIndex reopened;
    write(repo / "guide.md", "# Runtime\nCHITTA_RESTART_CACHE controls startup cache validation.\n");
    reopened.open((base / "roots.json").string(), "[]");
    hits = reopened.search("which env var is CHITTA_RESTART_CACHE?", "project:test", 3);
    assert(!hits.empty() && hits[0]["id"] == identity);
    std::filesystem::remove(repo / "guide.md");
    assert(reopened.search("CHITTA_RESTART_CACHE", "project:test", 3).empty());
    write(repo / "added.sh", "#!/bin/bash\nexport CHITTA_NEW_SOURCE=1\n");
    assert(!reopened.search("where is CHITTA_NEW_SOURCE", "project:test", 3).empty());
    const auto input = (repo / "added.sh").string();
    const auto captured = chitta::RepositoryIndex::file_hash(input);
    auto anchor = chitta::RepositoryIndex::source_anchor("[done] compile input:" + input + " sha:" + captured, nullptr);
    assert(anchor.is_object() && anchor["content_hash"] == captured);
    write(repo / "added.sh", "#!/bin/bash\nexport CHITTA_NEW_SOURCE=2\n");
    auto delayed = chitta::RepositoryIndex::source_anchor("[done] compile input:" + input + " sha:" + captured, nullptr);
    assert(delayed == anchor); // Never relabel delayed facts with today's hash.
    auto next_hash = chitta::RepositoryIndex::file_hash(input);
    auto next = chitta::RepositoryIndex::source_anchor("[done] compile input:" + input + " sha:" + next_hash, nullptr);
    assert(next["scope"] == anchor["scope"] && next["content_hash"] != anchor["content_hash"]);
    auto other_fact = chitta::RepositoryIndex::source_anchor("[done] lint input:" + input + " sha:" + next_hash, nullptr);
    assert(other_fact["scope"] != next["scope"]);
    assert(chitta::RepositoryIndex::source_anchor("[done] compile input:" + input + " sha:abcd", nullptr).is_null());
    anchor["path"] = "../secret";
    assert(chitta::RepositoryIndex::source_anchor("[artifact] example", anchor).is_null());
    assert(chitta::RepositoryIndex::content_hash("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(!chitta::RepositoryIndex::repository_question("my favorite breakfast"));
    std::filesystem::remove_all(base);
}
