#pragma once
// Read-only diagnostics HTTP server for remote monitoring over the tailnet.
//
// A tiny, purpose-built HTTP/1.1 server: GET-only, bearer-token guarded, and it
// never touches trading state. It exists because the terminal runs unattended
// on a VPS and this PC pulls status over Tailscale; a full HTTP library would
// be ~10k lines of dependency for three read-only routes, so this hand-rolls
// the minimum on the Winsock stack the app already links (see engine net_ws.h
// for the same low-level socket style).
//
// Routes: GET  /diag        -> JSON body from the diag provider (auth required),
//         GET  /logs?since=N-> incremental log lines from the logs provider (auth),
//         GET  /            -> HTML shell from the root provider (no auth: no
//                             secrets, its JS forwards the URL's ?token= onward),
//         POST /control/<a> -> control action (opt-in; SEPARATE token; see
//                             set_control). Absent a control token, /control/* 404s.
// Auth accepts either "Authorization: Bearer <token>" or a ?token=<token> query
// param, so both curl and a browser work; the token is high-entropy and the hop
// never leaves the encrypted tailnet.
//
// Threading: one accept thread, connections served serially (a single monitor
// polls every few seconds — no concurrency needed). Providers are invoked ON
// THE SERVER THREAD, so they must be thread-safe; the app satisfies this by
// publishing a pre-rendered snapshot string under its own lock.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tt::ui {

class DiagServer {
public:
    // Returns the current body for a route (called on the server thread).
    using BodyProvider = std::function<std::string()>;
    // /logs body for everything after cursor `since` (called on the server thread).
    using LogsProvider = std::function<std::string(uint64_t since)>;
    // SSE tail: returns the ready-to-send event text for everything after
    // `cursor`, advancing it; "" when nothing new. Called repeatedly on a
    // per-stream thread, so it must be thread-safe.
    using TailProvider = std::function<std::string(uint64_t& cursor)>;
    // Handles POST /control/<action>; returns the JSON response body. Invoked on
    // the server thread, so it must be thread-safe — it should hand the action
    // off to the owner (e.g. set an atomic the UI thread consumes), NOT mutate
    // engine state directly.
    using ControlFn = std::function<std::string(const std::string& action)>;
    using LogFn = std::function<void(std::string)>;

    DiagServer() = default;
    ~DiagServer();
    DiagServer(const DiagServer&) = delete;
    DiagServer& operator=(const DiagServer&) = delete;

    // Binds host:port and starts the accept thread. host "0.0.0.0" binds all
    // interfaces. token is the required bearer credential. diag/root supply the
    // /diag and / bodies. on_log (optional) receives one-line status/errors.
    // Returns false (after logging) if the socket cannot bind.
    bool start(const std::string& host, uint16_t port, std::string token,
               BodyProvider diag, LogsProvider logs, BodyProvider root,
               LogFn on_log = {});
    void stop();   // idempotent; unblocks and joins the accept thread
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Enable POST /control/<action>, guarded by `token` (separate from the
    // read-only token). Call before start(); an empty token leaves control off.
    void set_control(std::string token, ControlFn fn);

    // Enable GET /metrics (Prometheus exposition text, read-only token). Call
    // before start(); unset leaves /metrics returning 404.
    void set_metrics(BodyProvider fn);

    // Enable GET /events (Server-Sent Events log tail, read-only token). Call
    // before start(); unset leaves /events returning 404. Each stream runs on
    // its own thread so a long-lived tail never blocks the accept loop (control
    // and /diag stay responsive); concurrent streams are capped.
    void set_tail(TailProvider fn);

private:
    void accept_loop();
    void serve(uintptr_t client);
    // Serve one SSE connection to completion on its own thread, starting the
    // tail at start_cursor (0 = recent tail; else resume, e.g. Last-Event-ID).
    void run_stream(uintptr_t client, uint64_t start_cursor);
    void reap_streams();   // join+drop finished streams (call under streams_mu_)

    BodyProvider diag_, root_, metrics_;
    LogsProvider logs_;
    ControlFn control_;
    TailProvider tail_;
    LogFn log_;
    std::string token_;
    std::string control_token_;   // empty = control disabled
    uintptr_t listen_sock_ = ~uintptr_t{0};   // INVALID_SOCKET
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;

    // Live SSE streams. Each owns its socket + thread; done flips when the loop
    // exits so a later connect (or stop) can join and drop it.
    struct Stream {
        uintptr_t sock;
        std::atomic<bool> done{false};
        std::thread th;
    };
    static constexpr size_t kMaxStreams = 4;
    std::mutex streams_mu_;
    std::vector<std::unique_ptr<Stream>> streams_;
};

} // namespace tt::ui
