#pragma once
// Read-only mobile status page. A tiny embedded HTTP server on its own
// thread serves a phone-sized HTML dashboard plus GET /api/status JSON built
// from Engine::live_snapshot(). The engine thread is untouched: the server
// is just another snapshot reader, like the UI thread, at ~1 Hz per client.
//
// Security posture (v1, deliberate):
//  - read-only: no endpoint submits, cancels, or halts anything
//  - every request must carry the per-run random token (?t=... query)
//  - LAN use; for remote viewing prefer a VPN (Tailscale) over port
//    forwarding

#include "engine/engine.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace tt::ui {

class StatusServer {
public:
    // Generates the access token; call url() after start() for the full link.
    explicit StatusServer(Engine& engine, int port = 8420);
    ~StatusServer();   // stops and joins

    bool start();      // false if the port could not be bound
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    int port() const { return port_; }
    const std::string& token() const { return token_; }
    // "http://<lan-ip>:8420/?t=<token>" — what to type on the phone.
    std::string url() const;

private:
    void serve();
    std::string status_json() const;

    Engine& engine_;
    int port_;
    std::string token_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace tt::ui
