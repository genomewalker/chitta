#pragma once
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace chitta {
inline bool runtime_local_enabled() {
    const char* value = std::getenv("CHITTA_RUNTIME_LOCAL");
    return value && std::string(value) == "1";
}

// Same algorithm as runtime_state_dir in hooks/lib.sh: canonical mind path,
// DJB2 over UTF-8 bytes modulo 2^64, sixteen lowercase hexadecimal digits.
// The store and its cross-host instance lock never use this resolver.
inline std::string runtime_state_dir(const std::string& mind) {
    if (!runtime_local_enabled()) return mind;
    auto canonical = std::filesystem::weakly_canonical(mind).string();
    uint64_t hash = 5381;
    for (unsigned char byte : canonical) hash = hash * 33 + byte;
    std::ostringstream name;
    name << std::hex << std::setfill('0') << std::setw(16) << hash;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    auto root = std::filesystem::path(xdg && *xdg ? xdg : "/tmp");
    return (root / "chitta" / name.str()).string();
}

// Explicit aliases retain precedence in both placements.
inline std::string queue_path_for_mind(const std::string& mind) {
    for (const char* name : {"CHITTA_QUEUE", "CHITTA_QUEUE_PATH"}) {
        if (const char* value = std::getenv(name); value && *value) return value;
    }
    return (std::filesystem::path(runtime_state_dir(mind)) / "queue.jsonl").string();
}

// Single daemon on this node drains an append-only local ledger tail. The
// intent records the destination offset BEFORE append, closing the crash gap
// between durable append and local offset publication. Never truncate NFS data.
class RuntimeLedger {
    struct Fd {
        int value;
        explicit Fd(int fd) : value(fd) {
            if (fd < 0) throw std::runtime_error("runtime ledger open failed");
        }
        ~Fd() { ::close(value); }
    };
    std::string local_, durable_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    int interval_ = 5;

    static void write_all(int fd, const std::string& text) {
        size_t done = 0;
        while (done < text.size()) {
            auto n = ::write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("runtime ledger write failed");
            done += static_cast<size_t>(n);
        }
    }
    static void sync(int fd) {
        if (::fsync(fd)) throw std::runtime_error("runtime ledger fsync failed");
    }
    void publish(const std::string& path, const std::string& text) {
        Fd fd(::open((path + ".tmp").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
        write_all(fd.value, text);
        sync(fd.value);
        std::filesystem::rename(path + ".tmp", path);
        Fd dir(::open(local_.c_str(), O_RDONLY | O_DIRECTORY));
        sync(dir.value);
    }
public:
    explicit RuntimeLedger(const std::string& mind)
        : local_(runtime_state_dir(mind)), durable_(mind + "/outcome_ledger.jsonl") {
        std::filesystem::create_directories(local_);
        if (const char* raw = std::getenv("CHITTA_LEDGER_FLUSH_SECONDS")) {
            char* end = nullptr;
            long value = std::strtol(raw, &end, 10);
            if (!*end && value > 0 && value <= 300) interval_ = static_cast<int>(value);
        }
    }
    void flush(bool force = false) {
        const std::string tail = local_ + "/outcome_ledger.tail";
        const std::string offset_path = tail + ".offset";
        const std::string intent_path = tail + ".intent";
        uint64_t offset = 0, length = 0, destination = 0;
        { std::ifstream in(offset_path); if (in.good() && !(in >> offset))
            throw std::runtime_error("runtime ledger invalid offset"); }
        bool recovering = std::filesystem::exists(intent_path);
        if (recovering) {
            std::ifstream in(intent_path);
            if (!(in >> offset >> length >> destination))
                throw std::runtime_error("runtime ledger invalid intent");
        }
        if (!std::filesystem::exists(tail)) return;
        auto size = std::filesystem::file_size(tail);
        if (size < offset) throw std::runtime_error("runtime ledger tail truncated");
        auto now = std::chrono::steady_clock::now();
        if (!recovering && !force && size - offset < 65536 &&
            now - last_ < std::chrono::seconds(interval_)) return;
        if (size == offset && !recovering) { last_ = now; return; }
        // Bound each flush's allocation. Include complete records only; hooks
        // can append concurrently because this file is never truncated/rotated.
        auto count = recovering ? length : std::min<uint64_t>(size - offset, 1024 * 1024);
        std::string data(count, '\0');
        std::ifstream input(tail, std::ios::binary);
        input.seekg(offset);
        input.read(data.data(), static_cast<std::streamsize>(count));
        data.resize(static_cast<size_t>(input.gcount()));
        if (recovering && data.size() != length)
            throw std::runtime_error("runtime ledger incomplete recovery tail");
        if (!recovering) {
            auto newline = data.rfind('\n');
            if (newline == std::string::npos) {
                // One unusually large event must not stall every later flush.
                std::string remainder;
                if (!std::getline(input, remainder) || input.eof()) return;
                data += remainder + "\n";
            } else {
                data.resize(newline + 1);
            }
            length = data.size();
        }
        // This is a ledger append lock, never the store's instance lock.
        Fd lock(::open((durable_ + ".append.lock").c_str(), O_CREAT | O_RDWR, 0600));
        if (::flock(lock.value, LOCK_EX | LOCK_NB)) return;
        Fd output(::open(durable_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0600));
        auto end = ::lseek(output.value, 0, SEEK_END);
        if (end < 0) throw std::runtime_error("runtime ledger seek failed");
        size_t already = 0;
        if (recovering) {
            if (static_cast<uint64_t>(end) < destination)
                throw std::runtime_error("runtime ledger destination truncated");
            already = std::min<uint64_t>(length, static_cast<uint64_t>(end) - destination);
            std::string existing(already, '\0');
            auto n = ::pread(output.value, existing.data(), already, destination);
            if (n != static_cast<ssize_t>(already) || existing != data.substr(0, already))
                throw std::runtime_error("runtime ledger recovery conflict; tail retained");
        } else {
            destination = static_cast<uint64_t>(end);
            publish(intent_path, std::to_string(offset) + " " + std::to_string(length)
                    + " " + std::to_string(destination) + "\n");
        }
        write_all(output.value, data.substr(already));
        sync(output.value);
        Fd durable_dir(::open(std::filesystem::path(durable_).parent_path().c_str(), O_RDONLY | O_DIRECTORY));
        sync(durable_dir.value);
        publish(offset_path, std::to_string(offset + length) + "\n");
        std::filesystem::remove(intent_path);
        last_ = now;
    }
};
} // namespace chitta
