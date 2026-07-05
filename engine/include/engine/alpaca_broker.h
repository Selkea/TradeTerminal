#pragma once
// Alpaca broker adapter (paper or live): REST for order entry, the account
// websocket ("trade_updates" stream) for acks, fills, cancels, and rejects.
//
// Threading model per the IBrokerAdapter contract:
//  - Engine thread calls submit/cancel/cancel_all/flatten. Each pushes a
//    command into an SPSC ring and returns immediately.
//  - One I/O thread owns ALL network activity (libcurl REST calls and the
//    websocket). It drains commands, talks to Alpaca, and pushes translated
//    EngineEvents into an SPSC event ring the engine drains via poll_event().
//  - Orders carry client_order_id "tt-<session_ms>-<local_id>", so every
//    trade_updates message maps back to our id without a broker->local
//    lookup; the local->broker uuid map (needed for cancels) lives entirely
//    on the I/O thread.
//
// v1 limitations, by design:
//  - Positions/cash are still tracked locally by the engine's Portfolio from
//    fill events; no startup reconciliation against GET /v2/positions yet.
//  - If the websocket drops, submit() rejects until it reconnects (fills
//    that happened while disconnected would otherwise be lost silently).
//  - REST calls run on the same thread as the websocket poll, so a slow
//    submit briefly delays fill processing. Fine at paper-trading rates.

#include "engine/broker.h"
#include "engine/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tt {

struct AlpacaConfig {
    std::string key_id;       // APCA-API-KEY-ID
    std::string secret;       // APCA-API-SECRET-KEY
    std::string rest_url = "https://paper-api.alpaca.markets";
    std::string stream_url = "wss://paper-api.alpaca.markets/stream";
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
};

// One parsed trade_updates message. Split out (with the parser below) so the
// JSON -> event translation is unit-testable without a network.
struct AlpacaTradeUpdate {
    enum Kind : uint8_t { None = 0, Ack, Fill, Cancel, Reject };
    Kind kind = None;
    uint64_t local_id = 0;    // from client_order_id "tt-...-N"; 0 = not ours
    std::string broker_id;    // Alpaca order uuid
    uint32_t symbol_id = 0;   // 0 = symbol not in this session's table
    Side side = Side::Buy;
    double price = 0.0, qty = 0.0;   // per-execution, for Fill
    int64_t ts_ns = 0;               // 0 = timestamp missing/unparsable
};

// Returns false for anything that isn't a trade_updates message we act on
// (authorization acks, heartbeats, unknown events, malformed JSON).
bool alpaca_parse_trade_update(std::string_view json_text,
                               const std::vector<std::string>& symbols,
                               AlpacaTradeUpdate& out);

// Synchronous GET /v2/account credential check (blocks up to ~10 s — call it
// from a worker thread, never the UI or engine thread). True = credentials
// valid; detail carries "account <number> <status>" or the error message.
bool alpaca_verify_account(const std::string& rest_url, const std::string& key_id,
                           const std::string& secret, std::string& detail);

// One row of GET /v2/positions (exposed for unit tests).
struct AlpacaPosition {
    std::string symbol;
    uint32_t symbol_id = 0;   // 0 = not in this session's table
    double qty = 0.0;         // signed; negative = short
    double avg_price = 0.0;
};
bool alpaca_parse_positions(std::string_view json_text,
                            const std::vector<std::string>& symbols,
                            std::vector<AlpacaPosition>& out);

class AlpacaBroker final : public IBrokerAdapter {
public:
    explicit AlpacaBroker(AlpacaConfig cfg);
    ~AlpacaBroker() override;   // signals and joins the I/O thread

    uint64_t submit(const OrderRequest& r, int64_t now_ns) override;
    bool cancel(uint64_t order_id) override;
    void cancel_all() override;
    void flatten() override;
    bool poll_event(EngineEvent& out) override { return ev_ring_->try_pop(out); }
    bool ready() const override { return ready_.load(std::memory_order_acquire); }

    // Adapter status/log lines (I/O thread produces, UI drains each frame).
    bool pop_log(std::string& out);

private:
    struct Cmd {
        enum : uint8_t { Submit = 1, Cancel, CancelAll, Flatten } type = Submit;
        uint64_t local_id = 0;
        OrderRequest req{};
    };
    struct Io;   // all libcurl state; defined in alpaca_broker.cpp, I/O thread only

    void io_loop();
    void push_ev(const EngineEvent& ev);
    void push_reject(uint64_t local_id);
    void log(std::string line);
    bool push_cmd(const Cmd& c);

    AlpacaConfig cfg_;
    std::string client_prefix_;   // "tt-<session epoch ms>-"

    using CmdRing = SpscRing<Cmd, 1 << 10>;
    using EvRing = SpscRing<EngineEvent, 1 << 12>;
    std::unique_ptr<CmdRing> cmd_ring_ = std::make_unique<CmdRing>();
    std::unique_ptr<EvRing> ev_ring_ = std::make_unique<EvRing>();

    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    std::thread io_thread_;   // last member: starts in ctor, joined in dtor
};

} // namespace tt
