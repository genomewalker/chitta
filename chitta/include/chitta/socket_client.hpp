#pragma once
// Socket Client: Unix domain socket client for daemon IPC
//
// Connects to the soul daemon via Unix socket without auto-start.
//
// Mind-scoped: Socket path derived from mind database path.
// Version compatibility checked via handshake.

#include <chitta/socket_server.hpp>  // For socket_path_for_mind
#include <chitta/version.hpp>
#include <string>
#include <optional>
#include <cstdlib>
#include <unistd.h>

namespace chitta {

// Version info returned by daemon
struct DaemonVersion {
    std::string software;      // e.g., "2.36.0"
    int protocol_major = 0;
    int protocol_minor = 0;
};

struct DaemonHealth {
    std::string software;
    int protocol_major = 0;
    int protocol_minor = 0;
    int pid = 0;
    uint64_t uptime_ms = 0;
    std::string socket_path;
    std::string db_path;
    std::string status;
};

class SocketClient {
public:
    // Get mind path from env or default
    static std::string default_mind_path() {
        if (const char* db_path = std::getenv("CHITTA_DB_PATH")) {
            return db_path;
        }
        if (const char* home = std::getenv("HOME")) {
            return std::string(home) + "/.claude/mind";
        }
        return "";
    }

    // Mind-scoped paths - derived from mind database path
    static std::string default_socket_path() {
        return socket_path_for_mind(default_mind_path());
    }
    static std::string default_lock_path() {
        return lock_path_for_mind(default_mind_path());
    }
    static std::string default_pid_path() {
        return pid_path_for_mind(default_mind_path());
    }
    static constexpr int CONNECT_TIMEOUT_MS = 5000;
    static constexpr int RESPONSE_TIMEOUT_MS = 300000;  // 5 minutes for learn_codebase
    static constexpr size_t MAX_RESPONSE_SIZE = 16 * 1024 * 1024;

    // Default constructor uses UID-scoped socket path
    SocketClient();
    explicit SocketClient(std::string socket_path);
    ~SocketClient();

    // Non-copyable, non-movable (owns file descriptor)
    SocketClient(const SocketClient&) = delete;
    SocketClient& operator=(const SocketClient&) = delete;
    SocketClient(SocketClient&&) = delete;
    SocketClient& operator=(SocketClient&&) = delete;

    // Connection management
    bool connect();
    void disconnect();
    bool connected() const { return fd_ >= 0; }

    // Connect to daemon and verify version compatibility.
    // Never restarts or kills running daemons.
    bool ensure_daemon_running();

    // Safe connect: only connect to existing daemon, never kill/restart
    // Returns false with error if daemon not running or version mismatch
    // Use this for parallel agents to avoid killing shared daemon
    bool connect_only();

    // Check daemon version (must be connected)
    std::optional<DaemonVersion> check_version();

    // Check daemon health (must be connected)
    std::optional<DaemonHealth> check_health();

    // Request graceful daemon shutdown (for upgrades)
    bool request_shutdown();

    // Send JSON-RPC request, wait for response
    std::optional<std::string> request(const std::string& json_rpc, int timeout_ms = RESPONSE_TIMEOUT_MS);

    // Error message from last failed operation
    const std::string& last_error() const { return last_error_; }

    // Get socket path (for logging/debugging)
    const std::string& socket_path() const { return socket_path_; }

    // Wait for socket to disappear (for shutdown verification)
    bool wait_for_socket_gone(int timeout_ms);

private:
    std::string socket_path_;
    int fd_ = -1;
    std::string last_error_;
    std::string read_buffer_;  // bytes read past the last \n, kept for next request

    // Internal request without auto-reconnect (used by request())
    std::optional<std::string> request_internal(const std::string& json_rpc, int timeout_ms = RESPONSE_TIMEOUT_MS);
};

} // namespace chitta
