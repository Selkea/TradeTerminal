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
// Routes: GET /diag -> JSON body from the diag provider (auth required),
//         GET /     -> HTML shell from the root provider (no auth: no secrets,
//                      its JS forwards the URL's ?token= to /diag).
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
#include <string>
#include <thread>

namespace tt::ui {

class DiagServer {
public:
    // Returns the current body for a route (called on the server thread).
    using BodyProvider = std::function<std::string()>;
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
               BodyProvider diag, BodyProvider root, LogFn on_log = {});
    void stop();   // idempotent; unblocks and joins the accept thread
    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    void accept_loop();
    void serve(uintptr_t client);

    BodyProvider diag_, root_;
    LogFn log_;
    std::string token_;
    uintptr_t listen_sock_ = ~uintptr_t{0};   // INVALID_SOCKET
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace tt::ui
