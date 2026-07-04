#pragma once
// Client for the Python data sidecar. Owns the whole sidecar lifecycle:
//  - spawns `python dataservice/main.py --port 0` with stdout piped,
//    parses the TT_PORT=<n> handshake line
//  - puts the child in a kill-on-close Job Object so it can never outlive us
//  - connects over loopback TCP and speaks the framed protocol
//  - reader thread dispatches frames to callbacks (which run on that thread)
//  - heartbeats with PING/PONG; a dead or silent sidecar is torn down and
//    respawned with backoff

#include "market_data.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tt::net {

struct CandleBatch {
    uint32_t id = 0;
    std::string symbol, interval;
    bool cached = false;
    std::vector<Candle> candles;
};

class IpcClient {
public:
    struct Callbacks {
        std::function<void(CandleBatch&&)> on_candles;
        // symbol is resolved from the subscription's index map
        std::function<void(const std::string& symbol, const Quote&)> on_tick;
        std::function<void(uint32_t id, std::string code, std::string message)> on_error;
        std::function<void(std::string)> on_log;
    };

    IpcClient(std::string python_cmd, std::string service_dir);
    ~IpcClient();

    void start(Callbacks cbs);
    void stop();

    bool connected() const { return connected_.load(std::memory_order_relaxed); }
    // Bumped on every successful (re)connect; lets panels re-subscribe.
    uint64_t connection_generation() const { return conn_gen_.load(std::memory_order_relaxed); }

    // Thread-safe; return the request id used (0 if not connected).
    uint32_t request_candles(const std::string& symbol, const std::string& interval,
                             const std::string& range);
    uint32_t subscribe_quotes(const std::vector<std::string>& symbols, int poll_s);
    void unsubscribe(uint32_t sub_id);

private:
    void worker();
    bool spawn_sidecar(int& port_out);
    bool connect_socket(int port);
    void read_loop();
    void teardown();
    bool send_frame(uint16_t type, const void* payload, uint32_t len);
    bool send_json(uint16_t type, const std::string& json_text);
    void dispatch(uint16_t type, const std::vector<char>& payload);
    void log(std::string msg);

    std::string python_cmd_, service_dir_;
    Callbacks cbs_;

    std::thread worker_;
    std::thread stdout_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> conn_gen_{0};
    std::atomic<uint32_t> next_id_{1};

    std::mutex send_mu_;
    uintptr_t sock_ = ~uintptr_t{0};        // SOCKET, avoids winsock.h in header
    void* proc_ = nullptr;                   // HANDLE
    void* job_ = nullptr;                    // HANDLE
    void* stdout_read_ = nullptr;            // HANDLE

    std::mutex subs_mu_;
    std::unordered_map<uint32_t, std::vector<std::string>> pending_subs_;  // req id -> symbols
    std::unordered_map<uint32_t, std::vector<std::string>> subs_;          // sub id -> symbols
};

} // namespace tt::net
