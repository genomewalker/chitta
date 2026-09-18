// chitta_hintd — resident hint-extraction daemon.
//
// Wraps ONE HintYantra over a Unix-domain socket. Extraction only: no FieldStore,
// no embedding model. Loads the hint GGUF once at startup (eliminating the per-fire
// ~850 MB cold-load that the backgrounded scripts/hint_realtime.py pays today), then
// serves single-shot requests:
//
//   request : raw UTF-8 user text; the client half-closes its write side (SHUT_WR)
//             to signal end-of-request.
//   response: the extracted hint (possibly empty), followed by EOF.
//
// Admission control: at most one inference in flight (try_busy). A request that
// arrives while the engine is busy gets an empty response immediately, so the caller
// SKIPs rather than stacking a second 850 MB inference. The per-request wall-clock
// deadline is enforced inside HintYantra via llama_set_abort_callback.
//
// Fail-safe by construction: any client-side error (refused / timeout / empty) means
// the caller skips the hint for that turn. The whole feature is DEFAULT-OFF — the
// Python backend only talks to this socket when CHITTA_HINT_BACKEND=hintd.

#include <chitta/hint_yantra.hpp>
#include <chitta/llm_http.hpp>

#ifdef CHITTA_WITH_LLAMA_CPP

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <iostream>

namespace {

constexpr size_t MAX_REQ_BYTES = 64 * 1024;  // one user turn is <=500 chars; ample.
constexpr int    RECV_TIMEOUT_S = 10;        // bound idle/half-open client sockets.
constexpr int    MAX_INFLIGHT  = 32;         // cap thread-per-connection fan-out.

std::atomic<bool> g_running{true};
std::atomic<int>  g_inflight{0};
int               g_listen_fd = -1;

void on_signal(int) {
    // Async-signal-safe: only a lock-free atomic store. The accept loop uses a
    // 1 s SO_RCVTIMEO so it re-checks this flag promptly without needing to be
    // woken from the handler (shutdown()/close() are not async-signal-safe).
    g_running.store(false);
}

std::string read_all(int fd) {
    std::string buf;
    char tmp[4096];
    while (buf.size() < MAX_REQ_BYTES) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            buf.append(tmp, static_cast<size_t>(n));
        } else if (n == 0) {
            break;                       // EOF: client half-closed — request complete.
        } else if (errno == EINTR) {
            continue;
        } else {
            break;                       // EAGAIN (recv timeout) or hard error: stop.
        }
    }
    // If we hit the cap, drain remaining inbound bytes so the later close() sends a
    // clean FIN rather than an RST that could lose the response we are about to write.
    if (buf.size() >= MAX_REQ_BYTES) {
        char sink[4096];
        while (::read(fd, sink, sizeof(sink)) > 0) {}
    }
    return buf;
}

void write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else break;                      // SIGPIPE is ignored; a dead peer just ends here.
    }
}

void rstrip(std::string& s) {
    while (!s.empty()) {
        char c = s.back();
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') s.pop_back();
        else break;
    }
}

void handle_conn(int fd, chitta::HintYantra* hy, std::mutex* busy, int deadline_ms) {
    timeval tv{RECV_TIMEOUT_S, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string req = read_all(fd);
    rstrip(req);

    std::string resp;
    if (req == "PING") {
        resp = "PONG";                   // cheap liveness probe — no inference.
    } else if (!req.empty()) {
        // try_lock on *busy is the SOLE admission gate: it must stay non-blocking
        // so a concurrent request is skipped (empty resp), never queued behind a
        // second 850 MB inference. HintYantra::extract's own internal mutex must
        // not be relied on for admission.
        std::unique_lock<std::mutex> lk(*busy, std::try_to_lock);
        if (lk.owns_lock()) {
            const auto model = chitta::endpoint_role_model("hint");
            if (!model.empty()) {
                const auto endpoint = chitta::discover_gpu_endpoint("hint", model, nullptr, false);
                if (!endpoint.empty()) resp = chitta::call_llm_http(
                    endpoint, model, req, chitta::HintYantra::system_prompt(),
                    deadline_ms > 0 ? std::max(1, (deadline_ms + 999) / 1000) : 10,
                    0.0f, 256);
            } else {
                resp = hy->extract(req, deadline_ms);
            }
        }
        // Busy → empty response; the caller skips this turn instead of stacking work.
    }

    resp += "\n";
    write_all(fd, resp);
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    g_inflight.fetch_sub(1, std::memory_order_relaxed);
}

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && v[0]) ? std::string(v) : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_path;
    std::string mind_path = env_or("CHITTA_DB_PATH",
                                   env_or("HOME", ".") + "/.claude/mind");
    std::string sock_path = env_or("CHITTA_HINT_SOCK", "");
    int deadline_ms = std::atoi(env_or("CHITTA_HINT_DEADLINE_MS", "8000").c_str());

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--socket")          sock_path  = next();
        else if (a == "--model")      model_path = next();
        else if (a == "--mind-path")  mind_path  = next();
        else if (a == "--deadline-ms") deadline_ms = std::atoi(next().c_str());
        else {
            std::cerr << "chitta_hintd: unknown arg '" << a << "'\n"
                      << "usage: chitta_hintd [--socket PATH] [--model GGUF] "
                         "[--mind-path DIR] [--deadline-ms N]\n";
            return 2;
        }
    }
    if (sock_path.empty()) sock_path = mind_path + "/.hintd.sock";
    if (deadline_ms < 0) deadline_ms = 0;

    // A write to a peer that closed mid-response must not kill us.
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGINT,  on_signal);

    // Leaked intentionally: a resident daemon lives until the process is killed, so
    // never running ~HintYantra means a detached worker still mid-extract() at exit
    // cannot dereference a freed llama_context (the shutdown-UAF the review flagged).
    auto* hy = new chitta::HintYantra(model_path, mind_path);
    if (!hy->ready() && chitta::endpoint_role_model("hint").empty()) {
        std::cerr << "[chitta_hintd] hint model not loaded — set CHITTA_HINT_MODEL or "
                     "place GGUF in ~/.claude/models/. Exiting.\n";
        return 1;
    }

    if (sock_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "[chitta_hintd] socket path too long: " << sock_path << "\n";
        return 1;
    }

    g_listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        std::cerr << "[chitta_hintd] socket() failed: " << strerror(errno) << "\n";
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    // Refuse to hijack a socket a LIVE hintd already owns (restart overlap, or a
    // manual run alongside the service). Only unlink one that is truly stale.
    if (::access(sock_path.c_str(), F_OK) == 0) {
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        bool alive = (probe >= 0 &&
                      ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        if (probe >= 0) ::close(probe);
        if (alive) {
            std::cerr << "[chitta_hintd] another hintd is live on " << sock_path
                      << " — exiting.\n";
            ::close(g_listen_fd);
            return 1;
        }
        ::unlink(sock_path.c_str());
    }

    // Create the socket inode 0600 atomically (no world-accessible window between
    // bind() and chmod()); keep the chmod as belt-and-suspenders and check it.
    mode_t old_umask = ::umask(0177);
    int brc = ::bind(g_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    if (brc < 0) {
        std::cerr << "[chitta_hintd] bind(" << sock_path << ") failed: "
                  << strerror(errno) << "\n";
        ::close(g_listen_fd);
        return 1;
    }
    if (::chmod(sock_path.c_str(), 0600) != 0)
        std::cerr << "[chitta_hintd] warning: chmod 0600 failed: " << strerror(errno) << "\n";

    if (::listen(g_listen_fd, 16) < 0) {
        std::cerr << "[chitta_hintd] listen() failed: " << strerror(errno) << "\n";
        ::close(g_listen_fd);
        ::unlink(sock_path.c_str());
        return 1;
    }

    // Periodic accept() wakeups so the loop re-checks g_running after SIGTERM
    // without an async-signal-unsafe wakeup from inside the handler.
    timeval atv{1, 0};
    ::setsockopt(g_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &atv, sizeof(atv));

    auto* busy_mtx = new std::mutex;     // leaked alongside hy (see above): never destroyed.
    std::cerr << "[chitta_hintd] ready: socket=" << sock_path
              << " deadline_ms=" << deadline_ms << "\n";

    while (g_running.load()) {
        int cfd = ::accept(g_listen_fd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // accept timeout: re-check g_running
            if (!g_running.load()) break;
            if (errno == EBADF || errno == EINVAL) break;
            continue;                          // transient accept error — keep serving.
        }
        // Bound concurrent handlers (busy_mtx already serializes inference, but not
        // connection handling): over the cap, drop the connection — the caller skips.
        if (g_inflight.fetch_add(1, std::memory_order_relaxed) >= MAX_INFLIGHT) {
            g_inflight.fetch_sub(1, std::memory_order_relaxed);
            ::close(cfd);
            continue;
        }
        try {
            std::thread(handle_conn, cfd, hy, busy_mtx, deadline_ms).detach();
        } catch (...) {
            // Thread creation failed (resource pressure): drop this request, stay up.
            g_inflight.fetch_sub(1, std::memory_order_relaxed);
            ::close(cfd);
        }
    }

    if (g_listen_fd >= 0) ::close(g_listen_fd);
    ::unlink(sock_path.c_str());
    std::cerr << "[chitta_hintd] stopped.\n";
    return 0;
}

#else  // !CHITTA_WITH_LLAMA_CPP

#include <iostream>
int main() {
    std::cerr << "chitta_hintd: built without CHITTA_WITH_LLAMA_CPP\n";
    return 1;
}

#endif
