#include <chitta/socket_client.hpp>
#include <chitta/rpc/cli_tools.hpp>
#include <chitta/version.hpp>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace chitta {

namespace {

// SO_PEERCRED identifies the actual listener even for an explicit socket path.
// /proc start ticks plus boot ID distinguish PID reuse and machine restarts.
nlohmann::json cli_daemon_identity(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return nullptr;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return nullptr;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ucred peer{};
    socklen_t size = sizeof(peer);
    bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    bool identified = connected && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0;
    close(fd);
    if (!identified) return nullptr;

    std::ifstream stat("/proc/" + std::to_string(peer.pid) + "/stat");
    std::string line, field, boot;
    std::getline(stat, line);
    const auto end = line.rfind(')'); // comm may itself contain spaces or ')'.
    if (end == std::string::npos) return nullptr;
    std::istringstream fields(line.substr(end + 1));
    for (int number = 3; number <= 22; ++number) {
        if (!(fields >> field)) return nullptr;
    }
    std::ifstream boot_file("/proc/sys/kernel/random/boot_id");
    if (!std::getline(boot_file, boot)) return nullptr;
    return {{"pid", peer.pid}, {"start_time", field}, {"boot_id", boot}};
}

bool normalize_cli_tools(nlohmann::json& tools) {
    if (!tools.is_array()) return false;
    for (auto& tool : tools) {
        if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string() ||
            !tool.contains("inputSchema") || !tool["inputSchema"].is_object()) return false;
        if (!tool.contains("description")) tool["description"] = "";
        if (!tool["description"].is_string()) return false;
        auto& schema = tool["inputSchema"];
        if (!schema.contains("properties") || schema["properties"].is_null())
            schema["properties"] = nlohmann::json::object();
        if (!schema["properties"].is_object()) return false;
        for (const auto& property : schema["properties"]) {
            if (!property.is_object()) return false;
            for (const auto* key : {"type", "description"}) {
                if (property.contains(key) && !property[key].is_string()) return false;
            }
        }
        if (!schema.contains("required") || schema["required"].is_null())
            schema["required"] = nlohmann::json::array();
        if (!schema["required"].is_array()) return false;
        for (const auto& name : schema["required"]) {
            if (!name.is_string()) return false;
        }
    }
    return true;
}

void write_cli_cache(const std::string& path, const nlohmann::json& cache) {
    std::string temp = path + ".XXXXXX";
    int fd = mkstemp(temp.data()); // private, atomic publication, concurrent readers safe
    if (fd < 0) return;
    const auto bytes = cache.dump();
    size_t written = 0;
    while (written < bytes.size()) {
        auto n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    const int closed = close(fd);
    if (written == bytes.size() && closed == 0) std::rename(temp.c_str(), path.c_str());
    unlink(temp.c_str());
}

} // namespace

std::optional<nlohmann::json> discover_cli_tool(const std::string& socket_path,
                                               const std::string& name, nlohmann::json* loading) {
    using json = nlohmann::json;
    const auto socket = std::filesystem::absolute(socket_path).lexically_normal().string();
    const auto path = get_socket_dir() + "/cli-tools-" + std::to_string(djb2_hash(socket)) + ".json";
    json cached = json::object();
    {
        std::ifstream input(path);
        if (input) {
            auto candidate = json::parse(input, nullptr, false);
            if (candidate.is_object() && candidate.value("socket", json()) == socket &&
                candidate.value("version", json()) == 1 && candidate.contains("identity") &&
                candidate.contains("tools") && normalize_cli_tools(candidate["tools"]))
                cached = std::move(candidate);
        }
    }
    auto find = [&](const json& tools) -> std::optional<json> {
        for (const auto& tool : tools) if (tool.at("name") == name) return tool;
        return std::nullopt;
    };
    const auto identity = cli_daemon_identity(socket);
    if (cached.contains("tools") && (identity.is_null() || cached["identity"] == identity)) {
        if (auto tool = find(cached["tools"])) return tool;
    }

    SocketClient client(socket);
    if (!client.connect()) return std::nullopt;
    auto response = client.request(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    if (!response) return std::nullopt;
    auto reply = json::parse(*response, nullptr, false);
    if (loading && reply.is_object() && reply.contains("result") &&
        reply["result"].is_object() && reply["result"].contains("structured") &&
        reply["result"]["structured"].is_object() &&
        reply["result"]["structured"].value("loading", false)) {
        *loading = reply;
        return std::nullopt;
    }
    if (!reply.is_object() || !reply.contains("result") || !reply["result"].is_object() ||
        !reply["result"].contains("tools")) return std::nullopt;
    auto tools = reply["result"]["tools"];
    if (!normalize_cli_tools(tools)) return std::nullopt;
    // Never publish schemas for an identity that changed while fetching them.
    if (!identity.is_null() && cli_daemon_identity(socket) == identity) {
        write_cli_cache(path, {{"version", 1}, {"socket", socket}, {"identity", identity}, {"tools", tools}});
    }
    return find(tools);
}


SocketClient::SocketClient()
    : socket_path_(default_socket_path()) {}

SocketClient::SocketClient(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

SocketClient::~SocketClient() {
    disconnect();
}

bool SocketClient::connect() {
    if (fd_ >= 0) return true;  // Already connected

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        last_error_ = std::string("socket() failed: ") + strerror(errno);
        return false;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        last_error_ = "socket path too long: " + socket_path_;
        close(fd_);
        fd_ = -1;
        return false;
    }
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        last_error_ = std::string("connect() failed: ") + strerror(errno);
        close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

void SocketClient::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    read_buffer_.clear();
}

bool SocketClient::ensure_daemon_running() {
    if (!connect()) {
        last_error_ = "Cannot connect to daemon at " + socket_path_ + " - is daemon running?";
        return false;
    }

    auto health = check_health();
    if (!health) {
        last_error_ = "Daemon did not respond to health check";
        disconnect();
        return false;
    }

    bool compatible = chitta::version::protocol_compatible(
        health->protocol_major, health->protocol_minor);

    if (!compatible) {
        last_error_ = "Daemon v" + health->software + " incompatible with client v" +
                      std::string(CHITTA_VERSION);
        disconnect();
        return false;
    }

    return true;
}

std::optional<DaemonVersion> SocketClient::check_version() {
    using json = nlohmann::json;

    // JSON-RPC request for version info
    auto response = request(R"({"jsonrpc":"2.0","id":0,"method":"tools/call","params":{"name":"version_check"}})");
    if (!response) {
        return std::nullopt;
    }

    try {
        auto j = json::parse(*response);

        // Navigate: result.structured.{software_version, protocol_major, protocol_minor}
        if (!j.contains("result") || !j["result"].contains("structured")) {
            return std::nullopt;
        }

        auto& structured = j["result"]["structured"];
        DaemonVersion ver;
        ver.software = structured.value("software_version", "");
        ver.protocol_major = structured.value("protocol_major", 0);
        ver.protocol_minor = structured.value("protocol_minor", 0);

        if (ver.protocol_major > 0 || !ver.software.empty()) {
            return ver;
        }
    } catch (const std::exception& e) {
        std::cerr << "check_version failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "check_version failed: unknown error" << std::endl;
    }

    return std::nullopt;
}

std::optional<DaemonHealth> SocketClient::check_health() {
    using json = nlohmann::json;

    auto response = request(R"({"jsonrpc":"2.0","id":0,"method":"tools/call","params":{"name":"health_check"}})");
    if (!response) {
        return std::nullopt;
    }

    try {
        auto j = json::parse(*response);

        if (!j.contains("result") || !j["result"].contains("structured")) {
            return std::nullopt;
        }

        auto& structured = j["result"]["structured"];
        DaemonHealth health;
        health.software = structured.value("software_version", "");
        health.protocol_major = structured.value("protocol_major", 0);
        health.protocol_minor = structured.value("protocol_minor", 0);
        health.pid = structured.value("pid", 0);
        health.uptime_ms = structured.value("uptime_ms", 0);
        health.socket_path = structured.value("socket_path", "");
        health.db_path = structured.value("db_path", "");
        health.status = structured.value("status", "");

        if (health.protocol_major > 0 || !health.software.empty() || health.status == "warming_up") {
            return health;
        }
    } catch (const std::exception& e) {
        std::cerr << "check_health failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "check_health failed: unknown error" << std::endl;
    }

    return std::nullopt;
}

bool SocketClient::request_shutdown() {
    if (!connected() && !connect()) {
        return false;
    }

    // Use request_internal directly - don't retry on connection close
    // (daemon closing connection after shutdown is expected behavior)
    auto response = request_internal("shutdown");
    disconnect();

    // Success if we got a response OR if connection was closed (daemon shutting down)
    if (response.has_value()) {
        return true;
    }
    // Connection closed = daemon received shutdown and is stopping
    return last_error_.find("Connection closed") != std::string::npos;
}

bool SocketClient::wait_for_socket_gone(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();

    while (access(socket_path_.c_str(), F_OK) == 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}


std::optional<std::string> SocketClient::request_internal(const std::string& json_rpc) {
    using json = nlohmann::json;

    if (fd_ < 0) {
        last_error_ = "Not connected";
        return std::nullopt;
    }

    // Extract request id so we can match it against the response id.
    // If the request has no id, accept any response.
    std::optional<json> expected_id;
    try {
        auto req = json::parse(json_rpc);
        if (req.contains("id")) expected_id = req["id"];
    } catch (...) { /* malformed request: skip id check */ }

    // Send request (newline-delimited)
    std::string msg = json_rpc + "\n";
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = write(fd_, msg.data() + sent, msg.size() - sent);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pfd = {fd_, POLLOUT, 0};
                poll(&pfd, 1, 1000);
                continue;
            }
            last_error_ = std::string("write() failed: ") + strerror(errno);
            return std::nullopt;
        }
        sent += static_cast<size_t>(n);
    }

    // Read newline-terminated frames, keeping leftover bytes in read_buffer_.
    // Skip any frame whose id doesn't match expected_id (stale/out-of-order response).
    pollfd pfd = {fd_, POLLIN, 0};

    auto extract_frame = [this]() -> std::optional<std::string> {
        size_t pos = read_buffer_.find('\n');
        if (pos == std::string::npos) return std::nullopt;
        std::string frame = read_buffer_.substr(0, pos);
        read_buffer_.erase(0, pos + 1);
        return frame;
    };

    auto frame_id_matches = [&](const std::string& frame) -> bool {
        if (!expected_id) return true;
        try {
            auto j = json::parse(frame);
            return j.contains("id") && j["id"] == *expected_id;
        } catch (...) {
            return false;
        }
    };

    while (true) {
        if (auto frame = extract_frame()) {
            if (frame_id_matches(*frame)) return frame;
            // Null-id error frames (from thread-pool exception handler) match any caller.
            try {
                auto j = json::parse(*frame);
                if (j.contains("error") && (!j.contains("id") || j["id"].is_null())) {
                    return frame;
                }
            } catch (...) {}
            // id mismatch: stale frame, drop and keep reading
            continue;
        }

        int ret = poll(&pfd, 1, RESPONSE_TIMEOUT_MS);

        if (ret < 0) {
            if (errno == EINTR) continue;
            last_error_ = std::string("poll() failed: ") + strerror(errno);
            return std::nullopt;
        }
        if (ret == 0) {
            last_error_ = "Response timeout";
            return std::nullopt;
        }

        char buf[4096];
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) {
            last_error_ = n == 0 ? "Connection closed" :
                          std::string("read() failed: ") + strerror(errno);
            return std::nullopt;
        }

        read_buffer_.append(buf, static_cast<size_t>(n));
        if (read_buffer_.size() > SocketClient::MAX_RESPONSE_SIZE) {
            last_error_ = "Response too large";
            return std::nullopt;
        }
    }
}

std::optional<std::string> SocketClient::request(const std::string& json_rpc) {
    // Try the request
    auto result = request_internal(json_rpc);
    if (result) {
        return result;
    }

    // Request failed - check if it's a connection issue that warrants reconnect
    bool connection_lost = (last_error_.find("Connection closed") != std::string::npos ||
                           last_error_.find("write() failed") != std::string::npos ||
                           last_error_.find("Broken pipe") != std::string::npos ||
                           last_error_.find("Connection reset") != std::string::npos);

    if (!connection_lost) {
        return std::nullopt;  // Some other error, don't retry
    }

    // Connection lost - try simple reconnect (don't start daemon)
    std::cerr << "[socket_client] Connection lost (" << last_error_ << "), attempting reconnect...\n";
    disconnect();

    // Just try to reconnect - don't auto-start daemon
    if (!connect()) {
        std::cerr << "[socket_client] Reconnect failed: " << last_error_ << "\n";
        return std::nullopt;
    }

    std::cerr << "[socket_client] Reconnected, retrying request\n";

    // Retry the request once
    return request_internal(json_rpc);
}

bool SocketClient::connect_only() {
    // Safe connect: never kill or start daemons
    // Use this for parallel agents to avoid killing shared daemon

    // Only set default if not already set by constructor
    if (socket_path_.empty()) {
        socket_path_ = default_socket_path();
    }

    if (!connect()) {
        last_error_ = "Cannot connect to daemon at " + socket_path_ + " - is daemon running?";
        return false;
    }

    // Loading is a responsive daemon, not a connection failure. Forward the
    // caller's request immediately so its loading/retry response stays visible.
    auto health = check_health();
    if (!health) {
        last_error_ = "Daemon did not respond to health check";
        disconnect();
        return false;
    }
    if (health->status == "warming_up") return true;

    bool compatible = chitta::version::protocol_compatible(
        health->protocol_major, health->protocol_minor);

    if (!compatible) {
        last_error_ = "Daemon v" + health->software + " incompatible with client v" +
                      std::string(CHITTA_VERSION) + " - restart daemon manually";
        disconnect();
        return false;
    }

    return true;
}

} // namespace chitta
