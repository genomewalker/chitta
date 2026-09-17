#pragma once
#include <chitta/queue_path.hpp>
#include <openssl/sha.h>
#include <iostream>
#include <iterator>
#include <regex>
#include <sys/stat.h>
#include <vector>

namespace chitta::output_cache {
inline long setting(const char* key, long fallback) {
    try {
        const char* value = std::getenv(key);
        if (!value) return fallback;
        size_t end = 0;
        auto n = std::stol(value, &end);
        return end == std::string(value).size() && n >= 0 ? n : fallback;
    } catch (...) { return fallback; }
}
inline size_t chars(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text) if ((c & 0xc0) != 0x80) ++n;
    return n;
}
inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    for (size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        end = end == std::string::npos ? text.size() : end + 1;
        result.push_back(text.substr(begin, end - begin));
        begin = end;
    }
    return result;
}
inline std::filesystem::path directory() {
    const char* mind = std::getenv("CHITTA_DB_PATH");
    if (!mind) mind = std::getenv("CC_SOUL_DB_PATH");
    const char* home = std::getenv("HOME");
    return std::filesystem::path(runtime_state_dir(mind ? mind :
        std::string(home ? home : "") + "/.claude/mind")) / "outputs";
}
inline void expire(const std::filesystem::path& dir) {
    std::error_code error;
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto ttl = std::chrono::hours(std::min(setting("CHITTA_OUTPUT_REF_TTL_H", 48), 876000L));
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        const auto name = entry.path().filename().string();
        if (name.size() != 12 || name.find_first_not_of("0123456789abcdef") != std::string::npos)
            continue;
        if (entry.is_symlink(error) || !entry.is_regular_file(error)) continue;
        const auto modified = entry.last_write_time(error);
        if (!error && now - modified >= ttl) std::filesystem::remove(entry.path(), error);
    }
}
inline std::string cap(const std::string& text) {
    const auto limit = static_cast<size_t>(setting("CHITTA_OUTPUT_CAP_CHARS", 6000));
    if (chars(text) <= limit) return text;
    try {
        const auto dir = directory();
        std::filesystem::create_directories(dir);
        ::chmod(dir.c_str(), 0700);
        expire(dir);
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
        std::ostringstream hash;
        for (size_t i = 0; i < 6; ++i)
            hash << std::hex << std::setfill('0') << std::setw(2) << unsigned(digest[i]);
        const auto target = dir / hash.str();
        if (std::filesystem::exists(target)) {
            std::ifstream old(target, std::ios::binary);
            const std::string previous((std::istreambuf_iterator<char>(old)), {});
            if (previous != text) throw std::runtime_error("output hash collision");
        }
        std::string temporary = (dir / ".output-XXXXXX").string();
        int fd = ::mkstemp(temporary.data());
        if (fd < 0) throw std::runtime_error("output cache create failed");
        bool ok = true;
        size_t offset = 0;
        while (offset < text.size()) {
            auto n = ::write(fd, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            offset += static_cast<size_t>(n);
        }
        if (::close(fd) != 0) ok = false;
        if (!ok || ::rename(temporary.c_str(), target.c_str()) != 0) {
            ::unlink(temporary.c_str());
            throw std::runtime_error("output cache write failed");
        }
        const auto rows = lines(text);
        const auto header = "§ref:" + hash.str() + "§ " + std::to_string(chars(text)) +
            " chars, " + std::to_string(rows.size()) +
            " lines; first 20 and last 20 lines follow:\n";
        std::string preview;
        for (size_t i = 0; i < std::min(size_t(20), rows.size()); ++i) preview += rows[i];
        if (rows.size() > 40) preview += "\n…\n";
        for (size_t i = std::max(size_t(20), rows.size() > 20 ? rows.size() - 20 : 0);
             i < rows.size(); ++i) preview += rows[i];
        // Very long lines must not defeat the cap. The reference retains all bytes.
        const size_t room = limit > header.size() + 48 ? limit - header.size() - 48 : 0;
        if (preview.size() > room) {
            size_t first = room / 2, last = text.size() - room / 2;
            while (first && (static_cast<unsigned char>(text[first]) & 0xc0) == 0x80) --first;
            while (last < text.size() && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80) ++last;
            preview = text.substr(0, first) + "\n… [long-line preview truncated] …\n" + text.substr(last);
        }
        return header + preview;
    } catch (const std::exception& error) {
        std::cerr << "output_cap: " << error.what() << "; passing full output\n";
        return text;  // A cache failure must never lose the original output.
    }
}
inline int run(const std::string& tool, int argc, char** argv, int index) {
    if (tool == "output_cap") {
        const std::string text((std::istreambuf_iterator<char>(std::cin)), {});
        if (std::cin.bad()) return 1;
        std::cout << cap(text);
        return std::cout.good() ? 0 : 1;
    }
    std::string hash, range;
    for (int i = index + 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--hash" && i + 1 < argc) hash = argv[++i];
        else if (std::string(argv[i]) == "--lines" && i + 1 < argc) range = argv[++i];
        else { std::cerr << "usage: chitta output_ref --hash <12 hex> [--lines a-b]\n"; return 2; }
    }
    if (!std::regex_match(hash, std::regex("[0-9a-f]{12}"))) return 2;
    size_t first = 1, last = SIZE_MAX;
    if (!range.empty()) {
        if (!std::regex_match(range, std::regex("[1-9][0-9]*-[1-9][0-9]*"))) return 2;
        try { first = std::stoull(range); last = std::stoull(range.substr(range.find('-') + 1)); }
        catch (...) { return 2; }
        if (first > last) return 2;
    }
    try {
        auto dir = directory();
        expire(dir);
        if (std::filesystem::is_symlink(dir / hash)) return 1;
        std::ifstream input(dir / hash, std::ios::binary);
        if (!input) { std::cerr << "output reference not found or expired\n"; return 1; }
        if (range.empty()) std::cout << input.rdbuf();
        else {
            const std::string text((std::istreambuf_iterator<char>(input)), {});
            const auto rows = lines(text);
            for (size_t i = first - 1; i < std::min(last, rows.size()); ++i) std::cout << rows[i];
        }
        return std::cout.good() ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}  // namespace chitta::output_cache
