// chitta-cli: Simplified daemon for SimpleMind
//
// Usage: chittad <command> [options]
//
// Commands:
//   daemon     Run background daemon
//   shutdown   Stop running daemon
//   status     Check daemon status
//   stats      Show soul statistics

#ifdef __linux__
#include <sys/inotify.h>
#endif
#include <chitta/field_store.hpp>
#include <chitta/rpc/field_handler.hpp>
#include <chitta/mind/subconscious.hpp>
#include <chitta/sadhana/sadhana_manager.hpp>
#include <chitta/rpc/thread_pool.hpp>
#include <chitta/rpc/protocol.hpp>
#include <chitta/socket_server.hpp>
#include <chitta/socket_client.hpp>
#include <chitta/native_distiller.hpp>
#include <chitta/daemon_config.hpp>
#include <chitta/daemon_lifecycle.hpp>
#include <chitta/distillation.hpp>
#include <chitta/queue_processor.hpp>
#include <chitta/http_viz.hpp>
#include <chitta/version.hpp>
#include <chitta/vak_ollama.hpp>
#include <chitta/vak_llama.hpp>
#include <chitta/vak_timeout.hpp>
#include <chitta/hint_yantra.hpp>
#include <chitta/embed_queue.hpp>
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <unistd.h>
#include <climits>
#include <fcntl.h>
#if defined(__linux__)
#include <malloc.h>
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

using namespace chitta;
using json = nlohmann::json;

// Global flags
// Verify lock-free for async-signal-safety in daemon_signal_handler
static_assert(std::atomic<bool>::is_always_lock_free, "atomic<bool> must be lock-free for signal handler");
std::atomic<bool> daemon_running{true};
std::atomic<bool> verbose_mode{false};

// Distillation configuration
// Config structs, path resolution, daemon lifecycle, distillation:
// → daemon_config.hpp, daemon_lifecycle.hpp, distillation.hpp

void daemon_signal_handler(int sig) {
    std::cerr << "[daemon] Signal " << sig << " received, shutting down\n";
    daemon_running = false;
}

int cmd_daemon(FieldStore& field_store, VakYantra* yantra, chitta::EmbedQueue* embed_queue,
               int interval,
               SocketServer& server, const std::string& socket_path, const std::string& mind_path,
               const std::string& pid_file,
               const DistillConfig& distill_config, EnrichConfig& enrich_config,
               const SubconsciousConfig& subconscious_config, bool no_autonomous,
               int http_port, const std::string& http_static_dir,
               int rpc_port,
               DaemonLock& lock) {
    // Automatically reap child processes to prevent zombie accumulation
    signal(SIGCHLD, SIG_IGN);

    // Distillation uses HTTP to Ollama/vLLM (endpoint auto-discovered)
    if (distill_config.enabled) {
        std::cerr << "[daemon] Distillation enabled (timer=" << distill_config.interval_minutes
                  << "m, min_turns=" << distill_config.min_turns
                  << ", model=" << distill_config.model;
        if (distill_config.token_trigger_chars > 0) {
            std::cerr << ", token_trigger=" << distill_config.token_trigger_chars
                      << " chars, cooldown=" << distill_config.cooldown_seconds << "s";
        }
        std::cerr << ")\n";
    }

    if (enrich_config.enabled) {
        std::cerr << "[daemon] Code enrichment enabled (interval=" << enrich_config.interval_minutes
                  << "m, batch=" << enrich_config.batch_size
                  << ", idle=" << enrich_config.idle_seconds << "s)\n";
    }

    if (!pid_file.empty()) {
        std::ofstream pf(pid_file);
        if (pf) pf << getpid() << "\n";
    }

    // server already bound and started (early socket in main)

    FieldRpcHandler handler(&field_store, yantra);
    if (embed_queue) handler.set_embed_queue(embed_queue);
    handler.set_mind_path(mind_path);
    handler.set_distill_model(distill_config.model);
    handler.set_distill_enabled(distill_config.enabled);

    // Start subconscious background processor
    Subconscious subconscious(&field_store, yantra, subconscious_config);

    subconscious.start();
    handler.set_subconscious(&subconscious);
    subconscious.set_rpc_mutex(&handler.rpc_mutex());

    // HTTP visualization server (optional)
    std::unique_ptr<VizServer> viz_server;
    if (http_port > 0) {
        std::string viz_dir = http_static_dir;
        if (viz_dir.empty()) {
            // Derive from executable path: go up to cc-soul/, then docs/mind-viz/
#ifdef __linux__
            auto exe_path = std::filesystem::read_symlink("/proc/self/exe");
            viz_dir = exe_path.parent_path().parent_path().string() + "/docs/mind-viz";
#else
            // Fallback: derive from argv[0] or current working directory
            viz_dir = std::filesystem::current_path().string() + "/docs/mind-viz";
#endif
        }
        viz_server = std::make_unique<VizServer>(http_port, viz_dir, &field_store);
        viz_server->start();
        std::cerr << "[VizServer] listening on :" << http_port
                  << " (static=" << viz_dir << ")\n";
        handler.set_recall_callback(
            [&viz_server](const std::vector<uint64_t>& ids, int passes) {
                if (viz_server) viz_server->push_recall_event(ids, passes);
            });
    }

    // HTTP RPC server — stateless JSON-RPC over HTTP so clients survive daemon restarts
    std::unique_ptr<httplib::Server> rpc_http_svr;
    std::thread rpc_http_thread;
    if (rpc_port > 0) {
        rpc_http_svr = std::make_unique<httplib::Server>();
        rpc_http_svr->Post("/", [&handler](const httplib::Request& req, httplib::Response& res) {
            auto parsed = json::parse(req.body, nullptr, false);
            if (parsed.is_discarded()) {
                res.status = 400;
                res.set_content(R"({"error":"invalid json"})", "application/json");
                return;
            }
            res.set_content(
                handler.handle(parsed).dump(-1, ' ', false, json::error_handler_t::replace),
                "application/json");
        });
        rpc_http_thread = std::thread([&rpc_http_svr, rpc_port]() {
            rpc_http_svr->listen("127.0.0.1", rpc_port);
        });
        std::cerr << "[rpc-http] listening on 127.0.0.1:" << rpc_port << "\n";
    }

    // Sadhana manager — deferred until FieldStore is ready
    std::unique_ptr<SadhanaManager> sadhana_manager;
    std::cerr << "[daemon] Sadhana manager will init when chitta-field is ready\n";

    // Wire dream/think callbacks (unless --no-autonomous)
    if (!no_autonomous) {
        // Wire dream callback: auto-explore when soul has been idle for 10+ minutes
        subconscious.set_dream_callback([&]() {
            if (sadhana_manager)
                for (const auto& s : sadhana_manager->list_active())
                    if (s.goal_dsl.value("kind", "") == "dream") return;
            try {
                json req = {
                    {"method", "tools/call"},
                    {"params", {
                        {"name", "dream_wander"},
                        {"arguments", json::object()}
                    }},
                    {"id", nullptr}
                };
                handler.handle(req);
                std::cerr << "[dream] Auto-dream triggered\n";
            } catch (const std::exception& e) {
                std::cerr << "[dream] Auto-dream failed: " << e.what() << "\n";
            }
        });

        // Wire think callback: internal memory synthesis when idle 5+ min, hourly
        subconscious.set_think_callback([&]() {
            if (sadhana_manager)
                for (const auto& s : sadhana_manager->list_active())
                    if (s.goal_dsl.value("kind", "") == "think") return;
            try {
                json req = {
                    {"method", "tools/call"},
                    {"params", {
                        {"name", "think_wander"},
                        {"arguments", json::object()}
                    }},
                    {"id", nullptr}
                };
                handler.handle(req);
                std::cerr << "[think] Auto-think triggered\n";
            } catch (const std::exception& e) {
                std::cerr << "[think] Auto-think failed: " << e.what() << "\n";
            }
        });

        // Wire belief maintenance callback
        subconscious.set_maintenance_callback([&handler]() {
            handler.run_belief_maintenance();
        });
    }

    std::signal(SIGTERM, daemon_signal_handler);
    std::signal(SIGINT, daemon_signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    std::cerr << "[daemon] Started (socket=" << socket_path
              << ", interval=" << interval << "s, pid=" << getpid() << ")\n";

#ifdef __linux__
    // Record binary mtime at startup for self-update detection.
    // /proc/self/exe resolves to the actual binary path even through symlinks.
    auto startup_binary_mtime = std::filesystem::last_write_time("/proc/self/exe");
#endif

    // compact_wal runs on its own dedicated thread; declared here so the maintenance
    // lambda can check it and pause sync_foreign while compaction is in progress.
    std::atomic<bool> compact_wal_inflight{false};

    // Maintenance thread - sync and apply decay periodically
    std::atomic<size_t> cycle_count{0};
    auto maintenance_start = std::chrono::steady_clock::now();
    std::thread maintenance([&]() {
        auto interval_secs = std::chrono::seconds(interval);
        auto last_sync = std::chrono::steady_clock::now();
        auto last_embedding_flush = std::chrono::steady_clock::now();
        // Defer first sync_foreign by 60s (startup grace period).
        // sync_foreign holds ~40 Rust write guards; firing every 5s during the
        // restart-storm (many MCP sessions reconnecting with backlogged WAL ops)
        // blocks all pool workers for 4-5s per call → pool deadlock.
        // After 60s the backlog is drained and batches shrink to <1ms.
        auto last_foreign_sync = std::chrono::steady_clock::now() + std::chrono::seconds(55);
        auto last_binary_check = std::chrono::steady_clock::now();
        auto embedding_flush_interval = std::chrono::seconds(5);   // Flush queued embeddings every 5s
        auto foreign_sync_interval   = std::chrono::seconds(30);  // Ingest peer segment files every 30s
        // ceiling: 5s interval + 4-5s sync duration (148-segment WAL backlog) → 80% pool blockage
        // upgrade: adaptive interval (measure sync duration, scale next gap proportionally)
        auto binary_check_interval   = std::chrono::seconds(60);  // Check for updated binary every 60s

#ifdef __linux__
        // inotify watcher on segments/ dir for same-host peer writes
        int inotify_fd = inotify_init1(IN_NONBLOCK);
        std::string seg_dir = mind_path + "/chitta-field/segments";
        if (inotify_fd >= 0) {
            inotify_add_watch(inotify_fd, seg_dir.c_str(),
                              IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE);
        }
#else
        int inotify_fd = -1;  // No inotify on non-Linux; rely on polling fallback
#endif

        while (daemon_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto now_time = std::chrono::steady_clock::now();

            // Flush embedding queue frequently (non-blocking if empty)
            if (now_time - last_embedding_flush >= embedding_flush_interval) {
                last_embedding_flush = now_time;
                try {
                    subconscious.flush_embedding_queue();
                } catch (const std::exception& e) {
                    std::cerr << "[maint] Embedding flush failed: " << e.what() << "\n";
                }
            }

            // Backfill runs in a dedicated thread (backfill_thread below).

            // Tick sadhana manager for autonomous agents (runs every 100ms loop)
            try {
                if (sadhana_manager) sadhana_manager->tick();
            } catch (const std::exception& e) {
                std::cerr << "[maint] Sadhana tick failed: " << e.what() << "\n";
            }

            // Ingest new ops from peer instances.
            // Triggered immediately by inotify (same-host) or every 5s (NFS fallback).
            bool seg_event = false;
#ifdef __linux__
            if (inotify_fd >= 0) {
                alignas(struct inotify_event) char ibuf[4096];
                ssize_t len = ::read(inotify_fd, ibuf, sizeof(ibuf));
                if (len > 0) seg_event = true;
            }
#endif
            // ceiling: inotify fires sync_foreign every 100ms loop tick during WAL floods
            // (restart storms: 5+ MCP sessions reconnecting). Each sync_foreign holds
            // ~40 Rust write guards → pool workers never get read access → deadlock.
            // Rate-limit inotify-triggered syncs to ≥1s; 5s timer path unchanged.
            // Skip sync_foreign entirely while compact_wal is running: with 150+ WAL
            // segments each sync takes 4-5s and blocks all pool workers via rpc_mutex_.
            // After compact_wal completes, segment count drops to ~1 and syncs are fast.
            auto inotify_min_interval = foreign_sync_interval;
            bool inotify_ready = seg_event && (now_time - last_foreign_sync >= inotify_min_interval);
            if (!compact_wal_inflight.load(std::memory_order_relaxed) &&
                (inotify_ready || (now_time - last_foreign_sync >= foreign_sync_interval))) {
                try {
                    // EXCLUSIVE rpc lock, not shared: sync_foreign acquires
                    // ~40 Rust write guards in canonical order, which makes it
                    // a deadlock partner for ANY tool path holding one lock
                    // while acquiring an earlier-ordered one (production
                    // deadlock 2026-06-11: recall CW-refresh Phase A held
                    // semantic_idx.read wanting states.read while sync held
                    // states.write wanting semantic_idx readers drained).
                    // Mutual exclusion with tools removes the multi-holder
                    // from every such cycle. Batches are small (~ms) outside
                    // restart storms.
                    auto _lk = handler.acquire_lock();
                    int applied = field_store.sync_foreign();
                    if (verbose_mode && applied > 0) {
                        std::cerr << "[maint] sync_foreign: applied " << applied << " peer ops\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[maint] sync_foreign failed: " << e.what() << "\n";
                }
                // Timestamp AFTER sync completes so the next fire is foreign_sync_interval
                // after this one FINISHES — pool workers always get a free window between syncs.
                // (Formerly set before sync; with 4s syncs every 5s, pool was blocked 80%.)
                last_foreign_sync = std::chrono::steady_clock::now();
            }

#ifdef __linux__
            // Self-update detection: restart if binary has been replaced on disk.
            // Uses /proc/self/exe and systemctl — Linux only.
            if (now_time - last_binary_check >= binary_check_interval) {
                last_binary_check = now_time;
                try {
                    auto current_mtime = std::filesystem::last_write_time("/proc/self/exe");
                    if (current_mtime != startup_binary_mtime) {
                        // PR4 self-update gate: probe the replacement binary's compiled
                        // vector-space id. If it differs from ours, the new binary would
                        // refuse this store's snapshots (.shdr fence) and restart-loop — so
                        // do NOT auto-restart; ack the mtime and require an operator restart.
                        char self_path[PATH_MAX];
                        ssize_t len = ::readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
                        bool compatible = true;
                        if (len > 0) {
                            self_path[len] = '\0';
                            std::string probe = std::string(self_path) + " format-id 2>/dev/null";
                            if (FILE* p = ::popen(probe.c_str(), "r")) {
                                char buf[64];
                                if (::fgets(buf, sizeof(buf), p)) {
                                    unsigned long long new_vsid = std::strtoull(buf, nullptr, 10);
                                    unsigned long long own_vsid =
                                        (unsigned long long)cf_compiled_vector_space_id();
                                    if (new_vsid != 0 && new_vsid != own_vsid) {
                                        compatible = false;
                                        std::cerr << "[maint] Binary updated but its store vector-space ("
                                                  << new_vsid << ") differs from the running store ("
                                                  << own_vsid << "); NOT auto-restarting — operator "
                                                  << "restart required after store migration.\n";
                                    }
                                }
                                ::pclose(p);
                            }
                        }
                        if (compatible) {
                            std::cerr << "[maint] Binary updated, restarting daemon...\n";
                            { auto _lk = handler.acquire_lock(); field_store.flush(); }
                            daemon_running = false;
                            ::execlp("systemctl", "systemctl", "--user", "restart", "chittad", nullptr);
                            if (len > 0) { ::execv(self_path, nullptr); }
                            ::exit(0);
                        } else {
                            // Ack the new mtime so we don't re-probe (and re-warn) every tick.
                            startup_binary_mtime = current_mtime;
                        }
                    }
                } catch (...) {}
            }
#endif

            if (now_time - last_sync >= interval_secs) {
                last_sync = now_time;
                cycle_count++;

                try {
                    auto _lk = handler.acquire_shared_lock();
                    field_store.flush();
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    auto [demoted, pruned] = field_store.run_demotion(now_ms);
                    if (verbose_mode && (demoted > 0 || pruned > 0)) {
                        std::cerr << "[maint] Cycle " << cycle_count
                                  << ": demoted=" << demoted
                                  << ", pruned=" << pruned << "\n";
                    }
                    if (verbose_mode) {
                        std::cerr << "[maint] Cycle " << cycle_count << " complete\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[maint] Cycle failed: " << e.what() << "\n";
                }
            }
        }
#ifdef __linux__
        if (inotify_fd >= 0) close(inotify_fd);
#endif
    });

    // Condvar used by the backfill thread to wake immediately when a write stores a
    // pending memory instead of sleeping the full 30s poll interval.
    std::mutex embed_cv_mutex;
    std::condition_variable embed_cv;

    handler.set_write_notify_callback([&embed_cv]() {
        embed_cv.notify_one();
    });

    // Backfill thread — re-embeds memories stored while yantra was unavailable.
    // Runs independently so ONNX calls never block the maintenance thread or RPCs.
    std::thread backfill_thread([&]() {
        // Run at lower scheduling priority so RPC handler threads are never starved.
#if defined(__linux__)
        ::nice(10);
#endif
        // NOTE: do NOT prepend "search_document: " here — EmbedQueue's worker calls
        // VakYantra::transform(text) which defaults to EmbedMode::Document and adds the
        // "search_document: " prefix itself. Prepending again double-prefixes every
        // document ("search_document: search_document: …"), while queries get a single
        // "search_query: " — that asymmetry collapses query↔document cosine at recall.
        // Initial delay: let WAL replay and HNSW load finish before hammering ONNX.
        for (int i = 0; i < 150 && daemon_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

        while (daemon_running) {
            if (embed_queue) {
                try {
                    std::vector<uint64_t> pending;
                    std::vector<std::string> contents;
                    {
                        auto _lk = handler.acquire_lock();
                        pending = field_store.pending_embeddings(100);
                        contents.reserve(pending.size());
                        for (uint64_t id : pending)
                            contents.push_back(field_store.get_content(id));
                    }
                    if (!pending.empty()) {
                        std::cerr << "[backfill] Processing " << pending.size() << " memories\n";
                        for (size_t i = 0; i < pending.size() && daemon_running; ++i) {
                            if (contents[i].empty()) continue;
                            auto text  = chitta::ssl::retrieval_text(contents[i]);
                            // READ path with long timeout — gets vector back for persistence.
                            // Backfill runs at nice(10) so this 30s wait never blocks RPCs.
                            auto emb = embed_queue->query(text,
                                                          std::chrono::milliseconds(30000));
                            if (emb.size() == EMBED_DIM) {
                                // MUST hold acquire_lock() here: backfill_embedding()
                                // calls hnsw.upsert() which acquires semantic_idx.write().
                                // Without the C++ lock, this races with pool workers
                                // holding rpc_mutex_.rdlock() while waiting for
                                // semantic_idx.read() → ABBA deadlock with sync_foreign
                                // (which holds semantic_idx.write() and waits for
                                // rpc_mutex_.wrlock()). Serialising via acquire_lock()
                                // breaks the cycle: no pool worker can hold rpc_mutex_
                                // while backfill holds semantic_idx.write().
                                auto _lk = handler.acquire_lock();
                                field_store.backfill_embedding(pending[i], emb);
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    if (verbose_mode)
                        std::cerr << "[backfill] Error: " << e.what() << "\n";
                }
            }
            // Wait up to 5s for a write-notify or until shutdown.
            // fire_write_notify() wakes us immediately when a new pending memory arrives.
            {
                std::unique_lock<std::mutex> lk(embed_cv_mutex);
                embed_cv.wait_for(lk, std::chrono::seconds(5),
                                  [&] { return !daemon_running.load(); });
            }
        }
    });

    // Distillation thread - process transcripts periodically
    std::atomic<size_t> distill_count{0};
    std::thread distillation([&]() {
        if (!distill_config.enabled) return;

        auto interval_mins = std::chrono::minutes(distill_config.interval_minutes);
        auto last_distill = std::chrono::steady_clock::now();

        // Initial delay to let things settle — interruptible on shutdown
        for (int _i = 0; _i < 30 && daemon_running; ++_i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        auto last_busy_skip_start = std::chrono::steady_clock::now();

        while (daemon_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            auto now_time = std::chrono::steady_clock::now();
            if (now_time - last_distill >= interval_mins) {
                // Skip if daemon is actively handling queries (prevent blocking)
                if (!subconscious.is_idle()) {
                    auto busy_duration = now_time - last_busy_skip_start;
                    if (busy_duration < std::chrono::minutes(5)) {
                        if (verbose_mode) {
                            std::cerr << "[distill] Skipping - daemon is busy\n";
                        }
                        continue;  // Still within cap, skip
                    }
                    std::cerr << "[distill] Busy-skip cap reached, running distillation anyway\n";
                }
                last_busy_skip_start = now_time;

                last_distill = now_time;

                try {
                    // Scan transcript directory for pending transcripts
                    std::vector<TranscriptState> transcripts;
                    std::string transcript_dir = mind_path + "/transcripts";
                    try {
                        for (const auto& entry : std::filesystem::directory_iterator(transcript_dir)) {
                            if (!entry.is_regular_file()) continue;
                            auto path = entry.path();
                            if (path.extension() != ".jsonl") continue;
                            TranscriptState ts;
                            ts.session_id = path.stem().string();
                            ts.transcript_path = path.string();
                            ts.realm = "brahman";
                            // Look up realm from transcript registry
                            std::optional<std::string> reg, progress;
                            {
                                auto _lk = handler.acquire_lock();
                                reg = field_store.get_latest_event("transcript", "register", ts.session_id);
                                progress = field_store.get_latest_event("transcript", "progress", ts.session_id);
                            }
                            if (reg) {
                                try {
                                    auto r = json::parse(*reg);
                                    ts.realm = r.value("realm", "brahman");
                                } catch (...) {}
                            }
                            ts.last_processed_line = 0;
                            // Check FieldStore for last progress event
                            if (progress) {
                                try {
                                    auto p = json::parse(*progress);
                                    ts.last_processed_line = p.value("last_line", (int64_t)0);
                                } catch (...) {}
                            }
                            transcripts.push_back(ts);
                        }
                    } catch (const std::filesystem::filesystem_error&) {
                        // transcript_dir may not exist yet
                    }

                    size_t processed = 0;
                    for (const auto& state : transcripts) {
                        if (!daemon_running) break;

                        if (run_distillation(field_store, yantra, state, distill_config, &handler)) {
                            processed++;
                            distill_count++;
                        }
                    }

                    if (verbose_mode && processed > 0) {
                        std::cerr << "[distill] Processed " << processed
                                  << " transcript(s), total=" << distill_count << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[distill] Error: " << e.what() << "\n";
                }
            }
        }
    });

    // Code enrichment thread — disabled pending CfSymbolHit description field in FFI
    std::atomic<size_t> enrich_count{0};
    std::thread enrichment([&]() {
        (void)enrich_config;
        (void)enrich_count;
    });

    // Hint enricher thread — runs hint_enricher.py when new memories land
    std::atomic<size_t> hint_enrich_count{0};
    std::thread hint_enrichment([&]() {
        std::string script = default_hint_enricher_script();
        if (script.empty()) {
            std::cerr << "[hint-enrich] script not found — set CHITTA_HINT_ENRICHER or run smart-install\n";
            return;
        }
        std::cerr << "[hint-enrich] started (script=" << script << ")\n";

        static constexpr int  TRIGGER_NEW   = 3;    // new memories needed to trigger
        static constexpr int  COOLDOWN_SECS = 600;  // min 10 min between runs
        static constexpr int  LIMIT         = 50;   // memories per run
        static constexpr int  INITIAL_DELAY = 60;   // let daemon settle

        for (int i = 0; i < INITIAL_DELAY && daemon_running; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        size_t last_count = field_store.memory_count();
        auto   last_run   = std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SECS);

        while (daemon_running) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!daemon_running) break;

            size_t cur_count = field_store.memory_count();
            auto   now_time  = std::chrono::steady_clock::now();
            bool   enough_new  = (cur_count >= last_count + TRIGGER_NEW);
            bool   cooled_down = (now_time - last_run >= std::chrono::seconds(COOLDOWN_SECS));

            if (!enough_new || !cooled_down) continue;

            last_count = cur_count;
            last_run   = now_time;

            std::vector<std::string> cmd = {
                "python3", script,
                "--mind",  mind_path,
                "--limit", std::to_string(LIMIT),
            };
            try {
                // Build argv for execvp
                std::vector<const char*> argv;
                for (const auto& s : cmd) argv.push_back(s.c_str());
                argv.push_back(nullptr);

                pid_t pid = ::fork();
                if (pid < 0) {
                    std::cerr << "[hint-enrich] fork failed\n";
                    continue;
                }
                if (pid == 0) {
                    // Child: redirect stdout/stderr to /dev/null to avoid clogging daemon log
                    int devnull = ::open("/dev/null", O_WRONLY);
                    if (devnull >= 0) { ::dup2(devnull, STDOUT_FILENO); ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
                    ::execvp(argv[0], const_cast<char* const*>(argv.data()));
                    ::_exit(127);
                }
                // Parent: reap child (non-blocking — don't stall daemon)
                std::thread reaper([pid, &hint_enrich_count]() {
                    int status = 0;
                    if (::waitpid(pid, &status, 0) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                        ++hint_enrich_count;
                });
                reaper.detach();
            } catch (const std::exception& e) {
                std::cerr << "[hint-enrich] error: " << e.what() << "\n";
            }
        }
    });

    // Queue processor - handles fire-and-forget writes from hooks
    std::atomic<size_t> queue_count{0};
    std::atomic<size_t> queue_distill_count{0};  // Separate counter for pre-compact distillations
    std::atomic<size_t> queue_fail_count{0};
    std::string queue_path = "/tmp/chitta-queue.jsonl";
    std::string failed_queue_path = mind_path + "/.failed_queue.jsonl";

    // Token-triggered distillation: per-session content accumulators
    std::unordered_map<std::string, int64_t> session_content_accum;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> session_last_distill;
    std::mutex session_accum_mutex;  // Protect the maps

    QueueProcessor queue_proc(field_store, yantra, distill_config, handler,
                              queue_path, failed_queue_path,
                              queue_count, queue_distill_count, queue_fail_count);
    queue_proc.start();

    // Thread pool for async RPC handling (scales 8-16 workers based on load).
    // The queue cap sheds load with a JSON-RPC error instead of queuing
    // unboundedly (lock-convoy defence); tune via CHITTA_MAX_QUEUE_DEPTH.
    size_t max_queue_depth = 256;
    if (const char* qd = std::getenv("CHITTA_MAX_QUEUE_DEPTH")) {
        size_t v = std::strtoul(qd, nullptr, 10);
        if (v > 0) max_queue_depth = v;
    }
    ThreadPool pool(8, 16, max_queue_depth);

    // Dedup set for learn_codebase: key = path + "::" + project.
    // Prevents pool saturation when hooks fire multiple index requests for the same path.
    std::mutex inflight_lc_mutex;
    std::unordered_set<std::string> inflight_learn_codebase;

    // compact_wal runs on its own dedicated thread so it cannot starve behind the FIFO
    // recall queue.  At most one compaction runs at a time; concurrent requests get a
    // "already running" response rather than queuing.
    // NOTE: declared above (before maintenance thread) so the maintenance lambda can see it.

    // Watchdog callback - log stuck operations
    pool.set_watchdog_callback([]([[maybe_unused]] const std::string& method, [[maybe_unused]] int64_t secs) {
        std::cerr << "[watchdog] Stuck operation: " << method << " (" << secs << "s)\n";
    });
    pool.set_escalation_threshold(std::chrono::seconds(30));

    std::cerr << "[daemon] Thread pool started (" << pool.worker_count() << " workers)\n";
    std::cerr << "[daemon] Queue processor started (path=" << queue_path << ")\n";

    // SadhanaManager init — FieldStore is ready synchronously
    sadhana_manager = std::make_unique<SadhanaManager>(field_store);
    handler.set_sadhana_manager(sadhana_manager.get());
    handler.set_queue_stats(&queue_count, &queue_fail_count, failed_queue_path);
    sadhana_manager->set_stream_fn([&server](int fd, std::string line) {
        server.queue_response(fd, std::move(line));
    });
    server.set_disconnect_callback([&sadhana_manager](int fd) {
        if (sadhana_manager) sadhana_manager->stream_unsubscribe(fd);
    });
    if (no_autonomous) {
        auto running = sadhana_manager->list("running");
        if (!running.empty()) {
            std::cerr << "[daemon] --no-autonomous: pausing " << running.size() << " running sadhana(s)\n";
            for (const auto& s : running) {
                sadhana_manager->pause(s.id);
            }
        }
    }
    std::cerr << "[daemon] Sadhana manager initialized\n";
    std::cerr << "[daemon] chitta-field active: " << field_store.memory_count()
              << " memories, " << field_store.symbol_count() << " symbols\n";

    // Main loop - handle socket I/O (never blocks on RPC)
    auto last_stats = std::chrono::steady_clock::now();
    while (daemon_running) {
        // 1. Poll for I/O (fast, non-blocking)
        auto requests = server.poll(50);  // 50ms timeout for responsiveness

        // 2. Dispatch RPC requests to thread pool
        for (const auto& req : requests) {
            // Special commands (fast, handle directly)
            if (req.data == "stats") {
                server.respond(req.client_fd, generate_stats(field_store, yantra));
                continue;
            }
            if (req.data == "shutdown") {
                server.respond(req.client_fd, R"({"status":"shutting_down"})");
                { auto _lk = handler.acquire_lock(); field_store.flush(); }
                daemon_running = false;
                continue;
            }

            // Parse request to extract method for tracing
            auto parsed = json::parse(req.data, nullptr, false);
            if (parsed.is_discarded()) {
                std::string error = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"},"id":null})";
                server.respond(req.client_fd, error);
                continue;
            }

            std::string method = parsed.value("method", "unknown");
            std::string tool_name = method;

            // Extract tool name for better tracing
            if (method == "tools/call" && parsed.contains("params")) {
                tool_name = parsed["params"].value("name", "unknown");

                // For session-aware tools, inject peer PID for session lookup if session_id not provided
                // The peer_pid is the CLI process; its parent (PPID) is Claude
                if (parsed["params"].contains("arguments")) {
                    auto& args = parsed["params"]["arguments"];
                    if (!args.contains("session_id") || args["session_id"].get<std::string>().empty()) {
                        if (req.peer_pid > 1) {
                            args["pid"] = static_cast<int64_t>(req.peer_pid);
                        }
                    }
                }
            }

            // Fast-path: sadhana_watch - keep connection open for event streaming
            // Must handle in main loop (not thread pool) to keep the fd as a subscriber
            if (tool_name == "sadhana_watch") {
                int64_t watch_id = 0;
                if (parsed.contains("params") && parsed["params"].contains("arguments")) {
                    watch_id = parsed["params"]["arguments"].value("id", (int64_t)0);
                }
                if (sadhana_manager) sadhana_manager->stream_subscribe(req.client_fd, watch_id);
                // Send ack — connection stays open; daemon will push events as lines
                json ack;
                ack["jsonrpc"] = "2.0";
                ack["id"] = parsed.value("id", json());
                ack["result"]["content"] = json::array({
                    json::object({{"type", "text"}, {"text", "watching"}})
                });
                ack["result"]["structured"]["status"] = "watching";
                ack["result"]["structured"]["sadhana_id"] = watch_id;
                server.respond(req.client_fd, ack.dump());
                continue;
            }

            // health_check fast path: both counts are AtomicUsize — no locking needed.
            // Execute inline to avoid queuing behind long-running learn_codebase jobs.
            if (tool_name == "health_check") {
                auto req_json = json::parse(req.data);
                bool details = false;
                if (req_json.contains("params") && req_json["params"].contains("arguments"))
                    details = req_json["params"]["arguments"].value("details", false);
                if (!details) {
                    auto req_id = req_json.value("id", json());
                    std::string resp = handler.fast_health_check_json(
                        req_id, pool.worker_count(), pool.active_count(), pool.pending());
                    server.respond(req.client_fd, resp);
                    continue;
                }
            }

            // Dedup learn_codebase: if the same (path, project) is already queued or running,
            // return a skipped response immediately rather than saturating the pool with
            // redundant multi-minute index jobs.
            if (tool_name == "learn_codebase") {
                auto req_json = json::parse(req.data, nullptr, false);
                if (!req_json.is_discarded()) {
                    std::string lc_path, lc_project;
                    if (req_json.contains("params") && req_json["params"].contains("arguments")) {
                        const auto& args = req_json["params"]["arguments"];
                        lc_path    = args.value("path", "");
                        lc_project = args.value("project", "");
                    }
                    std::string lc_key = lc_path + "::" + lc_project;
                    bool already_inflight = false;
                    {
                        std::lock_guard<std::mutex> lk(inflight_lc_mutex);
                        already_inflight = inflight_learn_codebase.count(lc_key) > 0;
                        if (!already_inflight) inflight_learn_codebase.insert(lc_key);
                    }
                    if (already_inflight) {
                        auto req_id = req_json.value("id", json(nullptr));
                        std::string msg = "already indexing — skipped duplicate learn_codebase for " + lc_path;
                        json resp = {{"jsonrpc","2.0"}, {"id", req_id},
                            {"result", {
                                {"content", json::array({{{"type","text"},{"text", msg}}})},
                                {"structured", {{"status","skipped"},{"reason","already_in_progress"},{"path",lc_path}}}
                            }}};
                        server.respond(req.client_fd, resp.dump(-1, ' ', false, json::error_handler_t::replace));
                        continue;
                    }
                    auto submitted = pool.submit(req.client_fd, tool_name,
                        [&handler, data = req.data]() {
                            auto request = json::parse(data);
                            auto response = handler.handle(request);
                            return response.dump(-1, ' ', false, json::error_handler_t::replace);
                        },
                        [&server, &inflight_lc_mutex, &inflight_learn_codebase, lk = lc_key](int fd, std::string response) {
                            server.queue_response(fd, std::move(response));
                            std::lock_guard<std::mutex> lock(inflight_lc_mutex);
                            inflight_learn_codebase.erase(lk);
                        }
                    );
                    if (!submitted) {
                        {
                            std::lock_guard<std::mutex> lk2(inflight_lc_mutex);
                            inflight_learn_codebase.erase(lc_key);
                        }
                        server.respond(req.client_fd,
                            rpc::make_error(req_json.value("id", json()),
                                            rpc::error::INTERNAL_ERROR,
                                            "server overloaded").dump());
                    }
                    continue;
                }
            }

            // compact_wal fast-path: bypass the FIFO pool onto a dedicated thread so
            // maintenance is never blocked behind 150+ queued recall tasks.
            // At most one compaction runs at a time; extras get an immediate response.
            if (tool_name == "compact_wal") {
                bool expected = false;
                if (!compact_wal_inflight.compare_exchange_strong(expected, true)) {
                    auto req_id = parsed.value("id", json());
                    json resp = {{"jsonrpc","2.0"}, {"id", req_id},
                        {"result", {
                            {"content", json::array({{{"type","text"},{"text","compact_wal already running — skipped"}}})},
                            {"structured", {{"status","skipped"},{"reason","already_in_progress"}}}
                        }}};
                    server.respond(req.client_fd, resp.dump(-1, ' ', false, json::error_handler_t::replace));
                    continue;
                }
                std::thread([&handler, &server, &compact_wal_inflight,
                             fd = req.client_fd, data = req.data]() {
                    auto request = json::parse(data);
                    auto response = handler.handle(request);
                    compact_wal_inflight.store(false);
                    server.queue_response(fd, response.dump(-1, ' ', false, json::error_handler_t::replace));
                }).detach();
                continue;
            }

            // All other requests go to thread pool (health_check included — main thread must not block on rpc_mutex_)
            auto submitted = pool.submit(req.client_fd, tool_name,
                [&handler, &pool, data = req.data, tname = tool_name]() {
                    auto request = json::parse(data);
                    auto response = handler.handle(request);
                    if (tname == "health_check" && response.contains("result") && response["result"].contains("structured")) {
                        response["result"]["structured"]["pool_workers"] = pool.worker_count();
                        response["result"]["structured"]["pool_active"] = pool.active_count();
                        response["result"]["structured"]["pool_pending"] = pool.pending();
                    }
                    return response.dump(-1, ' ', false, json::error_handler_t::replace);
                },
                [&server](int fd, std::string response) {
                    server.queue_response(fd, std::move(response));
                }
            );
            if (!submitted) {
                server.respond(req.client_fd,
                    rpc::make_error(parsed.value("id", json()),
                                    rpc::error::INTERNAL_ERROR,
                                    "server overloaded").dump());
            }
        }

        // 3. Write queued responses from thread pool (non-blocking)
        for (const auto& resp : server.drain_responses()) {
            server.respond(resp.client_fd, resp.data);
        }

        // 4. Log pool stats periodically (every 30s if there's activity)
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats > std::chrono::seconds(30)) {
            last_stats = now;
            auto active = pool.get_active();
            if (!active.empty() || pool.pending() > 0) {
                std::cerr << "[pool] " << active.size() << " active, "
                          << pool.pending() << " pending\n";
                for (const auto& [method, ms] : active) {
                    if (ms > 1000) {
                        std::cerr << "[pool]   " << method << ": " << ms << "ms\n";
                    }
                }
            }
        }
    }

    // Stop socket first — removes file immediately, prevents new connections
    // while threads finish their current work.
    server.stop();
    if (viz_server) viz_server->stop();
    if (rpc_http_svr) { rpc_http_svr->stop(); if (rpc_http_thread.joinable()) rpc_http_thread.join(); }

    // Clean pid/lock BEFORE joining: distillation/enrichment may be in a long
    // LLM call (>15s) that cannot be interrupted; the alarm below will _Exit
    // the process if joins overrun, and we want the next daemon start to find
    // a clean state without relying on cleanup_stale_daemon().
    if (!pid_file.empty()) std::remove(pid_file.c_str());
    release_lock(lock);

    // Watchdog: force-exit if background threads don't finish within 15s.
    // The pid/lock cleanup above already ran, so a forced _Exit leaves clean
    // state for the next daemon. Detaching threads is unsafe — captures-by-ref
    // outlive main()'s stack frame, risking UAF under heavy concurrency.
    std::signal(SIGALRM, [](int) {
        std::cerr << "[daemon] Shutdown timeout — forcing exit\n";
        std::_Exit(0);
    });
    alarm(15);

    maintenance.join();
    if (backfill_thread.joinable()) backfill_thread.join();
    if (distillation.joinable()) distillation.join();
    if (enrichment.joinable()) enrichment.join();
    if (hint_enrichment.joinable()) hint_enrichment.join();
    queue_proc.stop();
    subconscious.stop();

    alarm(0);  // Cancel watchdog — all threads finished normally

    const auto& sc_stats = subconscious.stats();
    std::cerr << "[daemon] Stopped (cycles=" << cycle_count
              << ", distilled=" << distill_count
              << ", queue_distilled=" << queue_distill_count
              << ", enriched=" << enrich_count
              << ", hint_enriched=" << hint_enrich_count
              << ", queued=" << queue_count
              << ", subconscious_events=" << sc_stats.events_processed.load()
              << ", corrections=" << sc_stats.corrections_detected.load()
              << ", preferences=" << sc_stats.preferences_detected.load() << ")\n";
    return 0;
}

int cmd_shutdown(const std::string& socket_path) {
    SocketClient client(socket_path);
    if (!client.connect()) {
        std::cerr << "No daemon running\n";
        return 1;
    }
    if (client.request_shutdown()) {
        std::cout << "Daemon shutdown requested\n";
        // 30s timeout: daemon may be finishing distillation or other long-running work
        client.wait_for_socket_gone(30000);
        return 0;
    }
    return 1;
}

int cmd_status(const std::string& socket_path) {
    SocketClient client(socket_path);
    if (!client.connect()) {
        std::cout << "Daemon: not running\n";
        return 1;
    }
    auto version = client.check_version();
    std::cout << "Daemon: running\n";
    std::cout << "Socket: " << socket_path << "\n";
    if (version) std::cout << "Version: " << version->software << "\n";
    return 0;
}

int cmd_stats(FieldStore& field_store, VakYantra* yantra) {
    std::cout << "Soul Statistics (chitta-field)\n";
    std::cout << "═══════════════════════════════\n";
    std::cout << "  Memories: " << field_store.memory_count() << "\n";
    std::cout << "  Symbols:  " << field_store.symbol_count() << "\n";
    std::cout << "  Yantra:   " << (yantra ? "ready" : "not attached") << "\n";
    return 0;
}

int cmd_metrics(FieldStore& field_store, [[maybe_unused]] int days = 7) {
    int64_t n_memories = field_store.memory_count();
    int64_t n_symbols = field_store.symbol_count();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Soul Metrics (" << days << "d)\n";
    std::cout << "═══════════════════════════════\n";
    std::cout << "  Memories: " << n_memories << "\n";
    std::cout << "  Symbols:  " << n_symbols << "\n";
    std::cout << "\n  Note: Detailed SUS metrics require SoulProjection (future work).\n";

    return 0;
}

void print_usage(const char* prog) {
    std::cerr << "chittad " << CHITTA_VERSION << " - Soul daemon\n\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  daemon     Run background daemon\n"
              << "  shutdown   Stop running daemon\n"
              << "  status     Check daemon status\n"
              << "  stats      Show soul statistics\n"
              << "  metrics    Show SUS (Soul Utility Score) component metrics\n"
              << "  distill    Run manual distillation on a transcript\n"
              << "  help       Show this help\n\n"
              << "Options:\n"
              << "  --path PATH        Mind storage path (chitta-field)\n"
              << "  --interval SECS    Sync interval (default: 60)\n"
              << "  -f, --foreground   Run in foreground\n"
              << "  --verbose          Verbose logging\n"
              << "  -v, --version      Show version\n"
              << "\nManual Distillation (distill command):\n"
              << "  --transcript-path PATH   JSONL transcript file to distill (required)\n"
              << "  --session-id ID          Session ID (auto-extracted from path if omitted)\n"
              << "  --realm REALM            Target realm (default: brahman)\n"
              << "\nAutomatic Distillation (daemon):\n"
              << "  --distill-interval MINS  Timer-based interval (default: 15, safety net)\n"
              << "  --distill-min-turns N    Min turns before distilling (default: 4)\n"
              << "  --distill-script PATH    Distillation script path (ignored, uses native)\n"
              << "  --distill-model MODEL    LLM model for distillation (default: gemma4:26b)\n"
              << "  --distill-token-trigger N  Token-triggered: chars threshold (default: 120000 ~30k tokens, 0=off)\n"
              << "  --distill-cooldown SECS  Min seconds between token-triggered distillations (default: 180)\n"
              << "  --distill-max-tokens N   LLM output token limit (default: 8192)\n"
              << "  --distill-context-chars N  Max input chars (0=unlimited, default: 0)\n"
              << "  --no-distill             Disable automatic distillation\n"
              << "\nCode Enrichment (semantic descriptions):\n"
              << "  --enrich-interval MINS   Enrichment interval (default: 2)\n"
              << "  --enrich-batch N         Symbols per batch (default: 10)\n"
              << "  --enrich-model MODEL     LLM model for enrichment (default: gemma4:26b)\n"
              << "  --no-enrich              Disable code enrichment\n"
              << "\nEmbeddings:\n"
              << "  --embed-model PATH       GGUF model for in-process embeddings (overrides CHITTA_EMBED_MODEL)\n"
              << "\nSubconscious (background processing):\n"
              << "  --no-hygiene             Disable hygiene (decay, pruning, consolidation)\n"
              << "  --no-autonomous          Disable autonomous agents (dream, think)\n"
              << "  --merge-policy POLICY    LLM write dedup: off (default) | merge_aware\n"
              << "  --embed-interval SECS    Enable background embedding of wisdom memories (default: off)\n"
              << "\nHTTP Visualization Server:\n"
              << "  --http-port PORT         Enable HTTP viz server on PORT (0=disabled, default)\n"
              << "  --http-static-dir PATH   Static file directory (default: auto-detect docs/mind-viz/)\n"
              ;
}

int main(int argc, char* argv[]) {
    // CRITICAL: Set thread limits BEFORE any ONNX/OpenMP code loads
    // Must be at the very start of main() to take effect
    setenv("OMP_NUM_THREADS", "4", 1);
    setenv("MKL_NUM_THREADS", "4", 1);
    setenv("OPENBLAS_NUM_THREADS", "4", 1);
    setenv("ORT_NUM_THREADS", "4", 1);
    setenv("OMP_WAIT_POLICY", "PASSIVE", 1);  // Reduce busy-waiting
    setenv("KMP_BLOCKTIME", "0", 1);          // Intel OpenMP: don't spin

    std::string mind_path = default_mind_path();
    std::string command;
    int command_idx = -1;
    int interval = 60;
    bool foreground = false;

    // Distillation config
    DistillConfig distill_config;
    distill_config.script_path = default_distill_script();

    // Code enrichment config
    EnrichConfig enrich_config;
    enrich_config.script_path = default_enrich_script();

    // Subconscious config
    SubconsciousConfig subconscious_config;
    bool no_autonomous = false;  // Disable dream/think callbacks

    // HTTP viz server config
    int http_port = 0;
    std::string http_static_dir;
    int rpc_port = []() -> int {
        const char* e = std::getenv("CHITTA_RPC_PORT");
        return e ? std::atoi(e) : 0;
    }();

    // Manual distill command args
    std::string distill_transcript_path;
    std::string distill_session_id;
    std::string distill_realm = "brahman";

    // prune-memories command args
    std::string prune_match;       // comma-separated content substrings
    bool prune_apply = false;      // dry-run unless set
    int prune_action = 0;          // 0=delete (forget), 1=down-weight (archive)

    // export_content / import_embeddings command args (GPU re-embed migration)
    std::string io_file;           // --out (export) or --in (import)

    auto safe_stoi = [&](const char* arg, const char* option_name) -> int {
        try {
            return std::stoi(arg);
        } catch (const std::invalid_argument&) {
            std::cerr << "Error: invalid integer for " << option_name << ": " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        } catch (const std::out_of_range&) {
            std::cerr << "Error: integer out of range for " << option_name << ": " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    };

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            mind_path = argv[++i];
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval = safe_stoi(argv[++i], "--interval");
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            foreground = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = true;
        } else if (strcmp(argv[i], "--distill-interval") == 0 && i + 1 < argc) {
            distill_config.interval_minutes = safe_stoi(argv[++i], "--distill-interval");
        } else if (strcmp(argv[i], "--distill-min-turns") == 0 && i + 1 < argc) {
            distill_config.min_turns = safe_stoi(argv[++i], "--distill-min-turns");
        } else if (strcmp(argv[i], "--distill-script") == 0 && i + 1 < argc) {
            distill_config.script_path = argv[++i];
        } else if (strcmp(argv[i], "--distill-model") == 0 && i + 1 < argc) {
            distill_config.model = argv[++i];
        } else if (strcmp(argv[i], "--distill-token-trigger") == 0 && i + 1 < argc) {
            distill_config.token_trigger_chars = safe_stoi(argv[++i], "--distill-token-trigger");
        } else if (strcmp(argv[i], "--distill-cooldown") == 0 && i + 1 < argc) {
            distill_config.cooldown_seconds = safe_stoi(argv[++i], "--distill-cooldown");
        } else if (strcmp(argv[i], "--distill-max-tokens") == 0 && i + 1 < argc) {
            distill_config.max_tokens = safe_stoi(argv[++i], "--distill-max-tokens");
        } else if (strcmp(argv[i], "--distill-context-chars") == 0 && i + 1 < argc) {
            distill_config.max_context_chars = static_cast<size_t>(safe_stoi(argv[++i], "--distill-context-chars"));
        } else if (strcmp(argv[i], "--no-distill") == 0) {
            distill_config.enabled = false;
        } else if (strcmp(argv[i], "--embed-model") == 0 && i + 1 < argc) {
            setenv("CHITTA_EMBED_MODEL", argv[++i], 1);
        } else if (strcmp(argv[i], "--no-enrich") == 0) {
            enrich_config.enabled = false;
        } else if (strcmp(argv[i], "--no-hygiene") == 0) {
            subconscious_config.enable_hygiene = false;
            // Also disables sleep consolidation (encode + snapshot + demotion +
            // CW sweep). The recall starvation that once made this the default
            // posture is fixed (dirty-skipped saves, lock-ordered store,
            // per-memory encode); the flag remains as an explicit opt-out.
            subconscious_config.enable_sleep_consolidation = false;
        } else if (strcmp(argv[i], "--embed-interval") == 0 && i + 1 < argc) {
            subconscious_config.enable_background_embedding = true;
            subconscious_config.embedding_interval = std::chrono::seconds(safe_stoi(argv[++i], "--embed-interval"));
        } else if (strcmp(argv[i], "--no-autonomous") == 0) {
            no_autonomous = true;
        } else if (strcmp(argv[i], "--merge-policy") == 0 && i + 1 < argc) {
            setenv("CHITTA_MERGE_POLICY", argv[++i], 1);
        } else if (strcmp(argv[i], "--rpc-port") == 0 && i + 1 < argc) {
            rpc_port = safe_stoi(argv[++i], "--rpc-port");
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            http_port = safe_stoi(argv[++i], "--http-port");
        } else if (strcmp(argv[i], "--http-static-dir") == 0 && i + 1 < argc) {
            http_static_dir = argv[++i];
        } else if (strcmp(argv[i], "--transcript-path") == 0 && i + 1 < argc) {
            distill_transcript_path = argv[++i];
        } else if (strcmp(argv[i], "--session-id") == 0 && i + 1 < argc) {
            distill_session_id = argv[++i];
        } else if (strcmp(argv[i], "--realm") == 0 && i + 1 < argc) {
            distill_realm = argv[++i];
        } else if (strcmp(argv[i], "--match") == 0 && i + 1 < argc) {
            prune_match = argv[++i];
        } else if (strcmp(argv[i], "--apply") == 0) {
            prune_apply = true;
        } else if (strcmp(argv[i], "--delete") == 0) {
            prune_action = 0;
        } else if (strcmp(argv[i], "--background") == 0) {
            prune_action = 1;
        } else if ((strcmp(argv[i], "--out") == 0 || strcmp(argv[i], "--in") == 0) && i + 1 < argc) {
            io_file = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            std::cout << "chittad " << CHITTA_VERSION << "\n";
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-' && command.empty()) {
            command = argv[i];
            command_idx = i;
        }
    }

    std::string sock_path = socket_path_for_mind(mind_path);
    std::string pid_file = pid_path_for_mind(mind_path);

    if (command.empty() || command == "help") {
        print_usage(argv[0]);
        return 0;
    }

    if (command == "shutdown") return cmd_shutdown(sock_path);
    if (command == "status")   return cmd_status(sock_path);

#ifdef CHITTA_WITH_LLAMA_CPP
    if (command == "hint_extract") {
        // Standalone: no field store, no embedding yantra — just load the hint GGUF.
        chitta::HintYantra hy("", mind_path);
        if (!hy.ready()) {
            std::cerr << "[hint_extract] hint model not loaded — set CHITTA_HINT_MODEL or place GGUF in ~/.claude/models/\n";
            return 1;
        }
        if (command_idx >= 0 && command_idx + 1 < argc) {
            std::string hint = hy.extract(argv[command_idx + 1]);
            if (!hint.empty()) std::cout << hint << "\n";
        } else {
            std::string line;
            while (std::getline(std::cin, line)) {
                while (!line.empty() && line.back() == '\r') line.pop_back();
                std::cout << (line.empty() ? "" : hy.extract(line)) << "\n";
            }
        }
        return 0;
    }
#endif
    if (command == "health") {
        // Socket-only: never load FieldStore for a health ping.
        SocketClient client(sock_path);
        if (!client.connect()) {
            std::cerr << "Status: not running\n";
            return 1;
        }
        auto h = client.check_health();
        if (!h) {
            std::cerr << "Status: unreachable\n";
            return 1;
        }
        bool ok = (h->status == "ok");
        std::cerr << "Status: " << (ok ? "ok" : h->status) << "\n";
        if (h->pid)       std::cerr << "  pid      : " << h->pid << "\n";
        if (h->uptime_ms) std::cerr << "  uptime   : " << (h->uptime_ms / 1000) << "s\n";
        return ok ? 0 : 1;
    }

    if (command == "format-id") {
        // Print the compiled store vector-space id (model/dim/text-format). Handle-less —
        // no FieldStore load. Used by the self-update gate to detect an incompatible
        // replacement binary before execv (PR4).
        std::cout << cf_compiled_vector_space_id() << "\n";
        return 0;
    }

    // Commands that need Mind - daemonize first if needed
    if (command == "daemon") {
        if (!foreground) {
            const char* home = getenv("HOME");
            std::string log_path = std::string(home ? home : ".") + "/.claude/mind/.subconscious.log";
            if (!daemonize(log_path)) {
                std::cerr << "Failed to daemonize\n";
                return 1;
            }
        }
    }

    // Embeddings: GPU-first — try Ollama HTTP (GPU node) before falling back to
    // in-process GGUF (CPU). OllamaYantra::discover_gpu_endpoint() checks /tmp/ollama-server-*.url
    // and SLURM squeue; if an Ollama running the embed model is reachable, we use it and skip
    // the GGUF load entirely (~100-1000× faster for re_embed of large corpora).
    // CHITTA_EMBED_GPU_ONLY=1 makes the GPU path mandatory (no CPU fallback).
    std::shared_ptr<VakYantra> inner_yantra;
    {
        auto ollama = std::make_shared<chitta::OllamaYantra>();
        if (ollama->ready()) {
            inner_yantra = ollama;
            std::cerr << "[embed] GPU path active via Ollama\n";
        }
    }
#ifdef CHITTA_WITH_LLAMA_CPP
    if (!inner_yantra) {
        namespace fs = std::filesystem;
        std::string gguf;
        const char* env_model = std::getenv("CHITTA_EMBED_MODEL");
        // Default model filename follows the compiled embedding identity so auto-discovery
        // and the bundled GGUF never drift: public build -> bge-large-en-v1.5.gguf, a personal
        // build -> ssl_distiller_dpo.gguf, etc. (cf_embed_model_id() is build.rs-generated.)
        const std::string model_file = std::string(cf_embed_model_id()) + ".gguf";
        if (env_model && *env_model && fs::exists(env_model)) {
            gguf = env_model;
        } else if (const char* home = std::getenv("HOME")) {
            fs::path pm = fs::path(home) / ".claude" / "models" / model_file;
            if (fs::exists(pm)) gguf = pm.string();
            // Also probe ~/.claude/bin/ — the systemd unit keeps the GGUF beside the
            // binaries there, so a daemon started WITHOUT --embed-model still finds the
            // correct in-process model instead of silently falling back to the Ollama
            // backend (a DIFFERENT vector space → wrong-dim vectors, corrupt recall, and
            // concurrent-writer family churn). cf_embed_model_id() pins the right filename.
            if (gguf.empty()) {
                fs::path pb = fs::path(home) / ".claude" / "bin" / model_file;
                if (fs::exists(pb)) gguf = pb.string();
            }
        }
        if (gguf.empty()) {
            fs::path p = fs::path(mind_path) / ".." / ".." / "models" / model_file;
            if (fs::exists(p)) gguf = fs::canonical(p).string();
        }
        if (!gguf.empty()) {
            auto llama_yantra = std::make_shared<chitta::LlamaYantra>(gguf, mind_path);
            if (llama_yantra->ready()) inner_yantra = llama_yantra;
        }
    }
#endif
    if (!inner_yantra)
        inner_yantra = std::make_shared<chitta::OllamaYantra>();
    // EmbedQueue owns all serialization: single worker thread, LRU cache, two-lane queue.
    // TimeoutYantra is no longer needed — concurrent embed calls are never made directly.
    std::shared_ptr<VakYantra> yantra = inner_yantra;
    chitta::EmbedQueue embed_queue(inner_yantra);



    // Open chitta-field store — the sole storage backend
    std::string field_path = mind_path + "/chitta-field";
    std::unique_ptr<FieldStore> field_store_ptr;

    // For daemon: clean up stale files then bind socket early so clients get
    // "warming_up" instead of "connection refused" during the 3+ minute snapshot load.
    DaemonLock daemon_lock;
    if (command == "daemon") {
        if (!cleanup_stale_daemon(mind_path)) {
            std::cerr << "[daemon] Another daemon is running\n";
            return 1;
        }
        if (!acquire_lock(mind_path, daemon_lock)) {
            std::cerr << "[daemon] Another daemon is running (lock held)\n";
            return 1;
        }
    }
    std::unique_ptr<SocketServer> early_server;
    if (command == "daemon") {
        early_server = std::make_unique<SocketServer>(sock_path);
        if (!early_server->start()) {
            release_lock(daemon_lock);
            std::cerr << "[daemon] Another daemon is running (socket in use)\n";
            return 1;
        }
        std::cerr << "[socket_server] Listening (warming up) on " << sock_path << "\n";
    }

    std::atomic<bool> load_done{false};
    std::exception_ptr load_ex;
    std::thread loader([&]() {
        try {
            field_store_ptr = std::make_unique<FieldStore>(field_path, field_path);
        } catch (...) {
            load_ex = std::current_exception();
        }
        load_done.store(true, std::memory_order_release);
    });
    while (!load_done.load(std::memory_order_acquire)) {
        if (early_server) {
            auto reqs = early_server->poll(100);
            for (auto& r : reqs) {
                // Echo the request id so the client gets a matched response immediately
                // (id=null caused RESPONSE_TIMEOUT_MS=5min wait because client drops unmatched frames).
                json warming = {
                    {"jsonrpc", "2.0"},
                    {"id",      nullptr},
                    {"result",  {{"text", "daemon loading, please retry"},
                                 {"structured", {{"status", "warming_up"}}}}}
                };
                try {
                    auto rj = json::parse(r.data, nullptr, false);
                    if (!rj.is_discarded()) warming["id"] = rj.value("id", json(nullptr));
                } catch (...) {}
                early_server->respond(r.client_fd, warming.dump());
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    loader.join();
    if (load_ex) {
        try { std::rethrow_exception(load_ex); }
        catch (const std::exception& e) {
            std::cerr << "[daemon] Failed to open chitta-field store at " << field_path << ": " << e.what() << "\n";
        }
        return 1;
    }

    FieldStore& field_store = *field_store_ptr;
    // Return pages freed during load-time migration (triplet purge, dedup) back to OS.
#if defined(__linux__)
    malloc_trim(0);
#endif
    VakYantra* yantra_raw = yantra.get();
    std::cerr << "[Backend] chitta-field (" << field_store.memory_count() << " memories)\n";

    int result = 0;
    if (command == "daemon") {
        result = cmd_daemon(field_store, yantra_raw, &embed_queue, interval, *early_server, sock_path, mind_path, pid_file, distill_config, enrich_config, subconscious_config, no_autonomous, http_port, http_static_dir, rpc_port, daemon_lock);
    } else if (command == "stats") {
        result = cmd_stats(field_store, yantra_raw);
    } else if (command == "metrics") {
        result = cmd_metrics(field_store);
    } else if (command == "distill") {
        // Manual distillation command
        if (distill_transcript_path.empty()) {
            std::cerr << "Error: --transcript-path is required for distill command\n";
            return 1;
        }
        if (distill_session_id.empty()) {
            // Try to extract session ID from path (e.g., /path/to/abc123.jsonl -> abc123)
            size_t last_slash = distill_transcript_path.rfind('/');
            size_t start = (last_slash == std::string::npos) ? 0 : last_slash + 1;
            size_t dot = distill_transcript_path.rfind('.');
            if (dot != std::string::npos && dot > start) {
                distill_session_id = distill_transcript_path.substr(start, dot - start);
            } else {
                std::cerr << "Error: --session-id is required (could not extract from path)\n";
                return 1;
            }
        }

        verbose_mode = true;  // Enable verbose for manual distillation
        std::cerr << "[distill] Transcript: " << distill_transcript_path << "\n";
        std::cerr << "[distill] Session:    " << distill_session_id << "\n";
        std::cerr << "[distill] Realm:      " << distill_realm << "\n";
        std::cerr << "[distill] Model:      " << distill_config.model << "\n";

        // Create TranscriptState for the run_distillation function
        TranscriptState state;
        state.session_id = distill_session_id;
        state.transcript_path = distill_transcript_path;
        state.realm = distill_realm;
        state.last_processed_line = 0;  // Process from beginning

        bool success = run_distillation(field_store, yantra_raw, state, distill_config, nullptr, false);
        result = success ? 0 : 1;
    } else if (command == "re_embed") {
        // Re-embed all memories using the configured LlamaYantra model (nomic-embed-text-v1.5 768-d).
        // Marks every non-deleted memory embed_pending, then drains synchronously.
        if (!yantra_raw || !yantra_raw->ready()) {
            std::cerr << "[re_embed] ERROR: embed model not ready — set CHITTA_EMBED_MODEL or pass --embed-model\n";
            return 1;
        }
        const std::string model_id = cf_embed_model_id();
        int64_t queued = field_store.requeue_all_embeddings(model_id.c_str(), model_id.size());
        if (queued < 0) {
            std::cerr << "[re_embed] ERROR: requeue_all_embeddings failed\n";
            return 1;
        }
        std::cerr << "[re_embed] Queued " << queued << " memories for " << EMBED_DIM << "-d re-embedding\n";

        // Drain the queue using the same batched backfill logic as the daemon thread.
        size_t done = 0;
        while (true) {
            std::vector<uint64_t> pending = field_store.pending_embeddings(64);
            if (pending.empty()) break;
            std::vector<std::string> contents;
            contents.reserve(pending.size());
            for (uint64_t id : pending) contents.push_back(field_store.get_content(id));

            constexpr size_t SUB_BATCH = 16;
            // Track which IDs were successfully backfilled; the rest must be force-cleared
            // so they don't loop back into pending_embeddings() forever.
            std::vector<uint64_t> not_backfilled;
            for (size_t b = 0; b < pending.size(); b += SUB_BATCH) {
                size_t end = std::min(b + SUB_BATCH, pending.size());
                std::vector<std::string> batch_texts;
                batch_texts.reserve(end - b);
                // nomic-embed-text v1.5: 8192 token limit; ~6KB is safe for code/unicode
                static constexpr size_t MAX_CONTENT_BYTES = 6000;
                std::vector<size_t> nonempty_idx;
                for (size_t i = b; i < end; ++i) {
                    if (!contents[i].empty()) {
                        nonempty_idx.push_back(i);
                        const auto& c = contents[i];
                        // Mirror the live backfill document text EXACTLY: content + SSL gloss.
                        // Do NOT prepend "search_document: " — transform_batch() defaults to
                        // EmbedMode::Document and adds that prefix itself. Prepending here
                        // double-prefixes and mismatches the single-prefixed query path.
                        size_t trunc = MAX_CONTENT_BYTES;
                        if (c.size() > MAX_CONTENT_BYTES) {
                            while (trunc > 0 && (static_cast<unsigned char>(c[trunc]) & 0xC0) == 0x80) --trunc;
                        }
                        std::string body = c.substr(0, trunc);
                        batch_texts.push_back(chitta::ssl::retrieval_text(body));
                    } else {
                        not_backfilled.push_back(pending[i]);
                    }
                }
                if (batch_texts.empty()) { done += end - b; continue; }
                auto arthas = yantra_raw->transform_batch(batch_texts);
                for (size_t j = 0; j < arthas.size() && j < nonempty_idx.size(); ++j) {
                    size_t i = nonempty_idx[j];
                    const auto& emb = arthas[j].nu.data;
                    if (emb.size() != EMBED_DIM) {
                        not_backfilled.push_back(pending[i]);
                        continue;
                    }
                    field_store.backfill_embedding(pending[i], emb);
                }
                done += end - b;
            }
            // Force-clear any IDs that couldn't be embedded (empty content, wrong dim).
            if (!not_backfilled.empty())
                field_store.force_clear_embed_pending(not_backfilled);
            if (done % 1000 < 64)
                std::cerr << "[re_embed] " << done << "/" << queued << " embedded...\n";
        }
        std::cerr << "[re_embed] Done: " << done << " memories re-embedded at " << EMBED_DIM << "-d\n";
        // Rebuild derived search structures (binary codes, coarse, LSH, HNSW) over the
        // new embeddings — otherwise they stay built in the prior vector space.
        std::cerr << "[re_embed] Rebuilding search indices...\n";
        if (!field_store.force_reindex()) {
            std::cerr << "[re_embed] ERROR: force_reindex failed\n";
            return 1;
        }
        // Persist: backfill_embedding only mutates the in-memory store. Without an
        // explicit snapshot the re-embedded vectors are lost on process exit.
        std::cerr << "[re_embed] Persisting full snapshot + .emb sidecar...\n";
        field_store.flush();
        if (!field_store.save_full_snapshot()) {
            std::cerr << "[re_embed] ERROR: save_full_snapshot failed — embeddings NOT persisted!\n";
            return 1;
        }
        std::cerr << "[re_embed] Persisted " << done << " embeddings at " << EMBED_DIM << "-d\n";
        result = 0;
    } else if (command == "export_content") {
        // GPU re-embed migration, phase 1: dump every content-bearing memory's id + content
        // as JSONL ({"id":<u64>,"content":<str>}) so an external GPU embedder can produce
        // vectors out-of-process. No embed model required. Run with CHITTA_MIGRATE_REEMBED=1
        // when exporting from a foreign-vsid store.
        if (io_file.empty()) { std::cerr << "[export_content] --out <file.jsonl> required\n"; return 1; }
        int64_t marked = field_store.requeue_all_embeddings("export", 6);
        if (marked < 0) { std::cerr << "[export_content] ERROR: requeue failed\n"; return 1; }
        std::vector<uint64_t> ids = field_store.pending_embeddings(2000000);
        std::ofstream out(io_file);
        if (!out) { std::cerr << "[export_content] ERROR: cannot open " << io_file << "\n"; return 1; }
        // Emit the EXACT string re_embed/live-backfill feed the embedder:
        //   "search_document: " + ssl::retrieval_text(content[:6000])
        // so the external GPU embedder lands in the same vector space as runtime queries
        // (which embed "search_query: " + query through the same model). Token-truncation to
        // the model's context is the GPU embedder's job, matching vak_llama.
        static constexpr size_t MAX_CONTENT_BYTES = 6000;
        size_t written = 0;
        for (uint64_t id : ids) {
            std::string c = field_store.get_content(id);
            if (c.empty()) continue;
            std::string body = c.size() > MAX_CONTENT_BYTES ? c.substr(0, MAX_CONTENT_BYTES) : c;
            std::string doc = "search_document: " + chitta::ssl::retrieval_text(body);
            json j; j["id"] = id; j["text"] = doc;
            // replace error handler: 6000-byte truncation can split a multi-byte UTF-8 char
            // (and some content may be non-UTF-8); replace invalid bytes instead of throwing.
            out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
            ++written;
        }
        out.flush();
        std::cerr << "[export_content] wrote " << written << " memories to " << io_file << "\n";
        result = out.good() ? 0 : 1;
    } else if (command == "import_embeddings") {
        // GPU re-embed migration, phase 3: read precomputed EMBED_DIM-d vectors and backfill
        // them, then rebuild indices + snapshot. Binary record format (little-endian, native):
        //   u64 id, then EMBED_DIM * f32. No embed model required. Run with
        //   CHITTA_MIGRATE_REEMBED=1 to load a foreign-vsid store for the one-shot migration.
        if (io_file.empty()) { std::cerr << "[import_embeddings] --in <file.bin> required\n"; return 1; }
        std::ifstream in(io_file, std::ios::binary);
        if (!in) { std::cerr << "[import_embeddings] ERROR: cannot open " << io_file << "\n"; return 1; }
        // Mark all pending so backfill_embedding applies (it no-ops on non-pending memories).
        int64_t marked = field_store.requeue_all_embeddings("import", 6);
        if (marked < 0) { std::cerr << "[import_embeddings] ERROR: requeue failed\n"; return 1; }
        std::cerr << "[import_embeddings] " << marked << " memories pending; importing "
                  << EMBED_DIM << "-d vectors from " << io_file << "\n";
        size_t imported = 0, missing = 0;
        std::vector<float> emb(EMBED_DIM);
        while (true) {
            uint64_t id = 0;
            if (!in.read(reinterpret_cast<char*>(&id), sizeof(id))) break;
            if (!in.read(reinterpret_cast<char*>(emb.data()), (std::streamsize)(EMBED_DIM * sizeof(float)))) {
                std::cerr << "[import_embeddings] WARNING: truncated record for id=" << id << "\n";
                break;
            }
            try {
                field_store.backfill_embedding(id, emb);
                ++imported;
            } catch (...) { ++missing; }
            if (imported % 10000 == 0 && imported > 0)
                std::cerr << "[import_embeddings] " << imported << " imported...\n";
        }
        std::cerr << "[import_embeddings] imported " << imported << " (" << missing << " skipped)\n";
        std::cerr << "[import_embeddings] Rebuilding search indices...\n";
        if (!field_store.force_reindex()) { std::cerr << "[import_embeddings] ERROR: force_reindex failed\n"; return 1; }
        std::cerr << "[import_embeddings] Persisting snapshot + sidecars...\n";
        field_store.flush();
        if (!field_store.save_full_snapshot()) {
            std::cerr << "[import_embeddings] ERROR: save_full_snapshot failed — NOT persisted!\n";
            return 1;
        }
        std::cerr << "[import_embeddings] Done: " << imported << " embeddings at " << EMBED_DIM << "-d persisted\n";
        result = 0;
    } else if (command == "reindex") {
        // Rebuild all derived search indices (binary codes + coarse + LSH + HNSW)
        // from the current embeddings. Needed after an embedding-dimension migration,
        // where re_embed updates float vectors but the ANN indices remain built in the
        // old vector space. Runs on CPU; no embedding model required.
        std::cerr << "[reindex] Rebuilding search indices from " << EMBED_DIM << "-d embeddings...\n";
        if (!field_store.force_reindex()) {
            std::cerr << "[reindex] ERROR: force_reindex failed\n";
            return 1;
        }
        std::cerr << "[reindex] Persisting full snapshot + sidecars...\n";
        field_store.flush();
        if (!field_store.save_full_snapshot()) {
            std::cerr << "[reindex] ERROR: save_full_snapshot failed\n";
            return 1;
        }
        std::cerr << "[reindex] Done.\n";
        result = 0;
    } else if (command == "migrate-store-format") {
        // PR5: stamp the store with the .shdr identity sidecar (model/dim/text-format +
        // lineage) so snapshot selection + WAL replay can fence foreign-vector data. No
        // re-embed — save_full_snapshot writes a fresh instance family (each sidecar via
        // tmp+rename, old families pruned only after the new one is durable = atomic
        // rebuild) including the .shdr for the current compiled vector space. Legacy
        // foreign-dim snapshots that lack a .shdr remain caught by the payload dim-fence.
        std::cerr << "[migrate] Stamping store with .shdr identity sidecar (no re-embed)...\n";
        field_store.flush();
        if (!field_store.save_full_snapshot()) {
            std::cerr << "[migrate] ERROR: save_full_snapshot failed\n";
            return 1;
        }
        std::cerr << "[migrate] Done — .shdr written for the current vector space.\n";
        result = 0;
    } else if (command == "prune-memories") {
        // Maintenance: find memories whose content matches any of the comma-separated
        // --match substrings (case-insensitive). Dry-run by default (preview); --apply with
        // --delete (forget) or --background (down-weight to Archived status) to act.
        if (prune_match.empty()) {
            std::cerr << "[prune] --match \"pat1,pat2,...\" required (comma-separated content substrings)\n";
            return 1;
        }
        json pats = json::array();
        {
            size_t start = 0;
            while (start <= prune_match.size()) {
                size_t comma = prune_match.find(',', start);
                std::string item = prune_match.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                size_t a = item.find_first_not_of(" \t");
                size_t b = item.find_last_not_of(" \t");
                if (a != std::string::npos) pats.push_back(item.substr(a, b - a + 1));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        std::string res = field_store.prune_memories(pats.dump(), prune_apply, prune_action);
        json matches = json::parse(res.empty() ? std::string("[]") : res, nullptr, false);
        if (matches.is_discarded()) matches = json::array();
        const char* act = (prune_action == 1) ? "down-weight (archive)" : "delete";
        if (!prune_apply) {
            std::cerr << "[prune] DRY-RUN: " << matches.size() << " memories match (action would be: " << act << ")\n";
            for (auto& m : matches) {
                std::cerr << "  #" << m.value("id", (uint64_t)0) << "  [" << m.value("kind", std::string()) << "]  "
                          << m.value("content", std::string()) << "\n";
            }
            std::cerr << "[prune] Re-run with --apply and (--delete | --background) to act.\n";
            result = 0;
        } else {
            std::cerr << "[prune] " << act << " applied to " << matches.size() << " memories. Persisting...\n";
            field_store.flush();
            if (!field_store.save_full_snapshot()) {
                std::cerr << "[prune] ERROR: save_full_snapshot failed\n";
                return 1;
            }
            std::cerr << "[prune] Done.\n";
            result = 0;
        }
    } else if (command == "hint_extract") {
        // Should have been caught by the early-exit above; only reached if built without llama.cpp
        std::cerr << "[hint_extract] built without CHITTA_WITH_LLAMA_CPP\n";
        result = 1;
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage(argv[0]);
        result = 1;
    }

    return result;
}
